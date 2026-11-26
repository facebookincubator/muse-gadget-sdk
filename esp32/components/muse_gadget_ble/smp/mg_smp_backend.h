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
 * mg_smp_backend.h: what every Muse Gadget family implements under the
 * MCUmgr SMP core.
 *
 * Two tables, both const and owned by the platform:
 *   mg_smp_backend_t   the chip's update engine (stage, verify, commit, confirm)
 *   mg_smp_platform_t  the device around it (GATT send, memory, record store,
 *                      reset, OTA ownership and media stop, healthy mark)
 * The core (mg_smp.h) owns everything else: SMP framing and reassembly, CBOR,
 * the OS and image groups, the MCUboot envelope, the SHA-256 checks, resume,
 * the active-hash record and image state.
 *
 * Memory (update support takes no base memory): the core's only
 * static RAM is one pointer. Its state and request buffers come from the
 * platform's mem_get() at the first SMP write and go back through mem_put()
 * as soon as no update is in progress (see mg_smp.h). Pure C99, no stdio, no
 * atomics, no constructors, no allocation of its own.
 *
 * API version 3: v2 (v1 plus mem_get/mem_put; the polled healthy mark
 * became the mg_smp_healthy() event) plus the platform's may_change hook.
 */
#ifndef MG_SMP_BACKEND_H
#define MG_SMP_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MG_SMP_API_VERSION 3

/* SMP GATT service and characteristic (write without response + notify). */
#define MG_SMP_SERVICE_UUID "8D53DC1D-1DB7-4CD3-868B-8A527460AA84"
#define MG_SMP_CHAR_UUID "DA2E7828-FBCE-4E01-AE9E-261174997C48"
/* Little-endian byte order, as most stacks declare 128-bit UUIDs. */
#define MG_SMP_SERVICE_UUID_LE_BYTES \
    0x84, 0xAA, 0x60, 0x74, 0x52, 0x8A, 0x8B, 0x86, 0xD3, 0x4C, 0xB7, 0x1D, 0x1D, 0xDC, 0x53, 0x8D
#define MG_SMP_CHAR_UUID_LE_BYTES \
    0x48, 0x7C, 0x99, 0x74, 0x11, 0x26, 0x9E, 0xAE, 0x01, 0x4E, 0xCE, 0xFB, 0x28, 0x78, 0x2E, 0xDA

/*
 * Results. Every hook returns 0 or an MCUmgr MGMT_ERR_* code; the core sends a
 * non-zero code to the client as `rc` (both apps then fail the step). A
 * backend error the core cannot classify should be MG_SMP_EUNKNOWN.
 */
enum {
    MG_SMP_EOK = 0,
    MG_SMP_EUNKNOWN = 1,
    MG_SMP_ENOMEM = 2,      /* payload larger than the staging area */
    MG_SMP_EINVAL = 3,      /* bad request, bad envelope, wrong board in the payload */
    MG_SMP_ETIMEOUT = 4,
    MG_SMP_ENOENT = 5,      /* no image with that hash */
    MG_SMP_EBADSTATE = 6,   /* e.g. a new upload after a commit, before the reset */
    MG_SMP_EMSGSIZE = 7,
    MG_SMP_ENOTSUP = 8,
    MG_SMP_ECORRUPT = 9,    /* hash or vendor verify failed */
    MG_SMP_EBUSY = 10,      /* another front end owns the OTA engine, not healthy yet, or (refusal_rc) the gadget
                               asks for its confirm press: the client retries */
    MG_SMP_EACCESSDENIED = 11, /* a mutating request on a connection the
                                  platform does not authorize (may_change()) */
};
/* Backend write/finish/commit/confirm: "started, I will call
 * mg_smp_backend_done(rc) when it completes" (dual-bank callbacks, an OTA
 * library draining to NAND). The core holds that request's response
 * and every later request, in order, until then. */
#define MG_SMP_PENDING (-1)
/* Platform send(): no room in the stack's TX queue right now. The core keeps
 * the rest of the response and retries from mg_smp_poll(). */
#define MG_SMP_EAGAIN (-2)

/* An MCUboot image version: major.minor.revision+build, as the envelope
 * header carries it and image state reports it. */
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint16_t revision;
    uint32_t build;
} mg_smp_version_t;

