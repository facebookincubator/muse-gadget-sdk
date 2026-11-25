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

#ifndef MG_LINK_SETUP_H_
#define MG_LINK_SETUP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mgls_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The device side of Muse Link setup v5 for BLE Muse Gadgets, per the Muse
 * Gadget setup contract v2: the setup service's RX/TX characteristics with
 * 0xFE chunk framing, community pairing v5 (P-256 ECDH, HKDF-SHA256,
 * AES-256-GCM records, policy confirm_press), get_device_info with
 * wifi "none" and mgcommands 1, the physical-press confirmation,
 * pairing_confirmed with sdk_token, and token-only provision_v2 committed
 * through the platform before auth_ok. A set-up gadget accepts a new
 * pairing_client_hello (re-provisioning, contract 7).
 *
 *   get_device_info -> device_info
 *   pairing_client_hello -> pairing_ready                (60 s for:)
 *   encrypted pairing_client_finished -> confirm_required (the prompt: until a press)
 *   press -> pairing_confirmed [+ sdk_token]             (120 s for:)
 *   provision_v2 -> commit(tokens, K, setup marker, Wi-Fi skipped) -> auth_ok
 *
 * C99, no allocation, no threads, no OS calls. Every entry point runs to
 * completion on the caller's context; call them all from one context (the
 * main loop, or a task that serializes them). mgls_rx() only copies the
 * write into the workspace: decryption, ECDH and the commit run inside
 * mgls_poll(), so a GATT write callback stays short.
 *
 * A session provisions once: after auth_ok it is consumed, and a further
 * provision_v2 on it gets the encrypted error_pairing_confirm_required (a
 * new setup needs a new hello and press). A provision_v2 that failed
 * (nothing committed) doesn't consume it: a retry on the same session is
 * taken (within its window; error_storage also ends the connection 500 ms
 * later, as the reference firmware).
 *
 * Memory: mgls_t (36 B on a 32-bit target: Arm Cortex-M, RV32; 48 B on a 64-bit host) is all the permanent RAM. Everything else, the
 * session keys included, lives in a workspace the firmware lends through
 * borrow() only while the setup service is in use: from the first RX write
 * until the session can't continue (a plaintext-only exchange, a timeout or
 * error), the client disconnects, or the firmware calls mgls_abort(). After
 * auth_ok the consumed session keeps it (to answer as above) until one of
 * those. It comes back wiped through release(). A firmware can lend a media
 * buffer that is idle while setup runs and take it back with mgls_abort().
 */

/* ---- Protocol constants ------------------------------------------------ */

#define MGLS_SERVICE_UUID "7FDD3D1C-38EA-46CF-8B46-314ECF5F240C"
#define MGLS_RX_UUID      "4D593029-28A2-4A6E-A1F0-3C2D5E8F9B01" /* write, write w/o rsp */
#define MGLS_TX_UUID      "D75DC4CA-7B2B-4E9C-8F0A-1D2E3F4A5B6C" /* read, notify (CCCD) */
/* The same UUIDs as little-endian byte lists, as most BLE stacks take them. */
#define MGLS_SERVICE_UUID_LE                                                                       \
	0x0c, 0x24, 0x5f, 0xcf, 0x4e, 0x31, 0x46, 0x8b, 0xcf, 0x46, 0xea, 0x38, 0x1c, 0x3d, 0xdd, 0x7f
#define MGLS_RX_UUID_LE                                                                            \
	0x01, 0x9b, 0x8f, 0x5e, 0x2d, 0x3c, 0xf0, 0xa1, 0x6e, 0x4a, 0xa2, 0x28, 0x29, 0x30, 0x59, 0x4d
#define MGLS_TX_UUID_LE                                                                            \
	0x6c, 0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x0a, 0x8f, 0x9c, 0x4e, 0x2b, 0x7b, 0xca, 0xc4, 0x5d, 0xd7

