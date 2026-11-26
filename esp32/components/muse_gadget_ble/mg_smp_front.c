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

#include "mg_smp_front.h"

#include <string.h>

#define HDR 8
#define OP_WRITE 2
#define GRP_OS 0
#define GRP_IMG 1

/* ---- reassembly ---------------------------------------------------------------------- */

void mg_smp_front_rx_reset(mg_smp_front_rx_t *rx)
{
    rx->have = rx->want = 0;
    rx->skip = 0;
}

void mg_smp_front_rx(mg_smp_front_rx_t *rx, const uint8_t *p, size_t n, uint32_t now_ms, const mg_smp_front_out_t *out)
{
    if (rx->have && (uint32_t)(now_ms - rx->at_ms) >= MG_SMP_FRONT_GAP_MS) {
        rx->have = rx->want = 0;   /* a stale partial request: a write was lost */
    }
    rx->at_ms = now_ms;
    while (n) {
        if (rx->skip) {
            /* The rest of a request too large to hold: dropped here, never forwarded (the core answered EMSGSIZE from
             * its header; bytes passed on would start requests of their own there, past every check). */
            size_t k = n < rx->skip ? n : rx->skip;
            rx->skip -= (uint32_t)k;
            p += k;
            n -= k;
            continue;
        }
        size_t need = rx->have < HDR ? (size_t)(HDR - rx->have) : (size_t)rx->want - rx->have;
        size_t k = n < need ? n : need;
        memcpy(rx->buf + rx->have, p, k);
        rx->have = (uint16_t)(rx->have + k);
        p += k;
        n -= k;
        if (rx->have == HDR && !rx->want) {
            uint32_t total = HDR + ((uint32_t)rx->buf[2] << 8 | rx->buf[3]);
            if (total > MG_SMP_FRONT_MAX) {
                out->raw(out->ctx, rx->buf, HDR);   /* too large to hold: its header alone, the core's (EMSGSIZE) */
                rx->skip = total - HDR;
                rx->have = 0;
                continue;
            }
            rx->want = (uint16_t)total;
        }
        if (rx->want && rx->have == rx->want) {
            uint16_t whole = rx->want;
            rx->have = rx->want = 0;
            out->whole(out->ctx, rx->buf, whole);
        }
    }
}

/* ---- reading a request ----------------------------------------------------------------- */

typedef struct {
    const uint8_t *b;
    size_t n, i;
} cbor_t;

/* One item's head: major type, and its argument (31: indefinite). False past the end or on a reserved form. */
static bool head(cbor_t *c, uint8_t *major, uint64_t *arg, bool *indef)
{
    if (c->i >= c->n) {
        return false;
    }
    uint8_t ib = c->b[c->i++];
    uint8_t info = ib & 31;
    *major = ib >> 5;
    *indef = info == 31;
    if (info < 24 || info == 31) {
        *arg = info;
        return info != 31 || (*major >= 2 && *major != 6);   /* no indefinite integer or tag */
    }
    if (info > 27) {
        return false;
    }
    size_t size = (size_t)1 << (info - 24);
    if (c->n - c->i < size) {
        return false;
    }
    uint64_t v = 0;
    for (size_t k = 0; k < size; k++) {
        v = v << 8 | c->b[c->i++];
    }
    *arg = v;
    return true;
}

static bool is_break(const cbor_t *c)
{
    return c->i < c->n && c->b[c->i] == 0xff;
}

static bool skip(cbor_t *c, int depth)
{
    uint8_t major;
    uint64_t arg;
    bool indef;
    if (depth > 8 || !head(c, &major, &arg, &indef)) {
        return false;
    }
    switch (major) {
    case 0:
    case 1:
        return true;
    case 2:
    case 3:
        if (indef || arg > c->n - c->i) {
            return false;   /* no chunked strings */
        }
        c->i += (size_t)arg;
        return true;
    case 4:
    case 5: {
        uint64_t items = major == 5 ? arg * 2 : arg;
        for (uint64_t k = 0; indef || k < items; k++) {
            if (indef && is_break(c)) {
                c->i++;
                return true;
            }
            if (!skip(c, depth + 1)) {
                return false;
            }
        }
        return true;
    }
    case 6:
        return skip(c, depth + 1);
    default:
        return !indef;   /* a simple value or a float */
    }
}

static bool key_is(const uint8_t *k, size_t kn, const char *name)
{
    return kn == strlen(name) && !memcmp(k, name, kn);
}

