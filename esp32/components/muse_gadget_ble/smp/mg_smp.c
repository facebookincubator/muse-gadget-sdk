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

/*
 * mg_smp.c: the MCUmgr SMP server core (mg_smp.h, mg_smp_backend.h).
 *
 * Generalized from the ESP32 boards' SMP server: framing, CBOR subset, OS
 * and image groups, MCUboot envelope and SHA-256 checks, plus what every
 * gadget needs on top: indefinite-length CBOR (Android), resume by len + sha,
 * the whole-file sha check, the healthy-mark confirm, asynchronous backend
 * calls, a streaming TLV parse, the core-owned active-hash record, memory
 * only while an update runs (one static pointer otherwise) and no stdio.
 */
#include "mg_smp.h"

#include <stddef.h>
#include <string.h>

#define HDR 8u

#if defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

enum { OP_READ = 0, OP_WRITE = 2 };
enum { GRP_OS = 0, GRP_IMG = 1 };
enum { OS_ECHO = 0, OS_RESET = 5, OS_PARAMS = 6 };
enum { IMG_STATE = 0, IMG_UPLOAD = 1, IMG_ERASE = 5 };

#define MCUBOOT_MAGIC 0x96F3B83Du
#define TLV_INFO 0x6907u
#define TLV_PROT_INFO 0x6908u
#define TLV_SHA256 0x10u

/* flags */
#define F_LINKDOWN 0x0001u /* the link dropped (since drop_ms); a write brings it back */
#define F_RESET 0x0002u    /* reset once the current response is out */
#define F_RUN 0x0004u      /* run() is on the stack */
#define F_PEND 0x0008u     /* a backend call returned MG_SMP_PENDING */
#define F_TX 0x0010u       /* a response is (partly) out */
#define F_MUTE 0x0020u     /* the link dropped: the pending request gets no response */
#define F_PERM 0x0040u     /* the commit in progress is permanent */
#define F_ABORT 0x0080u    /* mg_smp_abort() while a write was pending */
#define F_CLAIM 0x0100u    /* we hold the platform's OTA claim */
#define F_RESET_DONE 0x0200u
#define F_HAVE_HASH 0x0400u   /* the SHA-256 TLV was seen */
#define F_FILE_SHA 0x0800u    /* the client sent `sha` */
#define F_XAUTH 0x1000u       /* this connection started the transfer under way while allowed */
#define F_HOLD 0x2000u        /* an upload the link dropped waits for its resume (clock: drop_ms) */

/* up_state */
enum { UP_IDLE = 0, UP_RECV, UP_STAGED, UP_COMMITTED, UP_FAILED };

/* pend_op */
enum { P_NONE = 0, P_WRITE, P_FINISH, P_COMMIT, P_CONFIRM, P_LATCH };

/* Results of handling one request. */
/* Handler results. Responses that need deep frames (image state, rc) are
 * built by run() after the handler returned, to keep the stack shallow. */
enum { H_DONE = 0, H_HOLD, H_PEND, H_STATE, H_DROP };   /* < 0: answer rc = -h */

/* ---- the record (record_get/put key 0) --------------------------------- */

#define REC_MAGIC 0x3153474Du   /* "MGS1" */
enum { RS_NONE = 0, RS_PENDING = 1, RS_CONFIRMED = 2 };

typedef struct {
    uint8_t hash[32];
    mg_smp_image_id_t id;
    uint32_t payload_len;
    uint8_t state;
    uint8_t rsv[3];
} rec_entry_t;

typedef struct {
    uint32_t magic;
    uint8_t ver;
    uint8_t rsv[3];
    rec_entry_t cur;    /* the last commit */
    rec_entry_t prev;   /* what ran before it */
} rec_t;

typedef char rec_size_ok[(sizeof(rec_t) == MG_SMP_REC_SIZE) ? 1 : -1];

/* rec_entry_t.rsv[0] */
#define REC_PERMANENT 0x01u   /* committed permanent (no test boot) */

/* ---- the state (allocated only while SMP has work) ------------------------ */

struct mg_smp {
    const mg_smp_env_t *env;
    const mg_smp_config_t *cfg;
    const mg_smp_platform_t *pf;
    const mg_smp_backend_t *be;
    uint8_t *buf;           /* request buffers (mem_get BUFFERS), NULL when released */

    /* Receive ring of cfg->rx_slots requests. */
    uint16_t rx_len[MG_SMP_MAX_SLOTS];
    uint8_t rx_head;        /* oldest complete request (being processed) */
    uint8_t rx_count;       /* complete requests queued */
    uint16_t flags;
    uint16_t rx_drop;       /* stats: writes or requests dropped */
    uint32_t rx_t;          /* when the partly received request last grew */
    uint32_t drop_ms;       /* when the link dropped (F_LINKDOWN) */

    /* Response in progress, in slot rx_head. */
    uint16_t tx_len, tx_off;

    /* Backend call in flight (MG_SMP_PENDING). */
    volatile uint8_t done;
    volatile int8_t done_rc;
    uint8_t pend_op;

    /* Upload. */
    uint8_t up_state;
    uint8_t up_err;         /* why the last upload failed (answers later chunks) */
    uint8_t tlv_n;          /* bytes of the current TLV header seen */
    uint8_t tlv_hb[4];      /* the current TLV header */
    uint16_t tlv_type, tlv_len, tlv_pos;
    uint16_t hdr_size, prot_size;
    uint16_t feed_at, feed_n;   /* the chunk bytes still to stage (slot offset, count) */
    uint32_t up_len;        /* whole file */
    uint32_t up_off;        /* bytes accepted */
    uint32_t img_size;
    mg_smp_image_id_t up_id;
    uint8_t up_sha[32];     /* the client's `sha` (resume identity, whole-file check) */
    uint8_t hdr_or_hash[32];/* MCUboot header while it arrives, then the TLV hash */
    uint32_t commit_ms;

    union {
        uint64_t align;
        uint8_t b[MG_SMP_SHA256_CTX_SIZE];
        uint8_t rec[MG_SMP_REC_SIZE];   /* record scratch when no upload is hashing */
    } sha;
#if MG_SMP_FILE_SHA
    union {
        uint64_t align;
        uint8_t b[MG_SMP_SHA256_CTX_SIZE];
    } fsha;
#endif
};

/* The core's only static RAM: NULL (nothing held), COMMIT_MARK (nothing held,
 * but a commit waits for the reset), COMMIT_MARK_AUTH (the same, and the
 * connection that made it was allowed: its OS reset is not asked again), or
 * the state. */
static mg_smp_t *volatile g_smp;
#define COMMIT_MARK ((mg_smp_t *)(uintptr_t)1)
#define COMMIT_MARK_AUTH ((mg_smp_t *)(uintptr_t)3)
#define IS_MARK(p) ((p) == COMMIT_MARK || (p) == COMMIT_MARK_AUTH)

static mg_smp_t *cur(void)
{
    mg_smp_t *s = g_smp;
    return IS_MARK(s) ? 0 : s;
}
/* ---- small helpers --------------------------------------------------------- */