/* Phase timeouts (ms), as the reference firmware. The confirm prompt has none: it stays up until the press or
 * tap, a dismiss (mgls_dismiss()), or the phone giving up (a disconnect, a new hello). The transcript's
 * confirm_timeout_seconds (60) is how long the phone waits. */
#define MGLS_CLIENT_FINISHED_MS (60u * 1000u)
#define MGLS_CONFIRMED_IDLE_MS  (120u * 1000u)
#define MGLS_PROVISIONING_MS    (120u * 1000u)

/* Link setup advertisement manufacturer data: company 0xFFFF, one flags
 * byte (contract 2.1). */
#define MGLS_ADV_FLAG_SET_UP 0x01u
#define MGLS_ADV_FLAG_GADGET 0x02u

/* Return codes of the platform callbacks. */
#define MGLS_OK    0
#define MGLS_AGAIN 1 /* notify(): no buffer free right now; retried at the next poll */
#define MGLS_E_CONFIG (-2) /* mgls_init(): a required hook is missing (set_up; may_prompt in a release build) */

/* ---- Platform interface ------------------------------------------------ */

/* What provisioning hands to commit(). The token strings live in the
 * workspace and are valid only during the call; they are not
 * NUL-terminated. */
typedef struct mgls_credentials {
	const char *access_token;
	size_t access_len;
	const char *refresh_token;
	size_t refresh_len;
	const uint8_t *k; /* the token proof key K, 32 bytes */
	uint8_t flags;    /* MGLS_CRED_* */
} mgls_credentials_t;

#define MGLS_CRED_SETUP_COMPLETE 0x01u
#define MGLS_CRED_WIFI_SKIPPED   0x02u /* token-only: setup_wifi=skipped */

enum mgls_event {
	/* pairing_ready went out: a phone is pairing. */
	MGLS_EV_SESSION_STARTED = 1,
	/* confirm_required went out and the prompt is up (from the first
	 * mgls_poll() after it went out): show "Tap to pair" or "Press <key>
	 * to pair" (mgls_confirm_armed() is true until it ends). */
	MGLS_EV_CONFIRM_ARMED,
	/* The press confirmed: pairing_confirmed is going out. */
	MGLS_EV_CONFIRMED,
	/* pairing_confirmed goes out without sdk_token: the firmware holds none.
	 * Show or log that the gadget has no SDK token (contract 5). */
	MGLS_EV_NO_SDK_TOKEN,
	/* The prompt was dismissed (mgls_dismiss()), or no provision_v2 came
	 * within 120 s of the confirm. */
	MGLS_EV_CONFIRM_TIMEOUT,
	/* commit() succeeded and auth_ok is going out. The old token set and K
	 * are gone: reset the token proof on every live mgcommands connection
	 * (mgtp_reset()) and advertise as set up (flags 0x03). Once mgls_idle()
	 * is true auth_ok has gone out, and mgls_abort() may take the lent
	 * workspace back (e.g. for media). */
	MGLS_EV_PROVISIONED,
	/* commit() failed: error_storage went out; the old set stays. */
	MGLS_EV_STORAGE_FAILED,
	/* An encrypted unpair erased the set (erase() succeeded). Reset the
	 * token proof (mgtp_reset()) and any device session (mgds_reset()) on
	 * every live mgcommands connection and advertise as not set up (flags
	 * 0x02). */
	MGLS_EV_UNPAIRED,
	/* A session ended without provisioning (timeout, error, a new hello,
	 * disconnect): drop any pairing prompt. */
	MGLS_EV_SESSION_ENDED,
};