/* An upload's body: off, len, sha and data (the rest skipped). */
static bool read_upload(const uint8_t *body, size_t n, muse_update_req_t *r, const uint8_t **data, bool *has_off,
                        bool *has_len, bool *has_data, bool *sha_bad)
{
    cbor_t c = { body, n, 0 };
    uint8_t major;
    uint64_t arg;
    bool indef;
    if (!head(&c, &major, &arg, &indef) || major != 5) {
        return false;
    }
    for (uint64_t k = 0; indef || k < arg; k++) {
        if (indef && is_break(&c)) {
            c.i++;
            break;
        }
        uint8_t km;
        uint64_t kl;
        bool ki;
        if (!head(&c, &km, &kl, &ki) || km != 3 || ki || kl > c.n - c.i) {
            return false;   /* text keys only */
        }
        const uint8_t *key = c.b + c.i;
        c.i += (size_t)kl;
        size_t at = c.i;
        uint8_t vm;
        uint64_t v;
        bool vi;
        if (!head(&c, &vm, &v, &vi)) {
            return false;
        }
        if ((key_is(key, kl, "off") || key_is(key, kl, "len")) && vm == 0) {
            if (v > UINT32_MAX) {
                return false;
            }
            if (key_is(key, kl, "off")) {
                r->off = (uint32_t)v;
                *has_off = true;
            } else {
                r->id.len = (uint32_t)v;
                *has_len = true;
            }
        } else if ((key_is(key, kl, "sha") || key_is(key, kl, "data")) && vm == 2 && !vi) {
            if (v > c.n - c.i) {
                return false;
            }
            if (key_is(key, kl, "data")) {
                *data = c.b + c.i;
                r->n = (uint32_t)v;
                *has_data = true;
            } else if (v == 32) {
                memcpy(r->id.sha, c.b + c.i, 32);
                r->id.has_sha = true;
            } else {
                *sha_bad = true;   /* the core refuses it (EINVAL) */
            }
            c.i += (size_t)v;
        } else {
            c.i = at;
            if (!skip(&c, 1)) {
                return false;
            }
        }
    }
    return true;
}

static mg_smp_front_kind_t front_read(const uint8_t *req, size_t n, muse_update_req_t *out, const uint8_t **data,
                                      bool *sha_bad)
{
    memset(out, 0, sizeof(*out));
    *data = NULL;
    if (n < HDR || (req[0] & 7) != OP_WRITE) {
        return MG_SMP_FRONT_NOT_CHANGE;
    }
    uint16_t group = (uint16_t)(req[4] << 8 | req[5]);
    uint8_t id = req[7];
    bool upload = group == GRP_IMG && id == 1, state = group == GRP_IMG && id == 0;
    bool change = (group == GRP_IMG && id == 5) || (group == GRP_OS && id == 5);
    if (!upload && !state && !change) {
        return MG_SMP_FRONT_NOT_CHANGE;
    }
    if (HDR + ((size_t)req[2] << 8 | req[3]) != n) {
        return MG_SMP_FRONT_UNREADABLE;
    }
    if (upload) {
        bool has_off = false, has_len = false, has_data = false;
        if (!read_upload(req + HDR, n - HDR, out, data, &has_off, &has_len, &has_data, sha_bad) || !has_off
            || !has_data) {
            return MG_SMP_FRONT_UNREADABLE;
        }
        out->kind = out->off == 0 && has_len ? MUSE_UPDATE_UPLOAD0 : MUSE_UPDATE_CHUNK;
        return MG_SMP_FRONT_CHANGE;
    }
    /* What a press on it covers: its group, command and body (not its sequence number). */
    uint64_t h[(MG_SMP_SHA256_CTX_SIZE + 7) / 8];
    mg_smp_sha256_init(h);
    mg_smp_sha256_update(h, req + 4, 2);
    mg_smp_sha256_update(h, req + 7, 1);
    mg_smp_sha256_update(h, req + HDR, (uint32_t)(n - HDR));
    mg_smp_sha256_final(h, out->key);
    out->kind = state ? MUSE_UPDATE_STATE : MUSE_UPDATE_CHANGE;
    return MG_SMP_FRONT_CHANGE;
}

mg_smp_front_kind_t mg_smp_front_read(const uint8_t *req, size_t n, muse_update_req_t *out, const uint8_t **data)
{
    bool sha_bad = false;
    return front_read(req, n, out, data, &sha_bad);
}

/* ---- the slot ---------------------------------------------------------------------------- */

static bool slot_is(const mg_smp_front_slot_t *s, const muse_update_ident_t *id)
{
    return s && s->on && id->has_sha && s->len == id->len && !memcmp(s->sha, id->sha, 32);
}

