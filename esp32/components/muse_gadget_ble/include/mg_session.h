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

/*
 * The device side of mg_command_session (protocols/mgcommands.h, Device
 * session) for one connection, with no BLE stack in it so the host tests
 * build it: hello, the credentials after it, the client's session state,
 * messages in both directions with credit, abort, and credentials_update.
 * mg_proto hands it the client's session commands and it answers on Control
 * through ops.notify. Messages from the VM go up whole (ops.message, then
 * mg_session_take()); messages to it come down whole (mg_session_send()) and
 * go out in fragments of at most ATT_MTU - 3 bytes, within the client's
 * credit. Not thread-safe: mg_proto's caller serializes every call.
 *
 * Sealed credentials (mgcommands.h, Sealed credentials): a hello with mg_session_hello_flag_sealed needs no
 * encrypted link; the credentials go sealed under keys from K, the matching
 * token proof's nonces (taken once: one sealed session per proof) and the
 * node id, with the shared seal (portable/mg_device_session,
 * mg_session_seal.c, vendored unchanged), and that session takes only sealed
 * credentials_updates, each with the next counter.
 *
 * Sealed data (mgcommands.h, Sealed data): in a sealed session data is
 * sealed too, both ways, with the same keys and one counter per direction
 * shared with the credentials and updates. mg_session_send() seals each
 * message whole before fragmentation; a client's message takes its counter
 * at its first fragment, its tag is checked at its last (layer 1's
 * incremental tag: a refused tag gives the counter back, an abort keeps it
 * used), and mg_session_take() decrypts it in place. Unsealed data or
 * updates get insecure.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mgcommands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of mg_session_config_t.buf for a device max_message: the messages
 * from the VM, and a bit for each of those bytes marking where one ends. */
#define MG_SESSION_BUF_SIZE(max_message) ((max_message) + ((max_message) + 7) / 8)
/* Control answers (hello, credentials_saved, errors) held for a full stack at most. */
#define MG_SESSION_CTL_HELD 8
/* The longest of them: a sealed session's hello, with its hello_flags. */
#define MG_SESSION_CTL_MAX 12
/* Largest fragment the device sends, headers included (ATT_MTU - 3 caps it lower). */
#define MG_SESSION_FRAGMENT_MAX 512
/* Room for each token of the pair: the credentials as a whole are at most
 * MG_SESSION_MAX_CREDENTIALS bytes. */
#define MG_SESSION_TOKEN_MAX MG_SESSION_MAX_CREDENTIALS
/* A sealed credentials_saved (only for a hello with
 * mg_session_hello_flag_sealed_saved): its plaintext, [result ||
 * SHA-256(refresh_token)[0:8]], and its sealed form (counter and tag added). */
#define MG_SESSION_SAVED_PT (1 + MG_SESSION_SAVED_HASH_LEN)
#define MG_SESSION_SAVED_SEALED (MG_SESSION_SAVED_PT + 20)
#define MG_SESSION_CLEAR_SEALED (1 + 20)   /* the sealed clear and its answer: counter || one byte || tag */
#define MG_SESSION_CLEAR_PENDING (-1)     /* ops.clear: the erase is running (mg_session_clear_done()) */
/* Sealed credentials_saved answers held until they go (one per update the
 * client sent; it waits for each answer, so more than one is a client
 * that didn't). */
#define MG_SESSION_SAVED_HELD 4

/* The device's credential, NUL-terminated (device_key and key_id empty
 * without a device key: they're left out of the JSON, and so are vm_id and
 * vm_kind, which are empty too when setup bound the key to no VM). */
typedef struct {
    char node_id[64];
    char access_token[MG_SESSION_TOKEN_MAX];
    char refresh_token[MG_SESSION_TOKEN_MAX];
    char device_key[48];   /* base64, 32 bytes */
    char key_id[28];       /* base64, 16 bytes */
    char vm_id[65];        /* the fetch_vms entry the key is bound to, at most 64 bytes */
    char vm_kind[4];       /* the VM's kind, as setup stored it */
    /* When the access token runs out, unix seconds, as the client last wrote
     * it back (credentials_update's access_expires_at): carried as the
     * credentials' last field so the client needn't refresh at once. 0: not
     * known, left out (the client then treats the pair as expired). */
    uint64_t access_expires_at;
    /* The token proof key K, read in one snapshot with the node id and the pair, so a sealed session's keys are
     * always this pair's setup's. Not set: the seal reads K through ops.proof_key. */
    uint8_t proof_k[32];
    bool proof_k_set;
} mg_session_creds_t;