typedef struct mgls_config {
	void *ctx; /* passed to every callback */

	/* Identity. bd_addr is the identity address, most significant byte
	 * first (as printed). Strings left NULL are derived from it the way the
	 * reference firmware does: mac "aa:bb:cc:dd:ee:ff" (lowercase),
	 * device_id "hatch-link:<mac>", node_id "homelink-" + the last three
	 * bytes in lowercase hex. Each string is at most MGLS_ID_MAX bytes. */
	const uint8_t *bd_addr;
	const char *node_id;
	const char *device_id;
	const char *mac;
	const char *firmware_version;
	/* device_info.mgcommands: 1 when the gadget serves the musegadgets
	 * service (MG_SERVICE_UUID), 0 to omit the field. wifi is always
	 * "none": this core provisions tokens only. */
	uint8_t mgcommands;

	/* Lends the workspace: at least `need` (= sizeof(mgls_workspace_t))
	 * bytes, 4-byte aligned, or NULL if it can't now (the write is then
	 * dropped and the phone times out). Kept until release(), which gets it
	 * back already wiped. */
	void *(*borrow)(void *ctx, size_t need);
	void (*release)(void *ctx, void *ws);

	/* One notification on the TX characteristic. The callee must copy the
	 * bytes before returning. MGLS_OK: queued. MGLS_AGAIN: no buffer now
	 * (the core retries from the same chunk at the next mgls_poll()). < 0:
	 * failed (not connected or not subscribed): the message is dropped. */
	int (*notify)(void *ctx, const uint8_t *data, size_t len);
	/* Optional, for a board whose stack takes more than one connection at a time (mgls_conn_*()): as notify, to
	 * connection conn. Set, the core uses it for every notification (its followed connection's, mgls_conn(), and
	 * the busy answer to another) and doesn't call notify. */
	int (*notify_to)(void *ctx, uint16_t conn, const uint8_t *data, size_t len);
	/* Ends the current connection, mgls_conn() (the core then expects
	 * mgls_disconnected()). */
	void (*disconnect)(void *ctx);
	/* Cryptographically secure random bytes. 0 on success. Also feeds the
	 * builtin P-256 (key generation and Z randomization). */
	int (*random)(void *ctx, uint8_t *out, size_t len);

	/* Atomically replaces the stored set with this one: both tokens, K, the
	 * setup-complete marker and the Wi-Fi-skipped marker. Return 0 only once
	 * it is durable; on any failure return nonzero and leave the previous
	 * set (if any) exactly as it was. mgls_store.h implements this on two
	 * flash slots. */
	int (*commit)(void *ctx, const mgls_credentials_t *cred);
	/* Erases the stored set (encrypted unpair on a confirmed session). 0 on
	 * success. May be NULL: unpair then answers error_storage. */
	int (*erase)(void *ctx);
	/* The SDK token (mgst_...), or NULL / *len 0 without one. May be NULL. */
	const char *(*sdk_token)(void *ctx, size_t *len);

	/* UI and integration events (enum mgls_event). May be NULL. */
	void (*event)(void *ctx, enum mgls_event ev);
	/* Log lines: fmt is a printf format taking two longs (a, b), e.g.
	 * printf(fmt, a, b). Never carries a token or key. May be NULL. */
	void (*log)(void *ctx, const char *fmt, long a, long b);

	/* Required. The gadget holds a set (asked at each pairing_client_hello). True: the session is a re-setup (never
	 * confirmed by the talk key: mgls_press_key()), and provision_v2's ota_url is ignored. mgls_init() refuses a
	 * config without it (MGLS_E_CONFIG) and the core fails closed: it can't tell a set-up gadget from a new one, so
	 * every hello gets error_pairing_unavailable and nothing shows or confirms. A test build (MGLS_RELEASE 0) may
	 * pass MGLS_HOOK_TEST_DEFAULT(set_up): every setup a first setup. */
	bool (*set_up)(void *ctx);
	/* Asked at a valid pairing_client_hello of a set-up gadget (set_up() true) that would show a new prompt (none
	 * is up), before anything of it: since_ms is how long ago this connection's last re-setup prompt was dismissed
	 * (mgls_dismiss(), or the talk key through mgls_press_key(), at mgls_poll() times), MGLS_NO_PROMPT if none on
	 * this connection. False: the hello gets error_operation_in_progress in plaintext and shows nothing. A hello on
	 * the prompt's connection while it is up is never refused and never asks: it takes the prompt over (it stays on
	 * screen, the one press confirms the newest hello); one from another connection is busy (mgls_conn_rx()). A
	 * release build returns since_ms >= MGLS_RESETUP_PROMPT_MS, and requires it: mgls_init() refuses a config without
	 * it (MGLS_E_CONFIG, every hello refused). Test, bench and automation builds (MGLS_RELEASE 0) may leave it NULL
	 * or pass MGLS_HOOK_TEST_DEFAULT(may_prompt): every hello goes on. */
	bool (*may_prompt)(void *ctx, uint32_t since_ms);
	/* Set only by a board whose only key is its talk key: whether it is in setup mode now (booted with that key held,
	 * into its normal firmware; never SAFE). Setup mode ends at the confirmed re-setup (MGLS_EV_CONFIRMED) or 5 min
	 * after that boot; test and bench builds are always in it. Asked before may_prompt(), for a new re-setup prompt:
	 * false, the hello gets error_pairing_unavailable in plaintext and nothing shows. In setup mode that board
	 * reports its key as MGLS_KEY_OTHER (it isn't the talk key there). NULL (a board with another key or a touch
	 * screen): every re-setup hello goes on. */
	bool (*setup_mode)(void *ctx);
} mgls_config_t;

