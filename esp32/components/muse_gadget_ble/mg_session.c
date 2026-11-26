/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mg_session.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "mg_session_seal.h"
#include "mg_token_proof.h"
#include "mgcommands.h"

struct mg_session_out {
    mg_session_out_t *next;
    uint32_t len;
    /* Sealed session: the plaintext waits at data + 4 until the first
     * fragment goes, then it's sealed in place under the next device ->
     * client counter (so counters go out in order: a credentials_saved
     * queued meanwhile takes one too). */
    bool seal_pending;
    uint8_t data[];
};

_Static_assert(MG_SESSION_SAVED_PT == MGSEAL_SAVED_LEN && MG_SESSION_SAVED_SEALED == MGSEAL_SAVED_SEALED,
               "a sealed credentials_saved, as layer 1 makes it");

static uint32_t rd32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* Every allocation the session makes (out-queue copies, credentials both
 * ways, keys, the incremental tag) goes through the platform's allocator:
 * PSRAM first where the board has some (mg_ble.c), so a session never runs
 * the internal heap down (the Watcher's malloc puts anything under
 * 4 KB in internal RAM). NULL: the C library's. */
static void *m_alloc(const mg_session_t *s, size_t n)
{
    return s->cfg.msg_alloc ? s->cfg.msg_alloc(n) : malloc(n);
}

static void *m_calloc(const mg_session_t *s, size_t n)
{
    void *p = m_alloc(s, n);
    if (p) {
        memset(p, 0, n);
    }
    return p;
}

static void m_free(const mg_session_t *s, void *p)
{
    if (s->cfg.msg_release) {
        s->cfg.msg_release(p);
    } else {
        free(p);
    }
}

/* A sealed message's counter: its first 4 bytes, big-endian (mgcommands.h, Sealed credentials). */
static uint32_t rd_be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* A sealed data message's incremental tag goes when the message does
 * (taken only while one arrives: nothing held between messages). */
static void drop_dtag(mg_session_t *s)
{
    if (s->dtag) {
        mgseal_wipe(s->dtag, sizeof(*s->dtag));
        m_free(s, s->dtag);
        s->dtag = NULL;
    }
}

/* A client's data message refused (for the log: why and its total_len, never its bytes). */
static void data_refused(mg_session_t *s, const char *why, uint32_t len)
{
    if (s->ops.data_refused) {
        s->ops.data_refused(s->ops.ctx, why, len);
    }
}

static void put32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
}

/* A control answer, kept until a notification takes it (mg_session_pump()):
 * a full stack mustn't lose the hello answer ahead of the credentials, a
 * credentials_saved or an error. Every answer fits a slot (asserted at each
 * caller); dropped only if MG_SESSION_CTL_HELD wait, and then counted
 * (ctl_dropped, on the console's status line). */
static void queue_ctl(mg_session_t *s, const uint8_t *m, size_t n)
{
    if (s->ctl_count == MG_SESSION_CTL_HELD || n > sizeof(s->ctl[0])) {
        s->ctl_dropped++;
        return;
    }
    uint8_t i = (uint8_t)((s->ctl_head + s->ctl_count++) % MG_SESSION_CTL_HELD);
    memcpy(s->ctl[i], m, n);
    s->ctl_len[i] = (uint8_t)n;
}

/* The device's counter, once a sealed message took it: 0xffffffff is the last, so the session seals
 * nothing more and refuses the client's sealed messages (the client starts a new one: a new proof and hello). */
static inline void out_counter_used(mg_session_t *s)
{
    if (s->ctr_out == UINT32_MAX) {
        s->out_spent = true;
    } else {
        s->ctr_out++;
    }
}

static void send_error(mg_session_t *s, uint16_t code)
{
    uint8_t m[5] = { mg_command_error, mg_command_session, mg_error_data_code, (uint8_t)code, (uint8_t)(code >> 8) };
    _Static_assert(sizeof(m) <= MG_SESSION_CTL_MAX, "an error fits a slot");
    queue_ctl(s, m, sizeof(m));
}

/*
 * credentials_saved, after the commit. A sealed session whose hello carried
 * sealed_saved seals it (vectors sealed_credentials_saved
 * and sealed_data_large): pt is its plaintext, [1 || SHA-256(refresh_token)[0:8]]
 * or, NULL, not saved ([0 || zeros]); queued, it goes from mg_session_pump()
 * under the next device -> client counter. Any other session (plain, or
 * sealed by a client that doesn't count the answer: an older iOS stack and
 * the bench app) gets the legacy one byte [result], which takes no
 * counter: exactly as before 5.7.
 */
static void send_saved(mg_session_t *s, bool saved, const uint8_t *pt)
{
    if (!s->saved_sealed) {
        /* A sealed client without sealed_saved: the one-byte answer. */
        uint8_t m[3] = { mg_command_session, mg_session_credentials_saved, saved ? 1 : 0 };
        _Static_assert(sizeof(m) <= MG_SESSION_CTL_MAX, "credentials_saved fits a slot");
        queue_ctl(s, m, sizeof(m));
        return;
    }
    if (s->saved_count == MG_SESSION_SAVED_HELD) {
        return;   /* a client that didn't wait for its answers: the oldest go first, this one not at all */
    }
    uint8_t *q = s->saved_q[(s->saved_head + s->saved_count++) % MG_SESSION_SAVED_HELD];
    if (saved && pt) {
        memcpy(q, pt, MG_SESSION_SAVED_PT);
    } else {
        memset(q, 0, MG_SESSION_SAVED_PT);
    }
}

/* ---- state ---------------------------------------------------------------- */

static void drop_cred_in(mg_session_t *s)
{
    if (s->cred_in) {
        mg_token_proof_wipe(s->cred_in, s->cred_in_total);
        m_free(s, s->cred_in);
    }
    s->cred_in = NULL;
    s->cred_in_len = s->cred_in_total = 0;
}

static void drop_sealed(const mg_session_t *s, uint8_t **msg, uint32_t *len)
{
    if (*msg) {
        mg_token_proof_wipe(*msg, *len);
        m_free(s, *msg);
    }
    *msg = NULL;
    *len = 0;
}

static void drop_update(mg_session_t *s)
{
    drop_sealed(s, &s->upd_sealed, &s->upd_sealed_len);
    drop_sealed(s, &s->upd_prev, &s->upd_prev_len);
    if (s->upd_access) {
        mg_token_proof_wipe(s->upd_access, MG_SESSION_TOKEN_MAX);
        mg_token_proof_wipe(s->upd_refresh, MG_SESSION_TOKEN_MAX);
        m_free(s, s->upd_access);
        m_free(s, s->upd_refresh);
    }
    s->upd_access = s->upd_refresh = NULL;
    s->upd_expires = 0;
}

/* The sealed session's keys go with its hello. */
static void drop_keys(mg_session_t *s)
{
    if (s->keys) {
        mgseal_wipe(s->keys, sizeof(*s->keys));
        m_free(s, s->keys);
    }
    s->keys = NULL;
    s->sealed = false;
    s->saved_sealed = false;
    s->ctr_out = s->ctr_in = 0;
    s->ctr_hole = 0;
    s->in_spent = s->out_spent = false;
    /* Its credentials_saved answers go with it (a new hello: the client
     * sends a pair still pending again). */
    mgseal_wipe(s->saved_q, sizeof(s->saved_q));
    mgseal_wipe(s->saved_out, sizeof(s->saved_out));
    mgseal_wipe(s->taken_pt, sizeof(s->taken_pt));
    s->saved_head = s->saved_count = s->saved_off = 0;
}

static void drop_cred_out(mg_session_t *s)
{
    if (s->cred_out) {
        mg_token_proof_wipe(s->cred_out, s->cred_out_cap);   /* the whole allocation */
        m_free(s, s->cred_out);
    }
    s->cred_out = NULL;
    s->cred_out_len = s->cred_out_off = s->cred_out_cap = 0;
}

/*
 * The session's end (a state other than open, a new hello, the connection):
 * every message either way and every partial one goes, and credit is the
 * window again on both sides. Nothing is ever replayed into the next session.
 */
static void end_session(mg_session_t *s, bool tell)
{
    bool was = s->open;
    s->open = false;
    s->send_credit = s->client_window;
    s->owed_credit = 0;
    s->rx_credit = s->cfg.window;   /* the client's whole window again */
    s->nudge = false;
    s->in_head = s->in_used = s->in_part = s->in_total = s->in_count = 0;
    s->in_early = s->head_early = 0;
    drop_dtag(s);   /* a sealed message in flight keeps its counter used */
    if (s->ring && !s->cfg.buf) {
        /* Taken for the session: given back (nothing held between sessions). */
        s->cfg.release(s->ring);
        s->ring = NULL;
    } else if (s->ring) {
        memset(s->ring + s->cfg.max_message, 0, (s->cfg.max_message + 7) / 8);
    }
    if (s->sealed && s->out_head && s->out_off == 0 && !s->out_head->seal_pending) {
        /* Sealed, its first fragment refused by the stack, never out: dropped with its counter given back, as at a
         * clear, so the next sealed message takes it and the client sees no gap. Only the
         * head can be sealed and not started. */
        if (s->out_spent) {
            s->out_spent = false;   /* it held the last one */
        } else {
            s->ctr_out--;
        }
    }
    while (s->out_head) {
        mg_session_out_t *m = s->out_head;
        s->out_head = m->next;
        m_free(s, m);
    }
    s->out_tail = NULL;
    s->out_bytes = 0;
    s->out_off = 0;
    if (was && tell && s->ops.state) {
        s->ops.state(s->ops.ctx, mg_session_state_closed, s->epoch);
    }
}

void mg_session_init(mg_session_t *s, const mg_session_config_t *cfg, const mg_session_ops_t *ops)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    s->ops = *ops;
    s->mtu = 23;
    s->open_request = -1;
    s->rx_credit = s->cfg.window;
    s->ring = s->cfg.buf;
    if (s->ring) {
        memset(s->ring + s->cfg.max_message, 0, (s->cfg.max_message + 7) / 8);
    }
}

/* A sealed clear part way in, taken, erasing or answering belongs to the session that ends here: its bytes
 * and its answer go, so nothing of it completes under a later hello's counters (mgcommands.h: a new hello
 * drops partial sealed messages). A taken clear's erase still runs (its tag checked: the Forget happens). */
