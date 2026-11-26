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

#include "mg_envelope.h"
#include "mg_vm_session.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "mg_core.h"
#include "mg_json.h"
#include "mg_psa.h"
#include "mg_session.h"
#include "mg_session_seal.h"
#include "mg_setup.h"
#include "mg_setup_crypto.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#include "mg_vm_link.h"

LOG_MODULE_REGISTER(mg_vm_session, CONFIG_MG_LOG_LEVEL);

#define MAX_MESSAGE CONFIG_MG_VM_SESSION_MAX_MESSAGE
#define WINDOW      CONFIG_MG_VM_SESSION_WINDOW
#define CREDS_MAX   MG_SESSION_MAX_CREDENTIALS

/* The largest notification: an ATT value (the link's MTU sets the rest; 247
 * on the XIAO, up to 517 on boards and links that take it). */
#define FRAME_MAX MG_MAX_ATT_VALUE
/* A notification that couldn't be queued at once is tried again after
 * this. */
#define RETRY_MS 20

#if MAX_MESSAGE < MG_SESSION_MIN_MESSAGE || MAX_MESSAGE > MG_SESSION_MAX_MESSAGE
#error "CONFIG_MG_VM_SESSION_MAX_MESSAGE outside MG_SESSION_MIN_MESSAGE..MG_SESSION_MAX_MESSAGE"
#endif
/* The device's hello is always sealed (an unsealed one is refused
 * insecure), and a sealed hello's window is at least 4 (mgcommands.h,
 * Opening: a whole counter in a sealed message's first fragment): a client
 * refuses a smaller one with invalid_value, and could never start a message
 * in it anyway. */
#if WINDOW < 4 || WINDOW > MAX_MESSAGE
#error "CONFIG_MG_VM_SESSION_WINDOW must be 4..CONFIG_MG_VM_SESSION_MAX_MESSAGE (a sealed hello's window)"
#endif

/*
 * The session's buffers, about 30 KB with the defaults, come from the malloc
 * arena (all the RAM no static data uses, CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE)
 * at a hello that's answered, and go back, wiped, when the connection ends
 * (a disconnect, a new token proof challenge, a setup erase): the session is
 * used once per connection, so it takes no base RAM. A hello that finds no
 * memory waits unanswered, retried every 100 ms, and is dropped with no
 * answer after 30 s or at the connection's end; it changes nothing.
 * Firmware updates (SMP) take their memory from the same arena only while
 * they run: it holds both.
 */
struct session_mem {
	/* The message being received. At a hello it holds the tokens while the
	 * credentials are written (the hello drops any message). */
	uint8_t rx_msg[MAX_MESSAGE];
	/* A credentials_update being received, sealed or not (NUL-terminated
	 * for the parser). */
	char cred_in[MGSEAL_OVERHEAD + CREDS_MAX + 1];
	struct mg_json_obj cred_obj;
	/* The credentials going out: the JSON at CREDS_JSON, sealed in place
	 * (counter in front, tag after) when the session is sealed. */
	uint8_t creds_buf[MGSEAL_OVERHEAD + CREDS_MAX + 1];
	/* Device -> client messages waiting for credit: [u32 length][bytes]... */
	uint8_t txq[CONFIG_MG_VM_SESSION_TX_QUEUE];
	/* /link-control's (mg_vm_link.c). */
	struct mg_vm_link_mem link;
};
static struct session_mem *sm;
#define rx_msg     (sm->rx_msg)
#define cred_in    (sm->cred_in)
#define cred_obj   (sm->cred_obj)
#define creds_buf  (sm->creds_buf)
#define txq        (sm->txq)
#define CREDS_JSON ((char *)&creds_buf[4])

static bool mem_get(void)
{
	if (sm == NULL) {
		sm = malloc(sizeof(*sm));
		if (sm != NULL) {
			memset(sm, 0, sizeof(*sm));
		}
	}
	return sm != NULL;
}

static void mem_put(void)
{
	if (sm != NULL) {
		mg_psa_wipe(sm, sizeof(*sm));
		free(sm);
		sm = NULL;
	}
}

struct mg_vm_link_mem *mg_vm_session_link_mem(void)
{
	return &sm->link;
}

bool mg_vm_session_mem_held(void)
{
	return sm != NULL;
}

static struct {
	bool hello; /* a hello succeeded on this connection */
	bool open;  /* the client reports the VM session open */
	int64_t opened_at; /* when it opened (the re-ask series, below) */
	bool ended; /* the client reported a state other than open (connecting
		     * excepted) since the hello: a session that ended */
	uint8_t ended_series; /* RA_NONET after no_network, else RA_CLOSED */
	uint16_t epoch;
	uint32_t client_max;
	uint32_t client_window;
	uint64_t credit; /* message bytes the client still takes */
	uint64_t owed;   /* credit to give back, not sent yet */
	/* The message being received, and how many of its bytes haven't been
	 * given back yet. */
	bool in_open;
	uint32_t in_total;
	uint32_t in_got;
	uint32_t in_held;
	uint32_t credit_overruns; /* client fragments past the credit granted */
	uint32_t in_ctr; /* a sealed message's counter, read from its first fragment */
	uint8_t ctr_bad; /* counter refusals in a row (CTR_END) */
	/* The credentials_update being received. */
	bool cred_open;
	uint32_t cred_total;
	uint32_t cred_got;
	uint32_t cred_ctr;
	/* The sealed mg_session_clear being received (counter || [0] || tag). */
	bool clr_open;
	bool clr_erased; /* a sealed clear erased the setup; its answer may still wait */
	uint8_t clr_got;
	uint32_t clr_ctr;
	uint8_t clr_in[MGSEAL_OVERHEAD + 1];
	/* The sealed clear's answer, waiting for a device message already part
	 * way out (mgcommands.h); the session ends once it has gone. */
	uint8_t clr_out[MGSEAL_OVERHEAD + 1];
	uint8_t clr_out_len, clr_out_off;
	/* Control frames waiting to go out: the hello reply (the credentials
	 * wait for it), credentials_saved values (bit i the i-th of n, oldest
	 * first) and open_request frames (2: close, then open; 1: open). */
	bool hello_out;
	uint8_t saved_n;
	uint8_t saved_bits;
	/* In a sealed session each credentials_saved is sealed:
	 * result || SHA-256(refresh_token)[0:8], zeros when not saved,
	 * under the next device counter, sealed only once no data waits in the
	 * queue (whose counters come first) and framed as credentials. */
	uint8_t saved_hash[8][MG_SESSION_SAVED_HASH_LEN];
	/* The sealed answer only when the client's sealed hello carried
	 * mg_session_hello_flag_sealed_saved (echoed in ours): an older app
	 * doesn't count it, its relay falls one counter
	 * behind and parks the session. Without it the legacy one-byte answer,
	 * which takes no counter, in a sealed session too. */
	bool sealed_saved;
	uint8_t saved_blob[MGSEAL_OVERHEAD + 1 + MG_SESSION_SAVED_HASH_LEN];
	uint8_t saved_blob_len;
	uint8_t saved_blob_off;
	uint8_t reopen_out;
	/* The credentials going out, outside credit: creds_len bytes from
	 * creds_buf[creds_base]. */
	size_t creds_base;
	size_t creds_len;
	size_t creds_off;
	/* Sealed credentials (mgcommands.h, Sealed credentials): this hello asked for them, the keys from K and the proof's
	 * nonces, and each direction's next counter. */
	bool sealed;
	mgseal_keys_t keys;
	uint32_t ctr_out;
	uint32_t ctr_in;
	/* Counters never wrap (mgcommands.h, Sealed credentials): the client's
	 * 0xffffffff taken (rx), or the device's sealed (tx); a spent tx refuses
	 * the client's sealed messages, ending it. */
	bool rx_spent, tx_spent;
	/* A client counter refused at its tag after a later one was taken (a
	 * credentials_update and a data message in flight together): accepted
	 * once. */
	bool hole_set;
	uint32_t hole;
	/* Queued messages: bytes used in txq, and how much of the first went. */
	size_t tx_len;
	size_t out_off;
} s;

/* The device's counter after sealing one message: the 0xffffffff-th spends
 * the direction. */
static void ctr_out_next(void)
{
	if (s.ctr_out == UINT32_MAX) {
		s.tx_spent = true;
		LOG_WRN("mg.session: device counters spent: the sealed session ends");
	}
	s.ctr_out++;
}

static struct k_work_delayable retry_work;
static struct k_work_delayable clr_end_work;