#define MGLS_NO_PROMPT         0xFFFFFFFFu /* may_prompt(): no dismissed re-setup prompt on this connection yet */
#define MGLS_RESETUP_PROMPT_MS 10000u      /* may_prompt(): a release build's least gap after a dismiss */

/* ---- Sizes -------------------------------------------------------------- */

#define MGLS__B64LEN(n) (((n) * 4 + 2) / 3)
#define MGLS__ALIGN4(n) (((n) + 3u) & ~3u)

/* RX: a pairing_encrypted envelope carrying the largest provision_v2, plus
 * room for the GCM tag after the ciphertext decoded in place and a NUL. */
#define MGLS_RX_CAP MGLS__ALIGN4(192 + MGLS__B64LEN(MGLS_PROVISION_MAX) + 16 + 1)

/* TX: pairing_ready with the longest identity strings, or an encrypted
 * status (with sdk_token) and its plaintext staged behind it; 3 bytes of
 * chunk header room in front. */
#define MGLS__ENC_PT_CAP  (62 + MGLS_SDK_TOKEN_MAX)
#define MGLS__ENC_MAX     (137 + MGLS__B64LEN(MGLS__ENC_PT_CAP) + MGLS__ENC_PT_CAP + 16)
#define MGLS__READY_MAX   (440 + 4 * MGLS_ID_MAX)
#define MGLS__MAX(a, b)   ((a) > (b) ? (a) : (b))
#define MGLS_TX_CAP       MGLS__ALIGN4(3 + MGLS__MAX(MGLS__READY_MAX, MGLS__ENC_MAX))

/* One parsed JSON member: offsets into the parse buffer. Private. */
typedef struct mgls_jfield {
	uint16_t key; /* NUL-terminated key */
	uint16_t val; /* decoded NUL-terminated string, or the raw number text */
	uint16_t len; /* string length, or number text length */
	uint8_t type;
	uint8_t pad;
} mgls_jfield_t;

/*
 * The lent workspace. Its fields are private; only its size matters to the
 * firmware: lend sizeof(mgls_workspace_t) bytes (about 4.4 KB with the
 * default limits).
 */
