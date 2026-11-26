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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_smp_backend.h"
#include "muse_local_press.h"

/*
 * The SMP request front (protocols/mgcommands.h, Token proof; test-vectors/mg-update-v2.json): on a link that needs
 * the confirm press (mg_smp_gate.h), every update change is asked about whole before the SMP core sees it. The
 * vendored core (smp/) asks its may_change/may_upload hooks only at a transfer's start, with the image's length and
 * SHA-256, so it can't refuse a write past the bound length, a last chunk that isn't the bound image, or a change a
 * press didn't cover. The front reassembles each request from the link's writes, reads it, keeps the slot's running
 * SHA-256, asks muse_local_press, and either answers rc 10 or 11 itself (the core never sees the request) or hands
 * the core the whole request, whose hooks then say yes. Pure: mg_ble.c and tests/mg_smp_esp_server.c run it.
 */

/* The largest request the front holds whole (the core's slot); a larger one passes to the core unread (EMSGSIZE). */
#define MG_SMP_FRONT_MAX 1024
/* A partial request older than this is dropped (a write was lost), as the core does. */
#define MG_SMP_FRONT_GAP_MS 2000

/* One request in reassembly (in mg_ble.c's FIFO block: there only while a link writes). */
typedef struct {
    uint16_t have;       /* bytes held */
    uint16_t want;       /* the request's whole length, once its header is in */
    uint32_t skip;       /* bytes of an oversize request still to pass through */
    uint32_t at_ms;      /* the last write */
    uint8_t buf[MG_SMP_FRONT_MAX];
} mg_smp_front_rx_t;

/* What one write makes: whole requests, and the header alone of one too large to hold (MG_SMP_FRONT_MAX: the core's
 * slot, so the core answers it EMSGSIZE from that header); the rest of such a request is dropped, never passed on. */
typedef struct {
    void (*whole)(void *ctx, const uint8_t *req, size_t n);
    void (*raw)(void *ctx, const uint8_t *p, size_t n);
    void *ctx;
} mg_smp_front_out_t;

void mg_smp_front_rx_reset(mg_smp_front_rx_t *rx);
void mg_smp_front_rx(mg_smp_front_rx_t *rx, const uint8_t *p, size_t n, uint32_t now_ms, const mg_smp_front_out_t *out);

/* The slot an upload on the press's path writes, hashed as it goes (allocated while it's needed). */
typedef struct {
    bool on;           /* tracking an upload */
    bool matched;      /* all len bytes are in and hash to sha */
    uint32_t len;      /* the upload's length */
    uint32_t next;     /* the next offset it takes */
    uint8_t sha[32];   /* the whole-image SHA-256 its first chunk named */
    uint64_t h[(MG_SMP_SHA256_CTX_SIZE + 7) / 8];
} mg_smp_front_slot_t;

typedef enum {
    MG_SMP_FRONT_NOT_CHANGE = 0,   /* a read, or anything but an update change: the core's */
    MG_SMP_FRONT_CHANGE,           /* an update change, read into req */
    MG_SMP_FRONT_UNREADABLE,       /* an update change the front can't read: refused (rc 11) */
} mg_smp_front_kind_t;

/* Reads one whole request: an image upload (its off, len, sha and data), an image state write, an erase or the
 * reset (their key: SHA-256 of group, command and body). data/data_n: an upload's bytes. */
mg_smp_front_kind_t mg_smp_front_read(const uint8_t *req, size_t n, muse_update_req_t *out, const uint8_t **data);

/* The upload the core is taking, as far as the front saw it: the length its first chunk named (heap, while an update
 * runs). */
typedef struct {
    bool known;
    uint32_t len;
} mg_smp_front_up_t;

/* An image upload write that runs past its upload's length (smp-v12's rule on a gated platform, whatever opened the
 * link: the press, a matched proof, the wired arm, a bench build): an offset-0 request whose data is longer than its own
 * len, or a later chunk whose off + its data's length is past the len up holds while the core holds that upload
 * (core_holds: mg_smp_upload_active; up NULL: nothing known). True: answer rc 11 and end the upload (mg_smp_abort);
 * the core never sees it (smp-v11 drops the extra bytes and answers rc 0). */
bool mg_smp_front_past_len(const uint8_t *req, size_t n, const mg_smp_front_up_t *up, bool core_holds);
/* The shortest upload the core takes: an MCUboot header and a TLV area (smp/mg_smp.c h_upload: 32 + 40). */
#define MG_SMP_FRONT_MIN_UPLOAD (32u + 40u)
/* A request the core takes: an upload's first chunk sets the length the later ones are checked against (up NULL:
 * nowhere to keep it). True: it is an upload's first chunk the core takes as one (len at least MG_SMP_FRONT_MIN_UPLOAD,
 * a SHA-256 of 32 bytes or none); one it refuses leaves the length as it was. */
bool mg_smp_front_up_note(const uint8_t *req, size_t n, mg_smp_front_up_t *up);

/*
 * One whole request from a link that needs the press: -1, not an update change (to the core); 0, allowed (to the
 * core, whose hooks say yes); 10 or 11, the rc to answer (mg_smp_front_refusal) with the core never seeing it.
 * slot: the tracker (NULL: none, so no last chunk or state write can be the bound image's); core_holds: the core
 * holds an upload (mg_smp_upload_active: an upload's first chunk again resumes it there, so the slot keeps its bytes).
 * past_len: mg_smp_front_past_len said so: what the press core lets through is refused (11) all the same.
 * ask/bound: muse_local_press_ask and muse_local_press_bound, or the pure core's. *ended: the press's binding ended
 * here (the caller drops the core's transfer: mg_smp_abort).
 */
int mg_smp_front_gate(const uint8_t *req, size_t n, mg_smp_front_slot_t *slot, bool core_holds, bool past_len,
                      muse_update_answer_t (*ask)(void *ctx, const muse_update_req_t *r),
                      bool (*bound)(void *ctx, muse_update_ident_t *id), void *ctx, bool *ended);

/* How deep a request's CBOR may nest: containers (arrays, maps, tags, chunked strings) inside one another, the body's
 * own map counting as one. Every request, proven or not, update or not, is checked before the core decodes it. */
#define MG_SMP_FRONT_MAX_DEPTH 8

/* Whether a whole request's body is one well-formed CBOR item nesting no deeper than MG_SMP_FRONT_MAX_DEPTH, walked
 * iteratively (no recursion, a fixed stack, whatever the input: a ~500-deep array never reaches a decoder). It fails
 * closed: a malformed or cut-short item, a length or count not wholly inside the request, any length, count or tag
 * past 32 bits (which a 32-bit decoder could truncate), or bytes after the item are refused too. False: answer
 * MG_SMP_EINVAL. */
bool mg_smp_front_depth_ok(const uint8_t *req, size_t n);

/* The answer to a refused request: its header turned into a response, {"rc": rc}. Returns its length. */
size_t mg_smp_front_refusal(const uint8_t *req, int rc, uint8_t out[13]);

/* The slot tracker's parts (pure; tests). */
bool mg_smp_slot_would_match(const mg_smp_front_slot_t *s, const muse_update_req_t *r, const uint8_t *data,
                             const muse_update_ident_t *bound, bool core_holds);
void mg_smp_slot_apply(mg_smp_front_slot_t *s, const muse_update_req_t *r, const uint8_t *data, bool core_holds);