/* An upload's first chunk again, while the core holds that upload: it resumes there (the core answers its own
 * offset), so the slot keeps what it has. */
static bool resumes(const mg_smp_front_slot_t *s, const muse_update_req_t *r, bool core_holds)
{
    return core_holds && slot_is(s, &r->id) && s->next > 0;
}

bool mg_smp_slot_would_match(const mg_smp_front_slot_t *s, const muse_update_req_t *r, const uint8_t *data,
                             const muse_update_ident_t *bound, bool core_holds)
{
    uint8_t got[32];
    uint64_t h[(MG_SMP_SHA256_CTX_SIZE + 7) / 8];
    if (r->kind == MUSE_UPDATE_STATE || (r->kind == MUSE_UPDATE_UPLOAD0 && resumes(s, r, core_holds))) {
        return slot_is(s, bound) && s->matched;
    }
    if (r->kind == MUSE_UPDATE_UPLOAD0) {
        if (r->n != bound->len || !bound->has_sha) {
            return false;   /* a first chunk that is the whole image */
        }
        mg_smp_sha256_init(h);
        mg_smp_sha256_update(h, data, r->n);
        mg_smp_sha256_final(h, got);
        return !memcmp(got, bound->sha, 32);
    }
    if (r->kind != MUSE_UPDATE_CHUNK || !slot_is(s, bound)) {
        return false;
    }
    if (r->off < s->next) {
        return s->matched;   /* a late duplicate of the tail: the slot as it is */
    }
    if (r->off != s->next || (uint64_t)r->off + r->n != s->len) {
        return false;
    }
    memcpy(h, s->h, sizeof(h));
    mg_smp_sha256_update(h, data, r->n);
    mg_smp_sha256_final(h, got);
    return !memcmp(got, s->sha, 32);
}

void mg_smp_slot_apply(mg_smp_front_slot_t *s, const muse_update_req_t *r, const uint8_t *data, bool core_holds)
{
    if (!s) {
        return;
    }
    if (r->kind == MUSE_UPDATE_UPLOAD0 && !resumes(s, r, core_holds)) {
        memset(s, 0, sizeof(*s));   /* a new upload (or the core starts it over): the slot from offset 0 */
        s->on = r->id.has_sha;
        s->len = r->id.len;
        memcpy(s->sha, r->id.sha, 32);
        mg_smp_sha256_init(s->h);
    } else if (r->kind != MUSE_UPDATE_CHUNK) {
        return;
    }
    if (!s->on || r->off != s->next || s->next >= s->len) {
        return;   /* not where the slot is: the core answers its own offset and writes nothing */
    }
    uint32_t k = r->n < s->len - s->next ? r->n : s->len - s->next;
    mg_smp_sha256_update(s->h, data, k);
    s->next += k;
    if (s->next == s->len) {
        uint8_t got[32];
        mg_smp_sha256_final(s->h, got);
        s->matched = !memcmp(got, s->sha, 32);
    }
}

/* ---- nesting depth ------------------------------------------------------------------------- */

bool mg_smp_front_depth_ok(const uint8_t *req, size_t n)
{
    if (n <= HDR) {
        return true;   /* an empty body: the core reads it as an empty map */
    }
    cbor_t c = { req + HDR, n - HDR, 0 };
    /* Items still to read at each level (UINT64_MAX: indefinite, until its break), and what holds them (2, 3: a
     * chunked string's chunks, 4 an array, 5 a map, 6 a tag; 0: the body itself, one item). */
    uint64_t left[MG_SMP_FRONT_MAX_DEPTH + 1];
    uint8_t kind[MG_SMP_FRONT_MAX_DEPTH + 1];
    int level = 0;
    left[0] = 1;
    kind[0] = 0;
    /* Fails closed: anything it can't walk to the end is refused, so no decoder ever reads it (a length the 32-bit
     * target would truncate, #527's zcbor finding, included). */
    while (level > 0 || left[0] > 0) {
        if (c.i >= c.n) {
            return false;   /* truncated: an item missing */
        }
        if (left[level] == UINT64_MAX && is_break(&c)) {
            c.i++;
            level--;   /* the container ended (its own item was counted at its head) */
        } else {
            uint8_t major;
            uint64_t arg;
            bool indef;
            if (!head(&c, &major, &arg, &indef)) {
                return false;   /* malformed, or a head cut short */
            }
            if (major == 7 && indef) {
                return false;   /* a break where an item belongs */
            }
            if ((kind[level] == 2 || kind[level] == 3) && (major != kind[level] || indef)) {
                return false;   /* a chunked string's chunk: a definite string of its own type */
            }
            if (major >= 2 && major <= 6 && !indef && arg > UINT32_MAX) {
                return false;   /* no length, count or tag past 32 bits */
            }
            if (left[level] != UINT64_MAX) {
                left[level]--;
            }
            if ((major == 2 || major == 3) && !indef) {
                if (arg > c.n - c.i) {
                    return false;   /* past the request */
                }
                c.i += (size_t)arg;
            } else if (major >= 2 && major <= 6) {
                if (level + 1 > MG_SMP_FRONT_MAX_DEPTH) {
                    return false;   /* nested too deep */
                }
                uint64_t items = indef ? UINT64_MAX : major == 6 ? 1 : major == 5 ? arg * 2 : arg;
                if (!indef && items > c.n - c.i) {
                    return false;   /* more items than the request has bytes left */
                }
                level++;
                left[level] = items;
                kind[level] = major;
            }
        }
        while (level > 0 && left[level] == 0) {
            level--;   /* a definite container with all its items read */
        }
    }
    return c.i == c.n;   /* nothing after the body's one item */
}