/* After a state the device didn't ask for, it asks again with open_request
 * [1] (mgcommands.h, Session state): its commands must stay reachable.
 * At once, then 1, 2, 4 ... s while it stays connected (iOS ends a
 * suspended app's sessions at each wake: gaps under a second are normal),
 * in two series that never restart each other:
 * after closed (the server's end or refusal) capped at 60 s, after
 * no_network (the phone's network, or iOS killing the app's socket) capped
 * at 15 s. */
#define REASK_MIN_MS 1000
enum { RA_CLOSED, RA_NONET, RA_SERIES };
static const uint32_t REASK_CAP_MS[RA_SERIES] = {60000, 15000};
/* An open restarts the re-ask series only after a session that stayed open
 * this long; after a shorter one the backoff continues (mgcommands.h
 * Session state: a server that keeps ending at once
 * is never asked faster). A hello or a saved credentials_update restarts
 * both series. */
#define REASK_STABLE_MS 60000
static struct k_work_delayable reask_work;
static bool reask_pending; /* reask_work is scheduled */
static uint8_t reask_kind;  /* the series whose ask it is */
static int64_t reask_due;   /* when it goes */
static uint32_t reask_ms[RA_SERIES]; /* per series; 0: the next re-ask goes at once */
/* The waiting ask is reask_fn's own next step for a silent client: the
 * series advances only when it goes, and any state from the client cancels
 * it (the rules for that state then run as before, timed from it). */
static bool reask_tentative;

/* The session only for an active task (iOS suspends an idle backgrounded
 * app about 30 s after the last traffic, so re-asking whatever the reason
 * made a reopen loop all day, 136 reopens in 8 h on one gadget). A task is: (1) a user
 * action, for TASK_USER_MS after it (mg_vm_session_user_action()); (2) VM
 * traffic owed (mg_vm_link_owes()); (3) a board's wait
 * (mg_vm_wait_active()); (4) an open request of the gadget's own (the
 * close-then-open after a /link-control reset). The re-ask after closed or
 * no_network runs only while a task is active; with none the session is
 * left closed and nothing is asked; a task starting asks at once. While a
 * task is active and the session is open, mg_vm_link keeps it awake with an
 * empty /link-control frame every CONFIG_MG_VM_KEEPALIVE_MS. Registered
 * commands alone don't keep a gadget reachable: an agent's invoke while
 * the session is closed gets node_offline. */
#define TASK_USER_MS (2 * 60 * 1000)
static int64_t user_action_at; /* the last user action (0: none since boot) */

/* A board's wait (an avatar face with no picture yet, a chat ask, a draw.jpg
 * window): none on this app; a board with such a feature says so. */
__attribute__((weak)) bool mg_vm_wait_active(void)
{
	return false;
}

static bool own_reopen; /* an open request of the gadget's own is out (task (4)) */
/* About 20 s after the last task ends the gadget
 * closes its own session with open_request [0] (Android holds the VM socket
 * with pings while a session is open, keeping the VM awake); its closed
 * answer is asked for, so nothing re-asks; a new task asks again. */
static bool close_out;  /* open_request [0] to send */
static bool own_close;  /* sent, its closed not seen yet */
static void flush(void);

void mg_vm_session_close_idle(void)
{
	if (!s.hello || !s.open || own_close) {
		return;
	}
	LOG_INF("mg.session: no active task for %d s: closing the session (open_request 0)",
		CONFIG_MG_VM_IDLE_CLOSE_MS / 1000);
	close_out = true;
	own_close = true;
	flush();
}

bool mg_vm_session_task_active(void)
{
	return own_reopen || mg_vm_link_owes() || mg_vm_wait_active() ||
	       (user_action_at && k_uptime_get() - user_action_at < TASK_USER_MS);
}

static void reask_restart(void)
{
	reask_ms[RA_CLOSED] = reask_ms[RA_NONET] = 0;
}
/* After not_enrolled, auth_failed or too_large: no re-ask, even after a
 * later closed, until a new hello, a credentials_update or an open
 * (mg-session-v1.json, "re-ask after an unasked end"). */
static bool reask_blocked;

/* A hello that found no memory waits, unanswered (a deferral): tried again every DEFER_RETRY_MS, dropped with no answer after
 * DEFER_MS or at the connection's end. */
#define DEFER_MS       30000
#define DEFER_RETRY_MS 100
static struct {
	bool pending;
	int64_t since;
	uint8_t len;
	uint8_t p[16];
} deferred;
static struct k_work_delayable defer_work;

static const char *const STATE_NAMES[] = {
	"closed", "connecting", "open", "no network", "auth failed", "not enrolled", "too large",
};

static uint32_t get32(const uint8_t *b)
{
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* A sealed message's counter (big-endian, its first 4 bytes). */
static uint32_t get32be(const uint8_t *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
}

static void put32(uint32_t v, uint8_t *b)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static void error(uint16_t code)
{
	mg_core_send_error(mg_command_session, code, NULL);
}

/* Credit goes out with the next flush, ahead of the data: never lost to a
 * full notification queue, or the client's window would shrink for good. */
static void give_credit(size_t n)
{
	s.owed += n;
}

/* Room for message bytes in one notification after a header. */
static size_t room(size_t header)
{
	uint16_t mtu = mg_transport_att_mtu();
	size_t r = mtu > 3 + header ? mtu - 3 - header : 0;

	return MIN(MIN(r, FRAME_MAX - header), (size_t)MG_MAX_ATT_VALUE - header);
}

/* Every notification here goes out through this: queued only if that needs
 * no waiting, so the client's writes never wait behind the app work queue;
 * whatever didn't go is sent by retry_work. */
static int try_send(const uint8_t *b, size_t n)
{
	return mg_session_try_control(b, n);
}

/* Sends one fragment of msg[off..len) of at most max bytes as sub, with
 * flags (mg_session_flag_sealed) besides first and last; the bytes sent, or
 * a negative errno (nothing sent). */
static int send_fragment(uint8_t sub, uint8_t flags, const uint8_t *msg, size_t len, size_t off,
			 uint64_t max)
{
	uint8_t f[FRAME_MAX];
	bool first = off == 0;
	size_t h = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
	size_t n = MIN(MIN(room(h), len - off), max);
	int err;

	if (n == 0) {
		return -EMSGSIZE;
	}
	f[0] = mg_command_session;
	f[1] = sub;
	f[2] = flags | (first ? mg_session_flag_first : 0) |
	       (off + n == len ? mg_session_flag_last : 0);
	if (first) {
		put32((uint32_t)len, &f[3]);
	}
	memcpy(&f[h], &msg[off], n);
	err = try_send(f, h + n);
	return err ? err : (int)n;
}

/* Sends the hello reply, the credentials, credit owed, credentials_saved,
 * open_request, then queued messages as far as credit goes. */
static void flush(void)
{
	int n;

	if (s.hello_out) {
		/* A sealed session's hello echoes the client's sealed flag. */
		uint8_t m[12] = {mg_command_session, mg_session_hello, MG_SESSION_VERSION};

		put32(MAX_MESSAGE, &m[3]);
		put32(WINDOW, &m[7]);
		m[11] = mg_session_hello_flag_sealed |
			(s.sealed_saved ? mg_session_hello_flag_sealed_saved : 0);
		n = try_send(m, s.sealed ? 12 : 11);
		if (n) {
			goto retry;
		}
		s.hello_out = false;
	}
	while (s.creds_off < s.creds_len) {
		n = send_fragment(mg_session_credentials, s.sealed ? mg_session_flag_sealed : 0,
				  &creds_buf[s.creds_base], s.creds_len, s.creds_off, UINT64_MAX);
		if (n < 0) {
			goto retry;
		}
		s.creds_off += n;
	}
	if (s.creds_len) {
		mg_psa_wipe(creds_buf, sizeof(creds_buf));
		s.creds_len = s.creds_off = 0;
	}
	while (s.owed) {
		uint8_t m[6] = {mg_command_session, mg_session_credit};
		uint32_t c = (uint32_t)MIN(s.owed, UINT32_MAX);

		put32(c, &m[2]);
		n = try_send(m, sizeof(m));
		if (n) {
			goto retry;
		}
		s.owed -= c;
	}
	while ((!s.sealed || !s.sealed_saved) && s.saved_n) {
		const uint8_t m[3] = {mg_command_session, mg_session_credentials_saved,
				      s.saved_bits & 1};

		n = try_send(m, sizeof(m));
		if (n) {
			goto retry;
		}
		s.saved_bits >>= 1;
		s.saved_n--;
	}
	while (s.reopen_out) {
		const uint8_t m[3] = {mg_command_session, mg_session_open_request,
				      s.reopen_out == 1};

		n = try_send(m, sizeof(m));
		if (n) {
			goto retry;
		}
		s.reopen_out--;
	}
	if (close_out) {
		const uint8_t m[3] = {mg_command_session, mg_session_open_request, 0};

		n = try_send(m, sizeof(m));
		if (n) {
			goto retry;
		}
		close_out = false;
	}
	/* A sealed credentials_saved part way out holds a counter below every
	 * queued message's: they wait for it, so counters go out in order. */
data:
	while (s.open && s.tx_len && s.credit > 0 && !(s.sealed && s.saved_blob_len)) {
		size_t len = get32(txq);

		/* A sealed message's first fragment carries the whole 4-byte
		 * counter: with less credit it waits. */
		if (s.sealed && s.out_off == 0 && s.credit < 4) {
			break;
		}
		n = send_fragment(mg_session_data, s.sealed ? mg_session_flag_sealed : 0, &txq[4],
				  len, s.out_off, s.credit);
		if (n < 0) {
			goto retry;
		}
		s.credit -= n;
		s.out_off += n;
		if (s.out_off == len) {
			memmove(txq, &txq[4 + len], s.tx_len - 4 - len);
			s.tx_len -= 4 + len;
			s.out_off = 0;
		}
	}
	while (s.sealed && s.sealed_saved &&
	       (s.saved_blob_len || (s.saved_n && s.tx_len == 0))) {
		if (!s.saved_blob_len && s.tx_spent) {
			s.saved_n = 0; /* no counter left: nothing more is sealed */
			break;
		}
		if (!s.saved_blob_len) {
			uint8_t *pt = &s.saved_blob[4]; /* after the counter */

			pt[0] = s.saved_bits & 1;
			memcpy(&pt[1], s.saved_hash[0], MG_SESSION_SAVED_HASH_LEN);
			if (mgseal_seal(&s.keys, MGSEAL_DIR_DEVICE, s.ctr_out, s.saved_blob,
					1 + MG_SESSION_SAVED_HASH_LEN) != MGSEAL_OK) {
				LOG_WRN("mg.session: credentials_saved not sealed");
				mg_psa_wipe(s.saved_blob, sizeof(s.saved_blob));
			} else {
				ctr_out_next();
				s.saved_blob_len = sizeof(s.saved_blob);
				s.saved_blob_off = 0;
			}
			s.saved_bits >>= 1;
			memmove(s.saved_hash[0], s.saved_hash[1],
				sizeof(s.saved_hash) - sizeof(s.saved_hash[0]));
			s.saved_n--;
			continue;
		}
		n = send_fragment(mg_session_credentials_saved, mg_session_flag_sealed, s.saved_blob,
				  s.saved_blob_len, s.saved_blob_off, UINT64_MAX);
		if (n < 0) {
			goto retry;
		}
		s.saved_blob_off += n;
		if (s.saved_blob_off == s.saved_blob_len) {
			s.saved_blob_len = s.saved_blob_off = 0;
			if (s.tx_len) {
				goto data; /* what waited behind it */
			}
		}
	}
	/* The sealed clear's answer, once what was part way out has gone. */
	while (s.clr_out_len && s.tx_len == 0 && s.saved_blob_len == 0) {
		n = send_fragment(mg_session_clear, mg_session_flag_sealed, s.clr_out,
				  s.clr_out_len, s.clr_out_off, UINT64_MAX);
		if (n < 0) {
			goto retry;
		}
		s.clr_out_off += n;
		if (s.clr_out_off == s.clr_out_len) {
			s.clr_out_len = s.clr_out_off = 0;
			k_work_reschedule_for_queue(mg_app_wq(), &clr_end_work, K_NO_WAIT);
		}
	}
	return;
retry:
	if (n == -ENOTCONN) {
		/* Control isn't subscribed: the next write flushes again. */
		return;
	}
	/* The notification queue is full (or the MTU too small): later. */
	k_work_reschedule_for_queue(mg_app_wq(), &retry_work, K_MSEC(RETRY_MS));
}

static void retry_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	flush();
}