static void drop_clear(mg_session_t *s)
{
    if (s->clr_pending && s->ops.clear) {
        s->ops.clear(s->ops.ctx);
    }
    mg_token_proof_wipe(s->clr, sizeof(s->clr));
    s->clr_got = s->clr_off = 0;
    s->clr_in = s->clr_pending = s->clearing = s->clr_erasing = s->clr_due = false;
}

void mg_session_reset(mg_session_t *s)
{
    drop_clear(s);
    s->upd_taken = false;
    s->hello_gen++;   /* a pair being stored outside the lock gets no answer here */
    end_session(s, true);
    drop_cred_in(s);
    drop_update(s);   /* not stored: the client sends it again on the next connection */
    drop_cred_out(s);
    drop_keys(s);
    s->ctl_head = s->ctl_count = 0;
    s->hello = false;
    s->client_max = s->client_window = 0;
    s->send_credit = 0;
    s->open_request = -1;
    s->mtu = 23;
}

void mg_session_set_mtu(mg_session_t *s, uint16_t mtu)
{
    s->mtu = mtu;
}

static bool can_seal(const mg_session_t *s)
{
    return s->ops.take_nonces && s->ops.proof_key;
}

bool mg_session_sealed(const mg_session_t *s)
{
    return s->hello && s->sealed;
}

uint8_t mg_session_caps(const mg_session_t *s)
{
    if (!can_seal(s)) {
        return 0;
    }
    return (uint8_t)(mg_session_cap_sealed | (s->cfg.client_refreshes ? mg_session_cap_client_refreshes : 0)
                     | (s->cfg.sealed_clear ? mg_session_cap_sealed_clear : 0));
}

bool mg_session_hello_sealed(const uint8_t *a, size_t n)
{
    /* [hello, version, max_message (4), window (4), hello_flags] */
    return n >= 11 && a[0] == mg_session_hello && (a[10] & mg_session_hello_flag_sealed);
}

bool mg_session_open(const mg_session_t *s)
{
    return s->open;
}

uint32_t mg_session_id(const mg_session_t *s)
{
    return s->open ? s->opened : 0;
}

void mg_session_reask_marks(const mg_session_t *s, uint32_t *hellos, uint32_t *saved)
{
    *hellos = s->reask_hellos;
    *saved = s->reask_saved;
}

uint32_t mg_session_client_max(const mg_session_t *s)
{
    if (!s->hello) {
        return 0;
    }
    /* Within the client's max_message and the gadget's own (nothing is sized
     * by the client's values alone); sealed, less the counter and tag. */
    uint32_t max = s->client_max < s->cfg.max_message ? s->client_max : s->cfg.max_message;
    return !s->sealed ? max : max > MGSEAL_OVERHEAD ? max - MGSEAL_OVERHEAD : 0;
}

/* Fragment payload room after a header of hdr bytes, ATT_MTU - 3 at most. */
static size_t room_after(const mg_session_t *s, size_t hdr)
{
    size_t max = s->mtu > 3 ? (size_t)s->mtu - 3 : 0;
    if (max > MG_SESSION_FRAGMENT_MAX) {
        max = MG_SESSION_FRAGMENT_MAX;
    }
    return max > hdr ? max - hdr : 0;
}

/* ---- sending -------------------------------------------------------------- */

/* Messages to the VM, as far as the client's credit goes. A sealed one is
 * sealed as its first fragment goes (its counter then). False: the stack
 * took no more. */
static bool pump_data(mg_session_t *s)
{
    while (s->open && s->out_head && s->send_credit > 0) {
        mg_session_out_t *m = s->out_head;
        bool first = s->out_off == 0;
        if (first && (s->clr_pending || s->clr_erasing || s->clr_due || s->clearing)) {
            break;   /* a sealed clear taken: its answer goes next, no new message before it */
        }
        size_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        size_t n = room_after(s, hdr);
        size_t left = m->len - s->out_off;
        if (!n) {
            return false;
        }
        if (s->sealed && first && s->send_credit < 4) {
            break;   /* a sealed first fragment carries the whole counter (mgcommands.h): wait for credit */
        }
        if (m->seal_pending && s->out_spent) {
            /* No device counter left: nothing more is sealed in this session; dropped. */
            s->out_head = m->next;
            if (!s->out_head) {
                s->out_tail = NULL;
            }
            s->out_bytes -= m->len;
            mgseal_wipe(m->data, m->len);
            m_free(s, m);
            continue;
        }
        if (m->seal_pending) {
            m->seal_pending = false;
            if (mgseal_seal(s->keys, MGSEAL_DIR_DEVICE, s->ctr_out, m->data, m->len - MGSEAL_OVERHEAD) != MGSEAL_OK) {
                /* Can't seal it: dropped (its counter not used). */
                s->out_head = m->next;
                if (!s->out_head) {
                    s->out_tail = NULL;
                }
                s->out_bytes -= m->len;
                mgseal_wipe(m->data, m->len);
                m_free(s, m);
                continue;
            }
            out_counter_used(s);
        }
        n = n < left ? n : left;
        n = n < s->send_credit ? n : s->send_credit;
        uint8_t *f = s->frame;
        f[0] = mg_command_session;
        f[1] = mg_session_data;
        /* Every fragment of a sealed message says so (mgcommands.h, Sealed data). */
        f[2] = (uint8_t)((s->sealed ? mg_session_flag_sealed : 0) | (first ? mg_session_flag_first : 0)
                         | (n == left ? mg_session_flag_last : 0));
        if (first) {
            put32(f + 3, m->len);
        }
        memcpy(f + hdr, m->data + s->out_off, n);
        if (!s->ops.notify(s->ops.ctx, f, hdr + n)) {
            return false;
        }
        s->send_credit -= (uint32_t)n;
        s->out_off += (uint32_t)n;
        if (s->out_off == m->len) {
            s->out_head = m->next;
            if (!s->out_head) {
                s->out_tail = NULL;
            }
            s->out_bytes -= m->len;
            s->out_off = 0;
            m_free(s, m);
        }
    }
    return true;
}

/* ---- the sealed clear (mg_session_clear) ---------------------------------- */

static bool counter_ok(const mg_session_t *s, uint32_t ctr);
static void take_counter(mg_session_t *s, uint32_t ctr);
static void clear_answer(mg_session_t *s, bool ok);
static void clear_result(mg_session_t *s, bool ok);
/* A sealed clear is taken: waiting for its erase, erasing, erased with its answer to go, or answering. */
static inline bool clear_taken(const mg_session_t *s)
{
    return s->clr_pending || s->clr_erasing || s->clr_due || s->clearing;
}

bool mg_session_clear_owed(const mg_session_t *s)
{
    return clear_taken(s);
}
static void unuse_counter(mg_session_t *s, uint32_t ctr);

/* Its sealed answer, from clr[] (clr_off bytes sent), fragmented as credentials_saved; once it's out the session
 * ends as at a reset (the MTU kept). False: the stack took no more. */
static bool pump_clear(mg_session_t *s)
{
    while (s->clearing) {
        bool first = s->clr_off == 0;
        size_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        size_t n = room_after(s, hdr);
        size_t left = MG_SESSION_CLEAR_SEALED - s->clr_off;
        if (!n) {
            return false;
        }
        n = n < left ? n : left;
        uint8_t *f = s->frame;
        f[0] = mg_command_session;
        f[1] = mg_session_clear;
        f[2] = (uint8_t)(mg_session_flag_sealed | (first ? mg_session_flag_first : 0)
                         | (n == left ? mg_session_flag_last : 0));
        if (first) {
            put32(f + 3, MG_SESSION_CLEAR_SEALED);
        }
        memcpy(f + hdr, s->clr + s->clr_off, n);
        bool ok = s->ops.notify(s->ops.ctx, f, hdr + n);
        mg_token_proof_wipe(f, hdr + n);
        if (!ok) {
            return false;
        }
        s->clr_off += (uint8_t)n;
        if (s->clr_off == MG_SESSION_CLEAR_SEALED) {
            uint16_t mtu = s->mtu;
            s->clearing = false;
            mg_session_reset(s);   /* answered: the session ends */
            s->mtu = mtu;
        }
    }
    return true;
}

/* A taken sealed clear, once what goes out before it is out (the credentials, a credentials_saved, an update
 * waiting to be stored): erase, then the sealed answer [1] ([0] if the erase failed) on the next device -> client
 * counter, from the erase's own result (now, or when a board's erase running elsewhere ends), once a data message
 * part-way is out. */
static void clear_now(mg_session_t *s)
{
    if (s->clr_due && s->out_off == 0) {
        s->clr_due = false;   /* the data message part way out is out: the answer goes now */
        clear_answer(s, s->clr_ok);
        return;
    }
    /* The erase itself waits only for what it would undo or what needs the stored pair (the credentials, a
     * credentials_saved, an update waiting to be stored); never for a data message part way out, which is sealed
     * whole already: only the answer waits for that. */
    if (!s->clr_pending || !s->keys || s->cred_out || s->saved_count || s->upd_sealed || s->upd_access
        || s->upd_taken) {
        return;
    }
    s->clr_pending = false;
    int r = s->ops.clear(s->ops.ctx);
    if (r == MG_SESSION_CLEAR_PENDING) {
        s->clr_erasing = true;   /* answered from its result (mg_session_clear_done()) */
        return;
    }
    clear_result(s, r == 1);
}

void mg_session_clear_done(mg_session_t *s, bool erased)
{
    if (!s->clr_erasing) {
        return;   /* the session ended meanwhile: the erase went on, nobody to answer */
    }
    s->clr_erasing = false;
    clear_result(s, erased);
    mg_session_pump(s);
}

/* The erase's result: its answer now, or once the data message part way out is out (clear_now()). */
static void clear_result(mg_session_t *s, bool ok)
{
    if (s->out_off != 0) {
        s->clr_due = true;
        s->clr_ok = ok;
        return;
    }
    clear_answer(s, ok);
}