typedef struct mgls_workspace {
	/* session */
	uint8_t rx_key[32]; /* mobile -> device */
	uint8_t tx_key[32]; /* device -> mobile */
	char sid_b64[24];
	uint32_t rx_ctr, tx_ctr;
	/* reassembly */
	uint16_t rx_len;   /* bytes so far, or the complete message length */
	uint8_t rx_total;  /* chunks in the message being assembled */
	uint8_t rx_next;   /* next chunk index expected */
	uint8_t rx_state;  /* idle / assembling / complete / overflowed */
	/* transmission */
	uint8_t tx_kind;   /* none / chunked message / plaintext status */
	uint8_t tx_after;  /* what to do once it has all gone out */
	uint8_t tx_status; /* plaintext status index */
	uint16_t tx_len;   /* message length (at tx + 3) */
	uint16_t tx_usable;
	uint8_t tx_idx, tx_chunks;
	uint8_t nfields;
	uint8_t id_ready;
	/* identity strings derived from bd_addr */
	char id_node[16];
	char id_mac[18];
	char id_dev[30];
	mgls_jfield_t f[MGLS_JSON_MAX_FIELDS];
	uint8_t tx[MGLS_TX_CAP];
	uint8_t rx[MGLS_RX_CAP];
} mgls_workspace_t;

#define MGLS_WORKSPACE_SIZE (sizeof(mgls_workspace_t))

/* The permanent state. Private fields; allocate it statically. */
typedef struct mgls {
	const mgls_config_t *cfg;
	mgls_workspace_t *ws; /* NULL while not borrowed */
	uint32_t now;         /* the time of the last poll (ms) */
	uint32_t deadline;    /* the current phase's deadline */
	uint32_t disc_at;     /* a requested disconnect */
	uint16_t mtu;
	uint8_t phase;
	uint8_t flags;
	uint8_t gen;      /* changes with every phase and session reset */
	uint8_t disc_gen; /* the session a requested disconnect belongs to (0: any) */
	uint8_t last_status;
	uint8_t prompted;   /* prompt_at holds this connection's last dismissed re-setup prompt (may_prompt()) */
	uint8_t refused;    /* mgls_init() refused the config (MGLS_E_CONFIG): every hello gets error_pairing_unavailable */
	uint16_t conn;      /* the connection followed (mgls_conn_*(); 0 with the plain calls) */
	uint32_t prompt_at; /* when it was dismissed */
} mgls_t;

/* ---- API ---------------------------------------------------------------- */

/* Once at boot. cfg must stay valid (const, in flash) for the lifetime. MGLS_OK, or MGLS_E_CONFIG when a required
 * hook is missing (set_up; may_prompt in a release build): the core then fails closed (every pairing_client_hello
 * gets error_pairing_unavailable in plaintext, nothing shows) and the board's self-test must stop on it. */
int mgls_init(mgls_t *s, const mgls_config_t *cfg);

#if !MGLS_RELEASE
/* Test, bench and automation builds only: the hooks' hands-free defaults, named explicitly in the config
 * (.set_up = MGLS_HOOK_TEST_DEFAULT(set_up)). set_up: false (every setup a first setup); may_prompt: true (every
 * hello goes on). The names don't exist in a release build, so one can't ship them. */
bool mgls_test_default_set_up(void *ctx);
bool mgls_test_default_may_prompt(void *ctx, uint32_t since_ms);
#define MGLS_HOOK_TEST_DEFAULT(hook) mgls_test_default_##hook
#endif

/* Where the config is a compile-time constant: MGLS_STATIC_HOOKS(set_up_fn, may_prompt_fn) at file scope beside it
 * fails the build unless each is a function of its hook's shape (C can't test a function pointer's value
 * statically; NULL doesn't compile). */
#define MGLS__CAT2(a, b) a##b
#define MGLS__CAT(a, b)  MGLS__CAT2(a, b)
#define MGLS_STATIC_HOOKS(set_up_fn, may_prompt_fn)                                                          \
	typedef char MGLS__CAT(mgls_static_hooks_, __LINE__)[(sizeof((set_up_fn)((void *)0)) +               \
							       sizeof((may_prompt_fn)((void *)0, 0u))) > 0 ? 1 : -1]