/* What identifies a firmware image for the hash promise: its version and a
 * 32-bit build id (a commit or a CRC; 0 = none). The packer stamps both into
 * the envelope (the build id as protected TLV MG_SMP_TLV_BUILD_ID, covered by
 * the image hash); the running firmware reports the same two values. The
 * active slot shows the envelope hash only while both match the record
 * written at commit, so "same version, different build" never reports the
 * new hash. Both must be equal: an envelope without a build id names only a
 * running image that reports build id 0 (a platform with no build ids, where
 * the version alone names an image), never one that reports a build id.
 *
 * Two different images must never share an id. The core refuses (rc 6
 * EBADSTATE, at the last chunk, before finish) an upload with the running
 * image's id whose hash is not the one the running slot reports, unless
 * slot_info gives the running slot its own hash (MG_SMP_SLOT_HAS_HASH). A
 * hash that names the staged upload always tests (or confirms) that upload,
 * never "already running". */
typedef struct {
    mg_smp_version_t version;
    uint32_t build_id;
} mg_smp_image_id_t;

/* Protected TLV, 4 bytes big-endian: what `imgtool sign --custom-tlv 0xb1
 * 0x<8 hex digits>` writes (pack.py --build-id writes the same bytes). */
#define MG_SMP_TLV_BUILD_ID 0x00B1u

/* mem_get/mem_put kinds. */
enum { MG_SMP_MEM_STATE = 0, MG_SMP_MEM_BUFFERS = 1 };

/* One slot, for image state (slot 0 = running, slot 1 = the other/staged). */
enum {
    MG_SMP_SLOT_PRESENT = 0x01,
    MG_SMP_SLOT_ACTIVE = 0x02,
    MG_SMP_SLOT_CONFIRMED = 0x04,
    MG_SMP_SLOT_PENDING = 0x08,
    MG_SMP_SLOT_PERMANENT = 0x10,
    MG_SMP_SLOT_BOOTABLE = 0x20,
    MG_SMP_SLOT_HAS_HASH = 0x40,   /* backend filled `hash` itself */
};
typedef struct {
    mg_smp_image_id_t id;
    uint8_t hash[32];
    uint8_t flags;   /* MG_SMP_SLOT_* */
} mg_smp_slot_t;

/* ------------------------------------------------------------------------ */
/* Backend: the chip's update engine.                                        */
/* ------------------------------------------------------------------------ */

enum {
    /* A tested image boots once and reverts unless confirmed (ESP32 PENDING_
     * VERIFY, Nordic swap, a dual-bank loader). Without it every commit is
     * permanent, image state says so, and confirm is a no-op. */
    MG_SMP_BE_CAN_REVERT = 0x01,
};

typedef struct mg_smp_backend {
    void *ctx;
    uint8_t flags;          /* MG_SMP_BE_* */
    uint16_t max_write;     /* 0 = any size; else write() gets at most this many bytes */
    uint32_t max_payload;   /* staging capacity in bytes; 0 = begin() decides */

    /*
     * Order of calls, per upload (all from the core's processing context:
     * inline in mg_smp_rx() or in mg_smp_poll(), never from an ISR):
     *
     *   begin                 first chunk of a new upload (not on a resume)
     *   write ... write       payload bytes, strictly in order, off = payload
     *                         offset (the envelope header and TLVs are the
     *                         core's), any size up to max_write
     *   finish                at the image state TEST (or permanent) write,
     *                         and ONLY after the whole file arrived and the
     *                         envelope SHA-256 matched its TLV (and the upload's
     *                         `sha`): vendor verify of the staged payload.
     *                         May take seconds (return MG_SMP_PENDING); the
     *                         client allows 30 s for that request. A verify
     *                         of the whole staged image must not run as one
     *                         main-loop pass while media can play:
     *                         run it in steps or in its own task and return
     *                         MG_SMP_PENDING, or keep media refused while the
     *                         upload owns the engine.
     *   commit(permanent)     right after finish succeeded, in the same state
     *                         write: mark for the next boot. permanent = true
     *                         for a confirm write, or always when the backend
     *                         cannot revert. After this the core refuses new
     *                         uploads (EBADSTATE) until the reset.
     *   confirm               after the reboot, for the running test image:
     *                         from mg_smp_healthy() at the healthy mark, or
     *                         for an app confirm once boot_healthy() is true.
     *                         Never called on a !CAN_REVERT backend.
     *   abort                 any time before commit: upload replaced, hash
     *                         mismatch, finish failed, another front end took
     *                         over (mg_smp_abort). Must not fail.
     */
    int (*begin)(void *ctx, uint32_t payload_len, uint32_t *resume_off);
    int (*write)(void *ctx, uint32_t off, const uint8_t *buf, uint32_t n);
    int (*finish)(void *ctx);
    int (*commit)(void *ctx, bool permanent);
    int (*confirm)(void *ctx);
    void (*abort)(void *ctx);

    /*
     * Optional (NULL = the core reports slots from its own record). For
     * revert-capable chips that know both slots: fill `out` for slot 0 or 1,
     * return MG_SMP_ENOENT for an empty slot. Leave MG_SMP_SLOT_HAS_HASH clear
     * and the core fills the hash from the record by matching `id`; set it
     * (with the slot's own envelope hash) where the platform knows that hash,
     * and the record no longer decides what that slot is.
     */
    int (*slot_info)(void *ctx, uint8_t slot, mg_smp_slot_t *out);   /* O(1): see running_id */
} mg_smp_backend_t;