/* ---- the gate ------------------------------------------------------------------------------ */

bool mg_smp_front_past_len(const uint8_t *req, size_t n, const mg_smp_front_up_t *up, bool core_holds)
{
    muse_update_req_t r;
    const uint8_t *data;
    if (mg_smp_front_read(req, n, &r, &data) != MG_SMP_FRONT_CHANGE) {
        return false;   /* not an update change, or one the core refuses itself */
    }
    uint64_t end = (uint64_t)r.off + r.n;
    if (r.kind == MUSE_UPDATE_UPLOAD0) {
        return end > r.id.len;   /* its data longer than its own len */
    }
    return r.kind == MUSE_UPDATE_CHUNK && core_holds && up && up->known && end > up->len;
}

bool mg_smp_front_up_note(const uint8_t *req, size_t n, mg_smp_front_up_t *up)
{
    muse_update_req_t r;
    const uint8_t *data;
    bool sha_bad = false;
    if (front_read(req, n, &r, &data, &sha_bad) != MG_SMP_FRONT_CHANGE || r.kind != MUSE_UPDATE_UPLOAD0) {
        return false;
    }
    if (sha_bad || r.id.len < MG_SMP_FRONT_MIN_UPLOAD) {
        return false;   /* one the core refuses (EINVAL): the upload it holds keeps its length */
    }
    if (up) {
        up->known = true;
        up->len = r.id.len;
    }
    return true;
}

int mg_smp_front_gate(const uint8_t *req, size_t n, mg_smp_front_slot_t *slot, bool core_holds, bool past_len,
                      muse_update_answer_t (*ask)(void *ctx, const muse_update_req_t *r),
                      bool (*bound)(void *ctx, muse_update_ident_t *id), void *ctx, bool *ended)
{
    muse_update_req_t r;
    const uint8_t *data;
    *ended = false;
    mg_smp_front_kind_t k = mg_smp_front_read(req, n, &r, &data);
    if (k == MG_SMP_FRONT_NOT_CHANGE) {
        return -1;
    }
    if (k == MG_SMP_FRONT_UNREADABLE) {
        return MG_SMP_EACCESSDENIED;   /* an update change the front can't read: never past it */
    }
    bool had = bound(ctx, &r.slot_for);
    if (had) {
        r.slot_ok = mg_smp_slot_would_match(slot, &r, data, &r.slot_for, core_holds);
    }
    muse_update_answer_t a = ask(ctx, &r);
    muse_update_ident_t now;
    *ended = had && !bound(ctx, &now);
    if (a == MUSE_UPDATE_OPEN) {
        if (past_len) {
            return MG_SMP_EACCESSDENIED;   /* past its upload's length: refused whatever let it through */
        }
        mg_smp_slot_apply(slot, &r, data, core_holds);
        return 0;
    }
    return a == MUSE_UPDATE_ASKING ? MG_SMP_EBUSY : MG_SMP_EACCESSDENIED;
}

size_t mg_smp_front_refusal(const uint8_t *req, int rc, uint8_t out[13])
{
    out[0] = (uint8_t)(req[0] + 1);   /* read -> read response, write -> write response, the version bits kept */
    out[1] = 0;
    out[2] = 0;
    out[3] = 5;
    out[4] = req[4];
    out[5] = req[5];
    out[6] = req[6];
    out[7] = req[7];
    out[8] = 0xa1;   /* {"rc": rc} */
    out[9] = 0x62;
    out[10] = 'r';
    out[11] = 'c';
    out[12] = (uint8_t)rc;
    return 13;
}