/* The sealed clear's answer has gone: the session ends. */
static void clr_end_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	LOG_INF("mg.session: sealed clear answered: the session ends");
	mg_vm_session_connection_reset();
}

/* The counter of the first queued sealed message not started yet, else the
 * next device counter. */
static uint32_t first_unstarted_ctr(void)
{
	size_t first = s.out_off ? 4 + get32(txq) : 0; /* the head started: the next */

	return s.sealed && first + 8 <= s.tx_len ? get32be(&txq[first + 4]) : s.ctr_out;
}

/* The session ends (state other than open, a hello): streams, the partial
 * message and queued messages go; credit is the client's window again.
 * Counters restart only at a sealed hello, so the queued messages that never
 * started give their counters back: the next one sealed takes the first of
 * them, and the client sees no gap. */
static void reset_session(void)
{
	if (s.open) {
		mg_vm_link_close();
	}
	if (s.tx_len) {
		uint32_t c = first_unstarted_ctr();

		if (c != s.ctr_out) {
			s.ctr_out = c;
			s.tx_spent = false;
		}
	}
	s.open = false;
	s.credit = s.client_window;
	s.owed = 0;
	s.in_open = false;
	s.in_held = 0;
	s.tx_len = 0;
	s.out_off = 0;
}

/* Three counter refusals in a row end the sealed session (the portable
 * core's MGDS_CTR_END): a relay that counted a sealed message whose first fragment
 * never left (an epoch cut it) stays one counter ahead for good, and the
 * exact-next rule then refuses every later message until a hello that never
 * comes. Only counter refusals count (a first fragment whose counter is
 * neither the next one nor the hole); a tag refusal's counter was acceptable
 * (the run already reset), so a forged message can't end a good session.
 * An acceptable counter, a hello and any session end reset the run. The end
 * is complete (keys wiped, counters 0, the hello gone): a sealed hello then
 * needs a new token proof's nonces (one sealed session per proof). */
#define CTR_END 3
static uint32_t ctr_ends; /* sessions ended so, since boot */

unsigned int mg_vm_session_ctr_ends(void)
{
	return ctr_ends;
}

/* A sealed client message takes its counter at its first fragment: the next one, or the one open hole. Taken, it is
 * used (an abort, a dropped message, a state change) unless the message is
 * refused at its tag (ctr_give_back). */
static bool ctr_take(uint32_t c)
{
	if (s.rx_spent) {
		return false; /* 0xffffffff was taken: no other, never a wrapped one */
	}
	if (c == s.ctr_in) {
		s.rx_spent = c == UINT32_MAX;
		s.ctr_in++;
		s.ctr_bad = 0;
		if (s.hole_set && !mg_vm_session_hole_in_window(s.ctr_in, s.hole)) {
			s.hole_set = false; /* it fell more than 255 back: closed */
		}
		return true;
	}
	if (s.hole_set && c == s.hole) {
		s.hole_set = false;
		s.ctr_bad = 0;
		return true;
	}
	return false;
}

/* A first fragment's counter refused (its refusal already out): the
 * CTR_END-th in a row ends the session. */
static void ctr_refused(void)
{
	if (++s.ctr_bad < CTR_END) {
		return;
	}
	ctr_ends++;
	LOG_WRN("mg.session: %d counter refusals in a row: ending the session (a new proof "
		"and hello start over from 0)",
		CTR_END);
	mg_vm_session_connection_reset();
}

/* Refused at its tag: the counter isn't used. Given back if nothing later took
 * one, else it becomes the hole, accepted once (a newer hole replaces it). */
static void ctr_give_back(uint32_t c)
{
	if (c + 1u == s.ctr_in) {
		s.ctr_in = c;
		if (c == UINT32_MAX) {
			s.rx_spent = false;
		}
	} else if (mg_vm_session_hole_in_window(s.ctr_in, c)) {
		s.hole = c;
		s.hole_set = true;
	} /* else: more than 255 back, no hole */
}

/* Drops the message being received, giving back its bytes and extra more. */
static void drop_in(size_t extra)
{
	size_t n = (s.in_open ? s.in_held : 0) + extra;

	s.in_open = false;
	s.in_held = 0;
	give_credit(n);
}

/* ------------------------------------------------------------------------
 * hello and credentials
 * ---------------------------------------------------------------------- */

/* The credentials JSON for this token pair (mgcommands.h, Device session)
 * into buf, CREDS_MAX + 1 bytes (NULL: only measured); its length, -ENOSPC
 * if it is longer than MG_SESSION_MAX_CREDENTIALS, or -EIO. */