typedef struct {
    void *ctx;
    /* One Control notification; false if it couldn't go now. */
    bool (*notify)(void *ctx, const uint8_t *d, size_t n);
    /* Fills the credential; false if the device holds no token. */
    bool (*credentials)(void *ctx, mg_session_creds_t *c);
    /* Stores a refreshed pair, both in one commit; false if it wasn't saved.
     * Called from mg_session_commit() with cfg.store_later. */
    /* The pair and its access_expires_at (0: the update carried none, so
     * none is kept) in one commit. */
    bool (*store_tokens)(void *ctx, const char *access_token, const char *refresh_token, uint64_t access_expires_at);
    /* The client reported the VM session (mg_session_state_t): open starts a
     * new session with this epoch, anything else ends it. Every state the
     * client reports, once, a closed with no session open included (a failed
     * open: connecting, then closed; mg-session-v1 exchange 14). Also closed
     * when a new hello or the connection ends an open session. */
    void (*state)(void *ctx, uint8_t state, uint16_t epoch);
    /* A whole message from the VM is waiting: mg_session_take() it. */
    void (*message)(void *ctx);
    /*
     * Sealed credentials (all three, or none: no sealing). take_nonces hands
     * out the latest matching token proof's nonces once (false: none, or
     * taken already); proof_key copies K (false: none).
     */
    bool (*take_nonces)(void *ctx, uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN],
                        uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN]);
    bool (*proof_key)(void *ctx, uint8_t k[32]);
    /* The credentials can't be handed out now (the gadget is being set up
     * again: until the commit has ended the old setup's sessions, a client
     * proven with the old K could read the new pair): hello is refused busy
     * and keeps the proof. NULL: never. */
    bool (*busy)(void *ctx);
    /* A data message from the client was refused (a sealed session's plain
     * data, a wrong counter or tag): why, and its total_len. For the log,
     * never its bytes. NULL: not told. */
    void (*data_refused)(void *ctx, const char *why, uint32_t len);
    /* The sealed clear (mg_session_clear, a Forget in a sealed session): erases the tokens and K as the token
     * proof's clear does, without ending the session (it answers, then ends it). 1 erased, 0 not, or
     * MG_SESSION_CLEAR_PENDING: the erase runs elsewhere and mg_session_clear_done() gives its result, the
     * answer then. NULL: the sealed clear is refused unsupported. */
    int (*clear)(void *ctx);
} mg_session_ops_t;

typedef struct {
    /* Messages from the VM, whole and partial: MG_SESSION_BUF_SIZE(max_message)
     * bytes. NULL: taken with alloc when the client reports the session open
     * and given back with release when it ends or the link goes (nothing
     * held between sessions). */
    uint8_t *buf;
    void *(*alloc)(size_t n);
    void (*release)(void *p);
    /* Every other allocation of the session (out-queue copies, credentials,
     * keys, the incremental tag): PSRAM first on boards with some, so the
     * session never takes internal RAM a malloc threshold would give it.
     * NULL: malloc and free. Strings handed out by take_update come from it
     * too and are freed with free() (msg_release must accept that). */
    void *(*msg_alloc)(size_t n);
    void (*msg_release)(void *p);
    uint32_t max_message;    /* hello: the largest message the device takes */
    uint32_t window;         /* hello: message bytes it takes before giving credit, 1..max_message */
    size_t out_max;          /* bytes of messages to the VM queued at most */
    /* A credentials_update's pair waits for mg_session_commit() (another
     * task, where storing may take long) instead of being stored at once. */
    bool store_later;
    /* request_status caps: mg_session_cap_client_refreshes, the gadget
     * holds no Wi-Fi to refresh its own pair. */
    bool client_refreshes;
    /* request_status caps: mg_session_cap_sealed_clear, and mg_proto refuses an unsealed token_proof clear
     * (insecure). CONFIG_MUSE_GADGET_SEALED_CLEAR, off by default. */
    bool sealed_clear;
} mg_session_config_t;

struct mgseal_keys;   /* mg_session_seal.h */
struct mgseal_tag;    /* mg_session_seal.h: an incremental tag */

typedef struct mg_session_out mg_session_out_t;   /* a message queued to the VM (mg_session.c) */