static uint16_t rd16le(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t rd32le(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint8_t *slot(mg_smp_t *s, uint8_t i) { return s->buf + (uint32_t)i * s->cfg->slot_size; }

#if MG_SMP_LOGS
static void logs(mg_smp_t *s, const char *m)
{
    if (s->pf->log) {
        s->pf->log(s->pf->ctx, m);
    }
}
#else
#define logs(s, m) ((void)(s))
#endif

#if MG_SMP_REVERT
static bool healthy(mg_smp_t *s) { return !s->pf->boot_healthy || s->pf->boot_healthy(s->pf->ctx); }
static bool can_revert(mg_smp_t *s) { return (s->be->flags & MG_SMP_BE_CAN_REVERT) != 0; }
#define has_slot_info(s) ((s)->be->slot_info != 0)
#else
#define can_revert(s) ((void)(s), false)
#define has_slot_info(s) ((void)(s), false)
#endif

static bool id_eq(const mg_smp_image_id_t *rec, const mg_smp_image_id_t *run)
{
    return rec->version.major == run->version.major && rec->version.minor == run->version.minor &&
           rec->version.revision == run->version.revision && rec->version.build == run->version.build &&
           rec->build_id == run->build_id;
}

/* A hash for an image no record covers: never empty, never a real SHA-256. */
static void synth_hash(const mg_smp_image_id_t *id, uint8_t h[32])
{
    static const uint8_t tag[8] = { 'm', 'g', 's', 'm', 'p', 0, 'v', '1' };
    memset(h, 0, 32);
    memcpy(h, tag, 8);
    h[8] = id->version.major;
    h[9] = id->version.minor;
    h[10] = (uint8_t)id->version.revision;
    h[11] = (uint8_t)(id->version.revision >> 8);
    for (int i = 0; i < 4; i++) {
        h[12 + i] = (uint8_t)(id->version.build >> (8 * i));
        h[16 + i] = (uint8_t)(id->build_id >> (8 * i));
    }
}

static void rec_load(mg_smp_t *s, rec_t *r)
{
    uint16_t n = 0;
    if (s->pf->record_get(s->pf->ctx, 0, r, (uint16_t)sizeof(*r), &n) != MG_SMP_EOK || n != sizeof(*r) ||
        r->magic != REC_MAGIC || r->ver != 1) {
        memset(r, 0, sizeof(*r));
    }
}

static int rec_save(mg_smp_t *s, rec_t *r)
{
    r->magic = REC_MAGIC;
    r->ver = 1;
    return s->pf->record_put(s->pf->ctx, 0, r, (uint16_t)sizeof(*r));
}

/* The record entry that names this running image, or NULL. */
static const rec_entry_t *rec_find(const rec_t *r, const mg_smp_image_id_t *id)
{
    if (r->cur.state != RS_NONE && id_eq(&r->cur.id, id)) {
        return &r->cur;
    }
    if (r->prev.state != RS_NONE && id_eq(&r->prev.id, id)) {
        return &r->prev;
    }
    return 0;
}

/* ---- CBOR writer ------------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    uint16_t cap, len;
    bool over;
} cw_t;

static void cw_byte(cw_t *w, uint8_t b)
{
    if (w->len < w->cap) {
        w->p[w->len++] = b;
    } else {
        w->over = true;
    }
}

static void cw_head(cw_t *w, uint8_t major, uint32_t v)
{
    major = (uint8_t)(major << 5);
    if (v < 24) {
        cw_byte(w, (uint8_t)(major | v));
    } else if (v <= 0xff) {
        cw_byte(w, major | 24);
        cw_byte(w, (uint8_t)v);
    } else if (v <= 0xffff) {
        cw_byte(w, major | 25);
        cw_byte(w, (uint8_t)(v >> 8));
        cw_byte(w, (uint8_t)v);
    } else {
        cw_byte(w, major | 26);
        cw_byte(w, (uint8_t)(v >> 24));
        cw_byte(w, (uint8_t)(v >> 16));
        cw_byte(w, (uint8_t)(v >> 8));
        cw_byte(w, (uint8_t)v);
    }
}

static void cw_raw(cw_t *w, const void *p, uint16_t n)
{
    if (n > w->cap - w->len) {
        w->over = true;
        return;
    }
    memmove(w->p + w->len, p, n);
    w->len = (uint16_t)(w->len + n);
}

static void cw_text(cw_t *w, const char *t)
{
    uint16_t n = (uint16_t)strlen(t);
    cw_head(w, 3, n);
    cw_raw(w, t, n);
}

static void cw_kuint(cw_t *w, const char *k, uint32_t v)
{
    cw_text(w, k);
    cw_head(w, 0, v);
}

static void cw_ktrue(cw_t *w, const char *k)
{
    cw_text(w, k);
    cw_byte(w, 0xf5);
}

/* ---- CBOR reader: a request map of text keys to simple values ------------- */

typedef struct {
    const uint8_t *p;
    uint16_t n, i;
} cr_t;

/* One head. *ai = additional info; indefinite returns ai 31 with v 0. */
static bool cr_head(cr_t *r, uint8_t *major, uint8_t *ai, uint64_t *v)
{
    if (r->i >= r->n) {
        return false;
    }
    uint8_t b = r->p[r->i++];
    *major = b >> 5;
    *ai = b & 0x1f;
    if (*ai < 24) {
        *v = *ai;
        return true;
    }
    if (*ai == 31) {
        *v = 0;
        return *major >= 2 && *major != 6;   /* strings, arrays, maps, break */
    }
    uint8_t len = *ai == 24 ? 1 : *ai == 25 ? 2 : *ai == 26 ? 4 : *ai == 27 ? 8 : 0;
    if (!len || len > r->n - r->i) {
        return false;
    }
    *v = 0;
    while (len--) {
        *v = *v << 8 | r->p[r->i++];
    }
    return true;
}

static bool cr_break(cr_t *r)
{
    if (r->i < r->n && r->p[r->i] == 0xff) {
        r->i++;
        return true;
    }
    return false;
}

static bool cr_skip(cr_t *r, int depth);

/* Skips the body of an item whose head was read. */
static bool cr_skip_body(cr_t *r, uint8_t major, uint8_t ai, uint64_t v, int depth)
{
    if (depth > 8) {
        return false;
    }
    switch (major) {
    case 2:
    case 3:
        if (ai == 31) {
            while (!cr_break(r)) {
                uint8_t m2, a2;
                uint64_t v2;
                if (!cr_head(r, &m2, &a2, &v2) || m2 != major || a2 == 31 || v2 > (uint64_t)(r->n - r->i)) {
                    return false;
                }
                r->i = (uint16_t)(r->i + v2);
            }
            return true;
        }
        if (v > (uint64_t)(r->n - r->i)) {
            return false;
        }
        r->i = (uint16_t)(r->i + v);
        return true;
    case 4:
    case 5:
        if (ai == 31) {
            while (!cr_break(r)) {
                if (!cr_skip(r, depth + 1) || (major == 5 && !cr_skip(r, depth + 1))) {
                    return false;
                }
            }
            return true;
        }
        if (v > r->n) {
            return false;
        }
        for (uint64_t k = 0; k < (major == 5 ? 2 * v : v); k++) {
            if (!cr_skip(r, depth + 1)) {
                return false;
            }
        }
        return true;
    case 6:
        return cr_skip(r, depth + 1);
    case 7:
        return ai != 31;
    default:
        return true;
    }
}

static bool cr_skip(cr_t *r, int depth)
{
    uint8_t major, ai;
    uint64_t v;
    if (r->i < r->n && r->p[r->i] == 0xff) {
        return false;   /* a break where an item belongs */
    }
    return cr_head(r, &major, &ai, &v) && cr_skip_body(r, major, ai, v, depth);
}

typedef struct {
    uint32_t off, len;
    const uint8_t *data, *hash, *sha;
    const uint8_t *echo;
    uint16_t data_len, hash_len, sha_len, echo_len;
    uint8_t has;   /* Q_* */
} req_t;

enum { Q_OFF = 1, Q_LEN = 2, Q_DATA = 4, Q_HASH = 8, Q_SHA = 16, Q_CONFIRM = 32, Q_CONFIRM_TRUE = 64, Q_ECHO = 128 };

static bool key_is(const uint8_t *k, uint64_t n, const char *s)
{
    size_t l = strlen(s);
    return n == l && !memcmp(k, s, l);
}

static NOINLINE bool parse_req(const uint8_t *p, uint16_t n, req_t *q)
{
    memset(q, 0, sizeof(*q));
    if (!n) {
        return true;   /* an empty body reads as an empty map */
    }
    cr_t r = { p, n, 0 };
    uint8_t major, ai;
    uint64_t count;
    if (!cr_head(&r, &major, &ai, &count) || major != 5) {
        return false;
    }
    bool indef = ai == 31;
    for (uint64_t k = 0;; k++) {
        if (indef ? cr_break(&r) : k == count) {
            break;
        }
        uint64_t klen;
        if (!cr_head(&r, &major, &ai, &klen) || major != 3 || ai == 31 || klen > (uint64_t)(r.n - r.i)) {
            return false;
        }
        const uint8_t *key = r.p + r.i;
        r.i = (uint16_t)(r.i + klen);
        uint64_t v;
        if (!cr_head(&r, &major, &ai, &v)) {
            return false;
        }
        if (major == 0 && ai != 31 && (key_is(key, klen, "off") || key_is(key, klen, "len"))) {
            if (v > 0xffffffffu) {
                return false;
            }
            if (key[0] == 'o') {
                q->has |= Q_OFF;
                q->off = (uint32_t)v;
            } else {
                q->has |= Q_LEN;
                q->len = (uint32_t)v;
            }
        } else if ((major == 2 || major == 3) && ai != 31 &&
                   (key_is(key, klen, "data") || key_is(key, klen, "hash") || key_is(key, klen, "sha") ||
                    key_is(key, klen, "d"))) {
            if (v > (uint64_t)(r.n - r.i)) {
                return false;
            }
            const uint8_t *val = r.p + r.i;
            uint16_t vl = (uint16_t)v;
            if (major == 3 && key_is(key, klen, "d")) {
                q->has |= Q_ECHO;
                q->echo = val;
                q->echo_len = vl;
            } else if (major == 2 && klen == 4 && key[0] == 'd') {
                q->has |= Q_DATA;
                q->data = val;
                q->data_len = vl;
            } else if (major == 2 && key[0] == 'h') {
                q->has |= Q_HASH;
                q->hash = val;
                q->hash_len = vl;
            } else if (major == 2 && key[0] == 's') {
                q->has |= Q_SHA;
                q->sha = val;
                q->sha_len = vl;
            }
            r.i = (uint16_t)(r.i + vl);
        } else if (major == 7 && (v == 20 || v == 21) && key_is(key, klen, "confirm")) {
            q->has |= Q_CONFIRM | (v == 21 ? Q_CONFIRM_TRUE : 0);
        } else if (!cr_skip_body(&r, major, ai, v, 0)) {
            return false;   /* "image", "upgrade", "force", null hash, ... skipped */
        }
    }
    return true;
}

/* ---- responses ------------------------------------------------------------------ */

/* Starts a response in the head slot (the request is no longer needed). */
static cw_t rsp_begin(mg_smp_t *s)
{
    cw_t w = { slot(s, s->rx_head) + HDR, (uint16_t)(s->cfg->slot_size - HDR), 0, false };
    return w;
}

static void rsp_end(mg_smp_t *s, cw_t *w)
{
    uint8_t *h = slot(s, s->rx_head);
    if (w->over) {
        *w = rsp_begin(s);
        cw_byte(w, 0xa1);
        cw_kuint(w, "rc", MG_SMP_EMSGSIZE);
    }
    h[0] = (uint8_t)(h[0] + 1);   /* read -> read rsp, write -> write rsp; version bits kept */
    h[1] = 0;
    h[2] = (uint8_t)(w->len >> 8);
    h[3] = (uint8_t)w->len;
    s->tx_len = (uint16_t)(HDR + w->len);
    s->tx_off = 0;
    s->flags |= F_TX;
}

static void rsp_rc(mg_smp_t *s, int rc)
{
    cw_t w = rsp_begin(s);
    cw_byte(&w, 0xa1);
    cw_kuint(&w, "rc", (uint32_t)rc);
    rsp_end(s, &w);
}

/* ---- image state ------------------------------------------------------------------ */

/* One slot (0 or 1) as image state reports it; false = empty. Reads the
 * record each time (one small read) to keep the stack shallow. */
static NOINLINE bool slot_fill(mg_smp_t *s, uint8_t i, mg_smp_slot_t *x, const rec_t *rp)
{
#define r (*rp)
    const rec_entry_t *e;
    memset(x, 0, sizeof(*x));
#if MG_SMP_REVERT
    if (has_slot_info(s)) {
        if (s->be->slot_info(s->be->ctx, i, x) != MG_SMP_EOK) {
            return false;
        }
        x->flags |= MG_SMP_SLOT_PRESENT;
        if (!(x->flags & MG_SMP_SLOT_HAS_HASH)) {
            e = rec_find(&r, &x->id);
            if ((s->up_state == UP_STAGED || s->up_state == UP_COMMITTED) && !(x->flags & MG_SMP_SLOT_ACTIVE) &&
                id_eq(&s->up_id, &x->id)) {
                memcpy(x->hash, s->hdr_or_hash, 32);   /* the image just staged */
            } else if (e) {
                memcpy(x->hash, e->hash, 32);
            } else {
                synth_hash(&x->id, x->hash);
            }
        }
        return true;
    }
#endif
    if (i == 0) {
        s->pf->running_id(s->pf->ctx, &x->id);
        x->flags = MG_SMP_SLOT_PRESENT | MG_SMP_SLOT_ACTIVE | MG_SMP_SLOT_BOOTABLE | MG_SMP_SLOT_CONFIRMED;
        e = rec_find(&r, &x->id);
        if (e) {
            memcpy(x->hash, e->hash, 32);
            if (can_revert(s) && e == &r.cur && e->state != RS_CONFIRMED) {
                x->flags &= (uint8_t)~MG_SMP_SLOT_CONFIRMED;
            }
        } else {
            synth_hash(&x->id, x->hash);
        }
        if (!can_revert(s)) {
            x->flags |= MG_SMP_SLOT_PERMANENT;
        }
        return true;
    }
    if (s->up_state != UP_STAGED && s->up_state != UP_COMMITTED) {
        return false;
    }
    x->id = s->up_id;
    memcpy(x->hash, s->hdr_or_hash, 32);
    x->flags = MG_SMP_SLOT_PRESENT | MG_SMP_SLOT_BOOTABLE;
    if (s->up_state == UP_COMMITTED) {
        x->flags |= MG_SMP_SLOT_PENDING;
        if (!can_revert(s) || (s->flags & F_PERM)) {
            x->flags |= MG_SMP_SLOT_PERMANENT;
        }
    }
    return true;
#undef r
}

/* The record lives in the SHA context's space whenever no upload is hashing
 * (the common case), so image state and commit need no record on the
 * stack; while an upload is hashing, a deeper frame holds it. */
static rec_t *rec_scratch(mg_smp_t *s) { return s->up_state == UP_RECV ? 0 : (rec_t *)(void *)s->sha.rec; }

static NOINLINE bool slot_get_stacked(mg_smp_t *s, uint8_t i, mg_smp_slot_t *x)
{
    rec_t r;
    rec_load(s, &r);
    return slot_fill(s, i, x, &r);
}

static bool slot_get(mg_smp_t *s, uint8_t i, mg_smp_slot_t *x)
{
    rec_t *r = rec_scratch(s);
    if (!r) {
        return slot_get_stacked(s, i, x);
    }
    rec_load(s, r);
    return slot_fill(s, i, x, r);
}

/* The running slot: is `hash` (NULL = no hash: the running one) it, and is
 * it confirmed? Kept out of line so its slot copy leaves the stack. */
static NOINLINE bool active_is(mg_smp_t *s, const uint8_t *hash, bool *confirmed)
{
    mg_smp_slot_t x;
    for (uint8_t i = 0; i < 2; i++) {
        if (slot_get(s, i, &x) && (x.flags & MG_SMP_SLOT_ACTIVE)) {
            *confirmed = (x.flags & MG_SMP_SLOT_CONFIRMED) != 0;
            return !hash || !memcmp(hash, x.hash, 32);
        }
    }
    *confirmed = true;
    return !hash;
}

/* The staged upload carries the running image's id (the same version and
 * build id) but is not the image the running slot reports (a different build
 * with that id; or no record entry, so the running hash is unknown). Once
 * committed, the record could not tell the two apart: after a revert the old
 * image would read as the new one (the app reports success, the healthy mark
 * confirms the old image). Refused, unless the backend reports the running
 * slot's own hash (MG_SMP_SLOT_HAS_HASH), which keeps the record out of it.
 * Called once staged: the record goes in the hash's space. */
static NOINLINE bool up_id_taken(mg_smp_t *s)
{
    rec_t *r = (rec_t *)(void *)s->sha.rec;
    mg_smp_slot_t x;
    rec_load(s, r);
    for (uint8_t i = 0; i < (has_slot_info(s) ? 2 : 1); i++) {   /* without slot_info slot 0 runs */
        if (slot_fill(s, i, &x, r) && (x.flags & MG_SMP_SLOT_ACTIVE)) {
            return !(x.flags & MG_SMP_SLOT_HAS_HASH) && id_eq(&s->up_id, &x.id) &&
                   memcmp(x.hash, s->hdr_or_hash, 32) != 0;
        }
    }
    return false;
}

static void cw_ver(cw_t *w, const mg_smp_version_t *v)
{
    uint32_t parts[4] = { v->major, v->minor, v->revision, v->build };
    unsigned at = w->len, i;
    cw_byte(w, 0x60);   /* text head, length patched below (< 24) */
    for (i = 0; i < (v->build ? 4u : 3u); i++) {
        uint32_t x = parts[i], d = 1;
        if (i) {
            cw_byte(w, '.');
        }
        while (x / d >= 10) {
            d *= 10;
        }
        for (; d; d /= 10) {
            cw_byte(w, (uint8_t)('0' + x / d % 10));
        }
    }
    if (!w->over) {
        w->p[at] = (uint8_t)(0x60 | (w->len - at - 1));
    }
}

static NOINLINE void rsp_state(mg_smp_t *s)
{
    static const struct {
        uint8_t f;
        char k[10];
    } fl[] = { { MG_SMP_SLOT_BOOTABLE, "bootable" }, { MG_SMP_SLOT_PENDING, "pending" },
               { MG_SMP_SLOT_CONFIRMED, "confirmed" }, { MG_SMP_SLOT_ACTIVE, "active" },
               { MG_SMP_SLOT_PERMANENT, "permanent" } };
    mg_smp_slot_t x;
    cw_t w = rsp_begin(s);
    unsigned n = 0, at, i, k;
    cw_byte(&w, 0xa1);
    cw_text(&w, "images");
    at = w.len;
    cw_byte(&w, 0x80);   /* array head, patched below (at most 2) */
    for (i = 0; i < 2; i++) {
        if (!slot_get(s, (uint8_t)i, &x)) {
            continue;
        }
        unsigned m = 3;
        for (k = 0; k < sizeof(fl) / sizeof(fl[0]); k++) {
            m += (x.flags & fl[k].f) ? 1u : 0u;
        }
        cw_head(&w, 5, m);
        cw_kuint(&w, "slot", i);
        cw_text(&w, "version");
        cw_ver(&w, &x.id.version);
        cw_text(&w, "hash");
        cw_head(&w, 2, 32);
        cw_raw(&w, x.hash, 32);
        for (k = 0; k < sizeof(fl) / sizeof(fl[0]); k++) {
            if (x.flags & fl[k].f) {
                cw_ktrue(&w, fl[k].k);
            }
        }
        n++;
    }
    if (!w.over) {
        w.p[at] = (uint8_t)(0x80 | n);
    }
    rsp_end(s, &w);
}

/* ---- upload ------------------------------------------------------------------------ */

static void sha_both(mg_smp_t *s, const uint8_t *p, uint32_t n, bool image)
{
    if (image) {
        mg_smp_sha256_update(s->sha.b, p, n);
    }
#if MG_SMP_FILE_SHA
    mg_smp_sha256_update(s->fsha.b, p, n);
#endif
}

static void claim_release(mg_smp_t *s)
{
    if (s->flags & F_CLAIM) {
        s->flags &= (uint16_t)~F_CLAIM;
        if (s->pf->ota_claim) {
            s->pf->ota_claim(s->pf->ctx, false);
        }
    }
}

static void up_fail(mg_smp_t *s, int rc)
{
    if (s->up_state == UP_RECV || s->up_state == UP_STAGED) {
        if (s->up_off >= 32 && s->be->abort) {
            s->be->abort(s->be->ctx);   /* begin ran at the 32nd byte */
        }
        claim_release(s);
    }
    s->up_state = UP_FAILED;
    s->up_err = (uint8_t)rc;
    s->feed_n = 0;
}

/* The first 32 bytes are in: check the MCUboot header, start the backend. */
static int up_header(mg_smp_t *s)
{
    const uint8_t *h = s->hdr_or_hash;
    if (rd32le(h) != MCUBOOT_MAGIC) {
        return MG_SMP_EINVAL;
    }
    s->hdr_size = rd16le(h + 8);
    s->prot_size = rd16le(h + 10);
    s->img_size = rd32le(h + 12);
    s->up_id.version.major = h[20];
    s->up_id.version.minor = h[21];
    s->up_id.version.revision = rd16le(h + 22);
    s->up_id.version.build = rd32le(h + 24);
    s->up_id.build_id = 0;
    /* header, payload, protected TLVs, then at least the TLV info header and
     * a SHA-256 TLV (40 bytes) */
    uint64_t need = (uint64_t)s->hdr_size + s->img_size + s->prot_size + 40;
    if (s->hdr_size < 32 || !s->img_size || need > s->up_len) {
        return MG_SMP_EINVAL;
    }
    if (s->be->max_payload && s->img_size > s->be->max_payload) {
        return MG_SMP_ENOMEM;
    }
    uint32_t resume = 0;
    return s->be->begin(s->be->ctx, s->img_size, &resume);
}

/* Streaming TLV parse over [base, base + area) where the area starts with its
 * info header. Captures the build id (protected) and the SHA-256 (not). */
static void tlv_byte(mg_smp_t *s, uint8_t b, bool prot)
{
    if (s->tlv_n < 4) {
        s->tlv_hb[s->tlv_n++] = b;
        if (s->tlv_n == 4) {
            s->tlv_type = rd16le(s->tlv_hb);
            s->tlv_len = rd16le(s->tlv_hb + 2);
            s->tlv_pos = 0;
            if (s->tlv_type == TLV_INFO || s->tlv_type == TLV_PROT_INFO) {
                s->tlv_n = 0;   /* the area header: TLVs follow */
            } else if (s->tlv_len == 0) {
                s->tlv_n = 0;
            }
        }
        return;
    }
    if (prot && s->tlv_type == MG_SMP_TLV_BUILD_ID && s->tlv_len == 4) {
        s->up_id.build_id = s->up_id.build_id << 8 | b;   /* big-endian, as imgtool writes it */
    } else if (!prot && s->tlv_type == TLV_SHA256 && s->tlv_len == 32 && !(s->flags & F_HAVE_HASH)) {
        s->hdr_or_hash[s->tlv_pos] = b;
        if (s->tlv_pos == 31) {
            s->flags |= F_HAVE_HASH;
        }
    }
    if (++s->tlv_pos >= s->tlv_len) {
        s->tlv_n = 0;
    }
}

/* Stages the chunk bytes left in feed_at/feed_n. H_DONE, H_PEND, or <0 = -rc. */
static int up_feed(mg_smp_t *s)
{
    const uint8_t *p = slot(s, s->rx_head) + s->feed_at;
    while (s->feed_n) {
        uint32_t off = s->up_off;
        uint32_t pay = s->hdr_size, end = pay + s->img_size, hashed = end + s->prot_size;
        uint32_t k;
        if (off < 32) {
            k = 32 - off;
            if (k > s->feed_n) {
                k = s->feed_n;
            }
            memcpy(s->hdr_or_hash + off, p, k);
            sha_both(s, p, k, true);
            if (off + k == 32) {
                s->up_off = 32;   /* begin has run once up_off reaches 32 (up_fail) */
                int rc = up_header(s);
                if (rc != MG_SMP_EOK) {
                    s->up_off = 31;   /* begin failed: nothing to abort */
                    return -rc;
                }
                s->up_off = off;   /* restored; advanced below */
                s->tlv_n = 0;
            }
        } else if (off < pay) {
            k = pay - off;   /* the header's padding */
            if (k > s->feed_n) {
                k = s->feed_n;
            }
            sha_both(s, p, k, true);
        } else if (off < end) {
            k = end - off;
            if (k > s->feed_n) {
                k = s->feed_n;
            }
            if (s->be->max_write && k > s->be->max_write) {
                k = s->be->max_write;
            }
            sha_both(s, p, k, true);
            s->done = 0;
            int rc = s->be->write(s->be->ctx, off - pay, p, k);
            if (rc == MG_SMP_PENDING) {
                s->up_off += k;
                s->feed_at = (uint16_t)(s->feed_at + k);
                s->feed_n = (uint16_t)(s->feed_n - k);
                return H_PEND;
            }
            if (rc != MG_SMP_EOK) {
                return -(rc > 0 ? rc : MG_SMP_EUNKNOWN);
            }
        } else if (off < hashed) {
            k = hashed - off;   /* protected TLVs: hashed, parsed for the build id */
            if (k > s->feed_n) {
                k = s->feed_n;
            }
            sha_both(s, p, k, true);
            if (off == end) {
                s->tlv_n = 0;
            }
            for (uint32_t i = 0; i < k; i++) {
                tlv_byte(s, p[i], true);
            }
        } else {
            k = s->feed_n;   /* the TLV area: not hashed, parsed for the SHA-256 */
            if (off == hashed) {
                s->tlv_n = 0;
            }
            sha_both(s, p, k, false);
            for (uint32_t i = 0; i < k; i++) {
                tlv_byte(s, p[i], false);
            }
        }
        s->up_off += k;
        p += k;
        s->feed_at = (uint16_t)(s->feed_at + k);
        s->feed_n = (uint16_t)(s->feed_n - k);
    }
    return H_DONE;
}

/* The whole file is in: both hashes must match before anything is verified. */
static int up_check(mg_smp_t *s)
{
    uint8_t got[32];
    int rc = MG_SMP_EOK;
#if MG_SMP_FILE_SHA
    mg_smp_sha256_final(s->fsha.b, got);
    if ((s->flags & F_FILE_SHA) && memcmp(got, s->up_sha, 32) != 0) {
        rc = MG_SMP_ECORRUPT;
    }
#endif
    mg_smp_sha256_final(s->sha.b, got);
    if (!(s->flags & F_HAVE_HASH) || memcmp(got, s->hdr_or_hash, 32) != 0) {
        rc = MG_SMP_ECORRUPT;
    }
    if (rc != MG_SMP_EOK) {
        logs(s, "smp: upload refused, hash mismatch");
    }
    return rc;
}

static void rsp_off(mg_smp_t *s, bool match)
{
    cw_t w = rsp_begin(s);
    cw_byte(&w, match ? 0xa2 : 0xa1);
    cw_kuint(&w, "off", s->up_state == UP_RECV ? s->up_off : s->up_len);
    if (match) {
        cw_ktrue(&w, "match");
    }
    rsp_end(s, &w);
}

/* Continues an upload chunk after its bytes are staged (or a write completed). */
static int up_chunk_continue(mg_smp_t *s)
{
    int r = up_feed(s);
    if (r == H_PEND) {
        s->pend_op = P_WRITE;
        return H_PEND;
    }
    if (r < 0) {
        up_fail(s, -r);
        return r;
    }
    if (s->up_off == s->up_len) {
        int rc = up_check(s);
        if (rc == MG_SMP_EOK) {
            s->up_state = UP_STAGED;   /* the hash is final: the record check below uses its space */
            if (up_id_taken(s)) {
                logs(s, "smp: upload refused, running id on another image");
                rc = MG_SMP_EBADSTATE;
            }
        }
        if (rc != MG_SMP_EOK) {
            up_fail(s, rc);
            return -(rc);
        }
        rsp_off(s, (s->flags & F_FILE_SHA) != 0 && MG_SMP_FILE_SHA);
        return H_DONE;
    }
    rsp_off(s, false);
    return H_DONE;
}

/* Part of the transfer this connection started while allowed? Its
 * chunks, a late duplicate of the tail, and the client's retry from 0 with
 * the same len and sha. */
static bool up_same_transfer(const mg_smp_t *s, const req_t *q)
{
    if (!(s->flags & F_XAUTH) || (s->up_state != UP_RECV && s->up_state != UP_STAGED)) {
        return false;
    }
    if (q->off != 0) {
        return true;
    }
    return q->len == s->up_len && (q->has & Q_SHA) && q->sha_len == 32 && (s->flags & F_FILE_SHA) &&
           !memcmp(q->sha, s->up_sha, 32);
}

/* A refusal's rc: MG_SMP_EBUSY while the platform asks for its confirm press (refusal_rc), else
 * MG_SMP_EACCESSDENIED (no refusal_rc, or any other answer). */
static int refused(const mg_smp_t *s)
{
    return -(s->pf->refusal_rc && s->pf->refusal_rc(s->pf->ctx) == MG_SMP_EBUSY ? MG_SMP_EBUSY
                                                                               : MG_SMP_EACCESSDENIED);
}

/* The image an upload names, for may_upload: at offset 0 its own len and whole-image SHA-256, on a chunk at an
 * offset the upload it continues. The SHA-256 only where the core checks it at the end (MG_SMP_FILE_SHA), so a
 * confirmation bound to it covers that image alone; else NULL. */
static void up_names(const mg_smp_t *s, const req_t *q, uint32_t *len, const uint8_t **sha)
{
    *len = 0;
    *sha = NULL;
    if (q->off == 0) {
        if (q->has & Q_LEN) {
            *len = q->len;
        }
        if (MG_SMP_FILE_SHA && (q->has & Q_SHA) && q->sha_len == 32) {
            *sha = q->sha;
        }
    } else if (s->up_state == UP_RECV || s->up_state == UP_STAGED) {
        *len = s->up_len;
        if (MG_SMP_FILE_SHA && (s->flags & F_FILE_SHA)) {
            *sha = s->up_sha;
        }
    }
}

static NOINLINE int h_upload(mg_smp_t *s, const req_t *q, const uint8_t *body)
{
    if (s->pf->may_change && !up_same_transfer(s, q)) {
        bool ok;
        if (s->pf->may_upload) {
            uint32_t len;
            const uint8_t *sha;
            up_names(s, q, &len, &sha);
            ok = s->pf->may_upload(s->pf->ctx, len, sha);
        } else {
            ok = s->pf->may_change(s->pf->ctx, MG_SMP_OP_UPLOAD);
        }
        if (!ok) {
            return refused(s);   /* before anything changes */
        }
        s->flags |= F_XAUTH;
    }
    if ((q->has & (Q_OFF | Q_DATA)) != (Q_OFF | Q_DATA)) {
        return -(MG_SMP_EINVAL);
    }
    if (q->off == 0) {
        if (!(q->has & Q_LEN) || q->len < 32 + 40) {
            return -(MG_SMP_EINVAL);
        }
        if (q->has & Q_SHA && q->sha_len != 32) {
            return -(MG_SMP_EINVAL);
        }
        if (s->up_state == UP_COMMITTED) {
            return -(MG_SMP_EBADSTATE);   /* until the reset */
        }
        if ((s->up_state == UP_RECV || s->up_state == UP_STAGED) && q->len == s->up_len &&
            (q->has & Q_SHA) && (s->flags & F_FILE_SHA) && !memcmp(q->sha, s->up_sha, 32)) {
            s->flags &= (uint16_t)~F_HOLD;
            rsp_off(s, false);   /* the same upload again: resume at our offset */
            return H_DONE;
        }
        if (s->up_state == UP_RECV || s->up_state == UP_STAGED) {
            up_fail(s, 0);   /* a different upload replaces it */
        }
        if (s->pf->ota_claim && !s->pf->ota_claim(s->pf->ctx, true)) {
            return -(MG_SMP_EBUSY);
        }
        s->flags |= F_CLAIM;
        if (s->pf->media_stop) {
            s->pf->media_stop(s->pf->ctx);
        }
        s->flags &= (uint16_t)~(F_HAVE_HASH | F_FILE_SHA | F_PERM);
        s->up_state = UP_RECV;
        s->up_err = 0;
        s->up_len = q->len;
        s->up_off = 0;
        s->hdr_size = 32;
        s->prot_size = 0;
        s->img_size = 0;
        s->tlv_n = 0;
        memset(&s->up_id, 0, sizeof(s->up_id));
        if (q->has & Q_SHA) {
            memcpy(s->up_sha, q->sha, 32);
            s->flags |= F_FILE_SHA;
        }
        mg_smp_sha256_init(s->sha.b);
#if MG_SMP_FILE_SHA
        mg_smp_sha256_init(s->fsha.b);
#endif
    } else if (s->up_state == UP_STAGED) {
        rsp_off(s, false);   /* a late duplicate of the tail */
        return H_DONE;
    } else if (s->up_state != UP_RECV) {
        return -(s->up_state == UP_FAILED && s->up_err ? s->up_err :
                  s->up_state == UP_COMMITTED ? MG_SMP_EBADSTATE : MG_SMP_EINVAL);
    }
    if (q->off != s->up_off) {
        rsp_off(s, false);   /* not where we are: the client continues from ours */
        return H_DONE;
    }
    uint32_t n = q->data_len;
    if (n > s->up_len - s->up_off) {
        n = s->up_len - s->up_off;
    }
    s->flags &= (uint16_t)~F_HOLD;   /* the upload goes on: its resume clock stops */
    s->feed_at = (uint16_t)(q->data - body + HDR);
    s->feed_n = (uint16_t)n;
    return up_chunk_continue(s);
}

/* ---- state write ---------------------------------------------------------------------- */

/* Commit step 2 (after finish): record, then backend commit. */
static NOINLINE int commit_continue(mg_smp_t *s)
{
    rec_t *r = (rec_t *)(void *)s->sha.rec;   /* staged: the hash is final, its space is free */
    mg_smp_image_id_t run;
    rec_load(s, r);
    s->pf->running_id(s->pf->ctx, &run);
    const rec_entry_t *e = rec_find(r, &run);
    if (e == &r->cur) {
        r->prev = r->cur;   /* what runs now becomes the previous image */
        r->prev.state = RS_CONFIRMED;
    } else if (!e) {
        memset(&r->prev, 0, sizeof(r->prev));
    }
    memcpy(r->cur.hash, s->hdr_or_hash, 32);
    r->cur.id = s->up_id;
    r->cur.payload_len = s->img_size;
    r->cur.state = RS_PENDING;
    r->cur.rsv[0] = (uint8_t)(((s->flags & F_PERM) || !can_revert(s)) ? REC_PERMANENT : 0);
    if (rec_save(s, r) != MG_SMP_EOK) {
        logs(s, "smp: record write failed, not committing");
        up_fail(s, MG_SMP_EUNKNOWN);
        return -(MG_SMP_EUNKNOWN);
    }
    s->done = 0;
    int rc = s->be->commit(s->be->ctx, (s->flags & F_PERM) != 0 || !can_revert(s));
    if (rc == MG_SMP_PENDING) {
        s->pend_op = P_COMMIT;
        return H_PEND;
    }
    if (rc != MG_SMP_EOK) {
        up_fail(s, rc);
        return -(rc);
    }
    s->up_state = UP_COMMITTED;
    s->commit_ms = s->pf->now_ms ? s->pf->now_ms(s->pf->ctx) : 0;
    logs(s, "smp: image committed for the next boot");
    return H_STATE;
}

static int commit_start(mg_smp_t *s, bool permanent)
{
    if (permanent) {
        s->flags |= F_PERM;
    } else {
        s->flags &= (uint16_t)~F_PERM;
    }
    s->done = 0;
    int rc = s->be->finish(s->be->ctx);
    if (rc == MG_SMP_PENDING) {
        s->pend_op = P_FINISH;
        return H_PEND;
    }
    if (rc != MG_SMP_EOK) {
        up_fail(s, rc);
        return -(rc);
    }
    return commit_continue(s);
}

#if MG_SMP_REVERT
/* Confirm step 2 (after backend confirm): mark the record. */
static void confirm_rec(mg_smp_t *s, rec_t *r)
{
    mg_smp_image_id_t run;
    rec_load(s, r);
    s->pf->running_id(s->pf->ctx, &run);
    if (r->cur.state == RS_PENDING && id_eq(&r->cur.id, &run)) {
        r->cur.state = RS_CONFIRMED;
        rec_save(s, r);
    }
}

static NOINLINE void confirm_rec_stacked(mg_smp_t *s)
{
    rec_t r;
    confirm_rec(s, &r);
}

static void confirm_record(mg_smp_t *s)
{
    rec_t *r = rec_scratch(s);
    if (r) {
        confirm_rec(s, r);
    } else {
        confirm_rec_stacked(s);
    }
}

static int confirm_start(mg_smp_t *s, uint8_t op)
{
    s->done = 0;
    int rc = s->be->confirm ? s->be->confirm(s->be->ctx) : MG_SMP_EOK;
    if (rc == MG_SMP_PENDING) {
        s->pend_op = op;
        return H_PEND;
    }
    if (rc == MG_SMP_EOK) {
        confirm_record(s);
        return op == P_CONFIRM ? H_STATE : H_DONE;
    }
    return -(rc > 0 ? rc : MG_SMP_EUNKNOWN);
}

#endif

static NOINLINE int h_state_write(mg_smp_t *s, const req_t *q)
{
    bool confirm = (q->has & Q_CONFIRM_TRUE) != 0;
    if (s->pf->may_change && !((s->flags & F_XAUTH) && (s->up_state == UP_STAGED || s->up_state == UP_COMMITTED)) &&
        !s->pf->may_change(s->pf->ctx, MG_SMP_OP_STATE)) {
        return refused(s);   /* not the end of this connection's transfer */
    }
    if ((q->has & Q_HASH) && q->hash_len != 32) {
        return -(MG_SMP_EINVAL);
    }
    /* A hash naming the staged upload tests (or confirms) that upload, before
     * any "already running" answer: the running slot's hash may come from a
     * record entry matched by id, which still names an image that was tested
     * and reverted, so a re-upload of it must get a real test mark. */
    if ((q->has & Q_HASH) && (s->up_state == UP_STAGED || s->up_state == UP_COMMITTED) &&
        !memcmp(q->hash, s->hdr_or_hash, 32)) {
        if (s->up_state == UP_COMMITTED) {
            return H_STATE;   /* already marked */
        }
        return commit_start(s, confirm);
    }
    bool act_confirmed;
    bool is_active = active_is(s, (q->has & Q_HASH) ? q->hash : 0, &act_confirmed);
    if (confirm && is_active) {
#if MG_SMP_REVERT
        if (can_revert(s) && !act_confirmed) {
            if (!healthy(s)) {
                logs(s, "smp: confirm accepted, applied at the healthy mark");
                return H_STATE;   /* mg_smp_healthy() confirms it */
            }
            return confirm_start(s, P_CONFIRM);
        }
#endif
        return H_STATE;   /* already confirmed or permanent */
    }
    if (!(q->has & Q_HASH)) {
        return -(MG_SMP_EINVAL);   /* a test needs a hash */
    }
    if (is_active) {
        return H_STATE;   /* testing what already runs */
    }
    return -(MG_SMP_ENOENT);
}

/* ---- dispatch -------------------------------------------------------------------------- */

/* One ATT value at this MTU: MTU-3, never over 512 (the attribute limit;
 * Android refuses a longer value even at MTU 517). Notifications and the
 * write size the clients use both follow it. */
static uint16_t att_value_max(uint16_t mtu)
{
    if (mtu <= 23) {
        return 20;
    }
    return (uint16_t)(mtu - 3 > 512 ? 512 : mtu - 3);
}

/* buf_size as reported: the clients size every request to it and write it
 * in min(MTU-3, 512) pieces, `window` requests at a time (Android min(3,
 * buf_count) after the first chunk, iOS always 2). With max_writes the
 * writes in flight stay within what the transport holds. */
static uint16_t reported_buf(mg_smp_t *s)
{
    unsigned bs = s->cfg->buf_size ? s->cfg->buf_size : s->cfg->slot_size;
    if (s->cfg->max_writes) {
        unsigned frag = att_value_max(s->pf->mtu(s->pf->ctx));
        unsigned bc = s->cfg->buf_count ? s->cfg->buf_count : s->cfg->rx_slots;
        unsigned window = bc < 2 ? 2 : bc > 3 ? 3 : bc;
        unsigned per = s->cfg->max_writes / window;
        unsigned lim;
        lim = (per ? per : 1) * frag;
        if (lim < MG_SMP_MIN_REPORTED_BUF) {
            lim = MG_SMP_MIN_REPORTED_BUF;
        }
        if (bs > lim) {
            bs = lim;
        }
    }
    return (uint16_t)(bs > MG_SMP_MAX_REPORTED_BUF ? MG_SMP_MAX_REPORTED_BUF : bs);
}

static NOINLINE int handle(mg_smp_t *s)
{
    uint8_t *f = slot(s, s->rx_head);
    uint8_t op = f[0] & 0x07;
    uint16_t group = (uint16_t)(f[4] << 8 | f[5]);
    uint8_t id = f[7];
    uint16_t body_len = (uint16_t)(f[2] << 8 | f[3]);
    req_t q;
    if (f[1] & 0x80) {
        return -MG_SMP_EMSGSIZE;   /* marked by rx_take: the request did not fit */
    }
    if (!parse_req(f + HDR, body_len, &q)) {
        s->rx_drop++;   /* undecodable: a lost fragment; the client's timeout recovers */
        return H_DROP;
    }
    if (group == GRP_OS && id == OS_PARAMS && op == OP_READ) {
        cw_t w = rsp_begin(s);
        cw_byte(&w, 0xa2);
        cw_kuint(&w, "buf_size", reported_buf(s));
        cw_kuint(&w, "buf_count", s->cfg->buf_count ? s->cfg->buf_count : s->cfg->rx_slots);
        rsp_end(s, &w);
#if MG_SMP_ECHO
    } else if (group == GRP_OS && id == OS_ECHO && op == OP_WRITE) {
        cw_t w = rsp_begin(s);
        const uint8_t *t = q.echo;
        uint16_t n = q.echo_len;
        cw_byte(&w, 0xa1);
        cw_text(&w, "r");
        cw_head(&w, 3, n);
        cw_raw(&w, t ? t : (const uint8_t *)"", t ? n : 0);   /* memmove: in place */
        rsp_end(s, &w);
#endif
    } else if (group == GRP_OS && id == OS_RESET && op == OP_WRITE) {
        if (s->pf->may_change && !((s->flags & F_XAUTH) && s->up_state == UP_COMMITTED) &&
            !s->pf->may_change(s->pf->ctx, MG_SMP_OP_RESET)) {
            return refused(s);   /* not the reset after this connection's commit */
        }
        cw_t w = rsp_begin(s);
        cw_byte(&w, 0xa0);
        rsp_end(s, &w);
        s->flags |= F_RESET;
    } else if (group == GRP_IMG && id == IMG_STATE) {
        if (op == OP_WRITE) {
            return h_state_write(s, &q);
        }
        return H_STATE;
    } else if (group == GRP_IMG && id == IMG_UPLOAD && op == OP_WRITE) {
        return h_upload(s, &q, f + HDR);
    } else if (group == GRP_IMG && id == IMG_ERASE && op == OP_WRITE && s->pf->may_change &&
               !s->pf->may_change(s->pf->ctx, MG_SMP_OP_ERASE)) {
        return refused(s);
    } else {
        return -MG_SMP_ENOTSUP;
    }
    return H_DONE;
}

/* A backend call that returned MG_SMP_PENDING has completed. */
static int resume_pending(mg_smp_t *s, int rc)
{
    uint8_t op = s->pend_op;
    s->pend_op = P_NONE;
    if (rc < 0) {
        rc = MG_SMP_EUNKNOWN;
    }
    switch (op) {
    case P_WRITE:
        if (s->flags & F_ABORT) {
            s->flags &= (uint16_t)~F_ABORT;
            rc = MG_SMP_EBUSY;
        }
        if (rc != MG_SMP_EOK) {
            up_fail(s, rc);
            return -(rc);
        }
        return up_chunk_continue(s);
    case P_FINISH:
        if (rc != MG_SMP_EOK) {
            up_fail(s, rc);
            return -(rc);
        }
        return commit_continue(s);
    case P_COMMIT:
        if (rc != MG_SMP_EOK) {
            up_fail(s, rc);
            return -(rc);
        }
        s->up_state = UP_COMMITTED;
        s->commit_ms = s->pf->now_ms ? s->pf->now_ms(s->pf->ctx) : 0;
        return H_STATE;
#if MG_SMP_REVERT
    case P_CONFIRM:
    case P_LATCH:
        if (rc == MG_SMP_EOK) {
            confirm_record(s);
        }
        if (op == P_CONFIRM) {
            return rc != MG_SMP_EOK ? -rc : H_STATE;
        }
        return H_DROP;   /* the latch: no request behind it */
#endif
    default:
        return H_DONE;
    }
}

/* Sends what is left of the response. true = all out (or dropped). */
static bool tx_continue(mg_smp_t *s)
{
    uint16_t mtu = s->pf->mtu ? s->pf->mtu(s->pf->ctx) : 23;
    uint16_t frag = att_value_max(mtu);
    const uint8_t *b = slot(s, s->rx_head);
    while (s->tx_off < s->tx_len) {
        uint16_t n = (uint16_t)(s->tx_len - s->tx_off);
        if (n > frag) {
            n = frag;
        }
        int rc = (s->flags & F_MUTE) ? 0 : s->pf->send(s->pf->ctx, b + s->tx_off, n);
        if (rc == MG_SMP_EAGAIN) {
            return false;
        }
        if (rc != 0) {
            break;   /* link gone: the rest is dropped */
        }
        s->tx_off = (uint16_t)(s->tx_off + n);
    }
    return true;
}

static void pop_head(mg_smp_t *s)
{
    s->rx_len[s->rx_head] = 0;
    s->rx_head = (uint8_t)((s->rx_head + 1) % s->cfg->rx_slots);
    s->rx_count--;
    s->flags &= (uint16_t)~(F_TX | F_MUTE);
}

/* The response a handler asked for, built with the handler's frames gone. */
static void respond(mg_smp_t *s, int h)
{
    if (h == H_STATE) {
        rsp_state(s);
    } else if (h < 0) {
        rsp_rc(s, -h);
    } else if (h == H_DROP) {
        pop_head(s);   /* no response */
    }
}

static void run(mg_smp_t *s)
{
    if (s->flags & F_RUN) {
        return;
    }
    s->flags |= F_RUN;
    for (;;) {
        if (s->flags & F_TX) {
            if (!tx_continue(s)) {
                break;
            }
            bool reset = (s->flags & F_RESET) != 0;
            pop_head(s);
            if (reset) {
                s->flags &= (uint16_t)~F_RESET;
                s->flags |= F_RESET_DONE;
                logs(s, "smp: reset requested");
                s->pf->reset(s->pf->ctx);
            }
            continue;
        }
        if (s->flags & F_PEND) {
            if (!s->done) {
                break;
            }
            s->done = 0;
            s->flags &= (uint16_t)~F_PEND;
            int rc = s->done_rc;
            bool latch = s->pend_op == P_LATCH;
            int h = resume_pending(s, rc);
            if (h == H_PEND) {
                s->flags |= F_PEND;
                if (s->done) {
                    continue;
                }
                break;
            }
            if (latch) {
                continue;
            }
            respond(s, h);
            continue;
        }
        if (s->rx_count == 0) {
            break;
        }
        int h = handle(s);
        if (h == H_HOLD) {
            break;
        }
        if (h == H_PEND) {
            s->flags |= F_PEND;
            continue;   /* done may already be set (a synchronous callback) */
        }
        respond(s, h);
    }
    /* A commit no OS reset followed. */
    if (s->up_state == UP_COMMITTED && s->cfg->reset_after_commit_ms && s->pf->now_ms &&
        !(s->flags & (F_RESET_DONE | F_RESET | F_TX)) &&
        (uint32_t)(s->pf->now_ms(s->pf->ctx) - s->commit_ms) >= s->cfg->reset_after_commit_ms) {
        s->flags |= F_RESET_DONE;
        logs(s, "smp: no reset after the commit, resetting");
        s->pf->reset(s->pf->ctx);
    }
    s->flags &= (uint16_t)~F_RUN;
}

/* ---- receive ------------------------------------------------------------------------------ */

/* A request header? 0 = no, 1 = yes, 2 = yes but larger than a slot. */
static int plausible(const mg_smp_config_t *cfg, const uint8_t *h)
{
    uint8_t op = h[0] & 0x07;
    unsigned len = (unsigned)(h[2] << 8 | h[3]);
    if ((h[0] & 0xe0) != 0 || ((h[0] >> 3) & 3) > 1 || (op != OP_READ && op != OP_WRITE) || h[4] != 0) {
        return 0;
    }
    return len <= cfg->slot_size - HDR ? 1 : 2;
}

/* Takes bytes into the slot being filled; returns how many it consumed, or
 * n when the rest of the write is dropped. *done = a request completed. */
static NOINLINE uint16_t rx_take(mg_smp_t *s, const uint8_t *p, uint16_t n, bool *done)
{
    uint8_t fi = (uint8_t)((s->rx_head + s->rx_count) % s->cfg->rx_slots);
    uint8_t *b = slot(s, fi);
    unsigned have = s->rx_len[fi], k, used = 0;
    *done = false;
    if (have < HDR) {
        k = HDR - have;
        if (k > n) {
            k = n;
        }
        memcpy(b + have, p, k);
        have += k;
        used = k;
        if (have == HDR) {
            int pl = plausible(s->cfg, b);
            if (pl == 0) {
                s->rx_len[fi] = 0;
                s->rx_drop++;
                return n;   /* not a request start: drop the rest of this write */
            }
            if (pl == 2) {
                /* larger than buf_size: answer EMSGSIZE from the header alone and
                 * drop the rest of this write (requests start on a write) */
                b[1] = 0x80;
                b[2] = b[3] = 0;
                s->rx_len[fi] = HDR;
                s->rx_count++;
                *done = true;
                return n;
            }
        }
        s->rx_len[fi] = (uint16_t)have;
        if (have < HDR) {
            return (uint16_t)used;
        }
    }
    unsigned total = HDR + (unsigned)(b[2] << 8 | b[3]);
    k = total - have;
    if (k > n - used) {
        k = n - used;
    }
    memcpy(b + have, p + used, k);
    have += k;
    used += k;
    s->rx_len[fi] = (uint16_t)have;
    if (have == total) {
        s->rx_count++;
        *done = true;
    }
    return (uint16_t)used;
}

/* ---- memory ------------------------------------------------------------------------------ */

static uint32_t align8(uint32_t n) { return (n + 7u) & ~7u; }
static uint32_t state_size(const mg_smp_env_t *e) { return align8(sizeof(mg_smp_t)) + align8(e->cfg->be_scratch); }
static uint32_t bufs_size(const mg_smp_env_t *e) { return (uint32_t)e->cfg->rx_slots * e->cfg->slot_size; }

int mg_smp_check(const mg_smp_env_t *env)
{
#if MG_SMP_TRUSTED_CFG
    return env ? MG_SMP_EOK : MG_SMP_EINVAL;
#else
    const mg_smp_config_t *cfg = env ? env->cfg : 0;
    const mg_smp_platform_t *pf = env ? env->pf : 0;
    const mg_smp_backend_t *be = env ? env->be : 0;
    if (!cfg || !pf || !be || cfg->slot_size < MG_SMP_MIN_SLOT || !cfg->rx_slots || cfg->rx_slots > MG_SMP_MAX_SLOTS ||
        cfg->buf_size > cfg->slot_size || (cfg->buf_size && cfg->buf_size < 64) || (cfg->max_writes && !pf->mtu) ||
        !pf->send || !pf->mem_get ||
        !pf->mem_put || !pf->record_get || !pf->record_put || !pf->running_id || !pf->reset || !be->begin ||
        !be->write || !be->finish || !be->commit || !be->abort ||
        ((be->flags & MG_SMP_BE_CAN_REVERT) && (!MG_SMP_REVERT || !pf->boot_healthy || !be->confirm)) ||
        (!MG_SMP_REVERT && be->slot_info)) {
        return MG_SMP_EINVAL;
    }
    return MG_SMP_EOK;
#endif
}

/* A commit made this boot waits for the reset, with nothing allocated: the
 * record written at commit tells what it was. */
static void restore_commit(mg_smp_t *s)
{
    rec_t *r = (rec_t *)(void *)s->sha.rec;
    rec_load(s, r);
    s->up_state = UP_COMMITTED;
    s->flags |= F_CLAIM | F_RESET_DONE;   /* the claim was kept; a timed reset already ran or isn't set */
    if (r->cur.state == RS_PENDING) {
        s->up_id = r->cur.id;
        memcpy(s->hdr_or_hash, r->cur.hash, 32);
        s->img_size = r->cur.payload_len;
        if (r->cur.rsv[0] & REC_PERMANENT) {
            s->flags |= F_PERM;
        }
    }
}

static mg_smp_t *state_get(const mg_smp_env_t *env)
{
    mg_smp_t *s = cur();
    if (s) {
        return s;
    }
    if (mg_smp_check(env) != MG_SMP_EOK) {
        return 0;
    }
    s = (mg_smp_t *)env->pf->mem_get(env->pf->ctx, MG_SMP_MEM_STATE, state_size(env));
    if (!s) {
        return 0;
    }
    memset(s, 0, state_size(env));
    s->env = env;
    s->cfg = env->cfg;
    s->pf = env->pf;
    s->be = env->be;
    if (IS_MARK(g_smp)) {
        restore_commit(s);
        if (g_smp == COMMIT_MARK_AUTH) {
            s->flags |= F_XAUTH;
        }
    }
    g_smp = s;
    return s;
}

static bool bufs_get(mg_smp_t *s)
{
    if (!s->buf) {
        s->buf = (uint8_t *)s->pf->mem_get(s->pf->ctx, MG_SMP_MEM_BUFFERS, bufs_size(s->env));
    }
    return s->buf != 0;
}

static void bufs_put(mg_smp_t *s)
{
    if (s->buf) {
        uint8_t *b = s->buf;
        s->buf = 0;
        s->pf->mem_put(s->pf->ctx, MG_SMP_MEM_BUFFERS, b);
    }
    for (uint8_t i = 0; i < MG_SMP_MAX_SLOTS; i++) {
        s->rx_len[i] = 0;
    }
    s->rx_count = 0;
    s->rx_head = 0;
    s->tx_len = s->tx_off = 0;
}

static void state_put(mg_smp_t *s)
{
    const mg_smp_platform_t *pf = s->pf;
    bufs_put(s);
    g_smp = s->up_state != UP_COMMITTED ? 0 : (s->flags & F_XAUTH) ? COMMIT_MARK_AUTH : COMMIT_MARK;
    pf->mem_put(pf->ctx, MG_SMP_MEM_STATE, s);
}

/* Gives memory back when nothing needs it. s may be gone afterwards. */
static void settle(mg_smp_t *s)
{
    if ((s->flags & (F_PEND | F_TX | F_RUN | F_RESET)) || s->rx_count) {
        return;
    }
    for (uint8_t i = 0; i < s->cfg->rx_slots; i++) {
        if (s->rx_len[i]) {
            if (s->flags & F_LINKDOWN) {
                s->rx_len[i] = 0;   /* half a request from a link that is gone */
            } else {
                return;   /* a request is arriving */
            }
        }
    }
    if (s->up_state == UP_RECV || s->up_state == UP_STAGED) {
        if (!(s->flags & F_HOLD)) {
            return;   /* the upload goes on */
        }
        bufs_put(s);   /* waiting for its resume (other requests don't stop the clock): only the state */
        if (s->cfg->resume_hold_s && s->pf->now_ms &&
            (int32_t)(s->pf->now_ms(s->pf->ctx) - s->drop_ms) < (int32_t)((uint32_t)s->cfg->resume_hold_s * 1000u)) {
            return;
        }
        logs(s, "smp: interrupted upload not resumed, dropped");
        up_fail(s, MG_SMP_EBUSY);
    } else if (s->up_state == UP_COMMITTED && s->cfg->reset_after_commit_ms && s->pf->now_ms &&
               !(s->flags & F_RESET_DONE)) {
        return;   /* the timed reset needs the state */
    }
    state_put(s);
}

/* No memory for a write: answer a request that starts in it with rc 2 from
 * the stack, so the client fails at once instead of timing out. */
static void no_memory(const mg_smp_env_t *env, const uint8_t *p, uint16_t n)
{
    uint8_t r[HDR + 5];
    if (mg_smp_check(env) != MG_SMP_EOK || n < HDR || !plausible(env->cfg, p)) {
        return;
    }
    r[0] = (uint8_t)(p[0] + 1);
    r[1] = 0;
    r[2] = 0;
    r[3] = 5;
    r[4] = p[4];
    r[5] = p[5];
    r[6] = p[6];
    r[7] = p[7];
    r[8] = 0xa1;   /* {"rc": 2} */
    r[9] = 0x62;
    r[10] = 'r';
    r[11] = 'c';
    r[12] = MG_SMP_ENOMEM;
#if MG_SMP_LOGS
    if (env->pf->log) {
        env->pf->log(env->pf->ctx, "smp: no memory for a request");
    }
#endif
    (void)env->pf->send(env->pf->ctx, r, (uint16_t)sizeof(r));
}

/* ---- public ------------------------------------------------------------------------------- */

void mg_smp_rx(const mg_smp_env_t *env, const uint8_t *p, uint16_t n)
{
    mg_smp_t *s;
    if (!n) {
        return;
    }
    s = state_get(env);
    if (!s || !bufs_get(s)) {
        no_memory(env, p, n);
        if (s) {
            settle(s);
        }
        return;
    }
    s->flags &= (uint16_t)~F_LINKDOWN;
#if MG_SMP_RX_GAP_MS
    if (s->pf->now_ms && s->rx_count < s->cfg->rx_slots) {
        uint8_t fi = (uint8_t)((s->rx_head + s->rx_count) % s->cfg->rx_slots);
        uint32_t now = s->pf->now_ms(s->pf->ctx);
        if (s->rx_len[fi] && !(s->flags & F_PEND) && (int32_t)(now - s->rx_t) >= MG_SMP_RX_GAP_MS) {
            s->rx_len[fi] = 0;   /* a stale partial request: a write was lost */
            s->rx_drop++;
        }
        s->rx_t = now;
    }
#endif
    while (n) {
        bool done;
        if (s->rx_count >= s->cfg->rx_slots) {
            s->rx_drop++;   /* every slot holds a request: should not happen with a truthful buf_count */
            break;
        }
        uint16_t k = rx_take(s, p, n, &done);
        p += k;
        n = (uint16_t)(n - k);
        if (done && s->cfg->inline_rx) {
            run(s);   /* frees the slot unless the request waits */
        }
    }
    if (!s->cfg->inline_rx && s->rx_count && s->pf->wake) {
        s->pf->wake(s->pf->ctx);
    }
    settle(s);
}

void mg_smp_poll(const mg_smp_env_t *env)
{
    mg_smp_t *s = cur();
    (void)env;
    if (s) {
        run(s);
        settle(s);
    }
}

int mg_smp_healthy(const mg_smp_env_t *env)
{
#if !MG_SMP_REVERT
    (void)env;
    return MG_SMP_EOK;
#else
    mg_smp_t *s;
    bool confirmed;
    int rc = MG_SMP_EOK;
    if (!env || !env->be || !(env->be->flags & MG_SMP_BE_CAN_REVERT)) {
        return MG_SMP_EOK;
    }
    s = state_get(env);
    if (!s) {
        return MG_SMP_ENOMEM;
    }
    if (s->flags & (F_PEND | F_RUN)) {
        return MG_SMP_EBUSY;   /* a backend call is in flight: try again */
    }
    if (active_is(s, 0, &confirmed) && !confirmed) {
        logs(s, "smp: healthy: confirming the test image");
        int h = confirm_start(s, P_LATCH);
        if (h == H_PEND) {
            s->flags |= F_PEND;
            rc = MG_SMP_PENDING;
        } else if (h < 0) {
            rc = -h;   /* the backend's confirm failed: the image stays unconfirmed */
        }
    }
    settle(s);
    return rc;
#endif
}

void mg_smp_backend_done(int rc)
{
    mg_smp_t *s = cur();
    if (!s) {
        return;
    }
    s->done_rc = (int8_t)(rc > 127 ? MG_SMP_EUNKNOWN : rc);
    s->done = 1;
    if (s->pf->wake) {
        s->pf->wake(s->pf->ctx);
    }
}

void mg_smp_disconnect(const mg_smp_env_t *env)
{
    mg_smp_t *s = cur();
    (void)env;
    if (g_smp == COMMIT_MARK_AUTH) {
        g_smp = COMMIT_MARK;   /* the connection that was allowed is gone */
    }
    if (!s) {
        return;
    }
    s->flags |= F_LINKDOWN | F_HOLD;
    s->flags &= (uint16_t)~F_XAUTH;
    s->drop_ms = s->pf->now_ms ? s->pf->now_ms(s->pf->ctx) : 0;
    if ((s->flags & F_PEND) && s->pend_op != P_LATCH && s->rx_count) {
        /* the request behind the pending call finishes without a response;
         * every other slot (queued requests, a partly received one) goes */
        s->flags |= F_MUTE;
        for (uint8_t i = 0; i < s->cfg->rx_slots; i++) {
            if (i != s->rx_head) {
                s->rx_len[i] = 0;
            }
        }
        s->rx_count = 1;
        return;
    }
    for (uint8_t i = 0; i < s->cfg->rx_slots; i++) {
        s->rx_len[i] = 0;
    }
    s->rx_count = 0;
    s->rx_head = 0;
    s->tx_len = s->tx_off = 0;
    if (s->flags & F_RESET) {
        /* the reset response may not have gone out, but the client asked */
        s->flags |= F_RESET_DONE;
        s->pf->reset(s->pf->ctx);
    }
    s->flags &= (uint16_t)~(F_TX | F_MUTE | F_RESET);
    settle(s);
}

void mg_smp_abort(const mg_smp_env_t *env)
{
    mg_smp_t *s = cur();
    (void)env;
    if (!s || (s->up_state != UP_RECV && s->up_state != UP_STAGED)) {
        return;
    }
    if ((s->flags & F_PEND) && s->pend_op == P_WRITE) {
        s->flags |= F_ABORT;   /* after the write in flight */
        return;
    }
    if (s->flags & F_PEND) {
        return;   /* finish/commit under way: the commit decides */
    }
    up_fail(s, MG_SMP_EBUSY);
    settle(s);
}

bool mg_smp_upload_active(void)
{
    mg_smp_t *g = g_smp;
    if (IS_MARK(g)) {
        return true;
    }
    return g && (g->up_state == UP_RECV || g->up_state == UP_STAGED || g->up_state == UP_COMMITTED);
}

bool mg_smp_idle(void)
{
    mg_smp_t *g = g_smp;
    return !g || IS_MARK(g);
}

uint32_t mg_smp_held(const mg_smp_env_t *env)
{
    mg_smp_t *s = cur();
    (void)env;
    if (!s) {
        return 0;
    }
    return state_size(s->env) + (s->buf ? bufs_size(s->env) : 0);
}

void *mg_smp_scratch(void)
{
    mg_smp_t *s = cur();
    if (!s || !s->cfg->be_scratch) {
        return 0;
    }
    return (uint8_t *)s + align8(sizeof(mg_smp_t));
}

bool mg_smp_staged(mg_smp_image_id_t *id, uint8_t hash[32])
{
    mg_smp_t *s = cur();
    if (!s || (s->up_state != UP_STAGED && s->up_state != UP_COMMITTED)) {
        return false;
    }
    if (id) {
        *id = s->up_id;
    }
    if (hash) {
        memcpy(hash, s->hdr_or_hash, 32);
    }
    return true;
}

#ifdef MG_SMP_TEST_HOOKS
void mg_smp_test_reboot(void) { g_smp = 0; }
#endif