/* A central connected / disconnected (any connection: the setup service is
 * registered all the time). now_ms is the platform's monotonic ms clock. */
void mgls_connected(mgls_t *s, uint32_t now_ms);
void mgls_disconnected(mgls_t *s);
/* A board whose stack takes more than one connection at a time names each (any id) and calls these instead of
 * mgls_connected(), mgls_disconnected(), mgls_mtu_changed() and mgls_rx() (which are connection 0), with
 * notify_to set. The core follows one connection, mgls_conn(): the first that connects, or another whose write
 * comes while no setup runs on the one followed. While one runs there (from its pairing_ready until its session
 * ends, a prompt included), a write from another connection is answered error_operation_in_progress in plaintext at
 * a message's first write and changes nothing: only a hello on the prompt's own connection takes the prompt over.
 * disconnect() and mgls_read_status() are about mgls_conn(). */
void mgls_conn_connected(mgls_t *s, uint16_t conn, uint32_t now_ms);
void mgls_conn_disconnected(mgls_t *s, uint16_t conn);
void mgls_conn_mtu_changed(mgls_t *s, uint16_t conn, uint16_t att_mtu);
void mgls_conn_rx(mgls_t *s, uint16_t conn, const uint8_t *data, size_t len);
uint16_t mgls_conn(const mgls_t *s);
/* The connection's ATT MTU changed (23 until called). */
void mgls_mtu_changed(mgls_t *s, uint16_t att_mtu);

/* A write on the RX characteristic (with or without response). */
void mgls_rx(mgls_t *s, const uint8_t *data, size_t len);

/*
 * Drives everything else: a received message, the press, pending
 * notifications, timeouts and delayed disconnects. Call it from the main
 * loop at least every ~50 ms while mgls_idle() is false, and right after
 * mgls_rx() / mgls_press() / a TX-complete event if convenient.
 */
void mgls_poll(mgls_t *s, uint32_t now_ms);

/* True when nothing is pending (no message or notification to handle, no
 * timer, no press): polling may stop until the next connect, write or press.
 * The workspace may still be lent (a consumed session after auth_ok, or a
 * chunked message the phone hasn't finished). */
bool mgls_idle(const mgls_t *s);

enum mgls_press {
	MGLS_PRESS_NONE = 0,   /* no pairing waits: the press is the firmware's */
	MGLS_PRESS_IGNORED,    /* a pairing waits but its prompt isn't up yet: swallow the press */
	MGLS_PRESS_CONFIRMING, /* the prompt is up: the next mgls_poll() sends pairing_confirmed */
	MGLS_PRESS_DISMISSED,  /* the talk key on a re-setup prompt: it dismissed the prompt (no talk turn) */
};

/* Which key a press came from (mgls_press_key()). */
enum mgls_key {
	MGLS_KEY_TALK = 1,  /* the push-to-talk key (on a one-key gadget, its only key outside setup mode) */
	MGLS_KEY_OTHER = 2, /* any other key: a dedicated confirm key, or the only key while in setup mode */
};

/*
 * A key was pressed. While the prompt is up (from the first mgls_poll()
 * after confirm_required went out) one press confirms a first setup, any
 * key; a re-setup only from a key that isn't the talk key (MGLS_KEY_OTHER):
 * the talk key dismisses a re-setup prompt instead (MGLS_PRESS_DISMISSED).
 * No hold, no press time and no edge check, whenever the press began.
 * Before the prompt is up it's swallowed, not remembered. While the result
 * is not MGLS_PRESS_NONE the press must not do anything else (push-to-talk,
 * menus). A board whose only key is the talk key re-sets up only in setup
 * mode: it sets config setup_mode(), and in setup mode reports that key as
 * MGLS_KEY_OTHER.
 */