/* The sealed answer [1] (erased) or [0] on the next device -> client counter; the session ends once it's out. */
static void clear_answer(mg_session_t *s, bool ok)
{
    s->clr[4] = ok ? 1 : 0;
    if (!s->keys || s->out_spent || mgseal_seal(s->keys, MGSEAL_DIR_DEVICE, s->ctr_out, s->clr, 1) != MGSEAL_OK) {
        uint16_t mtu = s->mtu;
        mg_session_reset(s);   /* erased, no answer */
        s->mtu = mtu;
        return;
    }
    out_counter_used(s);
    s->clearing = true;
    s->clr_off = 0;
}

/* Client -> device, sealed only, fragmented like a credentials_update (outside credit): counter || 0 || tag.
 * Every refusal erases nothing. */
static void on_clear(mg_session_t *s, const uint8_t *p, size_t n)
{
    if (n < 1) {
        send_error(s, mg_error_code_invalid_length);
        return;
    }
    if (!s->hello) {
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    if (!s->ops.clear) {
        send_error(s, mg_error_code_unsupported);
        return;
    }
    if (s->sealed && s->out_spent) {
        send_error(s, mg_error_code_invalid_value);   /* no device counter left for its answer */
        return;
    }
    uint8_t flags = p[0];
    const uint8_t *rest = p + 1;
    size_t rn = n - 1;
    if (flags & mg_session_flag_first) {
        if (rn < 4) {
            send_error(s, mg_error_code_invalid_length);
            return;
        }
        uint32_t total = rd32(rest);
        rest += 4;
        rn -= 4;
        s->clr_in = false;
        if (!s->sealed || !(flags & mg_session_flag_sealed)) {
            send_error(s, mg_error_code_insecure);   /* never unsealed */
            return;
        }
        if (rn < 4) {
            send_error(s, mg_error_code_invalid_length);   /* not the whole counter */
            return;
        }
        if (total != MG_SESSION_CLEAR_SEALED || s->clr_pending || s->clr_erasing || s->clearing
            || !counter_ok(s, rd_be32(rest))) {
            send_error(s, mg_error_code_invalid_value);
            return;
        }
        take_counter(s, rd_be32(rest));   /* used from here, unless its tag is refused */
        if (s->cred_in || s->upd_sealed || s->upd_access || s->upd_taken) {
            /* A credentials_update arriving or waiting to be stored: busy, its counter used (the client
             * retries on the next one). */
            send_error(s, mg_error_code_busy);
            return;
        }
        s->clr_in = true;
        s->clr_got = 0;
    } else if (!s->clr_in) {
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    size_t got = s->clr_got;
    if (!rn || got + rn > MG_SESSION_CLEAR_SEALED || !(flags & mg_session_flag_last) != !(got + rn == MG_SESSION_CLEAR_SEALED)
        || !(flags & mg_session_flag_sealed)) {
        s->clr_in = false;
        mg_token_proof_wipe(s->clr, sizeof(s->clr));
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    memcpy(s->clr + got, rest, rn);
    s->clr_got = (uint8_t)(got + rn);
    if (s->clr_got < MG_SESSION_CLEAR_SEALED) {
        return;
    }
    s->clr_in = false;
    uint32_t ctr = rd_be32(s->clr);
    size_t pl = 0;
    if (mgseal_open(s->keys, MGSEAL_DIR_CLIENT, ctr, s->clr, MG_SESSION_CLEAR_SEALED, &pl) != MGSEAL_OK) {
        mg_token_proof_wipe(s->clr, sizeof(s->clr));
        unuse_counter(s, ctr);   /* refused at its tag: the counter isn't used */
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    bool zero = pl == 1 && s->clr[4] == 0;
    mg_token_proof_wipe(s->clr, sizeof(s->clr));
    if (!zero) {
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    if (s->cred_out && s->cred_out_off == 0 && s->ctr_out == 1) {
        /* The credentials still wait for room, none of them out: a device message not yet started never goes out,
         * and its counter (0: nothing sealed goes before the credentials) is never used, so the answer takes it (a
         * receiver takes only the next counter; mg-session-v1.json, "sealed clear while the credentials wait"). */
        drop_cred_out(s);
        s->ctr_out = 0;
    }
    s->clr_pending = true;
    clear_now(s);
}

/* The sealed credentials_saved answers, oldest first,
 * each framed as credentials: [0x89, 8, first|last|sealed, total_len 29,
 * counter || ciphertext || tag], fragmented to the MTU. The head is sealed
 * under ctr_out as its first fragment goes. False: the stack took no more. */
static bool pump_saved(mg_session_t *s)
{
    while (s->saved_count && s->keys && !s->out_spent) {   /* no device counter left: none goes */
        bool first = s->saved_off == 0;
        size_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        size_t n = room_after(s, hdr);
        size_t left = MG_SESSION_SAVED_SEALED - s->saved_off;
        if (!n) {
            return false;
        }
        if (first) {
            memcpy(s->saved_out + 4, s->saved_q[s->saved_head], MG_SESSION_SAVED_PT);
            if (mgseal_seal(s->keys, MGSEAL_DIR_DEVICE, s->ctr_out, s->saved_out, MG_SESSION_SAVED_PT) != MGSEAL_OK) {
                return false;   /* tried again at the next pump */
            }
        }
        n = n < left ? n : left;
        uint8_t *f = s->frame;
        f[0] = mg_command_session;
        f[1] = mg_session_credentials_saved;
        f[2] = (uint8_t)(mg_session_flag_sealed | (first ? mg_session_flag_first : 0)
                         | (n == left ? mg_session_flag_last : 0));
        if (first) {
            put32(f + 3, MG_SESSION_SAVED_SEALED);
        }
        memcpy(f + hdr, s->saved_out + s->saved_off, n);
        bool ok = s->ops.notify(s->ops.ctx, f, hdr + n);
        mgseal_wipe(f, hdr + n);
        if (!ok) {
            return false;   /* the same fragment next time (sealed again the same, if it was the first) */
        }
        if (first) {
            out_counter_used(s);   /* its counter is taken as its first fragment goes */
        }
        s->saved_off = (uint8_t)(s->saved_off + n);
        if (s->saved_off == MG_SESSION_SAVED_SEALED) {
            mgseal_wipe(s->saved_q[s->saved_head], MG_SESSION_SAVED_PT);
            mgseal_wipe(s->saved_out, sizeof(s->saved_out));
            s->saved_head = (uint8_t)((s->saved_head + 1) % MG_SESSION_SAVED_HELD);
            s->saved_count--;
            s->saved_off = 0;
        }
    }
    return true;
}

bool mg_session_pump(mg_session_t *s)
{
    while (s->ctl_count) {
        if (!s->ops.notify(s->ops.ctx, s->ctl[s->ctl_head], s->ctl_len[s->ctl_head])) {
            return true;
        }
        s->ctl_head = (uint8_t)((s->ctl_head + 1) % MG_SESSION_CTL_HELD);
        s->ctl_count--;
    }
    if (s->owed_credit || (s->nudge && s->open)) {
        uint8_t m[6] = { mg_command_session, mg_session_credit };
        put32(m + 2, s->owed_credit);
        if (!s->ops.notify(s->ops.ctx, m, sizeof(m))) {
            return true;
        }
        uint64_t c = (uint64_t)s->rx_credit + s->owed_credit;   /* given back: the client may send it again */
        s->rx_credit = c < s->cfg.window ? (uint32_t)c : s->cfg.window;
        s->owed_credit = 0;
        s->nudge = false;
    }
    if (s->open_request >= 0) {
        uint8_t m[3] = { mg_command_session, mg_session_open_request, (uint8_t)s->open_request };
        if (!s->ops.notify(s->ops.ctx, m, sizeof(m))) {
            return true;
        }
        s->open_request = -1;
    }
    /* The credentials, outside credit, each fragment as full as it goes. */
    while (s->cred_out) {
        bool first = s->cred_out_off == 0;
        size_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        size_t n = room_after(s, hdr);
        size_t left = s->cred_out_len - s->cred_out_off;
        if (!n) {
            return true;
        }
        n = n < left ? n : left;
        uint8_t *f = s->frame;
        f[0] = mg_command_session;
        f[1] = mg_session_credentials;
        f[2] = (uint8_t)(s->cred_out_flags | (first ? mg_session_flag_first : 0)
                         | (n == left ? mg_session_flag_last : 0));
        if (first) {
            put32(f + 3, s->cred_out_len);
        }
        memcpy(f + hdr, s->cred_out + s->cred_out_off, n);
        bool ok = s->ops.notify(s->ops.ctx, f, hdr + n);
        mg_token_proof_wipe(f, hdr + n);
        if (!ok) {
            return true;
        }
        s->cred_out_off += (uint32_t)n;
        if (s->cred_out_off == s->cred_out_len) {
            drop_cred_out(s);
        }
    }
    /* credentials_saved answers (sealed), after the credentials; a data
     * message whose first fragment is out keeps its (earlier) counter, so an
     * answer may come between its fragments, and one that hasn't started
     * takes its counter after the answers. One already sealed (its first
     * fragment refused by the stack) goes first: it holds the earlier counter. */
    if (s->out_head && s->out_off == 0 && s->sealed && !s->out_head->seal_pending && !pump_data(s)) {
        return true;
    }
    if (!pump_saved(s)) {
        return true;
    }
    clear_now(s);
    if (s->clearing) {
        return !pump_clear(s) || s->clearing;
    }
    if (!pump_data(s)) {
        return true;
    }
    clear_now(s);   /* a data message part-way out has just ended */
    if (s->clearing) {
        return !pump_clear(s) || s->clearing;
    }
    return (s->open && s->out_head && s->send_credit > 0) || s->saved_count || s->clr_pending;
}

size_t mg_session_room(const mg_session_t *s)
{
    if (!s->open || s->out_bytes >= s->cfg.out_max) {
        return 0;
    }
    size_t room = s->cfg.out_max - s->out_bytes;
    if (s->sealed) {
        room = room > MGSEAL_OVERHEAD ? room - MGSEAL_OVERHEAD : 0;   /* the queue holds sealed bytes */
    }
    return room;
}

/*
 * The one send point for data. A sealed session's messages are sealed whole
 * before fragmentation (mgcommands.h, Sealed data):
 * counter || ciphertext || tag under this hello's keys, with the
 * device-to-client counter the credentials and credentials_saved share, so
 * total_len, the client's window and its credit count sealed bytes. Sealed
 * in the queue's own copy (no other buffer), as its first fragment goes
 * (pump_data()): counters go out in the order the client takes them.
 */
bool mg_session_send(mg_session_t *s, const uint8_t *msg, size_t len)
{
    if (!s->open || !len || len > mg_session_client_max(s) || len > mg_session_room(s)) {
        return false;
    }
    size_t over = s->sealed ? MGSEAL_OVERHEAD : 0;
    mg_session_out_t *m = m_alloc(s, sizeof(*m) + len + over);
    if (!m) {
        return false;
    }
    m->next = NULL;
    m->len = (uint32_t)(len + over);
    m->seal_pending = s->sealed;
    memcpy(m->data + (s->sealed ? 4 : 0), msg, len);
    if (s->out_tail) {
        s->out_tail->next = m;
    } else {
        s->out_head = m;
    }
    s->out_tail = m;
    s->out_bytes += len + over;
    mg_session_pump(s);
    return true;
}

void mg_session_request(mg_session_t *s, bool open)
{
    s->open_request = open ? 1 : 0;
    mg_session_pump(s);
}

void mg_session_nudge(mg_session_t *s)
{
    if (!s->open) return;
    s->nudge = true;
    mg_session_pump(s);
}

/* ---- receiving ------------------------------------------------------------ */

/* The ring's end marks: a bit for each byte of the ring. */
static uint8_t *ends(const mg_session_t *s)
{
    return s->ring + s->cfg.max_message;
}

/* The oldest whole message's length: up to the first end mark from in_head. */
static uint32_t oldest_len(const mg_session_t *s)
{
    const uint8_t *e = ends(s);
    uint32_t ring = s->cfg.max_message, at = s->in_head;
    for (uint32_t i = 0; i < s->in_used;) {
        if (!(at & 7) && at + 8 <= ring && !e[at >> 3]) {
            i += 8;   /* a whole byte of marks without one */
            at += 8;
        } else if (e[at >> 3] & (1u << (at & 7))) {
            return i + 1;
        } else {
            i++;
            at++;
        }
        if (at == ring) {
            at = 0;
        }
    }
    return 0;
}

/*
 * Where a whole message from the client is opened before the session code
 * sees it. Sealed (mgcommands.h, Sealed data): its counter was taken at its
 * first fragment and its tag checked at its last (on_data()); here it's
 * decrypted in place in the caller's buffer, on the session task (a 16 KB
 * message is about 500 HMAC-SHA256 keystream blocks, off the NimBLE host's
 * small stack), with no buffer of its own: the ring holds the sealed bytes.
 * Plain: as it came.
 */
static void give_early(mg_session_t *s);

/* One varint of at most 10 bytes from p[*i..n): false if it runs past n or is longer. */
static bool env_varint(const uint8_t *p, size_t n, size_t *i, uint64_t *v)
{
    *v = 0;
    for (int k = 0; k < 10; k++) {
        if (*i >= n || (k == 9 && (p[*i] & 0xFE))) {
            return false;
        }
        uint8_t b = p[(*i)++];
        *v |= (uint64_t)(b & 0x7F) << (7 * k);
        if (!(b & 0x80)) {
            return true;
        }
    }
    return false;
}

bool mg_session_is_envelope(const uint8_t *p, size_t n)
{
    /* noise_core ServiceCodec's rules (mg_session_ref.envelope_payload): each key a field
     * number 1..2^29-1 outside 19000-19999 and wire type 0, 1, 2 or 5 (field 1 delimited), varints of at most 10
     * bytes, every value inside the message, unknown fields skipped. */
    size_t i = 0;
    while (i < n) {
        uint64_t key, len;
        if (!env_varint(p, n, &i, &key)) {
            return false;
        }
        uint64_t num = key >> 3;
        unsigned wire = (unsigned)(key & 7);
        if (!num || num >= (1u << 29) || (num >= 19000 && num <= 19999) || (wire != 0 && wire != 1 && wire != 2
            && wire != 5) || (num == 1 && wire != 2)) {
            return false;
        }
        if (wire == 0) {
            if (!env_varint(p, n, &i, &len)) {
                return false;
            }
            continue;
        }
        if (wire == 2) {
            if (!env_varint(p, n, &i, &len)) {
                return false;
            }
        } else {
            len = wire == 1 ? 8 : 4;
        }
        if (len > n - i) {
            return false;
        }
        i += (size_t)len;
    }
    return true;
}

/* A client data message that isn't a ServiceResponseEnvelope (a clear's or a credentials_update's plaintext
 * relabeled as data, say): invalid_value, its credit back (owed already), its counter
 * used. Never handed on. */
static void not_an_envelope(mg_session_t *s, uint8_t *out, size_t len)
{
    mgseal_wipe(out, len);
    send_error(s, mg_error_code_invalid_value);
    data_refused(s, "not a service envelope", (uint32_t)len);
    mg_session_pump(s);
}

size_t mg_session_take(mg_session_t *s, uint8_t *out, size_t cap)
{
    for (;;) {
        uint32_t len = s->in_count && s->ring ? oldest_len(s) : 0;
        if (!len || len > cap) {
            return 0;
        }
        uint32_t ring = s->cfg.max_message, at = s->in_head;
        uint32_t a = len < ring - at ? len : ring - at;
        memcpy(out, s->ring + at, a);
        memcpy(out + a, s->ring, len - a);
        uint32_t last = (at + len - 1) % ring;
        ends(s)[last >> 3] &= (uint8_t)~(1u << (last & 7));
        s->in_head = (at + len) % ring;
        s->in_used -= len;
        s->in_count--;
        s->owed_credit += len - s->head_early;   /* handled from here on: the client may send as much again */
        s->head_early = 0;
        give_early(s);   /* a message still arriving may have filled the window meanwhile */
        if (!s->sealed) {
            if (!mg_session_is_envelope(out, len)) {
                not_an_envelope(s, out, len);
                continue;
            }
            mg_session_pump(s);
            return len;
        }
        size_t plain = len - MGSEAL_OVERHEAD;
        if (mgseal_xor_at(s->keys, MGSEAL_DIR_CLIENT, rd_be32(out), 0, out + 4, plain) == MGSEAL_OK) {
            memmove(out, out + 4, plain);
            if (!mg_session_is_envelope(out, plain)) {
                not_an_envelope(s, out, plain);   /* its counter stays used */
                continue;
            }
            mg_session_pump(s);
            return plain;
        }
        mgseal_wipe(out, len);
        send_error(s, mg_error_code_busy);
        data_refused(s, "the seal failed", len);
        mg_session_pump(s);
    }
}

#if MG_SESSION_TEST_HOOKS
void mg_session_test_set_counters(mg_session_t *s, int64_t rx, int64_t tx)
{
    if (rx >= 0) {
        s->ctr_in = (uint32_t)rx;
        s->ctr_hole = 0;
    }
    if (tx >= 0) {
        s->ctr_out = (uint32_t)tx;
    }
}
#endif

/* The client's counters, layer 1's rule (mgseal_rx_*,
 * the one portable layer 2 runs): a first fragment carries the next counter
 * or the one hole, and takes it; from then on it's used (an abort, a refused
 * fragment, a state change) unless its tag is refused. Data and a
 * credentials_update reassemble separately, so two can be in flight. */
static bool counter_ok(const mg_session_t *s, uint32_t ctr)
{
    /* Never wraps or repeats once 0xffffffff is taken, no other counter is the next one. */
    return !s->in_spent && mgseal_rx_ok(s->ctr_in, s->ctr_hole, ctr);
}

static void take_counter(mg_session_t *s, uint32_t ctr)
{
    if (ctr == UINT32_MAX) {
        s->in_spent = true;   /* the last one: this direction takes no other */
    }
    mgseal_rx_take(&s->ctr_in, &s->ctr_hole, ctr);
}

/* A message whose tag was refused doesn't use its counter:
 * given back if nothing later took a counter, else left open once (the
 * hole: its real message still goes through, a replay doesn't). */
static void unuse_counter(mg_session_t *s, uint32_t ctr)
{
    if (ctr == UINT32_MAX) {
        s->in_spent = false;   /* a refused tag gives the last one back */
    }
    mgseal_rx_unuse(&s->ctr_in, &s->ctr_hole, ctr);
}

/* A window smaller than max_message (mgcommands.h, Device session credit): once a message still arriving has
 * filled the window, its bytes so far are given back (they're in the ring), so a message larger than the
 * window completes. Only while nothing whole is held, and at most max_message - window in all: with the
 * client's window more to come, the ring then holds whatever it may send. */
static void give_early(mg_session_t *s)
{
    uint32_t window = s->cfg.window, max = s->cfg.max_message;
    if (window >= max || s->in_count || s->in_part - s->in_early < window) {
        return;
    }
    uint32_t give = s->in_part - s->in_early;
    if (give > max - window - s->in_early) {
        give = max - window - s->in_early;
    }
    s->in_early += give;
    s->owed_credit += give;
}

/* Drops the message being received, giving its bytes back and extra more. A
 * sealed one keeps its counter used (an abort, a refused fragment). */
static void drop_in(mg_session_t *s, uint32_t extra)
{
    s->owed_credit += s->in_part - s->in_early + extra;
    s->in_used -= s->in_part;
    s->in_part = s->in_total = s->in_early = 0;
    drop_dtag(s);
}

/* Sealed bytes [at, at + n) of the data message arriving: the counter (0..3)
 * was read at its first fragment, the ciphertext goes into the tag, the last
 * 16 bytes are the tag (portable layer 2's data_feed). */
static void dtag_feed(mg_session_t *s, uint32_t at, const uint8_t *b, size_t n)
{
    uint32_t ct_end = s->in_total - MGSEAL_TAG_LEN;
    for (size_t i = 0; i < n; i++, at++) {
        if (at >= ct_end) {
            s->dtag_rx[at - ct_end] = b[i];
        } else if (at >= 4u) {
            size_t run = n - i;
            if (run > ct_end - at) {
                run = ct_end - at;
            }
            mgseal_tag_update(s->dtag, &b[i], run);
            i += run - 1;
            at += (uint32_t)run - 1u;
        }
    }
}


static void on_data(mg_session_t *s, const uint8_t *p, size_t n)
{
    if (n < 1) {
        send_error(s, mg_error_code_invalid_length);
        return;
    }
    if (!s->hello || !s->open) {
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    uint8_t flags = p[0];
    const uint8_t *rest = p + 1;
    size_t rn = n - 1;
    if (flags & mg_session_flag_first) {
        if (rn < 4) {
            send_error(s, mg_error_code_invalid_length);
            return;
        }
        uint32_t total = rd32(rest);
        rest += 4;
        rn -= 4;
        if (s->sealed) {
            /* Sealed data (mgcommands.h, Sealed data): a sealed session takes
             * only sealed data (insecure, changing nothing); its first fragment
             * carries the whole counter; the counter is the next one,
             * or the hole a refused tag left (counter_ok). From here it's used
             * (an abort, a refused fragment, a state change), unless the tag is
             * refused at the last fragment. */
            if (!(flags & mg_session_flag_sealed)) {
                send_error(s, mg_error_code_insecure);
                drop_in(s, (uint32_t)rn);
                data_refused(s, "plain data on a sealed session (insecure)", total);
                return;
            }
            if (rn < 4) {
                send_error(s, mg_error_code_invalid_length);
                drop_in(s, (uint32_t)rn);
                data_refused(s, "a sealed first fragment without its whole counter", total);
                return;
            }
        }
        if (s->in_total || total == 0 || total > s->cfg.max_message || (s->sealed && total <= MGSEAL_OVERHEAD)) {
            send_error(s, mg_error_code_invalid_value);
            drop_in(s, (uint32_t)rn);
            if (s->sealed && total && total <= MGSEAL_OVERHEAD) {
                data_refused(s, "a sealed message too short", total);
            }
            return;
        }
        if (s->sealed) {
            if (!counter_ok(s, rd_be32(rest))) {
                send_error(s, mg_error_code_invalid_value);
                drop_in(s, (uint32_t)rn);
                data_refused(s, "a wrong counter", total);
                return;
            }
            s->dtag = m_alloc(s, sizeof(*s->dtag));
            uint32_t ctr = rd_be32(rest);
            if (!s->dtag || mgseal_tag_begin(s->dtag, s->keys, MGSEAL_DIR_CLIENT, ctr,
                                             total - MGSEAL_OVERHEAD) != MGSEAL_OK) {
                drop_dtag(s);
                send_error(s, mg_error_code_busy);   /* the counter not used: the client sends it again */
                drop_in(s, (uint32_t)rn);
                return;
            }
            s->data_ctr = ctr;
            take_counter(s, ctr);
        }
        s->in_total = total;
        s->in_part = 0;
    } else if (!s->in_total) {
        send_error(s, mg_error_code_invalid_value);
        drop_in(s, (uint32_t)rn);
        return;
    }
    if (rn > s->rx_credit) {
        /* mgcommands.h (every build): a fragment past the window less what the device hasn't
         * given back: dropped and counted with its message ([session, invalid_value], its bytes given back). */
        s->credit_overruns++;
        send_error(s, mg_error_code_invalid_value);
        drop_in(s, (uint32_t)rn);
        return;
    }
    s->rx_credit -= (uint32_t)rn;
    bool fills = s->in_part + rn == s->in_total;
    if (!rn || s->in_part + rn > s->in_total || !(flags & mg_session_flag_last) != !fills
        || rn > s->cfg.max_message - s->in_used || (s->sealed && !(flags & mg_session_flag_sealed))) {
        /* The last check: bytes past the client's credit (the window is at
         * most the ring), so there's nowhere to put them. */
        send_error(s, mg_error_code_invalid_value);
        drop_in(s, (uint32_t)rn);
        return;
    }
    if (s->sealed) {
        dtag_feed(s, s->in_part, rest, rn);
    }
    uint32_t ring = s->cfg.max_message, at = (s->in_head + s->in_used) % ring;
    size_t a = rn < ring - at ? rn : ring - at;
    memcpy(s->ring + at, rest, a);
    memcpy(s->ring, rest + a, rn - a);
    s->in_used += (uint32_t)rn;
    s->in_part += (uint32_t)rn;
    if (!fills) {
        give_early(s);
        return;
    }
    if (s->sealed) {
        /* The tag at the last fragment, before the session code sees any of
         * it (SHA-256 only on this task; the decryption's HMACs come in
         * mg_session_take(), on the session task). A refused tag:
         * invalid_value, the message dropped and its credit given back, its
         * counter not used. */
        int r = mgseal_tag_check(s->dtag, s->keys, s->dtag_rx);
        if (r != MGSEAL_OK) {
            uint32_t len = s->in_total;
            unuse_counter(s, s->data_ctr);
            send_error(s, r == MGSEAL_E_CRYPTO ? mg_error_code_busy : mg_error_code_invalid_value);
            drop_in(s, 0);
            data_refused(s, r == MGSEAL_E_CRYPTO ? "the seal failed" : "a wrong tag", len);
            return;
        }
        drop_dtag(s);
    }
    if (clear_taken(s) || s->out_spent) {
        /* A sealed clear is taken, or no device counter is left: from here on data isn't run (the session keys
         * stay only to seal the clear's answer). invalid_value, its credit back, its counter used. */
        uint32_t len = s->in_total;
        send_error(s, mg_error_code_invalid_value);
        drop_in(s, 0);
        data_refused(s, s->out_spent ? "data with no device counter left" : "data after a sealed clear", len);
        return;
    }
    uint32_t last = (s->in_head + s->in_used - 1) % ring;
    ends(s)[last >> 3] |= (uint8_t)(1u << (last & 7));
    if (!s->in_count) {
        s->head_early = s->in_early;   /* the oldest now: its read gives back only the rest */
    }
    s->in_count++;
    s->in_part = s->in_total = s->in_early = 0;
    if (s->ops.message) {
        s->ops.message(s->ops.ctx);
    }
}

/*
 * The seal keys for a sealed hello, from K (the credentials' own, read with
 * them), the matching proof's nonces and the node id. The nonces are taken
 * here, once: a proof keys one sealed session (two under one proof would
 * share a keystream). 0, or the refusal.
 */
static uint16_t seal_keys(mg_session_t *s, const mg_session_creds_t *c, mgseal_keys_t *keys)
{
    uint8_t k[32], cn[MG_TOKEN_PROOF_NONCE_LEN], dn[MG_TOKEN_PROOF_NONCE_LEN];
    _Static_assert(MG_TOKEN_PROOF_NONCE_LEN == MGSEAL_NONCE_LEN && MG_TOKEN_PROOF_KEY_LEN == MGSEAL_KEY_LEN
                       && sizeof(c->proof_k) == MGSEAL_KEY_LEN,
                   "the seal takes the proof's K and nonces");
    bool have_k = c->proof_k_set;
    if (have_k) {
        memcpy(k, c->proof_k, sizeof(k));
    }
    uint16_t refused = 0;
    if (!s->ops.take_nonces(s->ops.ctx, cn, dn)) {
        refused = mg_error_code_proof_required;   /* this proof sealed a session already */
    } else if (!have_k && !s->ops.proof_key(s->ops.ctx, k)) {
        refused = mg_error_code_not_found;
    } else if (mgseal_keys(keys, k, cn, dn, c->node_id, strlen(c->node_id)) != MGSEAL_OK) {
        refused = mg_error_code_busy;
    }
    mg_token_proof_wipe(k, sizeof(k));
    mg_token_proof_wipe(cn, sizeof(cn));
    mg_token_proof_wipe(dn, sizeof(dn));
    return refused;
}

static void on_hello(mg_session_t *s, const uint8_t *p, size_t n, bool proof)
{
    if (n < 9) {
        send_error(s, mg_error_code_invalid_length);
        return;
    }
    uint8_t version = p[0];
    uint32_t max_message = rd32(p + 1), window = rd32(p + 5);
    /* hello_flags, the optional last byte: sealed credentials need no
     * encrypted link (a device that can't seal answers as before). */
    bool sealed = n >= 10 && (p[9] & mg_session_hello_flag_sealed) && can_seal(s);
    /* sealed_saved counts only beside a sealed session (an unsealed hello's,
     * or one this device can't seal, is ignored and never echoed). */
    bool saved_sealed = sealed && (p[9] & mg_session_hello_flag_sealed_saved);
    if (s->ops.busy && s->ops.busy(s->ops.ctx)) {
        /* A re-setup is committing: nothing is read, the proof is kept, and
         * the client says hello again. */
        send_error(s, mg_error_code_busy);
        return;
    }
    mg_session_commit(s);   /* a refreshed pair still waiting goes first: the credentials carry it */
    mg_session_creds_t *c = m_calloc(s, sizeof(*c));
    if (!c) {
        send_error(s, mg_error_code_busy);
        return;
    }
    bool have = s->ops.credentials && s->ops.credentials(s->ops.ctx, c) && c->access_token[0];
    /* A re-setup that began since the check above: busy, as there. */
    uint16_t refused = !have && s->ops.busy && s->ops.busy(s->ops.ctx) ? mg_error_code_busy
                     : !have ? mg_error_code_not_found
                     : !proof ? mg_error_code_proof_required
                     /* Only sealed, whatever the link: a relayed proof on the relay's own
                      * encrypted bond gets nothing in the clear. */
                     : !sealed ? mg_error_code_insecure
                     : version != MG_SESSION_VERSION || max_message < MG_SESSION_MIN_MESSAGE
                           || max_message > MG_SESSION_MAX_MESSAGE || window < 4 || window > max_message
                           /* the sealed session's one-write messages (credentials_saved) need
                            * ATT_MTU >= MG_MIN_ATT_MTU */
                           || s->mtu < MG_MIN_ATT_MTU
                         ? mg_error_code_invalid_value
                     /* No room to hold the hello's answer (a full stack): busy,
                      * before a sealed hello takes the proof's nonces, so the
                      * client can say hello again under the same proof. */
                     : s->ctl_count >= MG_SESSION_CTL_HELD ? mg_error_code_busy
                         : 0;
    /* The JSON at buf + 4: sealing puts the counter in front and the tag
     * after, in place. */
    uint8_t *buf = refused ? NULL : m_alloc(s, MGSEAL_OVERHEAD + MG_SESSION_MAX_CREDENTIALS);
    size_t len = buf ? mg_session_creds_json(c, (char *)buf + 4, MG_SESSION_MAX_CREDENTIALS) : 0;
    if (!refused && !len) {
        refused = buf ? mg_error_code_invalid_value : mg_error_code_busy;   /* a credential too large to send */
    }
    /* Sealed: the keys last, after every other check, so a refused hello
     * never spends the proof. */
    mgseal_keys_t *keys = !refused && sealed ? m_alloc(s, sizeof(*keys)) : NULL;
    if (!refused && sealed) {
        refused = !keys ? mg_error_code_busy : seal_keys(s, c, keys);
        if (!refused && mgseal_seal(keys, MGSEAL_DIR_DEVICE, 0, buf, len) != MGSEAL_OK) {
            refused = mg_error_code_busy;
        }
    }
    mg_token_proof_wipe(c, sizeof(*c));
    m_free(s, c);
    if (refused) {
        if (buf) {
            mg_token_proof_wipe(buf, MGSEAL_OVERHEAD + MG_SESSION_MAX_CREDENTIALS);
            m_free(s, buf);
        }
        if (keys) {
            mgseal_wipe(keys, sizeof(*keys));
            m_free(s, keys);
        }
        /* A refused hello leaves a running session as it was. */
        send_error(s, refused);
        return;
    }
    /* A new hello resets the session as state closed does, its keys too,
     * and restarts the re-ask series (counted after that end is reported:
     * the hello comes after it, and drops the ask it may have made). */
    end_session(s, true);
    s->reask_hellos++;
    drop_cred_in(s);
    drop_cred_out(s);
    drop_clear(s);   /* every partial sealed message goes with the old keys and counters */
    drop_keys(s);
    s->hello_gen++;
    s->hello = true;
    s->client_max = max_message;
    s->client_window = window;
    s->send_credit = window;
    uint8_t m[MG_SESSION_CTL_MAX] = { mg_command_session, mg_session_hello, MG_SESSION_VERSION };
    _Static_assert(MG_SESSION_CTL_MAX >= 12, "hello's answer: 3 + max_message 4 + window 4 + the sealed flag");
    put32(m + 3, s->cfg.max_message);
    put32(m + 7, s->cfg.window);
    /* A sealed session's hello echoes the flags it accepted. */
    m[11] = (uint8_t)(mg_session_hello_flag_sealed | (saved_sealed ? mg_session_hello_flag_sealed_saved : 0));
    queue_ctl(s, m, 12);
    /* Only a sealed hello gets this far (an unsealed one is refused insecure above). */
    s->sealed = true;
    s->saved_sealed = saved_sealed;
    s->keys = keys;
    s->ctr_out = 1;   /* the credentials took counter 0 */
    s->ctr_in = 0;
    s->ctr_hole = 0;
    s->in_spent = s->out_spent = false;
    s->cred_out_len = (uint32_t)(len + MGSEAL_OVERHEAD);
    s->cred_out_flags = mg_session_flag_sealed;
    s->cred_out = (char *)buf;
    s->cred_out_cap = MGSEAL_OVERHEAD + MG_SESSION_MAX_CREDENTIALS;
    s->cred_out_off = 0;
}

static void on_state(mg_session_t *s, const uint8_t *p, size_t n)
{
    if (n < 3) {
        send_error(s, mg_error_code_invalid_length);
        return;
    }
    if (!s->hello) {
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    uint8_t st = p[0];
    if (st > mg_session_state_too_large) {
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    bool was = s->open;
    end_session(s, false);
    s->epoch = (uint16_t)(p[1] | p[2] << 8);
    s->open = st == mg_session_state_open;
    if (s->open && !s->ring) {
        /* The ring for this session only: PSRAM where the board
         * has some. None to be had: not open, and the client is asked to
         * close (it may try again later). */
        s->ring = s->cfg.alloc ? s->cfg.alloc(MG_SESSION_BUF_SIZE(s->cfg.max_message)) : NULL;
        if (s->ring) {
            memset(s->ring + s->cfg.max_message, 0, (s->cfg.max_message + 7) / 8);
        } else {
            s->open = false;
            s->open_request = 0;
            if (was && s->ops.state) {
                s->ops.state(s->ops.ctx, mg_session_state_closed, s->epoch);
            }
            return;
        }
    }
    if (s->open && !++s->opened) {
        s->opened = 1;
    }
    /* Each state the client reports goes to the session code once: open
     * (again), or why it isn't, a closed with no session open included. A
     * closed or no_network after connecting with none ever open (the open the
     * device asked for failed), or a closed answering an open_request at once
     * (refused), is an end the device didn't ask for like any other: the
     * re-ask series must hear it, or a gadget whose open failed goes silent
     * until a hello, a saved update or the link's end (mg-session-v1
     * exchange 14; mg_session_ref.py on_state takes every state, as the
     * Zephyr side does). Only the session's own ends (end_session(), tell)
     * report a closed the client didn't send. */
    if (s->ops.state) {
        s->ops.state(s->ops.ctx, st, s->epoch);
    }
}

/*
 * Whether the next hello's credentials, with this pair in place of the stored
 * one, fit MG_SESSION_MAX_CREDENTIALS: a pair that fits alone but not beside
 * the node id, device key and VM binding is refused, never stored (every
 * later hello would fail). 1 fits (or there's no stored credential to check
 * it against: the store decides), 0 too large, -1 out of memory.
 */
static int update_fits(mg_session_t *s, const char *access, const char *refresh, uint64_t expires)
{
    mg_session_creds_t *c = m_calloc(s, sizeof(*c));
    char *out = c ? m_alloc(s, MG_SESSION_MAX_CREDENTIALS) : NULL;
    int fits = out ? 1 : -1;
    if (out && s->ops.credentials && s->ops.credentials(s->ops.ctx, c)) {
        /* Both parsed into MG_SESSION_TOKEN_MAX buffers, NUL-terminated: they fit these. */
        memcpy(c->access_token, access, strlen(access) + 1);
        memcpy(c->refresh_token, refresh, strlen(refresh) + 1);
        c->access_expires_at = expires;
        fits = mg_session_creds_json(c, out, MG_SESSION_MAX_CREDENTIALS) ? 1 : 0;
    }
    if (out) {
        mg_token_proof_wipe(out, MG_SESSION_MAX_CREDENTIALS);
        m_free(s, out);
    }
    if (c) {
        mg_token_proof_wipe(c, sizeof(*c));
        m_free(s, c);
    }
    return fits;
}

/*
 * A whole credentials_update (msg, len bytes; opened in place when sealed):
 * true with its pair in *access and *refresh (allocated), or false with its
 * refusal queued. Sealed: the counter and the tag are checked before
 * anything is decrypted.
 */
static bool accept_update(mg_session_t *s, uint8_t *msg, size_t len, uint32_t ctr, char **access_out,
                          char **refresh_out, uint64_t *expires_out)
{
    const char *json = (const char *)msg;
    size_t json_len = len;
    if (s->sealed) {
        /* ctr: the counter its first fragment took (on_credentials_update()). */
        int r = mgseal_open(s->keys, MGSEAL_DIR_CLIENT, ctr, msg, len, &json_len);
        if (r != MGSEAL_OK) {
            if (r == MGSEAL_E_TAG) {
                unuse_counter(s, ctr);   /* a refused tag doesn't use its counter */
            }
            send_error(s, r == MGSEAL_E_CRYPTO ? mg_error_code_busy : mg_error_code_invalid_value);
            return false;
        }
        json = (const char *)msg + 4;
    }
    char *access = m_alloc(s, MG_SESSION_TOKEN_MAX), *refresh = m_alloc(s, MG_SESSION_TOKEN_MAX);
    bool parsed = access && refresh
                  && mg_session_parse_update(json, json_len, access, refresh, MG_SESSION_TOKEN_MAX, expires_out);
    int fits = parsed ? update_fits(s, access, refresh, *expires_out) : 0;
    if (fits > 0) {
        *access_out = access;
        *refresh_out = refresh;
        return true;
    }
    send_error(s, access && refresh && fits == 0 ? mg_error_code_invalid_value : mg_error_code_busy);
    if (access) {
        mg_token_proof_wipe(access, MG_SESSION_TOKEN_MAX);
    }
    if (refresh) {
        mg_token_proof_wipe(refresh, MG_SESSION_TOKEN_MAX);
    }
    m_free(s, access);
    m_free(s, refresh);
    return false;
}

/* A pair accepted: it waits to be stored, superseding one not stored yet. */
static void set_update(mg_session_t *s, char *access, char *refresh, uint64_t expires)
{
    if (s->upd_access || s->upd_sealed) {
        /* An earlier pair not stored yet: superseded, and said so ([0]). */
        send_saved(s, false, NULL);
        drop_update(s);
    }
    s->upd_access = access;
    s->upd_refresh = refresh;
    s->upd_expires = expires;
}

static void on_credentials_update(mg_session_t *s, const uint8_t *p, size_t n)
{
    if (n < 1) {
        send_error(s, mg_error_code_invalid_length);
        return;
    }
    if (!s->hello) {
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    if (clear_taken(s) || s->out_spent) {
        /* No device counter left: its answer couldn't be sealed; refused, as after a clear.
         * A sealed clear is taken (erasing, or its answer waiting for a device
         * message part way out): nothing is stored from here on, so its [1] still means
         * erased (mgcommands.h, mg_session_clear). The
         * session ends once the answer is out; the next one stores as ever. */
        drop_cred_in(s);
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    uint8_t flags = p[0];
    const uint8_t *rest = p + 1;
    size_t rn = n - 1;
    /* A sealed session takes only sealed updates (an unsealed one gets
     * insecure and changes nothing: mgcommands.h, Sealed data), a plain one
     * only plain ones (Sealed credentials). */
    if (s->sealed && !(flags & mg_session_flag_sealed)) {
        if (s->cred_in && !(flags & mg_session_flag_first)) {
            /* A later fragment of a sealed update without the flag drops it: nothing stored, its counter used
             * (mgcommands.h, Sealed credentials). */
            drop_cred_in(s);
            send_error(s, mg_error_code_invalid_value);
            return;
        }
        send_error(s, mg_error_code_insecure);
        return;
    }
    if (!s->sealed && (flags & mg_session_flag_sealed)) {
        drop_cred_in(s);
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    if (flags & mg_session_flag_first) {
        if (rn < 4) {
            send_error(s, mg_error_code_invalid_length);
            return;
        }
        uint32_t total = rd32(rest);
        rest += 4;
        rn -= 4;
        if (s->sealed && rn < 4) {
            drop_cred_in(s);
            send_error(s, mg_error_code_invalid_length);   /* the first fragment carries the whole counter */
            return;
        }
        uint32_t max = s->sealed ? MGSEAL_OVERHEAD + MG_SESSION_MAX_CREDENTIALS : MG_SESSION_MAX_CREDENTIALS;
        if (s->cred_in || total == 0 || total > max || (s->sealed && total <= MGSEAL_OVERHEAD)) {
            drop_cred_in(s);
            send_error(s, mg_error_code_invalid_value);
            return;
        }
        if (s->sealed) {
            /* The counter from the first fragment, the next one or the hole,
             * shared with data (mgcommands.h, Sealed data): used from here
             * on, unless its tag is refused. */
            if (!counter_ok(s, rd_be32(rest))) {
                send_error(s, mg_error_code_invalid_value);
                return;
            }
            s->cred_in_ctr = rd_be32(rest);
            take_counter(s, s->cred_in_ctr);
        }
        s->cred_in = m_alloc(s, total);
        if (!s->cred_in) {
            send_error(s, mg_error_code_busy);
            return;
        }
        s->cred_in_total = total;
        s->cred_in_len = 0;
    } else if (!s->cred_in) {
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    bool fills = s->cred_in_len + rn == s->cred_in_total;
    if (!rn || s->cred_in_len + rn > s->cred_in_total || !(flags & mg_session_flag_last) != !fills) {
        drop_cred_in(s);
        send_error(s, mg_error_code_invalid_value);
        return;
    }
    memcpy(s->cred_in + s->cred_in_len, rest, rn);
    s->cred_in_len += (uint32_t)rn;
    if (!fills) {
        return;
    }
    if (s->sealed && s->cfg.store_later) {
        /* Opened where it's stored (mg_session_commit()). One not stored
         * yet is superseded, and said so, once this one's tag checks there
         * (mg_session_take_update()): until then it's kept aside, so a
         * replacement that doesn't open discards nothing. */
        if (s->upd_access) {
            send_saved(s, false, NULL);
            drop_update(s);
        }
        if (s->upd_sealed && s->upd_prev) {
            send_saved(s, false, NULL);   /* the newest unchecked one is superseded; the oldest stays aside */
            drop_sealed(s, &s->upd_sealed, &s->upd_sealed_len);
        } else if (s->upd_sealed) {
            s->upd_prev = s->upd_sealed;
            s->upd_prev_len = s->upd_sealed_len;
            s->upd_prev_ctr = s->upd_sealed_ctr;
            s->upd_sealed = NULL;
            s->upd_sealed_len = 0;
        }
        s->upd_sealed = (uint8_t *)s->cred_in;
        s->upd_sealed_len = s->cred_in_len;
        s->upd_sealed_ctr = s->cred_in_ctr;
        s->cred_in = NULL;
        s->cred_in_len = s->cred_in_total = 0;
        return;
    }
    char *access, *refresh;
    uint64_t expires = 0;
    bool accepted = accept_update(s, (uint8_t *)s->cred_in, s->cred_in_len, s->cred_in_ctr, &access, &refresh,
                                  &expires);
    drop_cred_in(s);
    if (accepted) {
        set_update(s, access, refresh, expires);
        if (!s->cfg.store_later) {
            mg_session_commit(s);
        }
    }
}

bool mg_session_take_update(mg_session_t *s, char **access, char **refresh, uint64_t *expires, uint32_t *gen)
{
    while (s->upd_sealed) {
        uint8_t *msg = s->upd_sealed;
        uint32_t len = s->upd_sealed_len;
        s->upd_sealed = NULL;
        s->upd_sealed_len = 0;
        char *a, *r;
        uint64_t at = 0;
        bool accepted = accept_update(s, msg, len, s->upd_sealed_ctr, &a, &r, &at);
        mg_token_proof_wipe(msg, len);
        m_free(s, msg);
        if (accepted) {
            if (s->upd_prev) {
                send_saved(s, false, NULL);   /* the one it replaces: superseded, and said so */
                drop_sealed(s, &s->upd_prev, &s->upd_prev_len);
            }
            set_update(s, a, r, at);
            break;
        }
        if (!s->upd_prev) {
            mg_session_pump(s);   /* its refusal */
            return false;
        }
        /* Refused (answered): the one it would have superseded stands. */
        s->upd_sealed = s->upd_prev;
        s->upd_sealed_len = s->upd_prev_len;
        s->upd_sealed_ctr = s->upd_prev_ctr;
        s->upd_prev = NULL;
        s->upd_prev_len = 0;
    }
    if (!s->upd_access) {
        return false;
    }
    *access = s->upd_access;
    *refresh = s->upd_refresh;
    *expires = s->upd_expires;
    *gen = s->hello_gen;
    if (s->sealed && s->saved_sealed && mgseal_saved_plaintext(s->taken_pt, true, (const uint8_t *)s->upd_refresh,
                                            strlen(s->upd_refresh)) != MGSEAL_OK) {
        memset(s->taken_pt, 0, sizeof(s->taken_pt));   /* no hash: answered as not saved */
    }
    s->upd_access = s->upd_refresh = NULL;
    s->upd_expires = 0;
    s->upd_taken = true;
    return true;
}

void mg_session_update_saved(mg_session_t *s, uint32_t gen, bool saved)
{
    s->upd_taken = false;
    if (gen != s->hello_gen || !s->hello) {
        return;   /* the session it answered ended: the client sends the pair again */
    }
    saved = s->sealed && s->saved_sealed ? saved && s->taken_pt[0] == 1 : saved;
    if (saved) {
        s->reask_saved++;   /* new credentials: the re-ask series starts over */
    }
    send_saved(s, saved, s->taken_pt);
    mgseal_wipe(s->taken_pt, sizeof(s->taken_pt));
    mg_session_pump(s);
}

static void free_pair(const mg_session_t *s, char *access, char *refresh)
{
    mg_token_proof_wipe(access, MG_SESSION_TOKEN_MAX);
    mg_token_proof_wipe(refresh, MG_SESSION_TOKEN_MAX);
    m_free(s, access);
    m_free(s, refresh);
}

bool mg_session_commit(mg_session_t *s)
{
    bool waiting = s->upd_access || s->upd_sealed;
    char *access, *refresh;
    uint64_t expires = 0;
    uint32_t gen;
    if (!mg_session_take_update(s, &access, &refresh, &expires, &gen)) {
        return waiting;   /* none, or refused (answered) */
    }
    bool saved = s->ops.store_tokens && s->ops.store_tokens(s->ops.ctx, access, refresh, expires);
    free_pair(s, access, refresh);
    mg_session_update_saved(s, gen, saved);
    return true;
}

void mg_session_write(mg_session_t *s, const uint8_t *a, size_t n, bool proof)
{
    if (n < 1) {
        send_error(s, mg_error_code_invalid_length);
        mg_session_pump(s);
        return;
    }
    const uint8_t *p = a + 1;
    size_t pn = n - 1;
    if ((s->clearing || s->clr_erasing) && a[0] != mg_session_hello && a[0] != mg_session_credit) {
        /* The session ends once the clear's answer is out. Credit still counts: the answer, or the data message
         * part way out before it, may be waiting for it. */
        send_error(s, mg_error_code_invalid_value);
        mg_session_pump(s);
        return;
    }
    switch (a[0]) {
    case mg_session_hello:
        on_hello(s, p, pn, proof);
        break;
    case mg_session_state:
        on_state(s, p, pn);
        break;
    case mg_session_data:
        on_data(s, p, pn);
        break;
    case mg_session_credit:
        if (pn < 4) {
            send_error(s, mg_error_code_invalid_length);
        } else if (!s->hello) {
            send_error(s, mg_error_code_invalid_value);
        } else {
            uint32_t more = rd32(p);
            s->send_credit = more > UINT32_MAX - s->send_credit ? UINT32_MAX : s->send_credit + more;
        }
        break;
    case mg_session_abort:
        if (!s->hello) {
            send_error(s, mg_error_code_invalid_value);
        } else {
            drop_in(s, 0);
        }
        break;
    case mg_session_credentials_update:
        on_credentials_update(s, p, pn);
        break;
    case mg_session_clear:
        on_clear(s, p, pn);
        break;
    default:
        /* Device -> client sub-commands (open_request, credentials,
         * credentials_saved) and unknown ones. */
        send_error(s, mg_error_code_unsupported);
        break;
    }
    mg_session_pump(s);
}

/* ---- JSON ------------------------------------------------------------------ */

typedef struct {
    char *out;
    size_t cap, len;
    bool ok;
} jw_t;

static void jw_raw(jw_t *w, const char *s, size_t n)
{
    if (!w->ok || n > w->cap - w->len) {
        w->ok = false;
        return;
    }
    memcpy(w->out + w->len, s, n);
    w->len += n;
}

/* A JSON string as Python's json.dumps(ensure_ascii=False) writes it. */
static void jw_str(jw_t *w, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    jw_raw(w, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char e[6] = { '\\', 0 };
        size_t en = 2;
        switch (c) {
        case '"': e[1] = '"'; break;
        case '\\': e[1] = '\\'; break;
        case '\n': e[1] = 'n'; break;
        case '\r': e[1] = 'r'; break;
        case '\t': e[1] = 't'; break;
        case '\b': e[1] = 'b'; break;
        case '\f': e[1] = 'f'; break;
        default:
            if (c >= 0x20) {
                jw_raw(w, (const char *)&c, 1);
                continue;
            }
            e[1] = 'u';
            e[2] = '0';
            e[3] = '0';
            e[4] = hex[c >> 4];
            e[5] = hex[c & 15];
            en = 6;
            break;
        }
        jw_raw(w, e, en);
    }
    jw_raw(w, "\"", 1);
}

static void jw_field(jw_t *w, const char *key, const char *value, bool first)
{
    if (!first) {
        jw_raw(w, ",", 1);
    }
    jw_str(w, key);
    jw_raw(w, ":", 1);
    jw_str(w, value);
}

size_t mg_session_creds_json(const mg_session_creds_t *c, char *out, size_t cap)
{
    jw_t w = { out, cap, 0, true };
    jw_raw(&w, "{", 1);
    jw_field(&w, "node_id", c->node_id, true);
    jw_field(&w, "access_token", c->access_token, false);
    jw_field(&w, "refresh_token", c->refresh_token, false);
    if (c->device_key[0] && c->key_id[0]) {
        jw_field(&w, "device_key", c->device_key, false);
        jw_field(&w, "key_id", c->key_id, false);
        /* The VM the key is bound to: the client opens the session to it. */
        if (c->vm_id[0]) {
            jw_field(&w, "vm_id", c->vm_id, false);
        }
        if (c->vm_kind[0]) {
            jw_field(&w, "vm_kind", c->vm_kind, false);
        }
    }
    if (c->access_expires_at) {
        /* Last, as portable layer 2 sends it: ,"access_expires_at":N */
        char d[21];
        int n = 0;
        uint64_t at = c->access_expires_at;
        while (at) {
            d[n++] = (char)('0' + (int)(at % 10u));
            at /= 10u;
        }
        char num[21];
        for (int i = 0; i < n; i++) {
            num[i] = d[n - 1 - i];
        }
        static const char key[] = ",\"access_expires_at\":";
        jw_raw(&w, key, sizeof(key) - 1);
        jw_raw(&w, num, (size_t)n);
    }
    jw_raw(&w, "}", 1);
    return w.ok ? w.len : 0;
}

/* A strict JSON reader (RFC 8259), enough to check a credentials_update. */
typedef struct {
    const char *p, *end;
    int depth;
    uint64_t expires;   /* the update's access_expires_at (0: none) */
    bool expires_seen;
} jr_t;

static const char EXP_KEY[] = "access_expires_at";

/* An unsigned decimal integer (no sign, fraction or exponent) that fits 64
 * bits, else 0: portable layer 2's json_u64(). */
static uint64_t jr_u64(const char *w, size_t n)
{
    uint64_t v = 0;
    if (n == 0 || n > 20) {
        return 0;
    }
    for (size_t k = 0; k < n; k++) {
        if (w[k] < '0' || w[k] > '9') {
            return 0;
        }
        uint64_t d = (uint64_t)(w[k] - '0');
        if (v > (UINT64_MAX - d) / 10u) {
            return 0;
        }
        v = v * 10u + d;
    }
    return v;
}

static void jr_ws(jr_t *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
        r->p++;
    }
}

static int jr_hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            return -1;
        }
        v = v << 4 | d;
    }
    return v;
}

/* A string at r->p (the opening quote): decoded into out (cap bytes, NUL
 * terminated) when out isn't NULL. *fits: it fit, with no NUL inside. */
static bool jr_string(jr_t *r, char *out, size_t cap, bool *fits)
{
    size_t n = 0;
    bool ok = true;
    r->p++;
    for (;;) {
        if (r->p >= r->end) {
            return false;
        }
        unsigned char c = (unsigned char)*r->p++;
        uint32_t cp;
        if (c == '"') {
            break;
        }
        if (c < 0x20) {
            return false;
        }
        if (c != '\\') {
            if (out && n + 1 < cap) {
                out[n] = (char)c;
            } else {
                ok = false;
            }
            n++;
            continue;
        }
        if (r->p >= r->end) {
            return false;
        }
        char e = *r->p++;
        switch (e) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u': {
            int v = r->end - r->p >= 4 ? jr_hex4(r->p) : -1;
            if (v < 0) {
                return false;
            }
            r->p += 4;
            cp = (uint32_t)v;
            if (cp >= 0xD800 && cp < 0xDC00 && r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u') {
                int lo = jr_hex4(r->p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                    r->p += 6;
                }
            }
            break;
        }
        default:
            return false;
        }
        if (cp == 0) {
            ok = false;   /* a NUL can't be stored */
        }
        char u[4];
        size_t un = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (un == 1) {
            u[0] = (char)cp;
        } else if (un == 2) {
            u[0] = (char)(0xC0 | cp >> 6);
            u[1] = (char)(0x80 | (cp & 0x3F));
        } else if (un == 3) {
            u[0] = (char)(0xE0 | cp >> 12);
            u[1] = (char)(0x80 | (cp >> 6 & 0x3F));
            u[2] = (char)(0x80 | (cp & 0x3F));
        } else {
            u[0] = (char)(0xF0 | cp >> 18);
            u[1] = (char)(0x80 | (cp >> 12 & 0x3F));
            u[2] = (char)(0x80 | (cp >> 6 & 0x3F));
            u[3] = (char)(0x80 | (cp & 0x3F));
        }
        if (out && n + un < cap) {
            memcpy(out + n, u, un);
        } else {
            ok = false;
        }
        n += un;
    }
    if (out) {
        out[n < cap ? n : cap - 1] = '\0';
    }
    if (fits) {
        *fits = ok;
    }
    return true;
}

static bool jr_value(jr_t *r);

static bool jr_number(jr_t *r)
{
    const char *p = r->p;
    if (p < r->end && *p == '-') {
        p++;
    }
    if (p >= r->end) {
        return false;
    }
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < r->end && *p >= '0' && *p <= '9') {
            p++;
        }
    } else {
        return false;
    }
    if (p < r->end && *p == '.') {
        p++;
        if (p >= r->end || *p < '0' || *p > '9') {
            return false;
        }
        while (p < r->end && *p >= '0' && *p <= '9') {
            p++;
        }
    }
    if (p < r->end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < r->end && (*p == '+' || *p == '-')) {
            p++;
        }
        if (p >= r->end || *p < '0' || *p > '9') {
            return false;
        }
        while (p < r->end && *p >= '0' && *p <= '9') {
            p++;
        }
    }
    r->p = p;
    return true;
}

static bool jr_word(jr_t *r, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, w, n)) {
        return false;
    }
    r->p += n;
    return true;
}

/* A credentials_update is a flat object, as portable layer 2 reads it:
 * members whose values are strings, numbers, true, false or null; unknown
 * ones (access_expires_at, a number, from the apps) are ignored, and both
 * tokens are required. A nested array or object is refused, so a hostile
 * update can't make the reader recurse. */
#define MG_SESSION_UPDATE_DEPTH 1

/* An object or array's members, skipping each value (or, at the top, taking the two tokens). */
static bool jr_members(jr_t *r, bool object, char *access, char *refresh, size_t cap, bool *got_a, bool *got_r)
{
    char close = object ? '}' : ']';
    if (++r->depth > MG_SESSION_UPDATE_DEPTH) {
        return false;
    }
    r->p++;
    jr_ws(r);
    if (r->p < r->end && *r->p == close) {
        r->p++;
        r->depth--;
        return true;
    }
    for (;;) {
        char *into = NULL;
        bool *got = NULL;
        bool expiry = false;
        if (object) {
            char key[24];
            bool fits = false;
            if (r->p >= r->end || *r->p != '"' || !jr_string(r, key, sizeof(key), &fits)) {
                return false;
            }
            if (access && fits && !strcmp(key, "access_token")) {
                into = access;
                got = got_a;
            } else if (access && fits && !strcmp(key, "refresh_token")) {
                into = refresh;
                got = got_r;
            } else if (access && fits && !strcmp(key, EXP_KEY)) {
                if (r->expires_seen) {
                    return false;   /* a duplicate: invalid_value, as layer 2 */
                }
                r->expires_seen = true;
                expiry = true;
            }
            jr_ws(r);
            if (r->p >= r->end || *r->p != ':') {
                return false;
            }
            r->p++;
            jr_ws(r);
        }
        if (into) {
            /* The last of a repeated key wins, as in most readers. */
            bool fits = false;
            *got = false;
            if (r->p < r->end && *r->p == '"') {
                if (!jr_string(r, into, cap, &fits)) {
                    return false;
                }
                *got = fits && into[0];
            } else if (!jr_value(r)) {
                return false;
            }
        } else if (expiry) {
            /* Kept only as an unsigned integer; any other form: none. */
            const char *v0 = r->p;
            bool number = r->p < r->end && *r->p != '"' && *r->p != 't' && *r->p != 'f' && *r->p != 'n';
            if (!jr_value(r)) {
                return false;
            }
            r->expires = number ? jr_u64(v0, (size_t)(r->p - v0)) : 0;
        } else if (!jr_value(r)) {
            return false;
        }
        jr_ws(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            jr_ws(r);
            continue;
        }
        if (r->p < r->end && *r->p == close) {
            r->p++;
            r->depth--;
            return true;
        }
        return false;
    }
}

static bool jr_value(jr_t *r)
{
    if (r->p >= r->end) {
        return false;
    }
    switch (*r->p) {
    case '{':
        return jr_members(r, true, NULL, NULL, 0, NULL, NULL);
    case '[':
        return jr_members(r, false, NULL, NULL, 0, NULL, NULL);
    case '"':
        return jr_string(r, NULL, 0, NULL);
    case 't':
        return jr_word(r, "true");
    case 'f':
        return jr_word(r, "false");
    case 'n':
        return jr_word(r, "null");
    default:
        return jr_number(r);
    }
}

bool mg_session_parse_update(const char *json, size_t len, char *access, char *refresh, size_t cap,
                             uint64_t *expires)
{
    jr_t r = { json, json + len, 0, 0, false };
    bool got_a = false, got_r = false;
    access[0] = refresh[0] = '\0';
    if (expires) {
        *expires = 0;
    }
    /* '{' its first byte, no leading whitespace (mgcommands.h: so a short clear plaintext relabelled can't
     * become one). */
    if (r.p >= r.end || *r.p != '{' || !jr_members(&r, true, access, refresh, cap, &got_a, &got_r)) {
        return false;
    }
    jr_ws(&r);
    if (r.p != r.end || !got_a || !got_r) {
        return false;
    }
    if (expires) {
        *expires = r.expires;
    }
    return true;
}