static int credentials_json(const char *at, const char *rt, uint64_t exp, char *buf)
{
	static const char *const KINDS[] = {NULL, "svm", "cvm"};
	uint8_t dk[MG_SETUP_DEVICE_KEY_LEN], kid[MG_SETUP_KEY_ID_LEN];
	char dk_b64[48], kid_b64[28], vm_id[MG_SETUP_VM_ID_MAX + 1];
	size_t vm_id_len = 0;
	enum mg_setup_vm_kind vm_kind = MG_SETUP_VM_KIND_NONE;
	struct mg_jw w;
	bool have_dk = false;
	int n = 0;

	if (mg_setup_store_has_device_key()) {
		n = mg_setup_store_get_device_key(dk, kid);
		have_dk = n == 0 && mg_b64_encode(dk, sizeof(dk), dk_b64, sizeof(dk_b64)) > 0 &&
			  mg_b64_encode(kid, sizeof(kid), kid_b64, sizeof(kid_b64)) > 0 &&
			  mg_setup_store_get_vm(vm_id, &vm_id_len, &vm_kind) == 0 &&
			  (unsigned int)vm_kind < ARRAY_SIZE(KINDS);
		n = have_dk ? 0 : -EIO;
	}
	if (n == 0) {
		mg_jw_begin(&w, buf, CREDS_MAX + 1);
		mg_jw_str(&w, "node_id", mg_vm_link_node_id());
		mg_jw_str(&w, "access_token", at);
		mg_jw_str(&w, "refresh_token", rt);
		if (have_dk) {
			mg_jw_str(&w, "device_key", dk_b64);
			mg_jw_str(&w, "key_id", kid_b64);
			/* The VM the key is bound to: the client opens the session
			 * to it, with its kind's handshake. */
			if (vm_id_len) {
				mg_jw_str(&w, "vm_id", vm_id);
			}
			if (vm_kind != MG_SETUP_VM_KIND_NONE) {
				mg_jw_str(&w, "vm_kind", KINDS[vm_kind]);
			}
		}
		/* The pair's expiry, as the last credentials_update gave it. */
		if (exp != 0 && exp <= (uint64_t)INT64_MAX) {
			mg_jw_int(&w, "access_expires_at", (long long)exp);
		}
		n = mg_jw_end(&w);
	}
	mg_psa_wipe(dk, sizeof(dk));
	mg_psa_wipe(dk_b64, sizeof(dk_b64));
	return n;
}

/* The stored credentials into CREDS_JSON; their length, or a negative
 * errno. */
static int write_credentials(void)
{
	char *at = (char *)rx_msg, *rt = (char *)&rx_msg[CREDS_MAX];
	size_t at_len, rt_len;
	int n;

	BUILD_ASSERT(sizeof(rx_msg) >= 2 * CREDS_MAX, "the tokens go in rx_msg");
	n = mg_setup_store_get_tokens(at, CREDS_MAX, &at_len, rt, CREDS_MAX, &rt_len);
	if (n == 0) {
		n = credentials_json(at, rt, mg_setup_store_expires(), CREDS_JSON);
	}
	mg_psa_wipe(rx_msg, 2 * CREDS_MAX);
	return n;
}

/* The seal keys from K, the proof's nonces (taken: one sealed session per
 * proof) and the node id. -EACCES when this proof's nonces already sealed a
 * session, or no proof matched. */
static int seal_keys(mgseal_keys_t *keys)
{
	uint8_t k[MG_SETUP_K_LEN], cn[MG_TOKEN_PROOF_NONCE_LEN], dn[MG_TOKEN_PROOF_NONCE_LEN];
	const char *node = mg_vm_link_node_id();
	int err;

	BUILD_ASSERT(MG_SETUP_K_LEN == MGSEAL_KEY_LEN && MG_TOKEN_PROOF_NONCE_LEN == MGSEAL_NONCE_LEN,
		     "the seal takes the proof's K and nonces");
	if (!mg_token_proof_take_nonces(cn, dn)) {
		return -EACCES;
	}
	err = mg_setup_store_get_k(k);
	if (err == 0 && mgseal_keys(keys, k, cn, dn, node, strlen(node)) != MGSEAL_OK) {
		err = -EIO;
	}
	mg_psa_wipe(k, sizeof(k));
	mg_psa_wipe(cn, sizeof(cn));
	mg_psa_wipe(dn, sizeof(dn));
	return err;
}

static void on_hello(const uint8_t *p, size_t len)
{
	uint32_t max, window;
	mgseal_keys_t keys = {0};
	bool sealed, sealed_saved;
	int n;

	if (len < 9) {
		error(mg_error_code_invalid_length);
		return;
	}
	max = get32(&p[1]);
	window = get32(&p[5]);
	/* hello_flags, the optional last byte: sealed credentials need no
	 * encrypted link (mgcommands.h, Sealed credentials). */
	sealed = len >= 10 && (p[9] & mg_session_hello_flag_sealed);
	sealed_saved = sealed && (p[9] & mg_session_hello_flag_sealed_saved);
	if (!mg_setup_store_complete()) {
		error(mg_error_code_not_found);
		return;
	}
	if (!mg_token_proof_matched()) {
		error(mg_error_code_proof_required);
		return;
	}
	if (!sealed) {
		/* A token proof can be relayed, and a proxy
		 * that bonds on its own leg has an encrypted link, so credentials
		 * leave only sealed: every unsealed hello is insecure, on any link,
		 * whatever the bond. */
		error(mg_error_code_insecure);
		return;
	}
	if (p[0] != MG_SESSION_VERSION || max < MG_SESSION_MIN_MESSAGE ||
	    max > MG_SESSION_MAX_MESSAGE || window < (sealed ? 4u : 1u) || window > max ||
	    (sealed && mg_transport_att_mtu() < MG_MIN_ATT_MTU)) {
		/* A sealed session's window holds at least a whole counter,
		 * and its one-write messages (such as credentials_saved) need
		 * ATT_MTU >= MG_MIN_ATT_MTU. */
		error(mg_error_code_invalid_value);
		return;
	}
	/* The session's memory, before anything is taken or torn down: none
	 * left is a refusal that changes nothing (a sealed hello's nonces
	 * stay for the next try). */
	if (!mem_get()) {
		/* Held unanswered until the block is free; nothing is
		 * taken, so a sealed hello's nonces stay for it. */
		if (!deferred.pending) {
			LOG_WRN("mg.session: no memory for a session (%u B): hello deferred",
				(unsigned int)sizeof(*sm));
			deferred.pending = true;
			deferred.since = k_uptime_get();
		}
		deferred.len = (uint8_t)MIN(len, sizeof(deferred.p));
		memcpy(deferred.p, p, deferred.len);
		k_work_reschedule_for_queue(mg_app_wq(), &defer_work, K_MSEC(DEFER_RETRY_MS));
		return;
	}
	deferred.pending = false;
	if (sealed) {
		n = seal_keys(&keys);
		if (n == -EACCES) {
			/* This proof's nonces sealed a session already: a second
			 * one would reuse its keystream (one sealed session per
			 * proof). A new proof first; the open session stays. */
			error(mg_error_code_proof_required);
			if (!s.hello) {
				mem_put();
			}
			return;
		}
		if (n) {
			LOG_ERR("mg.session: seal keys failed (%d)", n);
			mg_core_send_error(mg_command_session, mg_error_code_not_found,
					   "credentials unreadable");
			if (!s.hello) {
				mem_put();
			}
			return;
		}
	}
	/* A new hello ends the session the last one opened, its keys too. */
	s.hello = false;
	s.cred_open = false;
	s.clr_open = false;
	s.clr_erased = false;
	s.client_max = max;
	s.client_window = window;
	reset_session();
	s.hello_out = false;
	s.saved_n = s.saved_bits = 0;
	s.saved_blob_len = s.saved_blob_off = 0;
	s.clr_out_len = s.clr_out_off = 0;
	s.reopen_out = 0;
	mg_psa_wipe(creds_buf, sizeof(creds_buf));
	s.creds_len = s.creds_off = 0;
	s.sealed = sealed;
	s.sealed_saved = sealed_saved;
	s.keys = keys;
	mg_psa_wipe(&keys, sizeof(keys));
	if (!sealed) {
		mg_psa_wipe(&s.keys, sizeof(s.keys));
	}
	s.ctr_out = s.ctr_in = 0;
	s.rx_spent = s.tx_spent = false;
	s.hole_set = false;
	s.ctr_bad = 0;
	n = write_credentials();
	if (n < 0) {
		/* Too long for MG_SESSION_MAX_CREDENTIALS, or unreadable. */
		LOG_ERR("mg.session: credentials unavailable (%d)", n);
		s.sealed = false;
		mg_psa_wipe(&s.keys, sizeof(s.keys));
		mem_put();
		mg_core_send_error(mg_command_session, mg_error_code_not_found,
				   n == -ENOSPC ? "credentials too long" : "credentials unreadable");
		return;
	}
	if (s.sealed && mgseal_seal(&s.keys, MGSEAL_DIR_DEVICE, s.ctr_out, creds_buf,
				    (size_t)n) != MGSEAL_OK) {
		LOG_ERR("mg.session: sealing the credentials failed");
		s.sealed = false;
		mg_psa_wipe(&s.keys, sizeof(s.keys));
		mem_put();
		mg_core_send_error(mg_command_session, mg_error_code_not_found,
				   "credentials unreadable");
		return;
	}
	s.hello = true;
	s.ended = false;
	reask_restart();
	reask_blocked = false;
	(void)k_work_cancel_delayable(&reask_work);
	reask_pending = false;
	reask_tentative = false;
	LOG_INF("mg.session: hello (client takes %u B, window %u B); credentials %d B%s", max,
		window, n, s.sealed ? ", sealed" : "");
	/* The hello, then right behind it the credentials, outside credit
	 * (flush). */
	s.hello_out = true;
	if (s.sealed) {
		ctr_out_next();
		s.creds_base = 0;
		s.creds_len = (size_t)n + MGSEAL_OVERHEAD;
	} else {
		s.creds_base = 4;
		s.creds_len = (size_t)n;
	}
}