typedef struct {
    mg_session_config_t cfg;
    mg_session_ops_t ops;
    uint16_t mtu;

    /* ---- this connection ---- */
    bool hello;              /* a hello succeeded */
    bool open;               /* the client last reported open */
    uint16_t epoch;
    uint32_t opened;         /* sessions opened so far: the open one's id (never 0) */
    /* Hellos answered and credentials_updates saved so far (kept across
     * connections, like opened): each restarts the gadget's re-ask series
     * (mgcommands.h, Device session; mg_session_ref.py). */
    uint32_t reask_hellos, reask_saved;
    uint32_t client_max, client_window;
    uint32_t send_credit;    /* message bytes the device may still send */
    uint32_t owed_credit;    /* bytes to give back, not yet notified */
    uint32_t rx_credit;      /* what the client may still send (the window, less taken, plus given back) */
    uint32_t credit_overruns;   /* fragments past it, dropped with their message */
    bool nudge;              /* a credit goes even with none owed (mg_session_nudge) */
    int8_t open_request;     /* open_request to send: -1 none, else its value */
    /* A sealed clear (mg_session_clear): its 21 bytes arriving (clr_got of them), then taken and waiting
     * behind what goes out before it (clr_pending), then its sealed answer going out (clearing, clr_off sent). */
    uint8_t clr[MG_SESSION_CLEAR_SEALED];
    uint8_t clr_got, clr_off;
    bool clr_in, clr_pending, clearing;
    bool clr_erasing;        /* the erase is running elsewhere: its result (mg_session_clear_done()) is the answer */
    bool clr_due, clr_ok;    /* erased (clr_ok its result): the answer waits for the data message part way out */
    bool upd_taken;          /* a pair handed out by mg_session_take_update(), its answer not yet queued */

    /* Sealed: this hello asked for sealed credentials, so every message with
     * a payload is sealed, data included (mgcommands.h, Sealed data). The
     * keys are allocated at the hello and live for the whole sealed session
     * (its credentials, credentials_updates and data, every state open and
     * epoch under it), wiped and freed at its end (a new hello, a reset, the
     * connection); each direction's next counter, shared by all of them,
     * restarts only at a sealed hello. ctr_hole: 0, or ctr_in minus the
     * one client counter a message refused at its tag left open after a
     * later one was taken (data and a credentials_update in flight together:
     * two in flight; layer 1's mgseal_rx_*). */
    bool sealed;
    /* Its hello also carried mg_session_hello_flag_sealed_saved: the
     * client counts a sealed credentials_saved on
     * ctr_out. Without it the answers are the legacy unsealed byte and take
     * no counter (older apps relay one counter behind otherwise and park).
     * In sealed's padding: no growth. */
    bool saved_sealed;
    struct mgseal_keys *keys;
    uint32_t ctr_out, ctr_in;
    uint8_t ctr_hole;
    /* Counters never wrap: in_spent once the client's 0xffffffff is taken (no other is),
     * out_spent once the device's is (nothing more is sealed; the client's sealed messages get invalid_value). */
    bool in_spent, out_spent;
    uint32_t hello_gen;      /* bumped at every hello and reset: fences a pair stored outside the lock */
    uint8_t *ring;           /* cfg.buf, or the one taken for the open session (NULL while none) */

    /* Messages from the VM: whole ones, then the partial one, in a ring of
     * max_message bytes at the start of buf (the oldest byte at in_head).
     * The bits after it mark each whole one's last byte, so the ring holds
     * as many messages as fit in it, however small: only bytes are bounded
     * (by the window, which the client's credit keeps to). */
    uint32_t in_head;
    uint32_t in_used;        /* bytes held, the partial message's included */
    uint32_t in_part;        /* bytes of the partial message */
    /* A window smaller than max_message: bytes of the partial message given back before it ends (they fill
     * the window; they're in the ring), and those of the oldest whole message, which its read doesn't give
     * back again. Only a message arriving with nothing whole held gets them, so it's the oldest when whole. */
    uint32_t in_early, head_early;
    uint32_t in_total;       /* its total_len (0: none open) */
    uint32_t in_count;       /* whole messages held */
    /* Sealed: the partial message's tag, kept as its fragments arrive (taken
     * at its first fragment, given back at its end: ~110 bytes only while one
     * arrives), its counter, and its last 16 bytes, the tag it carries. */
    struct mgseal_tag *dtag;
    uint32_t data_ctr;
    uint8_t dtag_rx[16];

    /* A credentials_update being received, its pair waiting to be stored,
     * and the credentials going out. */
    char *cred_in;
    uint32_t cred_in_len, cred_in_total;
    uint32_t cred_in_ctr;    /* sealed: the counter its first fragment took (mgcommands.h, Sealed data) */
    char *upd_access, *upd_refresh;
    uint64_t upd_expires;   /* the waiting pair's access_expires_at (0: none) */
    /* With cfg.store_later, a sealed update waits whole for
     * mg_session_commit(), which opens it there (the HMACs stay off the
     * task that takes the writes: the NimBLE host's stack is small). */
    uint8_t *upd_sealed;
    uint32_t upd_sealed_len;
    uint32_t upd_sealed_ctr;  /* the counter its first fragment took */
    /* A replacement arriving while one waits takes upd_sealed; the one it
     * would supersede waits here until the replacement's tag checks:
     * superseded (answered not saved) if it does, back in upd_sealed if not. */
    uint8_t *upd_prev;
    uint32_t upd_prev_len;
    uint32_t upd_prev_ctr;
    char *cred_out;
    uint32_t cred_out_len, cred_out_off;
    uint32_t cred_out_cap;   /* the allocation's size: all of it is wiped when it goes */
    uint8_t cred_out_flags;  /* mg_session_flag_sealed, or 0 */

    /* Sealed credentials_saved answers waiting to go,
     * oldest first: their plaintexts. Each takes the next device -> client
     * counter (ctr_out, shared with the credentials and data) when its first
     * fragment goes, after the credentials and before any data message that
     * hasn't started, so the client sees every sealed message in counter
     * order; saved_out holds the head's sealed form once it started
     * (saved_off bytes out). taken_pt: the answer for a pair taken out to be
     * stored (mg_session_take_update()), queued by mg_session_update_saved(). */
    uint8_t saved_q[MG_SESSION_SAVED_HELD][MG_SESSION_SAVED_PT];
    uint8_t saved_head, saved_count;
    uint8_t saved_out[MG_SESSION_SAVED_SEALED];
    uint8_t saved_off;       /* 0: the head hasn't started */
    uint8_t taken_pt[MG_SESSION_SAVED_PT];

    /* Control answers not yet notified, oldest first: they go ahead of
     * everything else, so the hello answer is always before the credentials. */
    uint8_t ctl[MG_SESSION_CTL_HELD][MG_SESSION_CTL_MAX];
    uint8_t ctl_len[MG_SESSION_CTL_HELD];
    uint8_t ctl_head, ctl_count;
    uint32_t ctl_dropped;    /* answers that found MG_SESSION_CTL_HELD waiting (never expected; counted) */

    /* Messages to the VM, oldest first. */
    mg_session_out_t *out_head, *out_tail;
    size_t out_bytes;
    uint32_t out_off;        /* bytes of out_head already sent */

    uint8_t frame[MG_SESSION_FRAGMENT_MAX];
} mg_session_t;