/*
 * begin(): *resume_off comes in as 0 and should be left at 0. Resume is in
 * RAM within one boot (the core keeps its offset and SHA-256 state across a
 * disconnect and answers a repeated off=0 with the same len + sha with its
 * offset; begin is not called again). Cross-reboot resume is reserved for a
 * later API version; the core ignores a non-zero value in v1.
 */

/* ------------------------------------------------------------------------ */
/* Platform: the device around the engine.                                   */
/* ------------------------------------------------------------------------ */

typedef struct mg_smp_platform {
    void *ctx;

    /* GATT transport. One notification on the SMP characteristic, at most
     * min(mtu()-3, 512) bytes (the core fragments). 0 = queued, MG_SMP_EAGAIN = retry
     * later, anything else = the link is gone (the response is dropped). */
    int (*send)(void *ctx, const uint8_t *buf, uint16_t n);
    uint16_t (*mtu)(void *ctx);   /* ATT MTU of the current link (23 if unknown) */

    /* Record store: one small blob that survives a reset (NVS, a vendor
     * SDK's record store or NVRAM property, a dedicated settings sector). key 0 is the only
     * key in v1 (the active-hash record, about 112 bytes; the platform names
     * it as its store needs). get: MG_SMP_ENOENT when absent. Written only
     * from SMP requests (commit, confirm), never at boot. The core treats
     * anything but its exact record (length, magic "MGS1" in the first 4
     * bytes, version) as absent. put must not leave a torn record
     * that reads as valid: use an atomic store (NVS, VM), or on raw NOR
     * program the first 4 bytes (the magic) last, after the rest. Choose the
     * sector against every writer, computed vendor addresses included. */
    int (*record_get)(void *ctx, uint8_t key, void *buf, uint16_t cap, uint16_t *len);
    int (*record_put)(void *ctx, uint8_t key, const void *buf, uint16_t len);

    /* The running firmware's version + build id, as its envelope carries
     * them (the packer takes them from the same build).
     *
     * Never hash the whole image in one pass: running_id, record_get and
     * slot_info run on the request path, in the same pass as the request
     * (the main loop on single-loop chips), up to 3 times per image-state
     * request. Each must be
     * O(1): read constants or a few bytes, never hash or CRC the image. Make
     * the build id a build-time constant passed to the packer
     * (`pack.py --build-id`), or a value the build already stored (the IDF app
     * descriptor's ELF SHA-256, a boot-info CRC); if it really must come from
     * the image, walk it in small steps outside the request (e.g. 4 KB per
     * pass into the update's memory), and hold the first request until done. */
    void (*running_id)(void *ctx, mg_smp_image_id_t *out);

    /* Orderly reset (clean disconnect first): called after the OS reset response went out
     * (or reset_after_commit_ms after a commit with no OS reset). Schedule a
     * clean disconnect, then the reset, and return. */
    void (*reset)(void *ctx);

    /* One OTA at a time across every update front end: claim before the first staged byte, release on abort. false =
     * another front end owns the engine (EBUSY). The claim is held through
     * commit until the reset. */
    bool (*ota_claim)(void *ctx, bool claim);
    /* Stop capture, playback and video before OTA data (shared
     * buffers). Called right after a successful claim. */
    void (*media_stop)(void *ctx);

    /* Memory. kind MG_SMP_MEM_STATE: the core's state (a few
     * hundred bytes, plus cfg.be_scratch for the backend); MG_SMP_MEM_BUFFERS:
     * the request buffers (rx_slots x slot_size). Return 8-byte-aligned memory
     * or NULL (the core then answers rc 2 ENOMEM). Source is the platform's
     * choice: heap, PSRAM, a leased RAM window, or an idle media buffer (it
     * may stop media here; media is stopped during an update anyway). At most
     * one block of each kind is out at a time; every block comes back through
     * mem_put. Called only from the core's context. */
    void *(*mem_get)(void *ctx, uint8_t kind, uint32_t size);
    void (*mem_put)(void *ctx, uint8_t kind, void *block);

    /* The healthy mark, for confirm on revert-capable backends only. The
     * core answers every request whenever the platform feeds it (start it
     * once the recovery tiers are up: ordering, not a gate).
     *   - the platform calls mg_smp_healthy() once at its mark: a test image
     *     confirms itself there, no phone needed;
     *   - an app confirm before the mark gets rc 0 at once; boot_healthy()
     *     tells the core whether the mark has passed (then it confirms now);
     *   - a test image that never reaches the mark reverts (the platform's
     *     boot guard); a confirmed image never rolls back.
     * Mandatory with MG_SMP_BE_CAN_REVERT, unused otherwise. Must be cheap. */
    bool (*boot_healthy)(void *ctx);

    /* Optional. wake: work is queued, call mg_smp_poll() soon (deferred
     * mode). now_ms: a millisecond clock, for the resume hold, the stale
     * partial request rule and reset_after_commit_ms (without it an
     * interrupted upload is dropped at the link drop). log: one short line,
     * no secrets. */
    void (*wake)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    void (*log)(void *ctx, const char *line);

    /* Optional (NULL = everything allowed). May this connection run a
     * mutating update operation? op is one of MG_SMP_OP_*. Called before the
     * operation changes anything; false = the request is answered rc 11
     * (MG_SMP_EACCESSDENIED) with no state change. Reads (image state read,
     * mcumgr_params, echo) are never asked. A transfer the connection started
     * while allowed stays allowed on that connection to its end: its chunks
     * (and the client's retry from offset 0 with the same len and sha), the
     * state write after the image is staged, and the OS reset after the
     * commit are not asked again. A link drop ends that: a resume on the next
     * connection is asked. Answer from what the platform knows about the
     * current connection (token proof, a key-hold/USB window, a SAFE/recovery
     * the user's key entered). O(1), like running_id. */
    bool (*may_change)(void *ctx, uint8_t op);
    /* Optional, used only with may_change (NULL = may_change(MG_SMP_OP_UPLOAD), today's behavior): asked instead of
     * may_change(MG_SMP_OP_UPLOAD) wherever that would be, with the image the upload names, so a confirmation can
     * bind to it: at offset 0 its len and whole-image SHA-256; on a chunk at an offset the upload it continues (a
     * resume on a new connection). sha32 is NULL when the request carries none, or when the core is built without
     * MG_SMP_FILE_SHA (it can't check one at the end); with it, an upload taken under a bound SHA-256 completes only
     * if the whole image matches. len is 0 when unknown. O(1). */
    bool (*may_upload)(void *ctx, uint32_t len, const uint8_t *sha32);
    /* Optional (NULL = every refusal is MG_SMP_EACCESSDENIED, today's behavior): asked when may_change or
     * may_upload refused a request: MG_SMP_EBUSY while the gadget asks for its confirm press (the client retries;
     * nothing changed), anything else = MG_SMP_EACCESSDENIED. O(1). */
    int (*refusal_rc)(void *ctx);
} mg_smp_platform_t;