/* credentials_saved [v] goes out with the next flush, after any before it;
 * h: SHA-256 of the update's refresh_token (its first bytes go in a sealed
 * answer), or NULL (zeros). */
static void queue_saved(bool v, const uint8_t *h)
{
	if (s.saved_n >= 8) {
		LOG_WRN("mg.session: credentials_saved not sent: too many unsent");
		return;
	}
	s.saved_bits |= (uint8_t)(v << s.saved_n);
	if (v && h) {
		memcpy(s.saved_hash[s.saved_n], h, MG_SESSION_SAVED_HASH_LEN);
	} else {
		memset(s.saved_hash[s.saved_n], 0, MG_SESSION_SAVED_HASH_LEN);
	}
	s.saved_n++;
}

/* The counter the clear's answer takes: the first device message not yet
 * started (it and everything after it are dropped as the session ends), else
 * the next one. */
static uint32_t clear_answer_ctr(void)
{
	uint32_t c = MIN(s.ctr_out, first_unstarted_ctr());

	if (s.sealed && s.creds_len && s.creds_off == 0) {
		c = 0; /* the credentials, still waiting for room, were sealed on 0 */
	}
	if (s.saved_blob_len && s.saved_blob_off == 0) {
		c = MIN(c, get32be(s.saved_blob));
	}
	return c;
}

/* mg_session_clear (12): Forget's
 * clear, taken only sealed in this connection's sealed session, on the next
 * client counter. Plaintext [0]: the tokens and K are erased first (as the
 * token proof's clear does), then the answer, its own sealed mg_session_clear
 * on the next device counter, [1] once erased or [0] if the erase failed, and
 * the session ends. Unsealed: insecure; a wrong tag or counter, or
 * another plaintext: invalid_value; one arriving while a credentials_update
 * is still arriving: busy, its counter used; none erases anything. Taken in every
 * build; CONFIG_MG_SESSION_SEALED_CLEAR declares it (the cap bit) and refuses
 * the unsealed token proof clear. */
static void on_session_clear(const uint8_t *p, size_t len)
{
	uint8_t flags;

	if (len < 1) {
		error(mg_error_code_invalid_length);
		return;
	}
	if (!s.hello) {
		error(mg_error_code_invalid_value);
		return;
	}
	flags = p[0];
	p++;
	len--;
	if (!s.sealed || !(flags & mg_session_flag_sealed)) {
		s.clr_open = false;
		error(mg_error_code_insecure);
		return;
	}
	if (s.tx_spent) {
		/* No counter left for its answer: refused. */
		s.clr_open = false;
		error(mg_error_code_invalid_value);
		return;
	}
	if (flags & mg_session_flag_first) {
		if (len < 4) {
			error(mg_error_code_invalid_length);
			return;
		}
		uint32_t total = get32(p);

		p += 4;
		len -= 4;
		if (s.clr_open || total != sizeof(s.clr_in)) {
			s.clr_open = false;
			error(mg_error_code_invalid_value);
			return;
		}
		if (len < 4) {
			error(mg_error_code_invalid_length);
			return;
		}
		if (!ctr_take(get32be(p))) {
			error(mg_error_code_invalid_value);
			ctr_refused();
			return;
		}
		if (s.cred_open) {
			/* Mid credentials_update: busy, its counter used; the
			 * client retries on the next. */
			error(mg_error_code_busy);
			return;
		}
		s.clr_open = true;
		s.clr_got = 0;
		s.clr_ctr = get32be(p);
	} else if (!s.clr_open) {
		error(mg_error_code_invalid_value);
		return;
	}
	if (len == 0 || s.clr_got + len > sizeof(s.clr_in) ||
	    !!(flags & mg_session_flag_last) != (s.clr_got + len == sizeof(s.clr_in))) {
		s.clr_open = false;
		error(mg_error_code_invalid_value);
		return;
	}
	memcpy(&s.clr_in[s.clr_got], p, len);
	s.clr_got += len;
	if (!(flags & mg_session_flag_last)) {
		return;
	}
	s.clr_open = false;
	size_t plen = 0;

	if (mgseal_open(&s.keys, MGSEAL_DIR_CLIENT, s.clr_ctr, s.clr_in, sizeof(s.clr_in),
			&plen) != MGSEAL_OK) {
		ctr_give_back(s.clr_ctr);
		LOG_WRN("mg.session: sealed clear refused (tag)");
		error(mg_error_code_invalid_value);
		return;
	}
	if (plen != 1 || s.clr_in[4] != 0) {
		error(mg_error_code_invalid_value);
		return;
	}
	/* Erased the moment the tag checks (mgcommands.h); only the
	 * answer, sealed on the next device counter under the session's keys
	 * (they stay in RAM just for it), waits for a device message already
	 * part way out (its counter was spent at its first fragment). What was
	 * queued and not started never goes; the session ends once the answer
	 * has gone. */
	uint32_t c = clear_answer_ctr();

	LOG_INF("mg.session: sealed clear: erasing the setup");
	s.clr_erased = true; /* from here on no credentials_update stores anything */
	s.clr_out[4] = mg_setup_erase("sealed clear", false) == 0 ? 1 : 0;
	if (s.creds_off == 0) {
		/* Credentials still waiting for room never go out (mg-session-v1's
		 * "sealed clear while the credentials wait"): their counter is
		 * the answer's. */
		mg_psa_wipe(creds_buf, sizeof(creds_buf));
		s.creds_len = 0;
	}
	s.tx_len = s.out_off ? 4 + get32(txq) : 0;
	if (s.saved_blob_off == 0) {
		mg_psa_wipe(s.saved_blob, sizeof(s.saved_blob));
		s.saved_blob_len = 0;
	}
	s.saved_n = 0;
	if (mgseal_seal(&s.keys, MGSEAL_DIR_DEVICE, c, s.clr_out, 1) != MGSEAL_OK) {
		LOG_WRN("mg.session: sealed clear's answer not sealed");
		k_work_reschedule_for_queue(mg_app_wq(), &clr_end_work, K_NO_WAIT);
		return;
	}
	s.clr_out_len = sizeof(s.clr_out);
	s.clr_out_off = 0;
	flush();
}