void mg_session_init(mg_session_t *s, const mg_session_config_t *cfg, const mg_session_ops_t *ops);
/* A connection began or ended: everything goes (an open session ends). */
void mg_session_reset(mg_session_t *s);
void mg_session_set_mtu(mg_session_t *s, uint16_t mtu);

/*
 * One mg_command_session write from the client (the bytes after the command
 * byte). proof: a token proof matched on this connection; it gates hello,
 * which is sealed or refused (insecure), whatever the link.
 */
void mg_session_write(mg_session_t *s, const uint8_t *a, size_t n, bool proof);

/* The session is open (the client said so, and nothing has ended it since). */
bool mg_session_open(const mg_session_t *s);
/* The open session's id, or 0: every state report, hello or connection
 * change ends a session, and a new one gets a new id. Callers on other
 * tasks check it so nothing of one session goes into the next. */
uint32_t mg_session_id(const mg_session_t *s);
/* Hellos answered and credentials_updates saved so far: a change in either
 * restarts the re-ask series (the open_request [1] backoff after an unasked
 * state). A hello counts after its own end of an open session is reported. */
void mg_session_reask_marks(const mg_session_t *s, uint32_t *hellos, uint32_t *saved);
/* Queues one message to the VM (copied) and sends what credit allows: the
 * one send point for data, sealed there on a sealed session (mgcommands.h,
 * Sealed data). False if the session isn't open, the message is empty or
 * larger than mg_session_client_max(), the queue has no room for it, or
 * sealing failed. */
bool mg_session_send(mg_session_t *s, const uint8_t *msg, size_t len);
/* Bytes mg_session_send() takes now (0 while not open; less the seal's 20
 * on a sealed session, whose queue holds sealed bytes). */