enum mgls_press mgls_press_key(mgls_t *s, enum mgls_key key);

/* mgls_press_key(s, MGLS_KEY_TALK): a press of the talk key. A board with a dedicated confirm key reports it with
 * mgls_press_key(s, MGLS_KEY_OTHER). */
enum mgls_press mgls_press(mgls_t *s);

/* The prompt dismissed, a first setup's or a re-setup's: pairing_confirm_timeout from the next mgls_poll(), a drop,
 * MGLS_EV_CONFIRM_TIMEOUT; the stored set stays (the phone can start again). Only the back gesture (a swipe, or a
 * separate back key) or the phone cancelling calls it; on a re-setup the talk key dismisses too, through
 * mgls_press_key(). No tap ever dismisses: a tap on the Confirm disc (a generous hit area) confirms
 * (mgls_confirm_gesture()) and a tap elsewhere does nothing; every key press goes to mgls_press_key(). True if it
 * dismissed one; false with no prompt up or once it confirmed. */
bool mgls_dismiss(mgls_t *s);

/* A tap on the on-screen Confirm disc while mgls_confirm_armed(): confirms a first setup or a re-setup from the
 * next mgls_poll(). False: nothing armed. */
bool mgls_confirm_gesture(mgls_t *s);

/* The prompt is up: confirm_required went out, and a press confirms (now, or once a hello that took the prompt over
 * reaches its own confirm_required, without the prompt leaving the screen). The UI shows the pairing prompt while
 * this is true: from MGLS_EV_CONFIRM_ARMED until MGLS_EV_CONFIRMED, MGLS_EV_CONFIRM_TIMEOUT or
 * MGLS_EV_SESSION_ENDED (a takeover sends none of these). */
bool mgls_confirm_armed(const mgls_t *s);

/* A pairing session is in progress (pairing_ready sent, not yet provisioned
 * or ended). */
bool mgls_session_active(const mgls_t *s);

/* What a read of the TX characteristic returns: the last plaintext status,
 * or "encrypted_status". The length (not NUL-terminated). */
size_t mgls_read_status(const mgls_t *s, char *out, size_t cap);

/* Ends any session at once and gives the workspace back (wiped): before a
 * media stream needs the lent RAM (after a setup, once mgls_idle()), or at a
 * factory reset. A notification still queued is dropped. */
void mgls_abort(mgls_t *s);

/* Advertising (contract 2): the manufacturer flags byte, and a whole Link
 * setup advertisement (Flags 0x06, the complete 128-bit service UUID,
 * manufacturer data FF FF <flags>): 26 bytes. Returns the length, or 0 if
 * cap < 26. */
uint8_t mgls_adv_flags(bool set_up);
size_t mgls_adv_build(uint8_t *out, size_t cap, bool set_up);

/* "MuseGadget-<model>-XXXXXX" (MGLS_NAME_PREFIX, then the last three
 * address bytes in uppercase hex), e.g. "MuseGadget-XN54-1A2B3C" (22
 * chars). Returns the length, or 0 if it doesn't fit (cap includes the
 * NUL). In a legacy scan response (31 bytes) the Complete Local Name element
 * takes 2 + length bytes, so the name can be at most 29 chars (a model of up
 * to 11 chars) and a 22-char name leaves 7 bytes for other elements. */
size_t mgls_format_name(char *out, size_t cap, const char *model, const uint8_t bd_addr[6]);

#if defined(MGLS_ENABLE_SIMULATED_PRESS)
/* Bench builds only (a console ">pair.confirm"): as mgls_press_key(s, MGLS_KEY_OTHER). */
enum mgls_press mgls_simulated_press(mgls_t *s);
#endif

#if defined(MGLS_TEST_HOOKS)
/* Host tests: the next hello uses this P-256 private scalar. */
void mgls_test_set_ephemeral(const uint8_t priv[32]);
#endif

#ifdef __cplusplus
}
#endif

#endif