static void on_credentials_update(const uint8_t *p, size_t len)
{
	const struct mg_json_field *at = NULL, *rt = NULL;
	uint8_t flags;
	int n;
	bool ok;

	if (len < 1) {
		error(mg_error_code_invalid_length);
		return;
	}
	if (!s.hello || s.clr_erased || (s.sealed && s.tx_spent)) {
		/* After a sealed clear's erase nothing is stored, so its [1]
		 * still means erased: refused before anything else, its
		 * counter untouched. Spent device counters refuse it too. */
		error(mg_error_code_invalid_value);
		return;
	}
	flags = p[0];
	p++;
	len--;
	/* A sealed session takes only sealed updates (an unsealed one is
	 * insecure; a later fragment of a sealed one without the flag is
	 * invalid_value and drops it), and a plain one only plain ones. */
	if (!!(flags & mg_session_flag_sealed) != s.sealed) {
		s.cred_open = false;
		error(s.sealed && (flags & mg_session_flag_first) ? mg_error_code_insecure
								  : mg_error_code_invalid_value);
		return;
	}
	if (flags & mg_session_flag_first) {
		if (len < 4) {
			error(mg_error_code_invalid_length);
			return;
		}
		uint32_t total = get32(p);

		p += 4;
		len -= 4;
		if (s.cred_open || total == 0 ||
		    total > (s.sealed ? MGSEAL_OVERHEAD + CREDS_MAX : CREDS_MAX) ||
		    (s.sealed && total <= MGSEAL_OVERHEAD)) {
			s.cred_open = false;
			error(mg_error_code_invalid_value);
			return;
		}
		/* Sealed: the counter is the first 4 sealed bytes, the next one
		 * (shared with data in this direction) or the open hole, and is
		 * taken here. */
		if (s.sealed && len < 4) {
			error(mg_error_code_invalid_length);
			return;
		}
		if (s.sealed && !ctr_take(get32be(p))) {
			error(mg_error_code_invalid_value);
			ctr_refused();
			return;
		}
		s.cred_open = true;
		s.cred_total = total;
		s.cred_got = 0;
		s.cred_ctr = s.sealed ? get32be(p) : 0;
	} else if (!s.cred_open) {
		error(mg_error_code_invalid_value);
		return;
	}
	if (len == 0 || s.cred_got + len > s.cred_total ||
	    !!(flags & mg_session_flag_last) != (s.cred_got + len == s.cred_total)) {
		s.cred_open = false;
		error(mg_error_code_invalid_value);
		return;
	}
	memcpy(&cred_in[s.cred_got], p, len);
	s.cred_got += len;
	if (!(flags & mg_session_flag_last)) {
		return;
	}
	s.cred_open = false;
	char *json = cred_in;
	size_t json_len = s.cred_total;

	if (s.sealed) {
		/* The tag is checked before anything is decrypted; a refused
		 * one changes nothing, gets no answer but the error and
		 * doesn't use the counter. */
		int r = mgseal_open(&s.keys, MGSEAL_DIR_CLIENT, s.cred_ctr, (uint8_t *)cred_in,
				    s.cred_total, &json_len);

		if (r != MGSEAL_OK) {
			ctr_give_back(s.cred_ctr);
			LOG_WRN("mg.session: sealed credentials_update refused (%s)",
				r == MGSEAL_E_COUNTER ? "counter" :
				r == MGSEAL_E_TAG ? "tag" : "seal");
			mg_psa_wipe(cred_in, sizeof(cred_in));
			error(mg_error_code_invalid_value);
			return;
		}
		json = &cred_in[4];
	}
	json[json_len] = '\0';
	/* The first byte is '{' (mgcommands.h: no whitespace before the
	 * object). */
	if (json_len == 0 || json[0] != '{' || mg_json_parse(json, json_len, &cred_obj) ||
	    (at = mg_json_get(&cred_obj, "access_token")) == NULL || at->type != MG_JSON_STRING ||
	    at->str_len == 0 || (rt = mg_json_get(&cred_obj, "refresh_token")) == NULL ||
	    rt->type != MG_JSON_STRING || rt->str_len == 0) {
		mg_psa_wipe(cred_in, sizeof(cred_in));
		error(mg_error_code_invalid_value);
		return;
	}
	/* The credentials the next hello sends must still fit: a pair that
	 * doesn't (with the node id and device key) would leave the gadget
	 * unable to open a session until it is set up again. */
	/* access_expires_at: kept when it's a positive integer (unix seconds);
	 * any other form is ignored, as other unknown members are. */
	const struct mg_json_field *ef = mg_json_get(&cred_obj, "access_expires_at");
	uint64_t exp = ef && ef->type == MG_JSON_NUMBER && ef->num_is_int && ef->num > 0
			       ? (uint64_t)ef->num
			       : 0;

	n = credentials_json(at->str, rt->str, exp, NULL);
	if (n == -ENOSPC) {
		/* The whole stored document (node id, the pair, the key and
		 * its binding, access_expires_at: what the next credentials
		 * send) must fit MG_SESSION_MAX_CREDENTIALS, checked before
		 * anything is stored (mgcommands.h). */
		LOG_WRN("mg.session: refreshed token pair refused: credentials too long");
		mg_psa_wipe(cred_in, sizeof(cred_in));
		error(mg_error_code_invalid_value);
		return;
	}
	if (n < 0) {
		LOG_ERR("mg.session: refreshed token pair not saved: credentials unreadable");
		ok = false;
	} else {
		/* Both in one commit; K stays (mg_setup_store.h). */
		ok = mg_setup_store_update_tokens_exp(at->str, at->str_len, rt->str, rt->str_len,
						      exp) == 0;
		LOG_INF("mg.session: refreshed token pair %s", ok ? "saved" : "not saved");
	}
	/* New credentials may be the change auth_failed or not_enrolled waited
	 * for: re-asks are allowed again. Saved, they restart
	 * the series (as mg_session_ref.py), and with the session
	 * closed they ask again at once, as the new series' first ask, right
	 * behind the credentials_saved (new credentials may fix what ended it, and with the apps' own
	 * recovery gone this is the only reopen; a new save each time, so no
	 * loop); the next step is 1 s. With the session open the next unasked
	 * end asks at once. */
	reask_blocked = false;
	if (ok) {
		reask_restart();
		/* Only with an active task (a save asks only with one): a token
		 * refresh on an idle gadget
		 * opens no session. */
		if (!s.open && s.ended && mg_vm_session_task_active()) {
			k_work_reschedule_for_queue(mg_app_wq(), &reask_work, K_NO_WAIT);
			reask_pending = true;
			reask_tentative = false;
			reask_kind = s.ended_series;
			reask_due = k_uptime_get();
			reask_ms[s.ended_series] = REASK_MIN_MS; /* that ask was its first */
		}
	}
	uint8_t h[MG_PSA_HASH_LEN];

	if (!ok || mg_psa_sha256((const uint8_t *)rt->str, rt->str_len, h)) {
		memset(h, 0, sizeof(h));
	}
	mg_psa_wipe(cred_in, sizeof(cred_in));
	/* Sent by flush, and again until it goes: the commit isn't redone. */
	queue_saved(ok, h);
}

/* ------------------------------------------------------------------------
 * state, data, credit, abort
 * ---------------------------------------------------------------------- */

static void on_state(const uint8_t *p, size_t len)
{
	if (len < 3) {
		error(mg_error_code_invalid_length);
		return;
	}
	if (!s.hello || p[0] > mg_session_state_too_large) {
		error(mg_error_code_invalid_value);
		return;
	}
	/* A close that answers the device's own open_request isn't unasked. */
	bool asked = s.reopen_out != 0 || (own_close && p[0] != mg_session_state_open);

	if (p[0] != mg_session_state_open && p[0] != mg_session_state_connecting) {
		own_close = close_out = false; /* answered (or ended anyway) */
	}
	/* The session ending stayed open long enough to restart the series. */
	bool long_open = s.open && k_uptime_get() - s.opened_at >= REASK_STABLE_MS;

	reset_session();
	/* An open_request still to go: none once a session is open, only the
	 * open once one has closed. */
	s.reopen_out = p[0] == mg_session_state_open ? 0 : MIN(s.reopen_out, 1);
	s.epoch = (uint16_t)(p[1] | p[2] << 8);
	LOG_INF("mg.session: VM session %s (epoch %u)", STATE_NAMES[p[0]], s.epoch);
	/* The client spoke: an ask kept going for its silence (reask_fn) gives
	 * way, and the rules below run as if none waited. */
	if (reask_tentative) {
		(void)k_work_cancel_delayable(&reask_work);
		reask_pending = false;
		reask_tentative = false;
	}
	if (p[0] != mg_session_state_closed && p[0] != mg_session_state_no_network) {
		(void)k_work_cancel_delayable(&reask_work);
		reask_pending = false;
		reask_tentative = false;
	}
	if (p[0] == mg_session_state_not_enrolled || p[0] == mg_session_state_auth_failed ||
	    p[0] == mg_session_state_too_large) {
		reask_blocked = true;
	}
	if (long_open) {
		reask_restart(); /* both series start over: at once */
	}
	if (p[0] == mg_session_state_open) {
		reask_blocked = false;
	} else if ((p[0] == mg_session_state_closed || p[0] == mg_session_state_no_network) &&
		   !asked && !reask_blocked) {
		/* Not asked for (or our open refused): ask again, with backoff.
		 * Only after closed or no network: not_enrolled,
		 * auth_failed and too_large wait for the credentials or the cause
		 * to change (a new hello). The device's only open_request [0] is
		 * the close of a close-then-open, whose closed answer finds the
		 * open still out (reopen_out). */
		int k = p[0] == mg_session_state_no_network ? RA_NONET : RA_CLOSED;
		int64_t due = k_uptime_get() + reask_ms[k];

		if (!mg_vm_session_task_active()) {
			/* No active task: the session stays closed and
			 * nothing is asked; a task starting asks then. */
			LOG_INF("mg.session: no active task: not asking for the session");
		} else if (reask_pending && reask_kind == k) {
			/* An ask of this kind already waits: it stands, and the
			 * series doesn't advance (as the ESP32). */
		} else {
			/* With the other kind's ask waiting, the earlier of the two. */
			if (!reask_pending || due < reask_due) {
				LOG_INF("mg.session: asking for the session again in %u ms (%s)",
					(unsigned int)reask_ms[k],
					k == RA_NONET ? "no network" : "closed");
				k_work_reschedule_for_queue(mg_app_wq(), &reask_work,
							    K_MSEC(reask_ms[k]));
				reask_due = due;
			}
			/* Whichever timer waits now is this kind's committed ask. */
			reask_tentative = false;
			reask_pending = true;
			reask_kind = (uint8_t)k;
			reask_ms[k] = reask_ms[k] ? MIN(reask_ms[k] * 2, REASK_CAP_MS[k])
						  : REASK_MIN_MS;
		}
	}
	s.ended = p[0] != mg_session_state_open && p[0] != mg_session_state_connecting;
	s.ended_series = p[0] == mg_session_state_no_network ? RA_NONET : RA_CLOSED;
	if (p[0] == mg_session_state_open) {
		/* A new session: streams start from the beginning. */
		s.open = true;
		s.opened_at = k_uptime_get();
		own_reopen = false; /* what it asked for came */
		mg_vm_link_open();
	}
}