size_t mg_session_room(const mg_session_t *s);
/* The largest message mg_session_send() takes: the client's max_message (0
 * before hello), less the seal's 20 bytes on a sealed session. */
uint32_t mg_session_client_max(const mg_session_t *s);
/* The bytes decode as a ServiceResponseEnvelope by noise_core ServiceCodec's rules (mgcommands.h, Sealed data): a
 * client data message that doesn't is refused with invalid_value. Pure. */
bool mg_session_is_envelope(const uint8_t *p, size_t n);
#if MG_SESSION_TEST_HOOKS
/* Host tests only: the next client and device counters jump (the vectors' "counters" event, a stand-in for a
 * long session). -1 leaves one as it is. */
void mg_session_test_set_counters(mg_session_t *s, int64_t rx, int64_t tx);
#endif
/* Copies out the oldest whole message from the VM and gives its bytes back
 * as credit. Its length, or 0 if none waits (or it doesn't fit cap: cap
 * counts the sealed bytes on a sealed session). Sealed: its tag was checked
 * as it arrived; it's decrypted in out and the plaintext's length returned. */
size_t mg_session_take(mg_session_t *s, uint8_t *out, size_t cap);
/* This hello's session is sealed (mgcommands.h, Sealed credentials). */
bool mg_session_sealed(const mg_session_t *s);
/* A sealed clear taken whose answer isn't out yet (the platform's restart waits for it, bounded). */
bool mg_session_clear_owed(const mg_session_t *s);
/* The caps byte for request_status's [supported_features, sub_feature,
 * mg_command_session, caps]: sealed when the ops can seal, and
 * cfg.client_refreshes. */
uint8_t mg_session_caps(const mg_session_t *s);
/* A client write (the bytes after the command byte) is a hello asking for
 * sealed credentials. */
bool mg_session_hello_sealed(const uint8_t *a, size_t n);
/* Asks the client to open (true) or close (false) the session: open_request. */
void mg_session_request(mg_session_t *s, bool open);
/* An open session: one credit with what's owed, [session, credit, 0] if nothing is. It reaches the client and nothing
 * past it (credit is never sealed nor forwarded), and is a Bluetooth event: iOS wakes a suspended app for it, which
 * then reads its socket. */
void mg_session_nudge(mg_session_t *s);
/* Sends what's pending (control answers, credit, open_request, credentials,
 * fragments) as far as notifications and credit go. True if something is
 * still pending. */
bool mg_session_pump(mg_session_t *s);
/* Stores a credentials_update's pair that waits (cfg.store_later) and
 * queues credentials_saved. True if there was one. */
bool mg_session_commit(mg_session_t *s);
/*
 * The same in three steps, so the store runs outside the caller's lock (it
 * is many flash writes): take_update hands out a waiting pair (opened first
 * when sealed; a refused one is answered here and false returned), the
 * caller stores it and frees both strings (wiped, MG_SESSION_TOKEN_MAX
 * bytes each), and update_saved queues credentials_saved, only if *gen is
 * still the session's (a hello or a reset meanwhile drops it: the client
 * sends the pair again).
 */
bool mg_session_take_update(mg_session_t *s, char **access, char **refresh, uint64_t *expires, uint32_t *gen);
void mg_session_update_saved(mg_session_t *s, uint32_t gen, bool saved);
/* A sealed clear's erase that ops.clear left running (MG_SESSION_CLEAR_PENDING) has ended: erased or not. Its
 * sealed answer ([1] or [0]) goes, then the session ends. Nothing if the session ended meanwhile. */
void mg_session_clear_done(mg_session_t *s, bool erased);

/* ---- JSON (mgcommands.h, Device session), exposed for the host tests ---- */

/* The credentials object, compact, keys in the spec's order
 * (access_expires_at last, when known): its length, or 0 if it doesn't fit
 * cap. */
size_t mg_session_creds_json(const mg_session_creds_t *c, char *out, size_t cap);
/* A credentials_update: a flat JSON object with non-empty string
 * access_token and refresh_token. False if it's anything else (or a token
 * doesn't fit, or access_expires_at comes twice). *expires (may be NULL):
 * its access_expires_at, as portable layer 2 reads it: an unsigned decimal
 * integer that fits 64 bits, else (absent, any other form) 0, none. */
bool mg_session_parse_update(const char *json, size_t len, char *access, char *refresh, size_t cap,
                             uint64_t *expires);

#ifdef __cplusplus
}
#endif
