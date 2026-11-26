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
 * mg_smp.h: the MCUmgr SMP server core for every Muse Gadget.
 * Backends and platform hooks: mg_smp_backend.h. Integration: VENDORED.md.
 *
 * Serves exactly what the Muse apps' mcumgr client uses (Android and iOS),
 * plus echo for tooling:
 *   OS    0 echo, 5 reset, 6 mcumgr_params
 *   image 0 state read/write (test, confirm), 1 upload (off/len/sha/data,
 *         pipelined, resume by reported offset)
 * The upload is an MCUboot envelope (header, vendor payload, TLVs with the
 * SHA-256 of header + payload + protected TLVs). The core strips the header,
 * streams the payload to the backend, hashes as it goes, refuses the last
 * chunk if the hash (or the upload's whole-file `sha`) does not match, and
 * only then lets the backend verify and commit, at the TEST state write.
 *
 * Threading: call every mg_smp_* function from ONE context (the BLE host
 * task, or a worker the platform forwards ATT writes to), except
 * mg_smp_backend_done(), which may be called from any task or callback.
 * One instance per firmware.
 */
#ifndef MG_SMP_H
#define MG_SMP_H

#include "mg_smp_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time options (define before including, or with -D). */
#ifndef MG_SMP_MAX_SLOTS
#define MG_SMP_MAX_SLOTS 4      /* upper bound for cfg->rx_slots */
#endif
#ifndef MG_SMP_FILE_SHA
#define MG_SMP_FILE_SHA 1       /* also check the upload's `sha` (whole file):
                                   costs a second SHA-256 context in RAM. 0 on the
                                   tightest chips: the image hash still covers
                                   every staged byte. */
#endif
#ifndef MG_SMP_ECHO
#define MG_SMP_ECHO 1           /* OS echo (tooling only; the apps never send it) */
#endif

#ifndef MG_SMP_RX_GAP_MS
#define MG_SMP_RX_GAP_MS 2000   /* a partly received request older than this is
                                   dropped when the next write arrives (a lost
                                   write without response: iOS never checks
                                   canSendWriteWithoutResponse). Needs now_ms;
                                   0 = off. Writes of one request come back to
                                   back, well inside this. */
#endif

#ifndef MG_SMP_REVERT
#define MG_SMP_REVERT 1         /* 0: the build never has a revert-capable or
                                   slot_info backend: the
                                   confirm, healthy-mark and slot_info code is
                                   compiled out (mg_smp_healthy is a no-op) */
#endif
#ifndef MG_SMP_LOGS
#define MG_SMP_LOGS 1           /* 0: no log lines (and none of their strings);
                                   pf->log is never called */
#endif
#ifndef MG_SMP_TRUSTED_CFG
#define MG_SMP_TRUSTED_CFG 0    /* 1: mg_smp_check only rejects a NULL env: for a
                                   port whose const config, platform and backend
                                   tables are covered by its own host tests */
#endif

#ifndef MG_SMP_MAX_REPORTED_BUF
#define MG_SMP_MAX_REPORTED_BUF 512   /* buf_size reported never exceeds this: the
                                         Android client sizes each request to
                                         buf_size and writes MTU-3 bytes at a time,
                                         and Android refuses any write over 512
                                         (the ATT attribute limit), so at MTU 517 a
                                         larger buf_size fails at once */
#endif

#define MG_SMP_MIN_REPORTED_BUF 64   /* floor of a max_writes-sized buf_size: a first
                                        upload chunk still carries data */

#define MG_SMP_MIN_SLOT 256     /* an image state response with two slots fits */
#define MG_SMP_REC_SIZE 112     /* the active-hash record (record_get/put key 0) */

typedef struct {
    uint16_t slot_size;     /* >= MG_SMP_MIN_SLOT: the largest request held, and
                               room for the image state response */
    uint16_t buf_size;      /* reported as buf_size (the clients size every
                               request to it); 0 = slot_size; always clamped
                               to MG_SMP_MAX_REPORTED_BUF (512). Set it to the
                               link's MTU-3 or less (<= slot_size) to keep
                               each request in one ATT write. */
    uint8_t rx_slots;       /* 1..MG_SMP_MAX_SLOTS requests held at once */
    uint8_t buf_count;      /* reported as buf_count (Android's window =
                               min(3, buf_count)); 0 = rx_slots. Report more
                               than rx_slots only when processing is inline
                               and the BLE stack holds the next writes while
                               a request is processed (link-layer flow
                               control), so nothing is ever dropped. */
    uint8_t max_writes;     /* 0 = off. Else the ATT writes the transport holds
                               in flight (e.g. a 4-entry vendor receive queue
                               that drops when full): the reported buf_size
                               becomes min(buf_size, (max_writes / window) x
                               min(MTU-3, 512)) at the live MTU, window =
                               the clients' window from buf_count (Android
                               min(3, buf_count), iOS 2), floor
                               MG_SMP_MIN_REPORTED_BUF. Needs pf->mtu. */
    uint8_t inline_rx;      /* 1 = mg_smp_rx() processes a complete request
                               at once (minimum footprint: one slot, inline,
                               synchronous backend). 0 = it queues and calls
                               wake(); processing runs in mg_smp_poll(). */
    uint16_t reset_after_commit_ms;   /* 0 = reset only on OS reset; else the
                               core calls platform reset() this long after a
                               commit that no OS reset followed (needs now_ms) */
    uint16_t resume_hold_s; /* after a link drop mid-upload, keep the upload's
                               state (not its buffers) this long for a resume,
                               then abort it and release (needs now_ms; 0 =
                               release at the drop). Fleet value: 120. */
    uint16_t be_scratch;    /* bytes of backend scratch allocated with the
                               state (mg_smp_scratch()), 0 = none */
} mg_smp_config_t;

/* The core's state: private, allocated through mem_get only while needed. */
typedef struct mg_smp mg_smp_t;

/* Everything the core needs, in one table the platform keeps in flash (const)
 * and passes to every call. */
typedef struct mg_smp_env {
    const mg_smp_config_t *cfg;
    const mg_smp_platform_t *pf;
    const mg_smp_backend_t *be;
} mg_smp_env_t;

/*
 * Memory (no base memory): the core's only static RAM is one pointer. The first SMP
 * write allocates the state (mem_get MG_SMP_MEM_STATE) and the request
 * buffers (MG_SMP_MEM_BUFFERS). Both go back (mem_put) as soon as the core is
 * idle with no upload in progress: after a plain request (params, image
 * state, echo) at once; during an upload when it ends (refused, aborted,
 * committed, or after the reset). A link drop mid-upload releases the buffers
 * and keeps only the state for cfg.resume_hold_s. A commit waiting for its
 * reset is remembered in the pointer itself, with nothing allocated.
 */

/* Checks the env (sizes, mandatory hooks): MG_SMP_EOK or MG_SMP_EINVAL. No
 * memory, no I/O. Optional: every allocation checks it too. */
int mg_smp_check(const mg_smp_env_t *env);

/* One ATT write on the SMP characteristic: a whole request, part of one, or
 * (on stream transports) several. */
void mg_smp_rx(const mg_smp_env_t *env, const uint8_t *data, uint16_t n);

/* Call from the main loop and after wake(): returns at once while the core
 * holds nothing; otherwise runs queued requests, retried sends, completed
 * backend calls, the timed reset and the end of a resume hold. */
void mg_smp_poll(const mg_smp_env_t *env);

/* Call once at the platform's healthy mark (revert-capable backends; a no-op
 * otherwise): a running test image confirms itself. Holds memory only for
 * the check. MG_SMP_ENOMEM or MG_SMP_EBUSY: call again later. Otherwise the
 * backend confirm's answer: 0 (or nothing to confirm), MG_SMP_PENDING (it
 * completes through mg_smp_backend_done()), or its failure: the image stays a
 * test image and reverts at the next reset unless confirmed later. */
int mg_smp_healthy(const mg_smp_env_t *env);

/* Completes the backend call that returned MG_SMP_PENDING. Any context: it
 * only records rc and calls wake(); the work continues in mg_smp_poll(). */
void mg_smp_backend_done(int rc);

/* The link dropped: a partial request and an unsent response are dropped;
 * an upload in progress keeps its state for cfg.resume_hold_s. Only its own
 * resume (a chunk at the core's offset, or off 0 with the same len + sha)
 * stops that clock; other requests on a new link don't. */
void mg_smp_disconnect(const mg_smp_env_t *env);

/* Another front end takes the OTA engine: abort an uncommitted SMP upload
 * (backend abort, claim released, memory released). No effect after a commit. */
void mg_smp_abort(const mg_smp_env_t *env);

/* An SMP upload owns the engine (receiving, staged, held for a resume, or
 * committed and waiting for the reset). */
bool mg_smp_upload_active(void);

/* The core holds no memory. */
bool mg_smp_idle(void);

/* Bytes the core holds right now (state + buffers). */
uint32_t mg_smp_held(const mg_smp_env_t *env);

/* The backend's scratch (cfg.be_scratch bytes, 8-aligned, zeroed at the first
 * allocation), valid from begin() until abort() or commit() returns; NULL
 * without one. */
void *mg_smp_scratch(void);

/* The staged upload (whole file in and hash-checked, or committed): its id
 * and envelope hash, for a backend that fills slot 1 in slot_info. false =
 * none. hash may be NULL. */
bool mg_smp_staged(mg_smp_image_id_t *id, uint8_t hash[32]);

#ifdef MG_SMP_TEST_HOOKS
/* Host harnesses only: forget everything, as a reboot does (the harness frees
 * the blocks it handed out). */
void mg_smp_test_reboot(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MG_SMP_H */