/* Client data must be a ServiceResponseEnvelope (noise_core ServiceCodec's
 * rules): anything else is invalid_value, its credit already back and its
 * counter used, so a clear or credentials_update plaintext relabeled as data
 * fails as data. */
static bool envelope_ok(const uint8_t *msg, size_t len)
{
	struct mg_env_bytes payload;

	if (mg_env_decode_response_envelope(msg, len, &payload)) {
		LOG_WRN("mg.session: data that isn't a service envelope (%u B) refused",
			(unsigned int)len);
		error(mg_error_code_invalid_value);
		return false;
	}
	return true;
}

static void on_data(const uint8_t *p, size_t len)
{
	uint8_t flags;

	if (len < 1) {
		error(mg_error_code_invalid_length);
		return;
	}
	if (!s.hello || !s.open) {
		error(mg_error_code_invalid_value);
		return;
	}
	flags = p[0];
	p++;
	len--;
	if (s.clr_erased || (s.sealed && s.tx_spent)) {
		/* From a sealed clear's erase on, or once the device's counters
		 * are spent, data is refused like a credentials_update (the keys
		 * stay only to seal the answer): its credit back, no counter
		 * taken. */
		if ((flags & mg_session_flag_first) && len >= 4) {
			len -= 4;
		}
		s.in_open = false;
		error(mg_error_code_invalid_value);
		drop_in(len);
		return;
	}
	if (flags & mg_session_flag_first) {
		if (len < 4) {
			error(mg_error_code_invalid_length);
			return;
		}
		uint32_t total = get32(p);

		p += 4;
		len -= 4;
		/* Sealed data: in a sealed session every data
		 * message is sealed, its counter the first 4 sealed bytes and the
		 * next one in this direction; unsealed is insecure. A refused
		 * message is dropped with its credit given back. */
		if (s.sealed && !(flags & mg_session_flag_sealed)) {
			error(mg_error_code_insecure);
			drop_in(len);
			return;
		}
		if (s.sealed && !s.in_open && len < 4) {
			/* A sealed first fragment shorter than its counter. */
			error(mg_error_code_invalid_length);
			drop_in(len);
			return;
		}
		if (s.in_open || total == 0 || total > MAX_MESSAGE ||
		    (!s.sealed && (flags & mg_session_flag_sealed)) ||
		    (s.sealed && total <= MGSEAL_OVERHEAD)) {
			error(mg_error_code_invalid_value);
			drop_in(len);
			return;
		}
		/* Its counter: the next one or the open hole, taken here. */
		if (s.sealed && !ctr_take(get32be(p))) {
			error(mg_error_code_invalid_value);
			drop_in(len);
			ctr_refused();
			return;
		}
		s.in_open = true;
		s.in_total = total;
		s.in_got = 0;
		s.in_held = 0;
		s.in_ctr = s.sealed ? get32be(p) : 0;
	} else if (!s.in_open) {
		error(mg_error_code_invalid_value);
		drop_in(len);
		return;
	}
	if (len == 0 || s.in_got + len > s.in_total ||
	    !!(flags & mg_session_flag_last) != (s.in_got + len == s.in_total)) {
		error(mg_error_code_invalid_value);
		drop_in(len);
		return;
	}
	if (s.in_held + s.owed + len > WINDOW) {
		/* Never past the credit granted (mgcommands.h, Credit): the
		 * window less what isn't given back. The message is dropped
		 * and counted, its bytes given back, nothing buffered. */
		s.credit_overruns++;
		LOG_WRN("mg.session: a fragment past the credit granted (%u held + %u owed + %u > "
			"%u): message dropped (%u so far)",
			s.in_held, (unsigned int)s.owed, (unsigned int)len, WINDOW,
			s.credit_overruns);
		error(mg_error_code_invalid_value);
		drop_in(len);
		return;
	}
	memcpy(&rx_msg[s.in_got], p, len);
	s.in_got += len;
	s.in_held += len;
	if (s.in_held >= WINDOW) {
		/* A message larger than the window could never finish if its
		 * bytes came back only once it's handled: rx_msg holds all of it,
		 * so they go back once they fill the window. */
		give_credit(s.in_held);
		s.in_held = 0;
	}
	if (!(flags & mg_session_flag_last)) {
		return;
	}
	s.in_open = false;
	/* Handled once it's here: all of it back, then what it asks for. */
	give_credit(s.in_held);
	s.in_held = 0;
	if (s.sealed) {
		size_t plen = 0;

		/* A refused tag doesn't use the counter. */
		if (mgseal_open(&s.keys, MGSEAL_DIR_CLIENT, s.in_ctr, rx_msg, s.in_total, &plen) !=
		    MGSEAL_OK) {
			ctr_give_back(s.in_ctr);
			LOG_WRN("mg.session: sealed data refused (tag), %u B dropped",
				(unsigned int)s.in_total);
			mg_psa_wipe(rx_msg, s.in_total);
			error(mg_error_code_invalid_value);
			return;
		}
		if (!envelope_ok(&rx_msg[4], plen)) {
			return;
		}
		mg_vm_link_rx(&rx_msg[4], plen);
		return;
	}
	if (envelope_ok(rx_msg, s.in_total)) {
		mg_vm_link_rx(rx_msg, s.in_total);
	}
}

static void on_credit(const uint8_t *p, size_t len)
{
	if (len < 4) {
		error(mg_error_code_invalid_length);
		return;
	}
	if (!s.hello) {
		error(mg_error_code_invalid_value);
		return;
	}
	s.credit += get32(p);
}

/* The keepalive (mgcommands.h, Credit): while a session is open, the credit
 * owed (bytes of messages fully taken and not given back), or credit [0] when
 * none is owed; nothing with no session open. From the app work queue, like
 * every entry here. */
void mg_vm_session_nudge(void)
{
	if (!s.open) {
		return;
	}
	/* Only fully taken messages earn credit: a message still arriving keeps
	 * its bytes until it's handled. */
	if (s.owed == 0) {
		uint8_t m[6] = {mg_command_session, mg_session_credit};

		put32(0, &m[2]);
		(void)try_send(m, sizeof(m)); /* a keepalive lost to a full queue is no loss */
		return;
	}
	flush();
}

static void on_abort(void)
{
	if (!s.hello) {
		error(mg_error_code_invalid_value);
		return;
	}
	/* A sealed message aborted after its first fragment has used its
	 * counter (taken at that fragment). */
	drop_in(0);
}

void mg_vm_session_rx(const uint8_t *p, size_t len)
{
	if (len < 1) {
		error(mg_error_code_invalid_length);
		return;
	}
	if (s.hello && !mg_setup_store_complete() && !s.clr_out_len) {
		/* The setup was erased (token proof clear): no credential, no
		 * session. A sealed clear's session ends itself once its
		 * answer has gone. */
		LOG_INF("mg.session: setup erased: session ended");
		mg_vm_session_connection_reset();
	}
	switch (p[0]) {
	case mg_session_hello:
		on_hello(&p[1], len - 1);
		break;
	case mg_session_state:
		on_state(&p[1], len - 1);
		break;
	case mg_session_data:
		on_data(&p[1], len - 1);
		break;
	case mg_session_credit:
		on_credit(&p[1], len - 1);
		break;
	case mg_session_abort:
		on_abort();
		break;
	case mg_session_credentials_update:
		on_credentials_update(&p[1], len - 1);
		break;
	case mg_session_clear:
		on_session_clear(&p[1], len - 1);
		break;
	default:
		/* Unknown, or a device -> client sub-command. */
		error(mg_error_code_unsupported);
		break;
	}
	flush();
}