/* may_change()'s op classes: the mutating update operations. */
enum {
    MG_SMP_OP_UPLOAD = 1,   /* image upload: a new upload, or a chunk on a connection that didn't start it */
    MG_SMP_OP_STATE = 2,    /* image state write: test or confirm */
    MG_SMP_OP_RESET = 3,    /* OS reset */
    MG_SMP_OP_ERASE = 4,    /* image erase (the core answers ENOTSUP once allowed) */
};

/*
 * SHA-256 hook. The core calls these three; mg_smp_sha256.c provides them.
 * A platform that already links a SHA-256 (hardware or library) leaves that
 * file out, builds the core with -DMG_SMP_SHA256_BUILTIN=0, implements the
 * three over its own, and sets MG_SMP_SHA256_CTX_SIZE (bytes, default 112) to
 * at least its context size. ctx is MG_SMP_SHA256_CTX_SIZE bytes, 8-aligned.
 */
#ifndef MG_SMP_SHA256_CTX_SIZE
#define MG_SMP_SHA256_CTX_SIZE 112
#endif
void mg_smp_sha256_init(void *ctx);
void mg_smp_sha256_update(void *ctx, const uint8_t *p, uint32_t n);
void mg_smp_sha256_final(void *ctx, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* MG_SMP_BACKEND_H */
