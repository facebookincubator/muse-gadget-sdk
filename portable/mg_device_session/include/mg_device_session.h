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

#ifndef MG_DEVICE_SESSION_H_
#define MG_DEVICE_SESSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_session_seal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Layer 2 of portable/mg_device_session: mg_command_session (0x89) for a
 * credentials-only gadget (protocols/mgcommands.h, Device session).
 * After a matching token proof the
 * device hands the app its own device credentials, sealed, and stores the
 * refreshed pair the app writes back. The
 * device never opens a VM session and never sends open_request; the rest of
 * the session is answered so a client sees a conforming device (state
 * recorded, client data dropped and credited back, credit and abort taken).
 *
 * The board supplies a port (mgds_port_t): the transport, the token proof,
 * the stored record (K, node_id, the pair: read in pieces, no plaintext
 * buffer), storage lent for an incoming credentials_update, a busy test
 * (media) and the commit. No allocation, no logging, one context (the main
 * loop or one task) for every call.
 *
 *  - hello: the refusals, in order: recovery, not_found (no pair, or a
 *    placeholder one), proof_required, insecure (any unsealed hello, whatever
 *    the link's encryption: a token proof can be relayed, and the relay's own
 *    bond is encrypted too; the error alone, no Security Request), invalid_value
 *    (version, max_message, window: 4..max_message, a sealed window holds the
 *    counter). A refused hello leaves an open session
 *    as it was. An accepted one resets the session (as state closed does).
 *  - One sealed session per token proof: the nonces are taken from the proof
 *    (take_nonces, mgtp_take_nonces) when a sealed hello is answered, and the
 *    session's keys derived from them, K and node_id then (kept in mgds_t for
 *    the session; the nonces aren't kept); a
 *    second sealed hello under the same proof is refused with proof_required
 *    and the first session goes on. The board calls mgds_reset() at every
 *    token proof challenge (before mgtp_rx() handles it): a session never
 *    outlives the proof that keyed it.
 *  - Everything is checked before the first byte goes out: the credentials
 *    are read once to make the tag (and to check every token byte), then
 *    streamed: each fragment is read and encrypted again as it is sent.
 *    If the session must end part-way (a reset, a new proof) abort goes out.
 *  - Deferral: while busy() a hello waits up to
 *    MGDS_HELLO_DEFER_MS (1 s), then is answered [session, busy] (the
 *    client's busy retry asks again); a verified credentials_update's commit
 *    waits up to MGDS_DEFER_MS (30 s). Anything at a disconnect gets nothing.
 *    In a sealed session
 *    every write-back the device gives up on while the link is up is
 *    answered sealed credentials_saved [0] (silence now reads as
 *    "assumed saved"): a commit held past MGDS_DEFER_MS, a held update
 *    dropped by mgds_release_storage(), an update that found no storage or
 *    lost it (answered at its last fragment), and an update refused when the
 *    stack had no buffer for its refusal. A held update stays through a
 *    refused first fragment (another update, an unsealed one): only the new
 *    one is refused. A session whose hello didn't ask for the sealed answer (an
 *    older client) keeps these silent, as before (the app resends the pair
 *    on its next connection).
 *  - Sealed data (mgcommands.h, Sealed data): in a sealed session every
 *    client message with a payload is sealed (credentials_update and data),
 *    with one client -> device counter shared by both, from 0 at the sealed
 *    hello. The counter is read from the first fragment and must be the next
 *    one (else invalid_value, the counter unused); once taken it is used
 *    (an abort, a state change, an update that finds no storage, a fragment
 *    refused before the tag), except by a message refused at its tag: a
 *    forged message that reaches its tag can't move the counter; one cut off
 *    before it (a first fragment with the next counter, then a malformed
 *    one) has used it, as an abort does.
 *    An unsealed data or credentials_update in a sealed session is refused
 *    with insecure and changes nothing. A data message's tag is checked as
 *    its fragments arrive (no buffer: mgseal_tag_begin / _update / _check);
 *    a bad one is invalid_value. Either way the data is dropped and its bytes
 *    go back as credit (this device keeps no VM session). The device's
 *    sealed messages are its credentials (counter 0) and its
 *    credentials_saved answers (1, 2, ...): one device -> client counter.
 *  - credentials_update: sealed only once a sealed hello opened the session
 *    (the shared counter above); the plaintext a flat JSON object ('{' its
 *    first byte, no leading whitespace) with
 *    access_token and refresh_token as non-empty strings of token characters
 *    (printable ASCII, no quote or backslash), access_expires_at optional (an
 *    unsigned integer, unix seconds: kept with the pair by a board with
 *    commit_expires, and handed back in its credentials; any other form is
 *    ignored); other fields ignored; the stored document it would make
 *    (node_id, the pair, access_expires_at where kept) within
 *    MGDS_MAX_CREDENTIALS. Any failure: invalid_value (an unsealed error
 *    frame), nothing stored. Otherwise, after the commit,
 *    credentials_saved: in a sealed session sealed (mgcommands.h,
 *    Credentials;
 *    vectors sealed_credentials_saved) under the next device -> client
 *    counter, framed as credentials (flags first|last|sealed, total_len 29,
 *    fragmented to the MTU and MGDS_FRAME_MAX, an abort if the session ends
 *    part-way on a live link), plaintext [1 || SHA-256(refresh_token)[0:8]]
 *    stored, [0 || zeros] not (mgseal_saved_plaintext()); in a plain
 *    session the one-byte [1] / [0]. Only for a client whose sealed hello
 *    carried mg_session_hello_flag_sealed_saved (echoed in the device's
 *    hello; a client without it doesn't count a sealed answer's device
 *    counter and its relay parks): to any other sealed hello the device
 *    answers exactly as a device without sealed answers does
 *    (the one-byte answer, no counter, give-ups silent). One sealed answer goes out at a time,
 *    after the credentials: a commit waits for one still going out, a give-up
 *    [0] queues behind it (mgds_t.owe0, at most 15; more are dropped).
 *    Counters never wrap (mgcommands.h, Sealed credentials): after the
 *    message with its counter's last value (0xFFFFFFFF; MGDS_TX_CTR_U8: 255)
 *    the device seals nothing more and refuses every sealed client message
 *    with invalid_value (data: its credit back; an update held for its
 *    commit too, nothing committed), and once the client's
 *    0xFFFFFFFF is taken no other client counter is (a hole still may be);
 *    the client re-proves into a new session either way. A sealed hello on a
 *    link below MG_MIN_ATT_MTU (100) gets invalid_value: the one-write
 *    messages (credentials_saved, 36 bytes) need it (MGDS_ATT_MTU_MAX).
 *
 *  - MGDS_DATA (default 0): the data path.
 *    The phone holds the Noise connection to the VM with the gadget's device
 *    token and relays that session's messages as data (device -> client
 *    ServiceRequestEnvelopes, client -> device ServiceResponseEnvelopes),
 *    sealed both ways in a sealed session on the counters above (device ->
 *    client: credentials 0, then data and credentials_saved in the order their
 *    first fragments go). Layer 2 reports every state the client sends
 *    (data_state), and asks for the session itself: open_request [1] after
 *    a closed or no_network it didn't ask for, in the two re-ask series of
 *    mgcommands.h (Session state) and mg-session-v1.json (their state in
 *    mgds_t's padding). While the board lends the data path's state
 *    (mgds_data_t at port.data, mgds_data_attach / _detach) it hands each
 *    client message to the port after its tag is checked (reassembled in a
 *    lent buffer, or in pieces), credits it back (as it lands when it is
 *    larger than the device's window), and sends the port's messages
 *    (mgds_send) one at a time within the client's max_message and credit.
 *    The caps lose credentials_only. mgds_t keeps its size. With MGDS_DATA 0
 *    nothing of this is compiled: the object code is byte-identical to the
 *    credentials-only layer's.
 *
 * Per connection, on rv32 (mgds_t): 264 bytes by default (the session's keys
 * 64, a sealed data message's tag state 128); 232 with MGDS_KEEP_KEYS=0 (the
 * nonces instead, the keys derived again for each sealed message); 136 with
 * MGDS_TAG_LEND=1 (the tag state lent by the board per message); 104 with
 * both, 92 with MGDS_STATS=0 too; 128 with MGDS_SEALED_DATA=0; 260 with
 * MGSEAL_EXT_SHA=1: each of those with MGDS_TX_CTR_U8=1. With the default
 * uint32_t device counter the default stays 264 and the others take 4 bytes
 * more (8 with MGDS_STATS=0 alone). Stack: the deepest call is one mgds_poll()
 * fragment: a fragment buffer (MGDS_FRAME_MAX) plus the seal's HMAC (~0.5 KB
 * with the builtin backend).
 */

#define MGDS_MAX_CREDENTIALS 2048u /* MG_SESSION_MAX_CREDENTIALS */
#define MGDS_SEALED_MAX      (MGDS_MAX_CREDENTIALS + MGSEAL_OVERHEAD)
#define MGDS_DEFER_MS        30000u /* a verified credentials_update's commit waits this long for !busy() */
/* A hello deferred while busy() is answered [session, busy] after this (mgcommands.h's ~1 s
 * rule for an answer the device can't give now; the client's busy retry is 250 / 500 / 1000 ms). A port may lower
 * it, not raise it. */
#ifndef MGDS_HELLO_DEFER_MS
#define MGDS_HELLO_DEFER_MS  1000u
#endif
#if MGDS_HELLO_DEFER_MS > 1000u
#error "MGDS_HELLO_DEFER_MS: at most 1000 (a client waits about 1 s for a hello's answer)"
#endif
#define MGDS_NODE_MAX        64u
/* 1: the data path (for a
 * watch face showing the user's avatar): the device has a VM session over this link. The phone holds
 * the Noise connection to the VM with the gadget's device token; the gadget
 * exchanges that session's messages as mg_command_session data (device ->
 * client ServiceRequestEnvelopes, client -> device ServiceResponseEnvelopes:
 * noise_core's ServiceCodec wire), sealed both ways in a sealed session on
 * the shared counters. Layer 2 then reports the client's state, hands the
 * port each client message (reassembled into the port's buffer, or in
 * pieces), sends the port's messages against the client's credit, and runs
 * open_request's re-ask series (mgcommands.h, Session state;
 * mg-session-v1.json). 0 (default): credentials only, the object code
 * unchanged. Needs MGDS_SEALED_DATA and the uint32_t device counter. */
#ifndef MGDS_DATA
#define MGDS_DATA 0
#endif
/* The caps a board lists for this session in request_status
 * ([supported_features, sub_feature, mg_command_session, MGDS_CAPS]):
 * mg_session_cap_sealed | mg_session_cap_client_refreshes |
 * mg_session_cap_credentials_only. Without MGDS_DATA layer 2 carries
 * credentials only (no VM session: it never sends open_request and records
 * and drops state), so the apps never open a VM socket for it. With
 * MGDS_DATA the device has a VM session: 0x03 (no credentials_only). A port
 * that runs a VM session of its own on this link defines MGDS_CAPS 0x03u
 * before including this header. */
/* mg_session_cap_sealed_clear (mgcommands.h) in MGDS_CAPS: 1 once both Muse apps send the sealed clear
 * (apps first). One switch with the token proof core's MGTP_SEALED_CLEAR (an unsealed token_proof
 * clear refused insecure): either name sets both, MGDS_CAP_SEALED_CLEAR follows MGTP_SEALED_CLEAR when only that one
 * is defined, and a build that sets them differently fails. That happens in the preprocessor: define it for every
 * unit of both libraries (a global define), since a define only one library's units see leaves the other at 0. The
 * sealed clear itself (mg_session_clear) is taken either way when the port has clear(). */
#ifndef MGDS_CAP_SEALED_CLEAR
#ifdef MGTP_SEALED_CLEAR
#define MGDS_CAP_SEALED_CLEAR MGTP_SEALED_CLEAR
#else
#define MGDS_CAP_SEALED_CLEAR 0
#endif
#endif
#if defined(MGTP_SEALED_CLEAR) && (MGTP_SEALED_CLEAR) != (MGDS_CAP_SEALED_CLEAR)
#error "MGDS_CAP_SEALED_CLEAR and MGTP_SEALED_CLEAR differ: the sealed clear is one switch"
#endif
/* The largest ATT_MTU the board's stack can negotiate, if it can't reach MG_MIN_ATT_MTU: the sealed session needs it
 * (a sealed hello on a smaller link gets invalid_value), so such a board doesn't declare mg_session_cap_sealed, and
 * without it no session opens (every unsealed hello is refused): it doesn't list mg_command_session at all. 0
 * (default): it can. */
#ifndef MGDS_ATT_MTU_MAX
#define MGDS_ATT_MTU_MAX 0
#endif
#define MGDS_MIN_ATT_MTU 100u /* MG_MIN_ATT_MTU (mgcommands.h) */
#if MGDS_ATT_MTU_MAX && MGDS_ATT_MTU_MAX < MGDS_MIN_ATT_MTU
#define MGDS__CAP_SEALED 0x00u
#else
#define MGDS__CAP_SEALED 0x01u
#endif
#ifndef MGDS_CAPS
#if MGDS_DATA
#define MGDS_CAPS (MGDS__CAP_SEALED | 0x02u | (MGDS_CAP_SEALED_CLEAR ? 0x20u : 0u))
#else
#define MGDS_CAPS (MGDS__CAP_SEALED | 0x06u | (MGDS_CAP_SEALED_CLEAR ? 0x20u : 0u))
#endif
#endif
#ifndef MGDS_FRAME_MAX
/* The fragment buffer: the largest credentials, data or clear fragment sent (ATT_MTU 247 - 3). A sealed
 * credentials_saved is one 36-byte write whatever this (mgcommands.h: one write; a sealed session's MTU is at
 * least MG_MIN_ATT_MTU), built in a buffer of its own. */
#define MGDS_FRAME_MAX 244u
#endif
#ifndef MGDS_STATS
#define MGDS_STATS 1
#endif
/* 1: a sealed message refused at its tag (data or credentials_update) also
 * ends the sealed session (a device may end it on any
 * refusal; the client re-proves): one forged message per token proof. 0 (the
 * default, as the reference receiver): the session goes on and the refused
 * message's counter stays unused. */
#ifndef MGDS_END_ON_BAD_TAG
#define MGDS_END_ON_BAD_TAG 0
#endif
/* 1 (default): the session's seal keys (64 B) are kept in mgds_t from its
 * sealed hello, so a sealed message costs only its own HMACs. 0: the proof's
 * nonces (32 B) are kept instead and the keys derived again (K and node_id
 * read, HKDF: three HMACs) at the first and last fragment of each sealed
 * message and for each credentials fragment: 32 B less, about 0.25 M more
 * instructions per message on rv32. */
#ifndef MGDS_KEEP_KEYS
#define MGDS_KEEP_KEYS 1
#endif
/* 1: the tag state of a sealed data message in flight (mgds_dtag_t, 128 B) is
 * lent by the board for that message (mgds_tag_lend / mgds_tag_return, which
 * the board then defines, e.g. from its setup lease) instead of living in
 * mgds_t; no lend: the message is refused busy at its end, its counter
 * unused. 0 (default): in mgds_t. */
#ifndef MGDS_TAG_LEND
#define MGDS_TAG_LEND 0
#endif
/* 1: a sealed data message's tag is checked (a bad one is
 * invalid_value). 0: 140 bytes less per connection (rv32); the counter rules
 * stay (the shared counter keeps credentials_update in step), and a sealed
 * data message is dropped and credited without its tag checked (no refusal
 * for a bad tag): a conformance gap to list. Needs MGSEAL_STREAM. */
#ifndef MGDS_SEALED_DATA
#define MGDS_SEALED_DATA 1
#endif
/* For a relay that counted a message whose first fragment never left
 * (every later sealed message failed the exact-next counter rule, refused
 * until a new hello that never came): this
 * many counter refusals in a row in one sealed session (a first fragment
 * whose counter is neither the next one nor the hole) end it, as any
 * session end (no new message): the keys or nonces wiped, the hello cleared,
 * both counters 0; a sealed hello needs a new token proof (take_nonces), so
 * the client re-proves and both sides count from 0 under new keys.
 * Only counter refusals count: a message refused at
 * its tag gives its counter back and resets nothing, so a forged message can't
 * end a good session; any first fragment with an acceptable counter, a hello
 * and a session end reset the run. 0 off; at most 3 (2 bits). */
#ifndef MGDS_CTR_END
#if MGDS_SEALED_DATA
#define MGDS_CTR_END 3
#else
#define MGDS_CTR_END 0          /* (its 2 bits live beside data_bad, a sealed-data field) */
#endif
#endif
#if MGDS_CTR_END && !MGDS_SEALED_DATA
#error "MGDS_CTR_END needs MGDS_SEALED_DATA (its counter shares data_bad's byte)"
#endif
/* The device -> client counter (mgds_t.tx_ctr: the credentials take 0, each
 * sealed credentials_saved the next). 0 (default): a uint32_t, in the tail
 * padding of the default mgds_t. 1: a uint8_t, in padding of every config
 * (RAM-tight boards: no growth anywhere); its counters then end at 255,
 * 254 refreshes into one sealed session. Either way the counter never wraps:
 * once the device has sealed its last one it seals nothing more in that
 * session and refuses every sealed client message with invalid_value (data:
 * its credit back), so the client re-proves and opens a new sealed session
 * (mgcommands.h, Sealed credentials). Each direction: once the client's
 * 0xFFFFFFFF is taken, no other counter is. */
#ifndef MGDS_TX_CTR_U8
#define MGDS_TX_CTR_U8 0
#endif
/* 1: layer 2's own stall rules, so a board needs no stall guard of its own around
 * the session:
 *  - the stall clock runs only while the device owes the client frames (its
 *    hello and credentials going out, a sealed credentials_saved going out,
 *    a give-up [0] waiting or a sealed clear's answer going out) or a sealed
 *    clear is part way in, and any client write on the link restarts it (a
 *    session frame through mgds_rx, any other write through
 *    mgds_client_frame): owing MGDS_STALL_MS with no client write ends the
 *    session with the link;
 *  - a credentials_update part-way (storage lent, or its fragments being
 *    discarded for want of storage) doesn't start that clock, but no session
 *    frame for MGDS_UPDATE_IDLE_MS while it is ends the session with the link
 *    too (a stalled client can't hold a battery gadget awake; only session
 *    frames count, so a client busy on Control that never finishes the update
 *    still reaches it);
 *  - a board that sleeps with the link up calls mgds_woke() as it wakes: the
 *    clocks run on the now it passes, and a sleep isn't a silent client.
 * Ending "with the link" (never silently): the session
 * ends as mgds_reset(t, true) does (an abort if credentials or an answer were
 * part-way, the storage back, the keys wiped, data_state closed for a VM
 * session that was open), then port.end_link() drops the Bluetooth link; the
 * next connection re-proves. Checked in mgds_poll(), which a board calls
 * while mgds_busy() (true in both cases). Needs port.end_link (NULL: the
 * rules are off). 0 (default): none of this; the object code unchanged. */
#ifndef MGDS_STALL
#define MGDS_STALL 0
#endif
/* 1: mgds_release_storage() doesn't seal the give-up [0] it owes for a held
 * update on the spot: it is owed (mgds_busy() true) and goes from the next
 * mgds_poll(), as one does when the stack has no buffer. For boards whose
 * storage is reclaimed at the bottom of a deep chain (a media start from the
 * console or UI): the seal's HMAC (~640 B of stack on rv32) moves to the
 * poll. The wire is the same, one poll later. 0 (default): sealed at once;
 * the object code unchanged. (Found on a board's deepest main-stack
 * chain.) */
#ifndef MGDS_GIVE_UP_LATER
#define MGDS_GIVE_UP_LATER 0
#endif
/* Accepted for older board configs: every MGDS_DATA build has mgds_ask_quiet()
 * and mgds_task_start() (a session only for an active task). Needs MGDS_DATA. */
#ifndef MGDS_ASK_QUIET
#define MGDS_ASK_QUIET 0
#endif
#if MGDS_ASK_QUIET && !MGDS_DATA
#error "MGDS_ASK_QUIET needs MGDS_DATA (the re-asks it quiets are the data path's)"
#endif
#define MGDS_STALL_MS        35000u  /* owing frames this long, no client frame: end with the link */
#define MGDS_UPDATE_IDLE_MS  120000u /* an update part-way, no client frame this long: end with the link */
#if MGDS_DATA && !MGDS_SEALED_DATA
#error "MGDS_DATA needs MGDS_SEALED_DATA (a client message's tag is checked before the port sees it)"
#endif
#if MGDS_DATA && MGDS_TX_CTR_U8
#error "MGDS_DATA needs the uint32_t device counter (MGDS_TX_CTR_U8=0): every data message takes one"
#endif
#if MGDS_TX_CTR_U8
typedef uint8_t mgds_txctr_t;
#define MGDS_TX_CTR_LAST 0xFFu
#else
typedef uint32_t mgds_txctr_t;
#define MGDS_TX_CTR_LAST 0xFFFFFFFFu
#endif

enum { MGDS_SENT = 0, MGDS_AGAIN = 1 }; /* send(): MGDS_AGAIN = no buffer now */
enum { MGDS_OK = 0, MGDS_NONE = -1, MGDS_FAIL = -2 };
enum { MGDS_CLEAR_PENDING = 2 }; /* port.clear(): the erase runs on; mgds_clear_done() reports it */

#if MGDS_DATA
/* The data path's state while the device can carry VM data (MGDS_DATA), at
 * the port's fixed address (mgds_port_t.data): lent by the board with
 * mgds_data_attach(), taken back with mgds_data_detach(). 72 bytes on rv32.
 * Layer 2 owns its contents while attached; the board sets nothing in it. */
typedef struct mgds_data {
	const uint8_t *tx_msg; /* the port's message going out (mgds_send), or NULL */
	uint32_t tx_len;       /* its plaintext length */
	uint32_t tx_off;       /* its wire bytes sent (sealed: counter || ciphertext || tag) */
	uint32_t tx_msg_ctr;   /* its device -> client counter (sealed: reserved when its tag is made) */
	uint8_t tx_tag[MGSEAL_TAG_LEN];
	uint32_t client_max, client_window; /* the client's hello (exact, or the rounded copy in mgds_t) */
	uint32_t send_credit;  /* bytes the client can take now */
	uint32_t credit_owed;  /* credit not yet sent (the stack had no buffer) */
	uint32_t opened_at;    /* when the client's open session opened (with DF_LIVE) */
	uint8_t *buf;          /* the reassembly buffer (mgds_data_attach), or NULL: pieces */
	uint32_t cap;          /* its size */
	uint32_t env_val;      /* the client message's envelope check: the varint so far, or the bytes left to skip */
	uint16_t epoch;        /* the client's last epoch */
	uint16_t data_cred;    /* bytes of the client message in flight already given back */
	uint8_t vm_state;      /* the client's last state (mg_session_state_t) */
	uint8_t flags;         /* DF_* in the source */
	uint8_t env_st, env_n; /* the envelope check's state and varint length (ENV_* in the source) */
} mgds_data_t;
#endif

typedef struct mgds_port {
	void *ctx;
	uint32_t max_message, window; /* this device's hello (window <= max_message <= 65535) */
	/* One Control notification. MGDS_SENT, MGDS_AGAIN (the same frame is
	 * offered again from the next mgds_poll()), or < 0 (the link is gone). */
	int (*send)(void *ctx, const uint8_t *frame, size_t len);
	uint16_t (*mtu)(void *ctx);   /* the connection's ATT MTU */
	bool (*encrypted)(void *ctx); /* the link is encrypted (a bond); unused: every unsealed hello is refused */
	bool (*recovery)(void *ctx);  /* a recovery mode */
	bool (*proven)(void *ctx);    /* a token proof matched on this connection (mgtp_matched) */
	/* The matching proof's nonces, once per proof (mgtp_take_nonces). */
	bool (*take_nonces)(void *ctx, uint8_t client_nonce[16], uint8_t device_nonce[16]);
	/* K: MGDS_OK; MGDS_NONE without a pair (or with a placeholder pair). */
	int (*get_k)(void *ctx, uint8_t k[32]);
	int (*node_id)(void *ctx, char node[MGDS_NODE_MAX], uint8_t *len);
	/* The pair, which 0 access, 1 refresh: its length, and n <= 32 bytes at off. */
	int (*token_len)(void *ctx, int which, uint16_t *len);
	int (*token_read)(void *ctx, int which, uint16_t off, uint8_t *buf, size_t n);
	bool (*busy)(void *ctx); /* media runs: answer a hello, commit an update later */
	/* need bytes for an incoming credentials_update (or a sealed clear in fragments, 21), or NULL (held
	 * elsewhere). */
	uint8_t *(*borrow)(void *ctx, size_t need);
	void (*release)(void *ctx); /* gives it back (wiped) */
	/* Replaces the pair in one commit (K and node_id unchanged). MGDS_OK:
	 * stored; anything else: the old pair stands. */
	int (*commit)(void *ctx, const char *access, uint16_t access_len, const char *refresh,
		      uint16_t refresh_len);
	/* Optional (NULL: the board keeps no expiry; these two go together):
	 * access_expires_at, unix seconds, stored with the pair. expires() gives
	 * the stored one for the credentials (true; false or 0: none, and the
	 * credentials carry none). commit_expires() replaces commit(): the pair
	 * and the update's access_expires_at (0: the update carried none) in one
	 * commit. A client treats credentials without it as expired, so a board
	 * that sets mg_session_cap_client_refreshes and keeps no expiry is
	 * refreshed (and commits) on every connection. */
	bool (*expires)(void *ctx, uint64_t *at);
	int (*commit_expires)(void *ctx, const char *access, uint16_t access_len, const char *refresh,
			      uint16_t refresh_len, uint64_t at);
	/* The sealed clear (mg_session_clear, a Forget in a sealed session): erases the tokens, K and the setup
	 * markers and returns the gadget to Link setup, as the token proof's clear() does. Called as soon as the
	 * clear's tag checks (behind credentials still going out, and without MGDS_KEEP_KEYS behind anything going
	 * out: those need the stored pair or K); only the answer waits for what goes out before it. MGDS_OK: erased; MGDS_CLEAR_PENDING: the erase runs
	 * on (a flash erase off this context), the port calls mgds_clear_done() when it ends; anything else: it
	 * failed. The device answers a sealed [1] once erased ([0] if the erase failed) and ends the session itself,
	 * so this must not reset the session. Optional (NULL: the sealed clear is refused unsupported). */
	int (*clear)(void *ctx);
#if MGDS_DATA
	/* ---- MGDS_DATA: the device's VM client (all called from mgds_rx / mgds_poll / mgds_reset /
	 * mgds_data_detach; each may call mgds_send, and data_state / data_sent also mgds_data_attach /
	 * _detach) ---- */
	/* The VM session's state (mg_session_state_t) and epoch: every state the
	 * client reports (open with a new epoch starts a session: open your
	 * streams from the start; any other ends it: drop every stream, partial
	 * message and pending request, replay nothing), with or without the lent
	 * state, and closed when layer 2 ends a session that was open (or, lent
	 * state attached, connecting: its epoch; else 0): a reset, a new hello, a
	 * lost link. Optional (NULL). */
	void (*data_state)(void *ctx, uint8_t state, uint16_t epoch);
	/* The data path's state (mgds_data_t, 72 bytes on rv32) at a fixed
	 * address, layer 2's only between mgds_data_attach() and
	 * mgds_data_detach() (a static on a board with RAM to spare, else e.g. a
	 * fixed spot in a lease the board holds while the VM session runs). NULL:
	 * no data path (as if never attached). */
	mgds_data_t *data;
	/* Client messages (only while a mgds_data_t is attached and live: see
	 * mgds_data_attach), one of two ways, chosen by the buffer given to
	 * mgds_data_attach():
	 *  - reassembled (a buffer of >= max_message bytes: a message is
	 *    reassembled there sealed, opened in place): data_message(), one call
	 *    per whole message after its tag is checked: the plaintext (at most
	 *    max_message - 20 bytes sealed, max_message plain), valid only during
	 *    the call (it is wiped after), its credit already given back;
	 *  - in pieces (no buffer): data_piece() with plaintext bytes [off, off +
	 *    n) of a message of len plaintext bytes, in order, each piece at most
	 *    64 bytes, then data_end(ok): true, the whole message arrived and its
	 *    tag matched; false, drop everything since off 0 (a bad tag, an abort,
	 *    a refused fragment, a state change, a reset, a detach). In a sealed
	 *    session the pieces are decrypted before the tag is checked: act on a
	 *    message only at data_end(true).
	 * Either way only a message whose plaintext decodes as a
	 * ServiceResponseEnvelope (noise_core ServiceCodec's rules) is handed
	 * over: anything else gets [session, invalid_value], its credit back and
	 * its counter used (in pieces: data_end(false)). */
	void (*data_message)(void *ctx, const uint8_t *msg, uint32_t len);
	void (*data_piece)(void *ctx, uint32_t off, const uint8_t *piece, size_t n, uint32_t len);
	void (*data_end)(void *ctx, bool ok);
	/* The message given to mgds_send() went out whole (ok: its last fragment
	 * was taken by the stack), or was dropped (false: a state other than
	 * open, a reset, a new hello, the link lost, a seal failure); its buffer
	 * is the port's again. Not called for mgds_send_abort() or a detach.
	 * Optional. */
	void (*data_sent)(void *ctx, bool ok);
#endif
#if MGDS_STALL
	/* MGDS_STALL: drop the Bluetooth link now (a stall or an idle update
	 * part-way ended the session; it is already reset, link_up true). Called
	 * from mgds_poll(); the board's disconnect then calls mgds_reset(t, false)
	 * as usual. NULL: the stall rules are off. */
	void (*end_link)(void *ctx);
#endif
} mgds_port_t;

/* A sealed data message's tag, as its fragments arrive. */
typedef struct mgds_dtag {
	mgseal_tag_t tag;
	uint8_t rx[MGSEAL_TAG_LEN]; /* its tag bytes */
} mgds_dtag_t;

#if MGDS_STATS
typedef struct mgds_stats { /* saturating counters for a console; no secrets */
	uint8_t hellos, sealed, refused, aborted, deferred, dropped, saved, not_saved, bad_updates;
	uint8_t data_in, bad_data; /* client data messages dropped whole; refused (insecure, counter, tag, overrun) */
	uint8_t last_refusal : 5; /* mg_error_code of the last refusal, 0 none (codes are 1..8) */
	uint8_t ctr_ends : 3;     /* sessions ended by MGDS_CTR_END counter refusals in a row (saturating at 7; shares
	                           * last_refusal's byte: mgds_t's size unchanged) */
} mgds_stats_t;
#endif


typedef struct mgds {
	const mgds_port_t *p;
	uint8_t *in;           /* lent storage for a credentials_update */
	uint32_t defer_at;     /* when the deferred hello / commit started */
	uint32_t rx_counter;   /* the next client -> device counter (credentials_update and data share it) */
	uint16_t out_len;      /* credentials bytes on the wire (sealed: counter + plaintext + tag) */
	uint16_t out_off;      /* how many went out */
	uint16_t in_total, in_got;
	uint16_t data_total, data_got;
#if MGDS_KEEP_KEYS
	mgseal_keys_t keys;    /* this session's seal keys (derived once, at its sealed hello) */
#else
	uint8_t nonces[32];    /* this session's proof nonces (taken at its sealed hello) */
#endif
	uint8_t tag[MGSEAL_TAG_LEN];
	unsigned state : 3;    /* the session's state (S_* in the source) */
	unsigned saved_sealed : 1; /* the client's sealed hello carried mg_session_hello_flag_sealed_saved */
	unsigned owe0 : 4;     /* give-up [0] answers waiting behind the credentials or the answer going out (<= 15) */
	uint8_t flags;
	uint8_t rx_hole;       /* 0, or rx_counter - a counter refused at its tag after a later one was taken (an update
	                        * and a data message in flight together): that counter may still come, once */
#if MGDS_DATA
	uint8_t ra;            /* MGDS_DATA: the re-ask flags (RA_* in the source), in padding; the waiting re-ask's time
	                        * shares data_ctr (free while the VM session isn't open). The rest of the data path's
	                        * state is the port's mgds_data_t: mgds_t doesn't grow on rv32 */
#endif
	/* A sealed session's credentials_saved. In state open, out_len != 0 means one is going out:
	 * its plaintext (MGSEAL_SAVED_LEN bytes) in tag[], free once the credentials are out, and out_off its progress;
	 * sealed again for each fragment (the same bytes). owe0 sits beside state in its byte, tx_ctr in padding. */
#if MGDS_TX_CTR_U8
	mgds_txctr_t tx_ctr;   /* the next device -> client counter (the credentials took 0); ends the session after 255 */
#endif
#if MGDS_SEALED_DATA
#if MGDS_TAG_LEND
	mgds_dtag_t *dt;       /* lent for the sealed data message in flight, or NULL */
#else
	mgds_dtag_t dt_own;
#endif
	uint32_t data_ctr;     /* that message's counter */
	uint8_t data_bad : 2;  /* 0, or why its tag can't match: 1 the keys failed, 2 no tag state lent */
	uint8_t ctr_bad : 2;   /* MGDS_CTR_END: counter refusals in a row in this session (in data_bad's byte: mgds_t's
	                        * size unchanged) */
	uint8_t tag_live : 1;  /* the tag state was begun and not checked: abandoning it releases its hash */
	uint8_t rx_top : 1;    /* client counter 0xFFFFFFFF was taken (rx_counter wrapped to 0): counters never wrap, so
	                        * no counter is the next one in this session (in data_bad's byte) */
	uint8_t nudge : 1;     /* mgds_nudge(): a credit goes even with none owed, from the next poll if the stack had no
	                        * buffer (in data_bad's byte) */
	uint8_t tx_back : 1;   /* a sealed clear dropped the credentials before they started: their counter 0 went back,
	                        * so tx_ctr 0 is the next counter, not a spent session (in data_bad's byte) */
#endif
#if MGDS_STATS
	mgds_stats_t st;
#endif
#if MGDS_DATA
	/* In the padding before tx_ctr (the struct keeps its size). */
	uint8_t ra_steps;      /* each re-ask series' asks so far: closed bits 0-2, no_network bits 3-5 (saturating);
	                        * bit 6: the port's mgds_data_t is attached; bit 7: mgds_ask_quiet() */
	uint8_t lim_max;       /* the client's hello, rounded down to powers of two (log2), for a mgds_data_t attached */
	uint8_t lim_win;       /* after the hello: max_message and window (never more than the client allows) */
#endif
#if !MGDS_DATA
	uint16_t credit_owed;  /* credit the stack had no buffer for (at most the window), from the next poll; in the
	                        * padding the data path's three bytes take with MGDS_DATA (its credit is in mgds_data_t) */
#endif
#if !MGDS_SEALED_DATA
	uint8_t rx_top : 1;    /* as with MGDS_SEALED_DATA (there bits beside data_bad), in padding */
	uint8_t nudge : 1;
	uint8_t tx_back : 1;
#endif
#if !MGDS_TX_CTR_U8
	mgds_txctr_t tx_ctr;   /* the next device -> client counter (the credentials took 0) */
#endif
#if MGDS_STALL
	uint32_t rx_at;        /* MGDS_STALL: the last session frame (mgds_rx) or wake: an update's 2 min */
	uint32_t owe_at;       /* owing began, or the last client write or wake since: the 35 s clock */
	uint8_t owing;         /* it owes them now (as of the last mgds_rx / mgds_poll) */
#endif
} mgds_t;

void mgds_init(mgds_t *t, const mgds_port_t *p);
/* One Control write whose first byte is mg_command_session; now in ms. */
void mgds_rx(mgds_t *t, const uint8_t *frame, size_t len, uint32_t now);
/* Sends what is owed, retries deferred work, ends what waited too long. Call
 * every main-loop pass (or tick) while mgds_busy(). */
void mgds_poll(mgds_t *t, uint32_t now);
/* The session ends: a disconnect (link_up false: nothing is sent), a setup
 * reset or commit, a new token proof challenge (link_up true: abort goes out
 * if credentials were part-way). The storage goes back; the keys are wiped.
 * A sealed clear taken and not yet run runs here (port.clear), so at a setup
 * commit call it before the new set is stored, never after. */
void mgds_reset(mgds_t *t, bool link_up);
/* The lent storage is needed elsewhere now: a credentials_update arriving in
 * it is discarded (sealed: answered [0] at its last fragment), a held one is
 * dropped (sealed: answered [0], at once or, with MGDS_GIVE_UP_LATER, from the
 * next mgds_poll()). */
void mgds_release_storage(mgds_t *t);
bool mgds_busy(const mgds_t *t); /* owes frames, holds storage or deferred work */
/* port.clear() returned MGDS_CLEAR_PENDING and the erase has ended (erased: the tokens, K and the setup markers are
 * gone): the sealed [1] ([0] if not erased) goes out, once what went out before the clear is out, and the session
 * ends. Once that is out and the erase still runs, the session holds: every session frame is refused invalid_value
 * and no data goes out. Ignored when no clear is pending (the link dropped
 * or the session ended while the erase ran: the erase still completes, nothing is sent). It has no handle: it
 * completes whichever clear is pending now, so the port keeps one erase at a time and binds it to its session: after
 * a mgds_reset() the erase still running belongs to no session (don't report it here), a newer clear() waits for it
 * (return MGDS_CLEAR_PENDING and start that erase after it), and a setup commit waits for it too (else the old erase
 * wipes the new pair). Call it from the context that calls mgds_rx() / mgds_poll(). */
void mgds_clear_done(mgds_t *t, bool erased);
/* The keepalive (mgcommands.h, Credit): one credit for what the device owes (messages it took whole
 * whose credit the stack had no buffer for; a message still arriving earns none until its end), or [session, credit,
 * 0] when it owes none, on a session the client reported open (state open, after the hello and credentials). Credit
 * is never sealed nor forwarded, so it reaches the client and nothing past it (the VM sees nothing; both Muse apps
 * take a zero credit silently), and it is a Bluetooth event: iOS wakes a suspended app for it, which then reads its
 * socket (a VM conversation the gadget shows). A board calls it every MGDS_NUDGE_MS while
 * such a view is shown, and stops when it closes or the screen sleeps. Nothing without an open session (before the
 * hello, before state open, after any other state, while a sealed clear is answered). The stack without a buffer: it
 * goes from the next mgds_poll() (mgds_busy() until then), unless the session ends first. Call it from the context
 * that calls mgds_rx() / mgds_poll(). */
void mgds_nudge(mgds_t *t);
#define MGDS_NUDGE_MS 20000u /* the keepalive's cadence while the view is shown */
#if MGDS_STALL
/* MGDS_STALL: a client write on this link that isn't a session frame (a Control
 * command, Data): it restarts the 35 s stall clock, not an update's 2 min. */
void mgds_client_frame(mgds_t *t, uint32_t now);
/* MGDS_STALL: the board woke from a sleep with the link up (mgds_poll didn't
 * run meanwhile): both stall clocks restart from now. Call it from the sleep's
 * exit, before the next mgds_poll(). */
void mgds_woke(mgds_t *t, uint32_t now);
#endif

#if MGDS_DATA
/* ---- MGDS_DATA: the VM client's calls (the same context as mgds_rx / mgds_poll) ---- */

/* The re-ask series (mgcommands.h, Session state; mg-session-v1.json). */
#define MGDS_ASK_FIRST_MS  1000u  /* a series' second ask; each next one doubles */
#define MGDS_ASK_CLOSED_MS 60000u /* the cap after closed */
#define MGDS_ASK_NONET_MS  15000u /* the cap after no_network */
#define MGDS_ASK_STABLE_MS 60000u /* a session open this long restarts both series at its end */

/* Lends the data path's state at the port's data (and, for reassembled client
 * messages, buf: cap >= this device's max_message bytes; NULL for pieces;
 * ignored without port data). It is wiped; the client's hello limits come
 * from mgds_t (rounded down to powers of two) if the hello was answered
 * before (exact when attached at the hello). Data flows from the next state
 * the client reports while it is attached (every state resets credit): attach before the
 * session opens (before mgds_open_request(true), or at the latest inside the
 * data_state() callback for open); one attached while the session is
 * already open carries nothing until the next state. Attached again: a
 * fresh state (mgds_data_detach() first, without waiting for it; an abort or
 * credit that detach still owes goes from the next mgds_poll()). */
void mgds_data_attach(mgds_t *t, uint8_t *buf, uint32_t cap);
/* Takes it back. true: detached (the memory is the board's again, wiped). false:
 * something was part-way or owed (the port's message part-way out: dropped,
 * abort owed; a client message in flight: dropped, its credit owed; credit
 * owed) and the stack had no buffer for it now: call again after the next
 * mgds_poll(). Not from data_message(), data_piece() or data_end() (nor
 * attach). Without it (never attached, or detached): every state is
 * still reported and the re-ask series run (their state is in mgds_t),
 * client data is dropped and credited back (as with MGDS_DATA 0),
 * mgds_can_send() is false, and a session open then counts as short for the
 * series (its open time is in the lent state). Boards with RAM to spare
 * attach once, after mgds_init(). */
bool mgds_data_detach(mgds_t *t);

/* The largest message mgds_send() takes now (plaintext bytes: the client's
 * max_message, less MGSEAL_OVERHEAD in a sealed session); 0 without a
 * session or without a live mgds_data_t. */
uint32_t mgds_send_max(const mgds_t *t);
/* mgds_send() would take a message of len plaintext bytes now: the state is
 * open, the credentials are out, a mgds_data_t is attached and saw the
 * session open, no message of the port's is going out, no sealed clear is
 * taken (its answer goes next, then the session ends), and len is
 * 1..mgds_send_max(). The client's credit doesn't count: a message
 * larger than its window goes out as credit comes back. */
bool mgds_can_send(const mgds_t *t, uint32_t len);
/* One message (a ServiceRequestEnvelope) to the VM: MGDS_OK, taken: msg must
 * stay valid and unchanged until data_sent() (or mgds_send_abort()); it goes
 * out from the next mgds_poll() (or the end of the mgds_rx() it was called
 * from), sealed in a sealed session under the next device -> client counter
 * (taken as its first fragment is made, shared with credentials_saved),
 * fragmented to the MTU and MGDS_FRAME_MAX, within the client's credit.
 * MGDS_FAIL: not taken (!mgds_can_send()), nothing used. */
int mgds_send(mgds_t *t, const uint8_t *msg, uint32_t len);
/* Drops the port's message: if part of it went out, abort goes out (its
 * counter is used on both sides); if none did, it takes no counter. No
 * data_sent() for it. */
void mgds_send_abort(mgds_t *t);
/* open_request [1] (open) or [0] (close), from the next poll. [0] stops the
 * re-asks until a hello, a saved credentials_update, an open or an
 * open_request [1]; [1] drops a re-ask waiting (it asks now). Ignored
 * before the session's hello, and [1] while the state is open. Layer 2 asks
 * again by itself after a closed or no_network it didn't ask for (the port
 * asks once per need). */
void mgds_open_request(mgds_t *t, bool open);
/* The session only for an active task: the re-ask waiting for its time
 * (after a closed or no_network the
 * device didn't ask for) dropped without sending anything; the series keeps
 * its step. A port with no active task calls it from its data_state
 * callback (layer 2 schedules the re-ask before it), and when a task ends
 * with the session down; the next task asks with mgds_open_request(true).
 * mgds_open_request(false) would send [0], a write that wakes a suspended
 * iOS app. Default (never called): no hold. mgds_ask_pending(): a
 * re-ask waits. (A board's API, taken unchanged.) */
void mgds_ask_hold(mgds_t *t);
bool mgds_ask_pending(const mgds_t *t);
/* The quiet bit ("no active task"): on, layer 2 asks nothing by itself until
 * mgds_open_request(true), mgds_task_start() or mgds_ask_quiet(t, false): no
 * re-ask after a closed or no_network it didn't ask for (not scheduled, the
 * series keeps its step) and no ask behind a saved credentials_update; a
 * re-ask waiting and a [1] layer 2 owes are dropped silently (the port's [0]
 * still goes). It survives a session's end, a hello and a save: a port with
 * no active task sets it once (at init, and when its task ends) instead of
 * calling mgds_ask_hold() from every data_state. mgds_init(): off (a port
 * that never calls it always has a task). */
void mgds_ask_quiet(mgds_t *t, bool on);
/* A task starts (a user action, traffic the device owes, a wait on the
 * VM): quiet off, and with the hello done and the VM session down layer 2
 * asks under the series rules, at once if the ended kind's series hasn't
 * asked yet, else at that series' next step (not after not_enrolled,
 * auth_failed, too_large or the port's [0]). mg_session_wake from the client
 * starts one too (after the hello; before it, wake is ignored). The task
 * ends with mgds_ask_quiet(t, true). mgds_open_request(true) instead asks at
 * once, outside the series. */
void mgds_task_start(mgds_t *t, uint32_t now);
/* The client's last state (mg_session_state_t; closed before any) and its
 * epoch; without the lent state: open or closed, as mgds_ready(), and epoch
 * 0 (attached since: the epoch of the states it has seen, 0 before one). */
uint8_t mgds_vm_state(const mgds_t *t, uint16_t *epoch);
#endif

/* mgds_ready(): the client reported its VM session open and this device's
 * hello and credentials are out (any build; no code unless used): for
 * boards, instead of reading the core's private state. */
#define MGDS_STATE_OPEN   4u    /* mgds_t.state: hello and credentials done (S_OPEN in the source) */
#define MGDS_FLAG_VM_OPEN 0x40u /* mgds_t.flags: the client reported state open (F_VM_OPEN in the source) */
static inline bool mgds_ready(const mgds_t *t)
{
	return (t->flags & MGDS_FLAG_VM_OPEN) != 0 && t->state == MGDS_STATE_OPEN;
}

#if MGDS_SEALED_DATA && MGDS_TAG_LEND
/* Defined by the board with MGDS_TAG_LEND: the tag state for one sealed data
 * message (ctx: the port's), or NULL; and taking it back (wiped). One at a
 * time; mgds_reset() returns one in flight. */
mgds_dtag_t *mgds_tag_lend(void *ctx);
void mgds_tag_return(void *ctx, mgds_dtag_t *d);
#endif

#ifdef __cplusplus
}
#endif

#endif