int mg_vm_session_sendv(const struct mg_vm_iov *iov, size_t n)
{
	size_t total = 0;

	for (size_t i = 0; i < n; i++) {
		total += iov[i].n;
	}
	if (!s.open || s.clr_out_len) {
		return -ENOTCONN;
	}
	/* Sealed: sealed whole when queued, in the order
	 * sent, under the next device counter (shared with the credentials);
	 * the sealed bytes are what's fragmented and what credit counts. */
	size_t over = s.sealed ? MGSEAL_OVERHEAD : 0;

	if (total == 0 || total + over > s.client_max) {
		return -EMSGSIZE;
	}
	if (total + over + 4 > sizeof(txq) - s.tx_len) {
		return -ENOSPC;
	}
	put32((uint32_t)(total + over), &txq[s.tx_len]);
	s.tx_len += 4;
	size_t at = s.tx_len;

	s.tx_len += s.sealed ? 4 : 0;
	for (size_t i = 0; i < n; i++) {
		memcpy(&txq[s.tx_len], iov[i].p, iov[i].n);
		s.tx_len += iov[i].n;
	}
	if (s.sealed) {
		if (s.tx_spent ||
		    mgseal_seal(&s.keys, MGSEAL_DIR_DEVICE, s.ctr_out, &txq[at], total) !=
		    MGSEAL_OK) {
			mg_psa_wipe(&txq[at - 4], 4 + total + over);
			s.tx_len = at - 4;
			return -EIO;
		}
		ctr_out_next();
		s.tx_len += MGSEAL_TAG_LEN;
	}
	flush();
	return 0;
}

bool mg_vm_session_is_open(void)
{
	return s.open;
}

size_t mg_vm_session_tx_room(void)
{
	size_t room = sizeof(txq) - s.tx_len;

	size_t over = s.sealed ? MGSEAL_OVERHEAD : 0;

	return s.open && room > 4 + over && s.client_max > over
		       ? MIN(room - 4 - over, s.client_max - over)
		       : 0;
}

void mg_vm_session_request_reopen(void)
{
	if (!s.open) {
		return;
	}
	LOG_INF("mg.session: asking the client for a new session");
	own_reopen = true; /* a task until the open comes (task (4)) */
	s.reopen_out = 2;
	flush();
}

void mg_vm_session_write_lost(void)
{
	if (!s.hello) {
		return;
	}
	/* Its bytes (and their credit) are gone, and nothing the device sends
	 * would tell the client: ending the connection does. */
	LOG_ERR("mg.session: a client write was lost: disconnecting");
	mg_vm_session_connection_reset();
	mg_transport_disconnect();
}

static void defer_fn(struct k_work *w)
{
	uint8_t p[sizeof(deferred.p)];
	size_t len = deferred.len;

	ARG_UNUSED(w);
	if (!deferred.pending) {
		return;
	}
	if (k_uptime_get() - deferred.since >= DEFER_MS) {
		LOG_WRN("mg.session: deferred hello dropped (no memory for %d s)", DEFER_MS / 1000);
		deferred.pending = false;
		return;
	}
	memcpy(p, deferred.p, len);
	on_hello(p, len);
	flush();
}

static void reask_fn(struct k_work *w)
{
	int k = reask_kind;

	ARG_UNUSED(w);
	reask_pending = false;
	if (!s.hello || s.open || reask_blocked) {
		reask_tentative = false;
		return;
	}
	if (!mg_vm_session_task_active()) {
		/* The task ended while the series ran: it stops. */
		reask_tentative = false;
		LOG_INF("mg.session: no active task: no more asks");
		return;
	}
	if (reask_tentative) {
		/* Our own next step went: the series advances now. */
		reask_tentative = false;
		reask_ms[k] = MIN(reask_ms[k] * 2, REASK_CAP_MS[k]);
	}
	if (!s.reopen_out) {
		LOG_INF("mg.session: open_request 1 (asking for the session again)");
		s.reopen_out = 1;
		flush();
	}
	/* And again while the client says nothing (gadgets, the ESP32 among them,
	 * once stopped after the first timed ask with a silent client): the series' next step, 1, 2, 4 ... s up to its cap,
	 * timed from this ask. */
	uint32_t next = reask_ms[k] ? reask_ms[k] : REASK_MIN_MS;

	reask_ms[k] = next;
	k_work_reschedule_for_queue(mg_app_wq(), &reask_work, K_MSEC(next));
	reask_pending = true;
	reask_tentative = true;
	reask_due = k_uptime_get() + next;
}

/* A task started (a user action, traffic owed): with the session ended
 * (closed or no_network, unasked) and no ask running, ask now, as the
 * ended kind's series' next ask (the session opens for a task). */
void mg_vm_session_reason(void)
{
	int k = s.ended_series;

	if (!s.hello || s.open || reask_blocked || !s.ended || s.reopen_out || reask_pending ||
	    !mg_vm_session_task_active()) {
		return;
	}
	reask_kind = (uint8_t)k;
	if (reask_ms[k]) {
		/* The series has asked already (a short session continues it):
		 * the next ask waits for its step (mgcommands.h). */
		uint32_t next = MIN(reask_ms[k] * 2, REASK_CAP_MS[k]);

		LOG_INF("mg.session: a task: asking for the session in %u ms", next);
		reask_ms[k] = next;
		k_work_reschedule_for_queue(mg_app_wq(), &reask_work, K_MSEC(next));
		reask_pending = true;
		reask_tentative = false;
		reask_due = k_uptime_get() + next;
		return;
	}
	LOG_INF("mg.session: a task: asking for the session now");
	k_work_reschedule_for_queue(mg_app_wq(), &reask_work, K_NO_WAIT);
	reask_pending = true;
	reask_tentative = false;
	reask_due = k_uptime_get();
	reask_ms[k] = REASK_MIN_MS;
}

void mg_vm_session_user_action(void)
{
	user_action_at = k_uptime_get();
	if (user_action_at == 0) {
		user_action_at = 1;
	}
	mg_vm_session_reason();
}

void mg_vm_session_connection_reset(void)
{
	if (s.open) {
		mg_vm_link_close();
	}
	mg_vm_link_disconnected();
	(void)k_work_cancel_delayable(&reask_work);
	reask_pending = false;
	reask_tentative = false;
	reask_restart();
	reask_blocked = false;
	own_reopen = false;
	own_close = close_out = false;
	(void)k_work_cancel_delayable(&retry_work);
	(void)k_work_cancel_delayable(&defer_work);
	(void)k_work_cancel_delayable(&clr_end_work);
	deferred.pending = false;
	mg_psa_wipe(&s, sizeof(s));
	mem_put();
}

void mg_vm_session_init(void)
{
	memset(&s, 0, sizeof(s));
	k_work_init_delayable(&retry_work, retry_fn);
	k_work_init_delayable(&clr_end_work, clr_end_fn);
	k_work_init_delayable(&defer_work, defer_fn);
	k_work_init_delayable(&reask_work, reask_fn);
	mg_vm_link_init();
}

#if defined(CONFIG_BT_L2CAP_TX_MTU)
/* The board reaches the sealed session's floor, so it declares the cap (a
 * board that can't doesn't). */
BUILD_ASSERT(CONFIG_BT_L2CAP_TX_MTU >= MG_MIN_ATT_MTU, "sealed sessions need ATT_MTU >= 100");
#endif

#if defined(CONFIG_ZTEST) || defined(MG_HOST_TEST)
/* The vectors' stand-in for a long session (event "counters"): the next
 * client (rx) and device (tx) counters jump there; negative leaves one. */
void mg_vm_session_test_set_counters(int64_t rx, int64_t tx)
{
	if (rx >= 0) {
		s.ctr_in = (uint32_t)rx;
	}
	if (tx >= 0) {
		s.ctr_out = (uint32_t)tx;
	}
}
int mg_vm_session_test_set_keys(const uint8_t *k, const uint8_t *cn, const uint8_t *dn,
				const char *node)
{
	return s.sealed && mgseal_keys(&s.keys, k, cn, dn, node, strlen(node)) == MGSEAL_OK ? 0
											     : -EINVAL;
}
#endif
