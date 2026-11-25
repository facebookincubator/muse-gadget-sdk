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

#include "mg_device_session.h"

#include <string.h>

#include "mgcommands.h"

#define S_IDLE        0u /* no session */
#define S_DEFER_HELLO 1u /* a valid hello waits while busy() */
#define S_SEND_HELLO  2u /* checked and tagged: the device's hello goes out first */
#define S_SEND_CREDS  3u /* credentials fragments going out */
#define S_OPEN        4u /* hello and credentials done */
#define S_CLEARING    5u /* a sealed clear was taken: its sealed answer goes out, then the session ends */
#define S_CLEAR_WAIT  6u /* the port's erase for a sealed clear is pending: both answers are sealed, one will go */

#define F_HELLO_DONE  0x01u /* the device's hello went out: the session takes sub-commands */
#define F_SEALED      0x02u /* this session is sealed (its keys, or nonces, are in t once its hello is answered) */
#define F_UPDATE_IN   0x04u /* a credentials_update arrives in the lent storage */
#define F_UPDATE_DROP 0x08u /* one found no storage: its fragments are discarded, no answer */
#define F_UPDATE_WAIT 0x10u /* a whole, verified update waits for its commit */
#define F_DATA_IN     0x20u /* a client data message is being dropped */
#define F_VM_OPEN     0x40u /* the client reported state open */
#define F_DATA_SEALED 0x80u /* that message is sealed: its counter is used, its tag is being made */

/* A sealed clear (mg_session_clear) uses no state of its own: fragments arrive in the lent storage like a
 * credentials_update's (t->in held, no F_UPDATE_* flag, in_total CLR_LEN); taken, in_total = CLR_TAKEN until what
 * goes out before it is out; while its answer goes out (S_CLEARING) in_got holds the sealed byte, t->tag the tag. */
#define CLR_LEN   (MGSEAL_OVERHEAD + 1u) /* counter || one byte (0) || tag */
#define CLR_TAKEN 0xFFFFu                /* never an update's total (at most MGDS_SEALED_MAX) */
/* in_got while a taken clear's answer waits (MGDS_KEEP_KEYS: the erase ran when its tag checked) */
#define CLR_E_NONE 0u                    /* not erased yet */
#define CLR_E_OK   1u
#define CLR_E_FAIL 2u
#define CLR_E_PEND 3u                    /* port.clear() returned MGDS_CLEAR_PENDING */
#if MGDS_SEALED_MAX >= 0xFFFFu
#error "CLR_TAKEN must stay above every credentials_update total"
#endif

#if MGDS_SEALED_DATA && !MGSEAL_STREAM
#error "MGDS_SEALED_DATA needs MGSEAL_STREAM (mgseal_tag_begin)"
#endif

#if MGDS_DATA
/* mgds_t.ra: the re-ask series (mgcommands.h, Session state; mg-session-v1.json) and open_request owed. */
#define RA_PENDING     0x01u /* a re-ask waits for ASK_DUE */
#define RA_KIND_NONET  0x02u /* ...it is the no_network series' (else closed's) */
#define RA_BLOCK       0x04u /* not_enrolled, auth_failed or too_large: no re-ask until a hello, a saved update or an open */
#define RA_OWN0        0x08u /* the port's open_request [0]: no re-ask until a hello, a saved update, an open or [1] */
#define RA_ENDED       0x10u /* the last state ended a session (not open or connecting) */
#define RA_ENDED_NONET 0x20u /* ...with no_network (a save then asks as that series' first) */
#define RA_OWE1        0x40u /* open_request [1] to send */
#define RA_OWE0        0x80u /* open_request [0] to send */
/* The waiting re-ask's time: data_ctr is only used while a client data message is in flight, which needs the VM
 * session open, and a re-ask only waits while it isn't (open drops it). */
#define ASK_DUE(t)     ((t)->data_ctr)
/* mgds_data_t.flags */
#define DF_TAGGED      0x01u /* tx_msg's counter is reserved (tx_msg_ctr) and its tag made */
#define DF_OWE_ABORT   0x02u /* an abort for a message dropped part-way waits for a buffer */
#define DF_LIVE        0x04u /* d handled the state that opened this VM session: data flows (credit known) */
#define DF_RX_KEEP     0x08u /* the client message in flight is the port's (its first fragment came while live) */
#define DATA_PIECE_MAX 64u   /* a data_piece() at most */
#define RA_STEP_MAX    7u    /* 3 bits per series in ra_steps: past 60 s (closed) and 15 s (no_network) anyway */
#define RS_LENT        0x40u /* ra_steps: the port's mgds_data_t is attached */
#define RS_QUIET       0x80u /* ra_steps: mgds_ask_quiet(): layer 2 asks nothing by itself (no active task) */
#define RS_KEEP        (RS_LENT | RS_QUIET) /* what a series restart keeps */
#define ASK_QUIET(t)   (((t)->ra_steps & RS_QUIET) != 0)

typedef char mgds_check_ready[(S_OPEN == MGDS_STATE_OPEN && F_VM_OPEN == MGDS_FLAG_VM_OPEN) ? 1 : -1];
#endif

typedef char mgds_check_frame[(MGDS_FRAME_MAX >= 20u && MGDS_FRAME_MAX <= 512u) ? 1 : -1];
typedef char mgds_check_min_mtu[(MGDS_MIN_ATT_MTU == MG_MIN_ATT_MTU) ? 1 : -1];
/* The sealed credentials_saved: its plaintext lives in t->tag[] while it goes out. */
#if MGDS_DATA
/* The port's mgds_data_t while it is attached, else NULL. */
static mgds_data_t *dget(const mgds_t *t)
{
	return (t->ra_steps & RS_LENT) ? t->p->data : NULL;
}
#endif
typedef char mgds_check_saved[(MGSEAL_SAVED_HASH_LEN == MG_SESSION_SAVED_HASH_LEN &&
			       MGSEAL_SAVED_LEN <= MGSEAL_TAG_LEN) ? 1 : -1];

static const char SEG0[] = "{\"node_id\":\"";
static const char SEG1[] = "\",\"access_token\":\"";
static const char SEG2[] = "\",\"refresh_token\":\"";

#if MGDS_STATS
static void inc(uint8_t *c)
{
	if (*c != 0xFFu) {
		(*c)++;
	}
}
#define STAT(t, f) inc(&(t)->st.f)
#else
#define STAT(t, f) ((void)0)
#endif

static void put32le(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static uint32_t get32le(const uint8_t *b)
{
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint32_t get32be(const uint8_t *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static int send(mgds_t *t, const uint8_t *f, size_t n)
{
	return t->p->send(t->p->ctx, f, n);
}

/* The refusal's send result: MGDS_SENT, or not sent (no buffer, no link). */
static int refuse(mgds_t *t, uint8_t code)
{
	const uint8_t m[5] = {mg_command_error, mg_command_session, mg_error_data_code, code, 0};

	STAT(t, refused);
#if MGDS_STATS
	t->st.last_refusal = code & 0x1fu;
#endif
	return send(t, m, sizeof(m));
}

static void send_credit(mgds_t *t, uint32_t bytes)
{
	uint8_t m[6] = {mg_command_session, mg_session_credit, 0, 0, 0, 0};

	if (bytes == 0) {
		return;
	}
#if MGDS_DATA
	/* With the data path's state lent, credit the stack had no buffer for goes
	 * with this, or from the next poll. */
	if (dget(t) != NULL) {
		bytes = bytes > UINT32_MAX - dget(t)->credit_owed ? UINT32_MAX : bytes + dget(t)->credit_owed;
		dget(t)->credit_owed = 0;
	}
	put32le(&m[2], bytes);
	if (send(t, m, sizeof(m)) == MGDS_AGAIN && dget(t) != NULL) {
		dget(t)->credit_owed = bytes;
	}
#else
	/* Credit the stack had no buffer for goes with this, or from the next poll (at most the window is owed). */
	bytes += t->credit_owed;
	t->credit_owed = 0;
	put32le(&m[2], bytes);
	if (send(t, m, sizeof(m)) == MGDS_AGAIN) {
		t->credit_owed = bytes > 0xFFFFu ? 0xFFFFu : (uint16_t)bytes;
	}
#endif
}

#if !MGDS_DATA
static void end_session(mgds_t *t, bool link_up);

/* Credit owed (the stack had no buffer), from every poll. */
static void credit_flush(mgds_t *t)
{
	uint8_t m[6] = {mg_command_session, mg_session_credit, 0, 0, 0, 0};
	int r;

	if (t->credit_owed == 0 || t->state == S_IDLE) {
		return;
	}
	put32le(&m[2], t->credit_owed);
	r = send(t, m, sizeof(m));
	if (r == MGDS_SENT) {
		t->credit_owed = 0;
	} else if (r < 0) {
		end_session(t, false);
	}
}
#endif

static void send_abort(mgds_t *t)
{
	const uint8_t m[2] = {mg_command_session, mg_session_abort};

	STAT(t, aborted);
	(void)send(t, m, sizeof(m));
}

static void drop_storage(mgds_t *t)
{
	if (t->in == NULL) {
		return;
	}
	mgseal_wipe(t->in, t->in_got); /* in_got: the bytes written into it */
	t->in = NULL;
	t->p->release(t->p->ctx);
}

/* A fragmented sealed clear is arriving in the lent storage. */
static bool clr_in(const mgds_t *t)
{
	return t->in != NULL && !(t->flags & (F_UPDATE_IN | F_UPDATE_WAIT)) && t->in_total == CLR_LEN;
}

/* A sealed clear was taken; its answer waits for what goes out before it. */
static bool clr_taken(const mgds_t *t)
{
	return t->in == NULL && t->in_total == CLR_TAKEN;
}

/* The device has sealed its last counter (MGDS_TX_CTR_LAST) in this sealed session: counters never wrap, so it seals
 * nothing more and refuses every sealed client message with invalid_value; the client re-proves into a new session
 * (mgcommands.h, Sealed credentials). tx_ctr is 0 only then once the hello is answered (the credentials took 0), or
 * when a sealed clear gave the credentials' counter back (tx_back). */
static bool tx_spent(const mgds_t *t)
{
	return (t->flags & F_SEALED) && t->state >= S_SEND_HELLO && t->tx_ctr == 0 && !t->tx_back;
}

void mgds_init(mgds_t *t, const mgds_port_t *p)
{
	memset(t, 0, sizeof(*t));
	t->p = p;
}

/* credentials_saved is sealed only
 * in a sealed session whose client hello carried
 * mg_session_hello_flag_sealed_saved; to any other client the answer is the
 * one-byte legacy form, which takes no device counter. */
static bool seal_saved(const mgds_t *t)
{
	return (t->flags & F_SEALED) != 0 && t->saved_sealed;
}

/* A sealed credentials_saved is going out (state open, out_len set), or a
 * give-up [0] waits for its turn. */
static bool saved_owed(const mgds_t *t)
{
	return (t->state == S_OPEN && t->out_len != 0) || t->owe0 != 0;
}

bool mgds_busy(const mgds_t *t)
{
	return t->in != NULL || t->state == S_DEFER_HELLO || t->state == S_SEND_HELLO || t->nudge ||
	       t->state == S_SEND_CREDS || t->state == S_CLEARING || t->state == S_CLEAR_WAIT || clr_taken(t) ||
	       saved_owed(t) ||
	       (t->flags & (F_UPDATE_IN | F_UPDATE_DROP | F_UPDATE_WAIT)) != 0
#if MGDS_DATA
	       || (dget(t) != NULL && (dget(t)->tx_msg != NULL || dget(t)->credit_owed != 0 || (dget(t)->flags & DF_OWE_ABORT) != 0)) ||
	       (t->ra & (RA_PENDING | RA_OWE1 | RA_OWE0)) != 0
#else
	       || t->credit_owed != 0
#endif
	       ;
}

#if MGDS_SEALED_DATA
/* The tag state of the sealed data message in flight. */
static mgds_dtag_t *dtag(mgds_t *t)
{
#if MGDS_TAG_LEND
	return t->dt;
#else
	return &t->dt_own;
#endif
}

/* Done with it: a begun tag abandoned (its backend hash released), wiped, and given back if it was lent. */
static void dtag_put(mgds_t *t)
{
	mgds_dtag_t *d = dtag(t);

	if (d != NULL && t->tag_live) {
		mgseal_tag_abort(&d->tag);
	}
	t->tag_live = 0;
	if (d != NULL) {
		mgseal_wipe(d, sizeof(*d));
	}
#if MGDS_TAG_LEND
	if (d != NULL) {
		mgds_tag_return(t->p->ctx, d);
	}
	t->dt = NULL;
#endif
	t->data_bad = 0;
}
#endif

#if MGDS_DATA
/* A lent mgds_data_t back to its first state (buf and cap kept). */
static void data_clear(mgds_data_t *d)
{
	uint8_t *buf = d->buf;
	uint32_t cap = d->cap;

	mgseal_wipe(d, sizeof(*d));
	d->buf = buf;
	d->cap = cap;
	d->vm_state = mg_session_state_closed;
}

/* The data path's part of a session's end, before the rest is cleared: an
 * abort for the port's message part-way out (link up), the reassembly buffer
 * wiped, the lent state cleared (the epoch kept for mgds_vm_state()), the
 * re-ask state in mgds_t cleared. Returns what data_end_finish() reports to
 * the port once the session is idle: 1 the port's message dropped, 2 a
 * client message in pieces dropped, 4 a state the port saw open or
 * connecting. */
static uint8_t data_end_begin(mgds_t *t, bool link_up)
{
	mgds_data_t *d = dget(t);
	uint8_t cb = 0;

	if (t->flags & F_VM_OPEN) {
		cb |= 4u;
	}
	if (d != NULL) {
		uint16_t epoch = d->epoch;

		if (link_up && d->tx_msg != NULL && d->tx_off != 0) {
			send_abort(t);
		}
		if (d->tx_msg != NULL) {
			cb |= 1u;
		}
		if ((t->flags & F_DATA_IN) && (d->flags & DF_RX_KEEP)) {
			cb |= 2u;
			if (d->buf != NULL) {
				mgseal_wipe(d->buf, t->data_got);
			}
		}
		if (d->vm_state == mg_session_state_connecting) {
			cb |= 4u;
		}
		data_clear(d);
		d->epoch = epoch;
	}
	t->ra = 0;
	t->ra_steps &= RS_KEEP; /* both series start over */
	t->lim_max = t->lim_win = 0;
	return cb;
}

/* The client's hello limits: rounded down to powers of two in mgds_t (for a
 * state lent later), exact in a lent state. */
static uint8_t log2_floor(uint32_t v)
{
	uint8_t n = 0;

	while (v > 1u) {
		v >>= 1;
		n++;
	}
	return n;
}

static void data_hello(mgds_t *t, uint32_t max, uint32_t window)
{
	t->lim_max = log2_floor(max);
	t->lim_win = log2_floor(window);
	if (dget(t) != NULL) {
		dget(t)->client_max = max;
		dget(t)->client_window = window;
	}
}

static void data_end_finish(mgds_t *t, uint8_t cb)
{
	const mgds_port_t *p = t->p;

	if ((cb & 2u) && dget(t) != NULL && dget(t)->buf == NULL && p->data_end != NULL) {
		p->data_end(p->ctx, false);
	}
	if ((cb & 1u) && p->data_sent != NULL) {
		p->data_sent(p->ctx, false);
	}
	if ((cb & 4u) && p->data_state != NULL) {
		p->data_state(p->ctx, mg_session_state_closed, dget(t) != NULL ? dget(t)->epoch : 0);
	}
}
#endif

static void end_session(mgds_t *t, bool link_up)
{
	bool erase = clr_taken(t) && t->in_got == CLR_E_NONE; /* a sealed clear taken, not erased yet: the Forget still runs */
#if MGDS_DATA
	uint8_t cb;
#endif
	/* The credentials, a sealed credentials_saved or the clear's answer, part-way out. */
	if (link_up && t->out_off != 0 &&
	    (t->state == S_SEND_CREDS || t->state == S_OPEN || (t->state == S_CLEARING && t->out_off < t->out_len))) {
		send_abort(t);
	}
#if MGDS_DATA
	cb = data_end_begin(t, link_up);
#endif
	if (t->in != NULL) {
		mgseal_wipe(t->in, t->in_got);
		t->in = NULL;
		t->p->release(t->p->ctx);
	}
#if MGDS_KEEP_KEYS
	mgseal_wipe(&t->keys, sizeof(t->keys));
#else
	mgseal_wipe(t->nonces, sizeof(t->nonces));
#endif
	mgseal_wipe(t->tag, sizeof(t->tag));
#if MGDS_SEALED_DATA
	dtag_put(t);
	t->data_ctr = 0;
#endif
	t->state = S_IDLE;
#if MGDS_CTR_END
	t->ctr_bad = 0;
#endif
	t->flags = 0;
	t->out_len = t->out_off = t->in_total = t->in_got = t->data_total = t->data_got = 0;
	t->rx_counter = 0;
	t->rx_hole = 0;
	t->rx_top = 0;
	t->nudge = 0;
	t->tx_ctr = 0;
	t->tx_back = 0;
	t->owe0 = 0;
	t->saved_sealed = 0;
#if !MGDS_DATA
	t->credit_owed = 0;
#endif
#if MGDS_STALL
	t->owing = 0;
#endif
#if MGDS_DATA
	data_end_finish(t, cb);
#endif
	if (erase && t->p->clear != NULL) {
		(void)t->p->clear(t->p->ctx);
	}
}

void mgds_reset(mgds_t *t, bool link_up)
{
	end_session(t, link_up);
}

static void give_up(mgds_t *t);
#if MGDS_GIVE_UP_LATER
static void give_up_later(mgds_t *t);
#endif

void mgds_release_storage(mgds_t *t)
{
	if (t->in == NULL) {
		return;
	}
	if (t->flags & F_UPDATE_IN) {
		t->flags = (uint8_t)((t->flags & ~F_UPDATE_IN) | F_UPDATE_DROP);
	}
	bool held = (t->flags & F_UPDATE_WAIT) != 0;

	if (held) {
		t->flags &= (uint8_t)~F_UPDATE_WAIT;
		STAT(t, dropped);
	}
	mgseal_wipe(t->in, t->in_got);
	t->in = NULL;
	t->in_total = t->in_got = 0;
	t->p->release(t->p->ctx);
	if (held) {
#if MGDS_GIVE_UP_LATER
		give_up_later(t); /* sealed [0] from the next mgds_poll(); one still arriving gets it at its last fragment */
#else
		give_up(t); /* sealed [0]; one still arriving gets it at its last fragment */
#endif
	}
}

/* ---- the credentials, read in pieces --------------------------------------- */

static bool json_safe(const uint8_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (s[i] < 0x20u || s[i] > 0x7Eu || s[i] == '"' || s[i] == '\\') {
			return false;
		}
	}
	return true;
}

typedef struct {
	const mgds_port_t *p;
	char node[MGDS_NODE_MAX];
	uint8_t nl;
	uint16_t tl[2]; /* access, refresh */
	char tail[48];  /* the closing "}, with ,"access_expires_at":N before the brace when the board keeps one */
	uint8_t tail_len;
} src_t;

static const char EXP_KEY[] = "access_expires_at";

/* "\"" + optional ,"access_expires_at":<at> + "}" */
static void make_tail(src_t *s, uint64_t at)
{
	char d[20];
	int n = 0;

	s->tail_len = 0;
	s->tail[s->tail_len++] = '"';
	if (at) {
		s->tail[s->tail_len++] = ',';
		s->tail[s->tail_len++] = '"';
		memcpy(&s->tail[s->tail_len], EXP_KEY, sizeof(EXP_KEY) - 1);
		s->tail_len += (uint8_t)(sizeof(EXP_KEY) - 1);
		s->tail[s->tail_len++] = '"';
		s->tail[s->tail_len++] = ':';
		while (at) {
			d[n++] = (char)('0' + (int)(at % 10u));
			at /= 10u;
		}
		while (n) {
			s->tail[s->tail_len++] = d[--n];
		}
	}
	s->tail[s->tail_len++] = '}';
}

/* The stored document's length: SEG0 node SEG1 access SEG2 refresh tail. */
static uint32_t doc_len(uint32_t nl, uint32_t al, uint32_t rl, uint32_t tail)
{
	return (uint32_t)(sizeof(SEG0) - 1 + sizeof(SEG1) - 1 + sizeof(SEG2) - 1) + nl + al + rl + tail;
}

/* node_id and the pair's lengths; the plaintext's length. 0 without a usable
 * record. */
static uint32_t load_src(const mgds_port_t *p, src_t *s)
{
	uint32_t len;

	memset(s, 0, sizeof(*s));
	s->p = p;
	if (p->node_id(p->ctx, s->node, &s->nl) != MGDS_OK || s->nl == 0 || s->nl > MGDS_NODE_MAX ||
	    !json_safe((const uint8_t *)s->node, s->nl)) {
		return 0;
	}
	for (int w = 0; w < 2; w++) {
		if (p->token_len(p->ctx, w, &s->tl[w]) != MGDS_OK || s->tl[w] == 0) {
			return 0;
		}
	}
	{
		uint64_t at = 0;

		if (p->expires == NULL || p->commit_expires == NULL || !p->expires(p->ctx, &at)) {
			at = 0;
		}
		make_tail(s, at);
	}
	len = doc_len(s->nl, s->tl[0], s->tl[1], s->tail_len);
	return len <= MGDS_MAX_CREDENTIALS ? len : 0;
}

/* A credentials_update's pair (al and rl bytes, access_expires_at exp or 0), stored: the next credentials hands the
 * whole stored document back (load_src), so it must fit MGDS_MAX_CREDENTIALS too. node_id as the board has it;
 * access_expires_at only where the board keeps one. */
static bool update_fits(const mgds_t *t, size_t al, size_t rl, uint64_t exp)
{
	const mgds_port_t *p = t->p;
	src_t s;
	bool fits;

	memset(&s, 0, sizeof(s));
	if (p->node_id(p->ctx, s.node, &s.nl) != MGDS_OK || s.nl == 0 || s.nl > MGDS_NODE_MAX ||
	    !json_safe((const uint8_t *)s.node, s.nl)) {
		return false; /* no hello could hand any pair back (load_src) */
	}
	make_tail(&s, p->expires != NULL && p->commit_expires != NULL ? exp : 0);
	fits = al <= MGDS_MAX_CREDENTIALS && rl <= MGDS_MAX_CREDENTIALS - al &&
	       doc_len(s.nl, (uint32_t)al, (uint32_t)rl, s.tail_len) <= MGDS_MAX_CREDENTIALS;
	mgseal_wipe(&s, sizeof(s));
	return fits;
}

/* mgseal_read_fn over the virtual plaintext SEG0 node SEG1 access SEG2
 * refresh tail ("} or ,"access_expires_at":N}); a token byte that isn't a token
 * character fails the read. */
static int src_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	const src_t *s = ctx;
	const uint32_t seg_len[7] = {sizeof(SEG0) - 1, s->nl, sizeof(SEG1) - 1, s->tl[0],
				     sizeof(SEG2) - 1, s->tl[1], s->tail_len};
	const char *const fixed[7] = {SEG0, s->node, SEG1, NULL, SEG2, NULL, s->tail};
	uint32_t start = 0;

	for (int i = 0; i < 7 && n; i++) {
		uint32_t end = start + seg_len[i];

		if (off < end) {
			uint32_t at = off - start;
			size_t m = end - off < n ? end - off : n;

			if (fixed[i] != NULL) {
				memcpy(buf, fixed[i] + at, m);
			} else {
				int w = i == 3 ? 0 : 1;

				if (s->p->token_read(s->p->ctx, w, (uint16_t)at, buf, m) != MGDS_OK ||
				    !json_safe(buf, m)) {
					return -1;
				}
			}
			buf += m;
			off += (uint32_t)m;
			n -= m;
		}
		start = end;
	}
	return n == 0 ? 0 : -1;
}

/* The seal keys from K, the proof's nonces and node_id. */
static int derive(const mgds_t *t, const src_t *s, const uint8_t nonces[32], mgseal_keys_t *keys)
{
	uint8_t k[32];
	int r = t->p->get_k(t->p->ctx, k) == MGDS_OK ? MGSEAL_OK : MGSEAL_E_CRYPTO;

	if (r == MGSEAL_OK) {
		r = mgseal_keys(keys, k, nonces, &nonces[16], s->node, s->nl);
	}
	mgseal_wipe(k, sizeof(k));
	return r;
}

/* This session's keys: kept (MGDS_KEEP_KEYS), or derived again into *tmp
 * (the caller wipes it). NULL if they can't be had. */
static const mgseal_keys_t *session_keys(mgds_t *t, mgseal_keys_t *tmp)
{
#if MGDS_KEEP_KEYS
	(void)tmp;
	return &t->keys;
#else
	src_t s;
	int r = load_src(t->p, &s) != 0 ? derive(t, &s, t->nonces, tmp) : MGSEAL_E_CRYPTO;

	mgseal_wipe(&s, sizeof(s));
	return r == MGSEAL_OK ? tmp : NULL;
#endif
}

/* The hello's checks after its length, in the reference's order. 0: answer it. */
static uint8_t hello_check(mgds_t *t, bool sealed)
{
	uint8_t k[32];
	int r;

	if (t->p->recovery(t->p->ctx)) {
		return mg_error_code_recovery;
	}
	r = t->p->get_k(t->p->ctx, k);
	mgseal_wipe(k, sizeof(k));
	if (r != MGDS_OK) {
		return mg_error_code_not_found;
	}
	if (!t->p->proven(t->p->ctx)) {
		return mg_error_code_proof_required;
	}
	if (!sealed) {
		return mg_error_code_insecure; /* credentials go only sealed: a relayed proof plus the relay's own bond would pass a link check */
	}
	return 0;
}

static void pump_saved(mgds_t *t);

/* Sends what the session owes until the stack has no buffer. */
static void pump(mgds_t *t)
{
	uint8_t f[MGDS_FRAME_MAX];
	src_t s;
	mgseal_keys_t tmp;
	const mgseal_keys_t *keys = NULL;
	bool sealed = (t->flags & F_SEALED) != 0, ready = false;

	for (int guard = 0; guard < 64; guard++) {
		if (t->state == S_SEND_HELLO) {
			size_t n = 11;
			int r;

			f[0] = mg_command_session;
			f[1] = mg_session_hello;
			f[2] = MG_SESSION_VERSION;
			put32le(&f[3], t->p->max_message);
			put32le(&f[7], t->p->window);
			if (sealed) {
				f[n++] = (uint8_t)(mg_session_hello_flag_sealed |
						   (t->saved_sealed ? mg_session_hello_flag_sealed_saved : 0));
			}
			r = send(t, f, n);
			if (r == MGDS_AGAIN) {
				break;
			}
			if (r < 0) {
				end_session(t, false);
				break;
			}
			t->flags |= F_HELLO_DONE;
			t->state = S_SEND_CREDS;
			STAT(t, hellos);
			if (sealed) {
				STAT(t, sealed);
			}
			continue;
		}
		if (t->state != S_SEND_CREDS) {
			break;
		}
		if (!ready) {
			/* The same plaintext as the tag's, under this session's keys. */
			uint32_t len = load_src(t->p, &s);

			if (len == 0 || len + (sealed ? MGSEAL_OVERHEAD : 0u) != t->out_len ||
			    (sealed && (keys = session_keys(t, &tmp)) == NULL)) {
				end_session(t, true);
				break;
			}
			ready = true;
		}
		uint16_t mtu = t->p->mtu(t->p->ctx);
		uint32_t room = mtu > 3u ? (uint32_t)mtu - 3u : 0u;
		bool first = t->out_off == 0;
		uint32_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
		uint32_t take, pos, plain = t->out_len - (sealed ? MGSEAL_OVERHEAD : 0u);
		int r = 0;

		if (room > MGDS_FRAME_MAX) {
			room = MGDS_FRAME_MAX;
		}
		if (room <= hdr) {
			end_session(t, true);
			break;
		}
		take = t->out_len - t->out_off;
		if (take > room - hdr) {
			take = room - hdr;
		}
		f[0] = mg_command_session;
		f[1] = mg_session_credentials;
		f[2] = (uint8_t)((first ? mg_session_flag_first : 0) |
				 (t->out_off + take == t->out_len ? mg_session_flag_last : 0) |
				 (sealed ? mg_session_flag_sealed : 0));
		if (first) {
			put32le(&f[3], t->out_len);
		}
		/* The wire bytes [out_off, out_off + take): sealed, counter (0) ||
		 * ciphertext || tag; plain, the plaintext. */
		pos = t->out_off;
		while (pos < (uint32_t)t->out_off + take && r == 0) {
			uint8_t *d = &f[hdr + (pos - t->out_off)];
			uint32_t end = (uint32_t)t->out_off + take, m;

			if (sealed && pos < 4u) {
				*d = 0;
				pos++;
				continue;
			}
			if (sealed && pos >= 4u + plain) {
				m = end - pos;
				memcpy(d, &t->tag[pos - 4u - plain], m);
				pos += m;
				continue;
			}
			uint32_t po = sealed ? pos - 4u : pos;

			m = plain - po;
			if (m > end - pos) {
				m = end - pos;
			}
			if (m > 32u) {
				m = 32u;
			}
			r = src_read(&s, po, d, m);
			if (r == 0 && sealed) {
				r = mgseal_xor_at(keys, MGSEAL_DIR_DEVICE, 0, po, d, m);
			}
			pos += m;
		}
		if (r != 0) {
			mgseal_wipe(f, sizeof(f));
			end_session(t, true);
			break;
		}
		r = send(t, f, hdr + take);
		mgseal_wipe(f, hdr + take);
		if (r == MGDS_AGAIN) {
			break;
		}
		if (r < 0) {
			end_session(t, false);
			break;
		}
		t->out_off = (uint16_t)(t->out_off + take);
		if (t->out_off == t->out_len) {
			t->state = S_OPEN;
			t->out_len = t->out_off = 0;
			mgseal_wipe(t->tag, sizeof(t->tag)); /* free now: a credentials_saved's plaintext goes here */
			break;
		}
	}
	if (ready) {
		mgseal_wipe(&tmp, sizeof(tmp));
		mgseal_wipe(&s, sizeof(s));
	}
	pump_saved(t); /* answers owed behind the credentials */
}

/* Sends the sealed credentials_saved answers owed, once
 * the credentials are out, until the stack has no buffer: one at a time,
 * each under the next device -> client counter, framed as credentials. Its
 * plaintext is in tag[] (out_len = MGSEAL_SAVED_SEALED while it goes out),
 * sealed again for every fragment (the same bytes); a give-up [0] waiting in
 * owe0 starts when the one before it is out. One write (mgcommands.h) at
 * any MTU from 39, whatever MGDS_FRAME_MAX: a sealed session's MTU is at
 * least MG_MIN_ATT_MTU. */
static void pump_saved(mgds_t *t)
{
	uint8_t f[MG_SESSION_FIRST_HEADER + MGSEAL_SAVED_SEALED], blob[MGSEAL_SAVED_SEALED];
	mgseal_keys_t tmp;

	for (int guard = 0; guard < 16 && t->state == S_OPEN && seal_saved(t); guard++) {
		const mgseal_keys_t *keys;
		uint16_t mtu = t->p->mtu(t->p->ctx);
		uint32_t room = mtu > 3u ? (uint32_t)mtu - 3u : 0u, hdr, take;
		bool first;
		int r;

		if (tx_spent(t)) {
			/* No counter left to seal an answer under: none goes out in this session. */
			t->owe0 = 0;
			t->out_len = t->out_off = 0;
			mgseal_wipe(t->tag, sizeof(t->tag));
			break;
		}
		if (t->out_len == 0) {
			if (t->owe0 == 0) {
				break;
			}
			t->owe0--;
			memset(t->tag, 0, MGSEAL_SAVED_LEN); /* not saved: result 0, a zero hash */
			t->out_len = MGSEAL_SAVED_SEALED;
			t->out_off = 0;
		}
		first = t->out_off == 0;
		hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
		if (room > sizeof(f)) {
			room = (uint32_t)sizeof(f);
		}
#if MGDS_DATA
		/* One device -> client counter for both, and never two sealed
		 * messages part-way: a data message that took
		 * its counter goes first, and an answer never starts between its
		 * fragments; it waits for the data's last fragment, the abort owed
		 * for it, or the state that ends it (the Muse apps count a device
		 * message's counter when it completes or a state cuts it off). */
		if (first && dget(t) != NULL && (dget(t)->flags & (DF_TAGGED | DF_OWE_ABORT)) != 0) {
			break;
		}
#endif
		keys = room > hdr ? session_keys(t, &tmp) : NULL;
		memcpy(&blob[4], t->tag, MGSEAL_SAVED_LEN);
		r = keys != NULL ? mgseal_seal(keys, MGSEAL_DIR_DEVICE, t->tx_ctr, blob, MGSEAL_SAVED_LEN)
				 : MGSEAL_E_CRYPTO;
		mgseal_wipe(&tmp, sizeof(tmp));
		if (r != MGSEAL_OK) {
			mgseal_wipe(blob, sizeof(blob));
			end_session(t, true); /* can't seal it: the client re-proves */
			break;
		}
		take = (uint32_t)t->out_len - t->out_off;
		if (take > room - hdr) {
			take = room - hdr;
		}
		f[0] = mg_command_session;
		f[1] = mg_session_credentials_saved;
		f[2] = (uint8_t)((first ? mg_session_flag_first : 0) |
				 (t->out_off + take == t->out_len ? mg_session_flag_last : 0) | mg_session_flag_sealed);
		if (first) {
			put32le(&f[3], t->out_len);
		}
		memcpy(&f[hdr], &blob[t->out_off], take);
		mgseal_wipe(blob, sizeof(blob));
		r = send(t, f, hdr + take);
		mgseal_wipe(f, hdr + take);
		if (r == MGDS_AGAIN) {
			break; /* the same fragment from the next mgds_poll() */
		}
		if (r < 0) {
			end_session(t, false);
			break;
		}
		t->out_off = (uint16_t)(t->out_off + take);
		if (t->out_off == t->out_len) {
			t->out_len = t->out_off = 0;
			mgseal_wipe(t->tag, sizeof(t->tag));
			t->tx_ctr++; /* past MGDS_TX_CTR_LAST it is 0: spent (tx_spent) */
			t->tx_back = 0;
		}
	}
}

/* A write-back given up on: in a sealed session whose client asked for
 * the sealed answer, answered sealed [0] once the credentials and any answer
 * before it are out. Any other session stays silent. */
static void give_up(mgds_t *t)
{
	if ((t->flags & (F_SEALED | F_HELLO_DONE)) != (F_SEALED | F_HELLO_DONE) || !t->saved_sealed) {
		return;
	}
	if (t->owe0 != 0xFu) {
		t->owe0++;
	}
	pump_saved(t);
}

#if MGDS_GIVE_UP_LATER
/* give_up() without the seal now: the [0] is owed (saved_owed(), so
 * mgds_busy()) and mgds_poll() sends it, as when the stack had no buffer. */
static void give_up_later(mgds_t *t)
{
	if ((t->flags & (F_SEALED | F_HELLO_DONE)) != (F_SEALED | F_HELLO_DONE) || !t->saved_sealed) {
		return;
	}
	if (t->owe0 != 0xFu) {
		t->owe0++;
	}
}
#endif

/* The (possibly deferred) answer to a valid hello: take the proof's nonces
 * for a sealed one, read the credentials once (the tag, and every token byte
 * checked), then send. */
static void answer_hello(mgds_t *t)
{
	bool sealed = (t->flags & F_SEALED) != 0;
	uint8_t code = hello_check(t, sealed);
	uint8_t nonces[32];
	src_t s;
	uint32_t len;

	if (code == 0 && sealed && !t->p->take_nonces(t->p->ctx, nonces, &nonces[16])) {
		code = mg_error_code_proof_required; /* one sealed session per token proof */
	}
	len = code == 0 ? load_src(t->p, &s) : 0;
	if (code == 0 && len == 0) {
		code = mg_error_code_not_found;
	}
	if (code == 0 && sealed) {
#if MGDS_KEEP_KEYS
		mgseal_keys_t *keys = &t->keys; /* kept for the session */
#else
		mgseal_keys_t tmp, *keys = &tmp;

		memcpy(t->nonces, nonces, sizeof(nonces)); /* kept for the session */
#endif
		int r = derive(t, &s, nonces, keys);

		if (r == MGSEAL_OK) {
			r = mgseal_tag_stream(keys, MGSEAL_DIR_DEVICE, 0, len, src_read, &s, t->tag);
		}
#if !MGDS_KEEP_KEYS
		mgseal_wipe(&tmp, sizeof(tmp));
#endif
		if (r != MGSEAL_OK) {
			code = mg_error_code_not_found;
		}
	} else if (code == 0) {
		uint8_t chunk[32];

		for (uint32_t off = 0; off < len && code == 0; off += 32u) {
			if (src_read(&s, off, chunk, len - off < 32u ? len - off : 32u) != 0) {
				code = mg_error_code_not_found;
			}
		}
		mgseal_wipe(chunk, sizeof(chunk));
	}
	mgseal_wipe(&s, sizeof(s));
	mgseal_wipe(nonces, sizeof(nonces));
	if (code != 0) {
		end_session(t, true);
		refuse(t, code);
		return;
	}
	t->out_len = (uint16_t)(len + (sealed ? MGSEAL_OVERHEAD : 0u));
	t->out_off = 0;
	t->tx_ctr = 1; /* the credentials take device -> client counter 0 */
	t->tx_back = 0;
	t->state = S_SEND_HELLO;
	pump(t);
}

static void on_hello(mgds_t *t, const uint8_t *p, size_t n, uint32_t now)
{
	bool sealed, saved;
	uint32_t max, window;
	uint8_t code;

	if (n < 9) {
		refuse(t, mg_error_code_invalid_length);
		return;
	}
	sealed = n >= 10 && (p[9] & mg_session_hello_flag_sealed) != 0;
	saved = sealed && (p[9] & mg_session_hello_flag_sealed_saved) != 0;
	max = get32le(&p[1]);
	window = get32le(&p[5]);
	code = hello_check(t, sealed);
	if (code == 0 && (p[0] != MG_SESSION_VERSION || max < MG_SESSION_MIN_MESSAGE ||
			  max > MG_SESSION_MAX_MESSAGE || window < 4u || window > max ||   /* a sealed window holds the counter */
			  t->p->mtu(t->p->ctx) < MGDS_MIN_ATT_MTU)) { /* its one-write messages need MG_MIN_ATT_MTU */
		code = mg_error_code_invalid_value;
	}
	/* One sealed session per token proof: a sealed session's nonces came from
	 * the current proof (the board resets the session at every new
	 * challenge), so a second sealed hello is refused and the first session
	 * goes on. */
	if (code == 0 && sealed && (t->flags & F_SEALED)) {
		code = mg_error_code_proof_required;
	}
	if (code != 0) {
		refuse(t, code); /* a refused hello leaves a session as it was */
		return;
	}
	/* An accepted hello resets the session, as state closed does. */
	end_session(t, true);
#if MGDS_DATA
	data_hello(t, max, window);
#endif
	if (sealed) {
		t->flags |= F_SEALED;
		t->saved_sealed = saved ? 1u : 0u;
	}
	if (t->p->busy(t->p->ctx)) {
		t->state = S_DEFER_HELLO;
		t->defer_at = now;
		STAT(t, deferred);
		return;
	}
	answer_hello(t);
}

/* ---- credentials_update ---------------------------------------------------- */

/* A JSON string at s[*i] (its opening quote); its body is s[*b, *e). Escapes
 * are skipped, not decoded. */
static bool json_string(const uint8_t *s, size_t n, size_t *i, size_t *b, size_t *e, bool *esc)
{
	if (*i >= n || s[*i] != '"') {
		return false;
	}
	*esc = false;
	*b = ++*i;
	while (*i < n) {
		uint8_t c = s[*i];

		if (c == '"') {
			*e = (*i)++;
			return true;
		}
		if (c < 0x20u) {
			return false;
		}
		if (c == '\\') {
			uint8_t x;

			*esc = true;
			if (*i + 1 >= n) {
				return false;
			}
			x = s[*i + 1];
			if (x == 'u') {
				if (*i + 5 >= n) {
					return false;
				}
				*i += 6;
				continue;
			}
			if (x == 0 || strchr("\"\\/bfnrt", x) == NULL) {
				return false;
			}
			*i += 2;
			continue;
		}
		(*i)++;
	}
	return false;
}

static void skip_ws(const uint8_t *s, size_t n, size_t *i)
{
	while (*i < n && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r')) {
		(*i)++;
	}
}

/* access_token and refresh_token in a flat JSON object (values: strings,
 * numbers, true / false / null; nothing nested), each once, as non-empty
 * strings of token characters. */
/* An unsigned decimal integer (no sign, fraction or exponent) that fits 64 bits, else 0. */
static uint64_t json_u64(const uint8_t *w, size_t n)
{
	uint64_t v = 0;

	if (n == 0 || n > 20) {
		return 0;
	}
	for (size_t k = 0; k < n; k++) {
		uint64_t d;

		if (w[k] < '0' || w[k] > '9') {
			return 0;
		}
		d = (uint64_t)(w[k] - '0');
		if (v > (UINT64_MAX - d) / 10u) {
			return 0;
		}
		v = v * 10u + d;
	}
	return v;
}

static bool parse_update(const uint8_t *s, size_t n, size_t *ab, size_t *al, size_t *rb, size_t *rl,
			 uint64_t *exp)
{
	size_t i = 0;
	unsigned seen = 0;

	if (n == 0 || s[i++] != '{') {
		return false; /* '{' first, no whitespace: other plaintext (a relabelled data message) is never an update */
	}
	for (;;) {
		size_t kb = 0, ke = 0, vb = 0, ve = 0, nb = 0, nl = 0;
		bool esc = false, vesc = false, str = false;
		int which = -1;

		skip_ws(s, n, &i);
		if (!json_string(s, n, &i, &kb, &ke, &esc)) {
			return false;
		}
		skip_ws(s, n, &i);
		if (i >= n || s[i++] != ':') {
			return false;
		}
		skip_ws(s, n, &i);
		if (i < n && s[i] == '"') {
			if (!json_string(s, n, &i, &vb, &ve, &vesc)) {
				return false;
			}
			str = true;
		} else {
			size_t v0 = i, vl;
			const char *w;

			while (i < n && (s[i] == '-' || s[i] == '+' || s[i] == '.' ||
					 (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') ||
					 s[i] == 'E')) {
				i++;
			}
			vl = i - v0;
			w = (const char *)&s[v0];
			if (vl == 0) {
				return false; /* an object, an array, or garbage */
			}
			if (!((vl == 4 && (!memcmp(w, "true", 4) || !memcmp(w, "null", 4))) ||
			      (vl == 5 && !memcmp(w, "false", 5)) || w[0] == '-' ||
			      (w[0] >= '0' && w[0] <= '9'))) {
				return false;
			}
			nb = v0;
			nl = vl;
		}
		if (!esc && ke - kb == 12 && !memcmp(&s[kb], "access_token", 12)) {
			which = 0;
		} else if (!esc && ke - kb == 13 && !memcmp(&s[kb], "refresh_token", 13)) {
			which = 1;
		} else if (!esc && ke - kb == sizeof(EXP_KEY) - 1 && !memcmp(&s[kb], EXP_KEY, sizeof(EXP_KEY) - 1)) {
			if ((seen >> 2) & 1u) {
				return false; /* a duplicate */
			}
			seen |= 1u << 2;
			*exp = str ? 0 : json_u64(&s[nb], nl); /* any other form: none */
		}
		if (which >= 0) {
			if ((seen >> which) & 1u) {
				return false; /* a duplicate */
			}
			if (!str || vesc || ve == vb || ve - vb > 0xFFFFu || !json_safe(&s[vb], ve - vb)) {
				return false;
			}
			seen |= 1u << which;
			if (which == 0) {
				*ab = vb;
				*al = ve - vb;
			} else {
				*rb = vb;
				*rl = ve - vb;
			}
		}
		skip_ws(s, n, &i);
		if (i < n && s[i] == ',') {
			i++;
			continue;
		}
		if (i < n && s[i] == '}') {
			i++;
			break;
		}
		return false;
	}
	skip_ws(s, n, &i);
	return i == n && (seen & 3u) == 3u;
}

/* A sealed message's counter, read at its first fragment: the next one, or the
 * one a tag refusal left behind (unuse_counter). The rule is layer 1's
 * (mgseal_rx_*), shared with the boards that run their own session code. */
static bool counter_ok(const mgds_t *t, uint32_t c)
{
	/* Counters never wrap: once 0xFFFFFFFF is taken (rx_top, rx_counter wrapped to 0) no counter is the next one;
	 * a hole still may be. */
	return mgseal_rx_ok(t->rx_counter, t->rx_hole, c) && !(t->rx_top && c == t->rx_counter);
}

/* Takes it (counter_ok): used from here (a drop, an abort, a state change),
 * unless its tag is refused. */
static void take_counter(mgds_t *t, uint32_t c)
{
	mgseal_rx_take(&t->rx_counter, &t->rx_hole, c);
	if (c == 0xFFFFFFFFu) {
		t->rx_top = 1; /* the last counter: rx_counter wrapped, nothing is next any more */
	}
}

/* A sealed message refused at its tag gives its counter back (the counter was
 * taken at its first fragment): a forged or damaged message can't move the
 * session's counter, so the client's real message with that counter still
 * goes through. When a later counter was taken meanwhile (a credentials_update
 * and a data message in flight together), the refused one is kept as the one
 * counter that may still come, once (rx_hole). */
static void unuse_counter(mgds_t *t, uint32_t ctr)
{
	if (t->rx_top && ctr != 0xFFFFFFFFu) {
		/* The next counter is 2^32 (rx_counter wrapped): the hole is 2^32 - ctr, if within 255. */
		if (0u - ctr <= 255u) {
			t->rx_hole = (uint8_t)(0u - ctr);
		}
		return;
	}
	mgseal_rx_unuse(&t->rx_counter, &t->rx_hole, ctr);
	if (t->rx_top && t->rx_counter == 0xFFFFFFFFu) {
		t->rx_top = 0; /* 0xFFFFFFFF given back: it is the next one again */
	}
}

#if MGDS_CTR_END > 3
#error "MGDS_CTR_END: 0..3 (mgds_t.ctr_bad is 2 bits)"
#endif
/* MGDS_CTR_END: a sealed first fragment's counter
 * checked (counter_ok): an acceptable one resets the run of counter refusals;
 * a refused one (its refusal already out) counts, and the MGDS_CTR_END-th in a
 * row ends the session (the client's next open runs a fresh hello). Never on
 * a tag refusal (its counter was acceptable, the run already reset). */
static bool ctr_check(mgds_t *t, uint32_t c)
{
	bool ok = counter_ok(t, c);

#if MGDS_CTR_END
	if (ok) {
		t->ctr_bad = 0;
	}
#endif
	return ok;
}

static void ctr_refused(mgds_t *t)
{
#if MGDS_CTR_END
	if (t->state != S_IDLE && ++t->ctr_bad >= MGDS_CTR_END) {
#if MGDS_STATS
		if (t->st.ctr_ends != 7u) {
			t->st.ctr_ends++;
		}
#endif
		end_session(t, true);
	}
#else
	(void)t;
#endif
}

/* MGDS_END_ON_BAD_TAG: a sealed message refused at its tag ends the sealed
 * session too (its refusal went out first): one forged message per proof. */
static void end_on_bad_tag(mgds_t *t)
{
#if MGDS_END_ON_BAD_TAG
	end_session(t, true);
#else
	(void)t;
#endif
}

/* Refused: only an update still arriving is dropped with it (as the reference drops its update in flight). A
 * verified update waiting for its commit and a sealed clear (arriving or taken) aren't this message's, so they stay.
 * A refusal the stack had no buffer for is answered sealed [0] instead (give_up(): owed, sent from the next poll),
 * never silence, which a client reads as saved. */
static void refuse_update(mgds_t *t, uint8_t code)
{
	STAT(t, bad_updates);
	if (t->flags & F_UPDATE_IN) {
		t->flags &= (uint8_t)~F_UPDATE_IN;
		drop_storage(t);
		t->in_total = t->in_got = 0;
	}
	if (refuse(t, code) != MGDS_SENT) {
		give_up(t);
	}
}

static void bad_update(mgds_t *t)
{
	refuse_update(t, mg_error_code_invalid_value);
}

/* The verified update (plaintext at in + base, in_total bytes): commit and
 * answer, after the commit. Sealed: [saved || SHA-256 of
 * its refresh_token, 8 bytes] into tag[] for pump_saved(); the caller made
 * sure the credentials and any answer before it are out (saved_owed()). */
static void commit_update(mgds_t *t)
{
	bool sealed = seal_saved(t); /* the answer's form; the update itself is sealed in any sealed session */
	size_t base = (t->flags & F_SEALED) ? 4u : 0u, ab = 0, al = 0, rb = 0, rl = 0;
	const uint8_t *pt = t->in + base;
	uint8_t m[3] = {mg_command_session, mg_session_credentials_saved, 0};
	uint64_t exp = 0;

	if (tx_spent(t)) {
		/* Held while the last counter went out: no counter is left to answer it under, so nothing is
		 * committed and it is refused as one arriving now is (on_update); the client re-proves. */
		t->flags &= (uint8_t)~F_UPDATE_WAIT;
		drop_storage(t);
		t->in_total = t->in_got = 0;
		refuse_update(t, mg_error_code_invalid_value);
		return;
	}
	if (parse_update(pt, t->in_total, &ab, &al, &rb, &rl, &exp)) {
		int r = t->p->commit_expires != NULL && t->p->expires != NULL
				? t->p->commit_expires(t->p->ctx, (const char *)&pt[ab], (uint16_t)al,
						       (const char *)&pt[rb], (uint16_t)rl, exp)
				: t->p->commit(t->p->ctx, (const char *)&pt[ab], (uint16_t)al,
					       (const char *)&pt[rb], (uint16_t)rl);

		m[2] = r == MGDS_OK ? 1 : 0;
	}
	if (m[2]) {
		STAT(t, saved);
	} else {
		STAT(t, not_saved);
	}
	if (sealed && (mgseal_saved_plaintext(t->tag, m[2] != 0, &pt[rb], rl) != MGSEAL_OK || !m[2])) {
		memset(t->tag, 0, MGSEAL_SAVED_LEN);
	}
	t->flags &= (uint8_t)~F_UPDATE_WAIT;
	drop_storage(t);
	t->in_total = t->in_got = 0;
	if (sealed) {
		t->out_len = MGSEAL_SAVED_SEALED;
		t->out_off = 0;
		pump_saved(t);
	} else {
		(void)send(t, m, sizeof(m)); /* no sealed_saved in the hello (an older client): the one-byte answer */
	}
#if MGDS_DATA
	if (m[2] && t->state == S_OPEN) {
		/* Saved: both series start over and nothing blocks a re-ask (new
		 * credentials may fix what ended the session, even auth_failed);
		 * with the session ended (since the hello) it asks at once, behind
		 * this answer, as the ended kind's first ask: that series' next step
		 * is 1 s. One ask per save: no loop. */
		t->ra_steps &= RS_KEEP; /* both series start over */
		t->ra &= (uint8_t)~(RA_BLOCK | RA_OWN0 | RA_PENDING);
		if ((t->ra & RA_ENDED) && !(t->flags & F_VM_OPEN) && !ASK_QUIET(t)) {   /* quiet: no task, no ask */
			t->ra_steps |= (t->ra & RA_ENDED_NONET) ? (uint8_t)(1u << 3) : 1u;
			t->ra = (uint8_t)((t->ra & ~RA_OWE0) | RA_OWE1);
		}
	}
#endif
}

static void on_update(mgds_t *t, const uint8_t *p, size_t n, uint32_t now)
{
	uint8_t flags;
	const uint8_t *rest;
	size_t rn, pl;
	bool sealed = (t->flags & F_SEALED) != 0, complete;

	if (n < 1) {
		refuse(t, mg_error_code_invalid_length);
		return;
	}
	if (!(t->flags & F_HELLO_DONE) || clr_taken(t)) {
		refuse(t, mg_error_code_invalid_value); /* a taken clear's answer still waits: keep its mark */
		return;
	}
	if (tx_spent(t)) {
		refuse_update(t, mg_error_code_invalid_value); /* no counter to answer under: an update arriving goes too */
		return;
	}
	flags = p[0];
	rest = &p[1];
	rn = n - 1;
	if (flags & mg_session_flag_first) {
		uint32_t total, lo = sealed ? MGSEAL_OVERHEAD + 1u : 1u;
		uint32_t hi = sealed ? MGDS_SEALED_MAX : MGDS_MAX_CREDENTIALS;

		if (rn < 4) {
			refuse(t, mg_error_code_invalid_length);
			return;
		}
		total = get32le(rest);
		rest += 4;
		rn -= 4;
		if (sealed && !(flags & mg_session_flag_sealed)) {
			refuse_update(t, mg_error_code_insecure); /* no downgrade: nothing changes */
			return;
		}
		if (sealed && rn < 4) {
			refuse_update(t, mg_error_code_invalid_length); /* not the whole counter */
			return;
		}
		if ((t->flags & (F_UPDATE_IN | F_UPDATE_WAIT)) || clr_in(t) || total < lo || total > hi) {
			bad_update(t);
			return;
		}
		if (sealed && !ctr_check(t, get32be(rest))) {
			bad_update(t);
			ctr_refused(t);
			return;
		}
		if (sealed) {
			take_counter(t, get32be(rest)); /* used from here (a drop, an abort), unless its tag is refused */
		}
		t->flags &= (uint8_t)~F_UPDATE_DROP;
		t->in = t->p->borrow(t->p->ctx, total);
		if (t->in == NULL) {
			/* No storage now: discarded (the app resends it); sealed, answered
			 * [0] at its last fragment. */
			STAT(t, dropped);
			if (!(flags & mg_session_flag_last)) {
				t->flags |= F_UPDATE_DROP;
			} else {
				give_up(t);
			}
			return;
		}
		t->flags |= F_UPDATE_IN;
		t->in_total = (uint16_t)total;
		t->in_got = 0;
	} else if (t->flags & F_UPDATE_DROP) {
		if (flags & mg_session_flag_last) {
			t->flags &= (uint8_t)~F_UPDATE_DROP;
			give_up(t); /* found no storage, or lost it: sealed [0] */
		}
		return;
	} else if (!(t->flags & F_UPDATE_IN)) {
		bad_update(t);
		return;
	}
	if (sealed && !(flags & mg_session_flag_sealed)) {
		bad_update(t); /* every fragment of a sealed update carries the flag: dropped, nothing stored */
		return;
	}
	complete = (size_t)t->in_got + rn == t->in_total;
	if (rn == 0 || (size_t)t->in_got + rn > t->in_total ||
	    ((flags & mg_session_flag_last) != 0) != complete) {
		bad_update(t);
		return;
	}
	memcpy(&t->in[t->in_got], rest, rn);
	t->in_got = (uint16_t)(t->in_got + rn);
	if (!complete) {
		return;
	}
	pl = t->in_total;
	if (sealed) {
		mgseal_keys_t tmp;
		const mgseal_keys_t *keys = session_keys(t, &tmp);
		uint32_t ctr = get32be(t->in); /* its counter, checked at the first fragment, is in front */
		int r = keys != NULL ? mgseal_open(keys, MGSEAL_DIR_CLIENT, ctr, t->in, t->in_total, &pl)
				     : MGSEAL_E_CRYPTO;

		mgseal_wipe(&tmp, sizeof(tmp));
		if (r != MGSEAL_OK) {
			unuse_counter(t, ctr); /* refused at its tag: a forged message doesn't move the counter */
			bad_update(t);
			if (r == MGSEAL_E_TAG) {
				end_on_bad_tag(t);
			}
			return;
		}
		if (pl > MGDS_MAX_CREDENTIALS) {
			bad_update(t);
			return;
		}
	}
	{
		size_t ab, al, rb, rl;
		uint64_t exp = 0;

		if (!parse_update(t->in + (sealed ? 4u : 0u), pl, &ab, &al, &rb, &rl, &exp) ||
		    !update_fits(t, al, rl, exp)) {
			bad_update(t); /* invalid_value, nothing stored; a sealed one's counter stays used */
			return;
		}
	}
	t->flags &= (uint8_t)~F_UPDATE_IN;
	t->in_total = (uint16_t)pl;
	/* Hold the commit while media runs, while the credentials are still going
	 * out (their plaintext must not change under them), and while an earlier
	 * credentials_saved is owed (its plaintext holds tag[]). */
	if (t->p->busy(t->p->ctx) || t->state == S_SEND_HELLO || t->state == S_SEND_CREDS || saved_owed(t)) {
		t->flags |= F_UPDATE_WAIT;
		t->defer_at = now;
		STAT(t, deferred);
		return;
	}
	commit_update(t);
}

/* ---- the sealed clear (mg_session_clear, mgcommands.h) --------------------------- */

/* Its sealed answer: counter t->tx_ctr || one byte (in_got) || tag (t->tag), t->out_off of t->out_len sent; when
 * it is out the session ends. */
static void pump_clear(mgds_t *t)
{
	uint8_t f[MGDS_FRAME_MAX], a[CLR_LEN];

	for (int guard = 0; guard < 8 && t->state == S_CLEARING; guard++) {
		uint16_t mtu = t->p->mtu(t->p->ctx);
		uint32_t room = mtu > 3u ? (uint32_t)mtu - 3u : 0u, hdr, take, c = (uint32_t)t->tx_ctr;
		bool first = t->out_off == 0;
		int r;

		hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
		if (room > MGDS_FRAME_MAX) {
			room = MGDS_FRAME_MAX;
		}
		if (room <= hdr) {
			break;
		}
		take = (uint32_t)t->out_len - t->out_off;
		if (take > room - hdr) {
			take = room - hdr;
		}
		a[0] = (uint8_t)(c >> 24);
		a[1] = (uint8_t)(c >> 16);
		a[2] = (uint8_t)(c >> 8);
		a[3] = (uint8_t)c;
		a[4] = (uint8_t)t->in_got;
		memcpy(&a[5], t->tag, MGSEAL_TAG_LEN);
		f[0] = mg_command_session;
		f[1] = mg_session_clear;
		f[2] = (uint8_t)((first ? mg_session_flag_first : 0) |
				 (t->out_off + take == t->out_len ? mg_session_flag_last : 0) | mg_session_flag_sealed);
		if (first) {
			put32le(&f[3], t->out_len);
		}
		memcpy(&f[hdr], &a[t->out_off], take);
		r = send(t, f, hdr + take);
		mgseal_wipe(f, hdr + take);
		if (r == MGDS_AGAIN) {
			break; /* the same fragment from the next mgds_poll() */
		}
		if (r < 0) {
			end_session(t, false);
			break;
		}
		t->out_off = (uint16_t)(t->out_off + take);
		if (t->out_off == t->out_len) {
			end_session(t, true); /* answered: the session ends */
		}
	}
}

/* Where a pending clear keeps its [0] answer's tag: the session's keys (or nonces) are spent once both answers
 * are sealed, and the session ends after the answer. */
static uint8_t *clr_tag0(mgds_t *t)
{
#if MGDS_KEEP_KEYS
	return (uint8_t *)&t->keys;
#else
	return t->nonces;
#endif
}

/* One sealed answer byte under the next device -> client counter: its ciphertext byte and tag. */
static bool clear_seal(const mgseal_keys_t *keys, uint32_t ctr, uint8_t v, uint8_t *c, uint8_t *tag)
{
	uint8_t blob[CLR_LEN];
	int r;

	blob[4] = v;
	r = mgseal_seal(keys, MGSEAL_DIR_DEVICE, ctr, blob, 1);
	*c = blob[4];
	memcpy(tag, &blob[5], MGSEAL_TAG_LEN);
	mgseal_wipe(blob, sizeof(blob));
	return r == MGSEAL_OK;
}

/* The erase's result: its answer (t->in_got and t->tag hold [1], clr_tag0 the [0] tag; the [0] byte is the [1]
 * byte ^ 1, one keystream) goes out, then the session ends. */
static void clear_answer(mgds_t *t, bool erased)
{
	if (!erased) {
		t->in_got ^= 1u;
		memcpy(t->tag, clr_tag0(t), MGSEAL_TAG_LEN);
	}
	mgseal_wipe(clr_tag0(t), MGSEAL_TAG_LEN);
	t->state = S_CLEARING;
	t->out_len = (uint16_t)CLR_LEN;
	t->out_off = 0;
	pump_clear(t);
}

/* mgds_nudge()'s rule: the client reported the session open. */
static bool nudge_ok(const mgds_t *t)
{
	return t->state == S_OPEN && (t->flags & (F_HELLO_DONE | F_VM_OPEN)) == (F_HELLO_DONE | F_VM_OPEN);
}

/* The keepalive owed: one credit with what is owed (it takes that credit with it), [session, credit, 0] if nothing. */
static void nudge_flush(mgds_t *t)
{
	uint8_t m[6] = {mg_command_session, mg_session_credit, 0, 0, 0, 0};
	uint32_t owed = 0;
	int r;

	if (!t->nudge) {
		return;
	}
	if (!nudge_ok(t)) {
		t->nudge = 0; /* the session moved on: no keepalive for it */
		return;
	}
#if MGDS_DATA
	if (dget(t) != NULL) {
		owed = dget(t)->credit_owed;
	}
#else
	owed = t->credit_owed;
#endif
	put32le(&m[2], owed);
	r = send(t, m, sizeof(m));
	if (r == MGDS_SENT) {
		t->nudge = 0;
#if MGDS_DATA
		if (dget(t) != NULL) {
			dget(t)->credit_owed = 0;
		}
#else
		t->credit_owed = 0;
#endif
	} else if (r < 0) {
		end_session(t, false);
	}
}

void mgds_nudge(mgds_t *t)
{
	if (!nudge_ok(t)) {
		return;
	}
	t->nudge = 1;
	nudge_flush(t);
}

void mgds_clear_done(mgds_t *t, bool erased)
{
	if (t->state == S_CLEAR_WAIT) {
		clear_answer(t, erased);
	} else if (clr_taken(t) && t->in_got == CLR_E_PEND) {
		t->in_got = erased ? CLR_E_OK : CLR_E_FAIL; /* the answer still waits for what goes out before it */
	}
}

#if MGDS_KEEP_KEYS
/* The port's erase, run as the clear is taken; its result kept for the answer (in_got). */
static void clear_erase(mgds_t *t)
{
	int r = t->p->clear(t->p->ctx);

	if (clr_taken(t)) {
		t->in_got = r == MGDS_OK ? CLR_E_OK : r == MGDS_CLEAR_PENDING ? CLR_E_PEND : CLR_E_FAIL;
	}
}
#endif

/* A taken sealed clear, once what goes out before it is out: both answers sealed on the next device -> client
 * counter (without MGDS_KEEP_KEYS the keys come from K, so before the erase; only one is ever sent), the erase (the
 * port's clear) unless it ran when the clear was taken, then [1] ([0] if the erase failed) and the session ends. A
 * pending erase holds the session in S_CLEAR_WAIT until mgds_clear_done(). */
static void clear_now(mgds_t *t)
{
	mgseal_keys_t tmp;
	const mgseal_keys_t *keys;
	uint8_t c0, c1 = 0; /* c0 = c1 ^ 1 (one keystream byte): clear_answer() derives it */
	int r;
	bool sealed;
	uint16_t erased;

	if (!clr_taken(t) || t->state != S_OPEN || saved_owed(t)) {
		return;
	}
#if MGDS_DATA
	if (dget(t) != NULL && (dget(t)->flags & (DF_TAGGED | DF_OWE_ABORT)) != 0) {
		return; /* never two sealed messages part-way: the data message's last fragment (or abort) first */
	}
#endif
	keys = session_keys(t, &tmp);
	if (keys != &tmp && keys != NULL) {
		tmp = *keys;
	}
	erased = t->in_got;
	t->in_total = t->in_got = 0;
	sealed = keys != NULL && !tx_spent(t) && clear_seal(&tmp, (uint32_t)t->tx_ctr, 1u, &c1, t->tag) &&
		 clear_seal(&tmp, (uint32_t)t->tx_ctr, 0u, &c0, clr_tag0(t));
	mgseal_wipe(&tmp, sizeof(tmp));
	t->in_got = c1;
	r = erased == CLR_E_NONE ? t->p->clear(t->p->ctx)
	    : erased == CLR_E_PEND ? MGDS_CLEAR_PENDING : erased == CLR_E_OK ? MGDS_OK : MGDS_FAIL;
	if (!sealed || t->state != S_OPEN) {
		end_session(t, true); /* no answer (or the port ended the session); the erase runs or ran anyway */
		return;
	}
	if (r == MGDS_CLEAR_PENDING) {
		t->state = S_CLEAR_WAIT;
		return;
	}
	clear_answer(t, r == MGDS_OK);
}

/* The whole sealed clear (counter || one byte || tag) in b, its counter taken: open it; taken, it waits for
 * clear_now(). */
static void clear_open(mgds_t *t, uint8_t b[CLR_LEN])
{
	mgseal_keys_t tmp;
	const mgseal_keys_t *keys = session_keys(t, &tmp);
	uint32_t ctr = get32be(b);
	size_t pl = 0;
	int r = keys != NULL ? mgseal_open(keys, MGSEAL_DIR_CLIENT, ctr, b, CLR_LEN, &pl) : MGSEAL_E_CRYPTO;
	bool zero = pl == 1u && b[4] == 0u;

	mgseal_wipe(&tmp, sizeof(tmp));
	mgseal_wipe(b, CLR_LEN);
	if (r != MGSEAL_OK) {
		unuse_counter(t, ctr); /* refused at its tag: the counter isn't used */
		refuse(t, mg_error_code_invalid_value);
		if (r == MGSEAL_E_TAG) {
			end_on_bad_tag(t);
		}
		return;
	}
	if (!zero) {
		refuse(t, mg_error_code_invalid_value); /* its counter stays used */
		return;
	}
	if (t->state == S_SEND_CREDS && t->out_off == 0) {
		/* The credentials not started never go out and their counter is never used (mgcommands.h: a device
		 * message not yet started never goes out, its counter is never used): counter 0 goes back, so the answer
		 * is sealed on it, the next counter the client takes (no hole on the client's side). */
		t->state = S_OPEN;
		t->out_len = 0;
		mgseal_wipe(t->tag, sizeof(t->tag));
		t->tx_ctr--;
		t->tx_back = t->tx_ctr == 0 ? 1u : 0u;
	}
	t->in_total = CLR_TAKEN;
	t->in_got = CLR_E_NONE;
#if MGDS_KEEP_KEYS
	if (t->state == S_OPEN) {
		/* Erased now; only the answer waits for what goes out before it, sealed later with the kept keys. The
		 * credentials part way out are read from the stored pair, so behind them the erase waits too (as it
		 * does without the kept keys: those come from K). */
		clear_erase(t);
	}
#endif
	clear_now(t);
}

/* Client -> device, sealed only, fragmented like a credentials_update (outside credit): counter || 0 || tag. One
 * write is opened from a copy on the stack; fragments (ATT MTU under 31) go into the port's lent storage. */
static void on_clear(mgds_t *t, const uint8_t *p, size_t n)
{
	uint8_t flags, b[CLR_LEN];
	const uint8_t *rest;
	size_t rn, got;

	if (n < 1) {
		refuse(t, mg_error_code_invalid_length);
		return;
	}
	if (!(t->flags & F_HELLO_DONE)) {
		refuse(t, mg_error_code_invalid_value);
		return;
	}
	if (tx_spent(t) && !clr_taken(t)) {
		if (clr_in(t)) {
			drop_storage(t); /* a clear arriving goes too: its answer could never be sealed */
			t->in_total = t->in_got = 0;
		}
		refuse(t, mg_error_code_invalid_value);
		return;
	}
	if (t->p->clear == NULL) {
		refuse(t, mg_error_code_unsupported);
		return;
	}
	flags = p[0];
	rest = &p[1];
	rn = n - 1;
	if (flags & mg_session_flag_first) {
		uint32_t total;
		bool whole;

		if (rn < 4) {
			refuse(t, mg_error_code_invalid_length);
			return;
		}
		total = get32le(rest);
		rest += 4;
		rn -= 4;
		if (clr_in(t)) {
			drop_storage(t); /* a new first fragment drops the clear arriving (its counter stays used) */
			t->in_total = t->in_got = 0;
		}
		if (!(t->flags & F_SEALED) || !(flags & mg_session_flag_sealed)) {
			refuse(t, mg_error_code_insecure); /* never unsealed: nothing erased */
			return;
		}
		if (rn < 4) {
			refuse(t, mg_error_code_invalid_length); /* not the whole counter */
			return;
		}
		whole = (flags & mg_session_flag_last) != 0;
		if (total != CLR_LEN || clr_taken(t) || (t->state != S_OPEN && t->state != S_SEND_CREDS) ||
		    (whole && rn != CLR_LEN) || (!whole && rn >= CLR_LEN)) {
			refuse(t, mg_error_code_invalid_value);
			return;
		}
		if (!ctr_check(t, get32be(rest))) {
			refuse(t, mg_error_code_invalid_value);
			ctr_refused(t);
			return;
		}
		/* Used from here, unless its tag is refused: a busy answer uses it too (mgcommands.h: the first
		 * fragment carries the whole counter; the client retries on the next one). */
		take_counter(t, get32be(rest));
		if (t->in != NULL || (t->flags & (F_UPDATE_IN | F_UPDATE_DROP | F_UPDATE_WAIT))) {
			refuse(t, mg_error_code_busy); /* a credentials_update holds the storage */
			return;
		}
		if (whole) {
			memcpy(b, rest, CLR_LEN);
			clear_open(t, b);
			return;
		}
		t->in = t->p->borrow(t->p->ctx, CLR_LEN);
		if (t->in == NULL) {
			refuse(t, mg_error_code_busy); /* no storage now */
			return;
		}
		t->in_total = (uint16_t)CLR_LEN;
		t->in_got = 0;
	} else if (!clr_in(t)) {
		refuse(t, mg_error_code_invalid_value);
		return;
	}
	got = t->in_got;
	if (rn == 0 || got + rn > CLR_LEN || ((flags & mg_session_flag_last) != 0) != (got + rn == CLR_LEN) ||
	    !(flags & mg_session_flag_sealed)) {
		drop_storage(t);
		t->in_total = t->in_got = 0;
		refuse(t, mg_error_code_invalid_value);
		return;
	}
	memcpy(&t->in[got], rest, rn);
	t->in_got = (uint16_t)(got + rn);
	if (t->in_got < CLR_LEN) {
		return;
	}
	memcpy(b, t->in, CLR_LEN);
	drop_storage(t);
	t->in_total = t->in_got = 0;
	clear_open(t, b);
}

/* ---- the rest of the session ----------------------------------------------- */

#if MGDS_DATA
/* The lent state, if the client message in flight is the port's (its first
 * fragment came while the data path was live). */
static mgds_data_t *rx_kept(const mgds_t *t)
{
	return (t->flags & F_DATA_IN) && dget(t) != NULL && (dget(t)->flags & DF_RX_KEEP) ? dget(t) : NULL;
}
#endif

/* A client message larger than this device's window that the port doesn't keep (no data path live at its first
 * fragment, or a credentials-only build): each fragment's bytes go back as it lands, or a client sending more than
 * the window could never finish (mgcommands.h: a receiver whose window is smaller than its max_message gives an
 * arriving message's bytes back once they fill the window). */
static bool rx_streamed(const mgds_t *t)
{
	return (t->flags & F_DATA_IN) && t->data_total > t->p->window
#if MGDS_DATA
	       && rx_kept(t) == NULL
#endif
		;
}

static void drop_data(mgds_t *t, size_t also)
{
	/* a streamed message's fragments already went back; only `also` (a fragment refused before it counted) is owed */
	uint32_t back = (rx_streamed(t) ? 0u : (uint32_t)t->data_got) + (uint32_t)also;
#if MGDS_DATA
	mgds_data_t *d = rx_kept(t);

	if (d != NULL) {
		back -= d->data_cred; /* what a message larger than the window already gave back */
		d->data_cred = 0;
		d->flags &= (uint8_t)~DF_RX_KEEP;
		if (d->buf != NULL) {
			mgseal_wipe(d->buf, t->data_got);
		}
	}
#endif

	t->flags &= (uint8_t)~(F_DATA_IN | F_DATA_SEALED);
	t->data_total = t->data_got = 0;
#if MGDS_SEALED_DATA
	dtag_put(t);
#endif
	send_credit(t, back);
#if MGDS_DATA
	if (d != NULL && d->buf == NULL && t->p->data_end != NULL) {
		t->p->data_end(t->p->ctx, false); /* drop the pieces */
	}
#endif
}

static void refuse_data(mgds_t *t, uint8_t code, size_t also)
{
	STAT(t, bad_data);
	refuse(t, code);
	drop_data(t, also);
}

#if MGDS_SEALED_DATA
/* Sealed bytes [at, at + n) of the message in flight: the counter (0..3) was
 * read at the first fragment, the ciphertext goes into the tag, the last
 * MGSEAL_TAG_LEN bytes are the tag. */
static void data_feed(mgds_t *t, uint32_t at, const uint8_t *b, size_t n)
{
	mgds_dtag_t *d = dtag(t);
	uint32_t ct_end = (uint32_t)t->data_total - MGSEAL_TAG_LEN;

	if (d == NULL || t->data_bad) {
		return; /* no tag state lent, or none begun (the keys failed): refused at the end */
	}

	for (size_t i = 0; i < n; i++, at++) {
		if (at >= ct_end) {
			d->rx[at - ct_end] = b[i];
		} else if (at >= 4u) {
			size_t run = n - i;

			if (run > ct_end - at) {
				run = ct_end - at;
			}
			mgseal_tag_update(&d->tag, &b[i], run);
			i += run - 1;
			at += (uint32_t)run - 1u;
		}
	}
}
#endif

#if MGDS_DATA
/* The client message's envelope check (mgcommands.h, Sealed data): its plaintext must decode as a
 * ServiceResponseEnvelope with noise_core ServiceCodec's rules (mg_session_ref.envelope_payload): each key a varint
 * field number 1..2^29-1 outside 19000-19999 with wire type 0, 1, 2 or 5 (field 1 delimited), varints of at most 10
 * bytes, every value inside the message, unknown fields skipped. A clear or credentials_update
 * relabeled as data never does (a lone 0x00, or a first byte 0x00, 0x01 or '{'). Fed the plaintext in order, bytes
 * [off, off + n) of len; d keeps where it is between pieces. */
#define ENV_KEY    0u    /* at a key (the state a wiped mgds_data_t starts a message in) */
#define ENV_VARINT 1u    /* in a wire type 0 value */
#define ENV_LEN    2u    /* in a wire type 2 length */
#define ENV_SKIP   3u    /* in a value: env_val bytes left */
#define ENV_BAD    4u    /* not an envelope */
#define ENV_BIG    0x80u /* env_n: the varint is past 32 bits */

static void env_varint_end(mgds_data_t *d, uint32_t rest)
{
	uint32_t v = d->env_val, num = v >> 3, wire = v & 7u;
	bool big = (d->env_n & ENV_BIG) != 0;

	d->env_val = 0;
	d->env_n = 0;
	if (d->env_st == ENV_VARINT) {
		d->env_st = ENV_KEY;
	} else if (d->env_st == ENV_LEN) {
		d->env_val = v;
		d->env_st = big || v > rest ? ENV_BAD : v != 0 ? ENV_SKIP : ENV_KEY;
	} else if (big || num == 0 || (num >= 19000u && num <= 19999u) || (num == 1u && wire != 2u)) {
		d->env_st = ENV_BAD;
	} else if (wire == 0u) {
		d->env_st = ENV_VARINT;
	} else if (wire == 2u) {
		d->env_st = ENV_LEN;
	} else if (wire == 1u || wire == 5u) {
		d->env_val = wire == 1u ? 8u : 4u;
		d->env_st = d->env_val > rest ? ENV_BAD : ENV_SKIP;
	} else {
		d->env_st = ENV_BAD;
	}
}

static void env_feed(mgds_data_t *d, uint32_t off, const uint8_t *b, size_t n, uint32_t len)
{
	for (size_t i = 0; i < n && d->env_st != ENV_BAD; i++) {
		uint8_t c = b[i];
		unsigned k = d->env_n & 0x7Fu;

		if (d->env_st == ENV_SKIP) {
			size_t run = n - i < d->env_val ? n - i : d->env_val;

			d->env_val -= (uint32_t)run;
			i += run - 1u;
			if (d->env_val == 0) {
				d->env_st = ENV_KEY;
			}
			continue;
		}
		if (k == 9u && (c & 0xFEu)) {
			d->env_st = ENV_BAD; /* past 10 bytes, or past 64 bits */
			break;
		}
		if (k < 4u || (k == 4u && (c & 0x70u) == 0)) {
			d->env_val |= (uint32_t)(c & 0x7Fu) << (7u * k);
		} else if (c & 0x7Fu) {
			d->env_n |= ENV_BIG;
		}
		d->env_n = (uint8_t)((d->env_n & ENV_BIG) | (k + 1u));
		if (!(c & 0x80u)) {
			env_varint_end(d, len - (off + (uint32_t)i) - 1u);
		}
	}
}

static bool env_ok(const mgds_data_t *d)
{
	return d->env_st == ENV_KEY && d->env_n == 0;
}

/* Wire bytes [at, at + n) of the client message in flight, kept: into the
 * port's buffer (reassembled), or (in pieces) their plaintext to
 * data_piece(): sealed, the counter (0..3) and the tag (the last 16) are
 * skipped and the ciphertext decrypted in pieces of at most DATA_PIECE_MAX
 * (the tag is checked at the last fragment; a keystream failure makes it
 * fail). */
static void data_keep(mgds_t *t, mgds_data_t *d, uint32_t at, const uint8_t *b, size_t n)
{
	const mgds_port_t *p = t->p;
	bool sealed = (t->flags & F_DATA_SEALED) != 0;
	uint32_t plain = (uint32_t)t->data_total - (sealed ? MGSEAL_OVERHEAD : 0u);
	uint8_t c[DATA_PIECE_MAX];
	mgseal_keys_t tmp;
	const mgseal_keys_t *keys = NULL;

	if (d->buf != NULL) {
		memcpy(&d->buf[at], b, n);
		return;
	}
	for (size_t i = 0; i < n;) {
		uint32_t a = at + (uint32_t)i, po;
		size_t m;

		if (sealed && (a < 4u || a >= 4u + plain)) {
			i++; /* the counter or the tag */
			continue;
		}
		po = sealed ? a - 4u : a;
		m = n - i;
		if (m > plain - po) {
			m = plain - po;
		}
		if (m > sizeof(c)) {
			m = sizeof(c);
		}
		memcpy(c, &b[i], m);
		if (sealed) {
			if (keys == NULL) {
				keys = session_keys(t, &tmp);
			}
			if (keys == NULL ||
			    mgseal_xor_at(keys, MGSEAL_DIR_CLIENT, t->data_ctr, po, c, m) != MGSEAL_OK) {
				t->data_bad = 1; /* refused at its end */
				break;
			}
		}
		env_feed(d, po, c, m, plain); /* before data_piece(), which may detach d */
		if (p->data_piece != NULL) {
			p->data_piece(p->ctx, po, c, m, plain); /* it may detach: only the copy above is used after */
		}
		i += m;
	}
	mgseal_wipe(c, sizeof(c));
	if (keys != NULL) {
		mgseal_wipe(&tmp, sizeof(tmp));
	}
}

/* A whole client message, its tag checked: its credit goes back first (as
 * the reference device: credit, then what handling it sends), then the port
 * has it: reassembled, opened in place and handed over, then wiped; in
 * pieces, data_end(true). One that isn't a ServiceResponseEnvelope: invalid_value,
 * then its credit, its counter used; the port never has it (in pieces,
 * data_end(false)). */
static void data_done(mgds_t *t, mgds_data_t *d)
{
	const mgds_port_t *p = t->p;
	bool sealed = (t->flags & F_DATA_SEALED) != 0;
	uint32_t total = t->data_total, back = (uint32_t)t->data_got - d->data_cred;
	uint8_t *buf = d->buf; /* the port's: it stays valid if a callback detaches d */
	uint32_t ctr = 0;
	int r = MGSEAL_OK;

#if MGDS_SEALED_DATA
	ctr = t->data_ctr;
	dtag_put(t);
#endif
	t->flags &= (uint8_t)~(F_DATA_IN | F_DATA_SEALED);
	t->data_total = t->data_got = 0;
	d->data_cred = 0;
	d->flags &= (uint8_t)~DF_RX_KEEP;
	STAT(t, data_in);
	if (buf != NULL && sealed) {
		mgseal_keys_t tmp;
		const mgseal_keys_t *keys = session_keys(t, &tmp);

		r = keys != NULL ? mgseal_xor_at(keys, MGSEAL_DIR_CLIENT, ctr, 0, &buf[4], total - MGSEAL_OVERHEAD)
				 : MGSEAL_E_CRYPTO;
		mgseal_wipe(&tmp, sizeof(tmp));
	}
	if (r != MGSEAL_OK) {
		/* The tag matched but it can't be opened (the backend): busy, its
		 * bytes back, its counter used. */
		mgseal_wipe(buf, total);
		STAT(t, bad_data);
		refuse(t, mg_error_code_busy);
		send_credit(t, back);
		return;
	}
	if (buf != NULL) {
		env_feed(d, 0, sealed ? &buf[4] : buf, sealed ? total - MGSEAL_OVERHEAD : total,
			 sealed ? total - MGSEAL_OVERHEAD : total);
	}
	if (!env_ok(d)) {
		if (buf != NULL) {
			mgseal_wipe(buf, total);
		}
		STAT(t, bad_data);
		refuse(t, mg_error_code_invalid_value);
		send_credit(t, back);
		if (buf == NULL && p->data_end != NULL) {
			p->data_end(p->ctx, false);
		}
		return;
	}
	send_credit(t, back);
	if (buf != NULL) {
		if (p->data_message != NULL) {
			p->data_message(p->ctx, sealed ? &buf[4] : buf, sealed ? total - MGSEAL_OVERHEAD : total);
		}
		mgseal_wipe(buf, total);
	} else if (p->data_end != NULL) {
		p->data_end(p->ctx, true);
	}
}
#endif

/* A client data message: this device keeps no VM session, so a whole one is
 * dropped and every byte goes back as credit. In a sealed session it must be
 * sealed (insecure otherwise) with the next counter (read from the first
 * fragment), and its tag must match (checked at the last). */
static void on_data(mgds_t *t, const uint8_t *p, size_t n)
{
	uint8_t flags;
	const uint8_t *rest;
	size_t rn;
	bool complete, sealed = (t->flags & F_SEALED) != 0;

	if (n < 1) {
		refuse(t, mg_error_code_invalid_length);
		return;
	}
	if (!(t->flags & F_HELLO_DONE) || !(t->flags & F_VM_OPEN)) {
		refuse(t, mg_error_code_invalid_value);
		return;
	}
	if (clr_taken(t) || tx_spent(t)) {
		/* From a taken sealed clear on, the keys seal only its answer: data is refused as credentials_update is,
		 * its bytes back (and those of a message part way in), no counter taken. So with the device's counters spent. */
		refuse_data(t, mg_error_code_invalid_value, (p[0] & mg_session_flag_first) ? (n > 5 ? n - 5 : 0) : n - 1);
		return;
	}
	flags = p[0];
	rest = &p[1];
	rn = n - 1;
	if (flags & mg_session_flag_first) {
		uint32_t total;

		if (rn < 4) {
			refuse(t, mg_error_code_invalid_length);
			return;
		}
		total = get32le(rest);
		rest += 4;
		rn -= 4;
		if (sealed && !(flags & mg_session_flag_sealed)) {
			/* No downgrade: refused, nothing changes (its bytes go back). */
			STAT(t, bad_data);
			refuse(t, mg_error_code_insecure);
			send_credit(t, (uint32_t)rn);
			return;
		}
		if (sealed && rn < 4) {
			refuse_data(t, mg_error_code_invalid_length, rn); /* not the whole counter */
			return;
		}
		if ((t->flags & F_DATA_IN) || total == 0 || total > t->p->max_message || total > 0xFFFFu ||
#if MGDS_DATA
		    (dget(t) != NULL && (dget(t)->flags & DF_LIVE) && dget(t)->buf != NULL && total > dget(t)->cap) ||
#endif
		    (sealed && total <= MGSEAL_OVERHEAD)) {
			refuse_data(t, mg_error_code_invalid_value, rn);
			return;
		}
		if (sealed && !ctr_check(t, get32be(rest))) {
			refuse_data(t, mg_error_code_invalid_value, rn);
			ctr_refused(t);
			return;
		}
		t->flags |= F_DATA_IN;
		t->data_total = (uint16_t)total;
		t->data_got = 0;
#if MGDS_DATA
		if (dget(t) != NULL && (dget(t)->flags & DF_LIVE)) {
			dget(t)->flags |= DF_RX_KEEP; /* the port's (else dropped and credited, as without the data path) */
			dget(t)->data_cred = 0;
			dget(t)->env_val = 0;
			dget(t)->env_st = ENV_KEY;
			dget(t)->env_n = 0;
		}
#endif
		if (sealed) {
			t->flags |= F_DATA_SEALED;
#if MGDS_SEALED_DATA
			{
				mgseal_keys_t tmp;
				const mgseal_keys_t *keys;
				mgds_dtag_t *d;

				t->data_ctr = get32be(rest);
#if MGDS_TAG_LEND
				t->dt = mgds_tag_lend(t->p->ctx);
#endif
				d = dtag(t);
				keys = d != NULL ? session_keys(t, &tmp) : NULL;
				if (d == NULL) {
					t->data_bad = 2; /* refused busy at its end, its counter unused */
				} else if (keys == NULL) {
					t->data_bad = 1;
				} else {
					t->tag_live = 1; /* released by its check, or by dtag_put (a failed begin too) */
					if (mgseal_tag_begin(&d->tag, keys, MGSEAL_DIR_CLIENT, t->data_ctr,
							     total - MGSEAL_OVERHEAD) != MGSEAL_OK) {
						t->data_bad = 1;
					}
				}
				mgseal_wipe(&tmp, sizeof(tmp));
			}
#endif
			take_counter(t, get32be(rest)); /* used from here (an abort, a state change), unless its tag is refused */
		}
	} else if (!(t->flags & F_DATA_IN)) {
		refuse(t, mg_error_code_invalid_value);
		send_credit(t, (uint32_t)rn);
		return;
	}
	complete = (size_t)t->data_got + rn == t->data_total;
	if (rn == 0 || (size_t)t->data_got + rn > t->data_total ||
	    ((flags & mg_session_flag_last) != 0) != complete ||
	    (sealed && !(flags & mg_session_flag_sealed))) {
		refuse_data(t, mg_error_code_invalid_value, rn);
		return;
	}
#if MGDS_DATA
	if (rx_kept(t) != NULL) {
		/* Past the credit this device granted (its window, less what it hasn't sent credit for: this message's bytes
		 * not given back, and a grant the stack had no buffer for, from an earlier message too): dropped and
		 * counted with its message, never buffered, every byte given back (mgcommands.h, Credit). */
		uint32_t w = t->p->window, owed = rx_kept(t)->credit_owed;
		uint32_t held = (uint32_t)t->data_got - rx_kept(t)->data_cred;

		if (owed > w || held > w - owed || rn > w - owed - held) {
			refuse_data(t, mg_error_code_invalid_value, rn);
			return;
		}
	}
#endif
#if MGDS_SEALED_DATA
	if (t->flags & F_DATA_SEALED) {
		data_feed(t, t->data_got, rest, rn);
	}
#endif
#if MGDS_DATA
	if (rx_kept(t) != NULL) {
		data_keep(t, dget(t), t->data_got, rest, rn);
	}
#endif
	(void)rest;
	t->data_got = (uint16_t)(t->data_got + rn);
	if (rx_streamed(t)) {
		send_credit(t, (uint32_t)rn); /* dropped at its end anyway: its bytes back as they land */
	}
#if MGDS_DATA
	if (!complete && rx_kept(t) != NULL && (uint32_t)t->data_got - rx_kept(t)->data_cred >= t->p->window) {
		/* Larger than the window: what it holds goes back once it fills the window, or it could never finish. */
		uint32_t held = (uint32_t)t->data_got - rx_kept(t)->data_cred;

		send_credit(t, held);
		rx_kept(t)->data_cred = (uint16_t)(rx_kept(t)->data_cred + held);
	}
#endif
	if (!complete) {
		return;
	}
#if MGDS_SEALED_DATA
	if (t->flags & F_DATA_SEALED) {
		mgseal_keys_t tmp;
		const mgseal_keys_t *keys = t->data_bad ? NULL : session_keys(t, &tmp);
		int r = MGSEAL_E_CRYPTO;
		uint8_t why = t->data_bad;

		if (keys != NULL) {
			r = mgseal_tag_check(&dtag(t)->tag, keys, dtag(t)->rx);
			t->tag_live = 0;
		}

		mgseal_wipe(&tmp, sizeof(tmp));
		if (r != MGSEAL_OK) {
			unuse_counter(t, t->data_ctr); /* a refused message doesn't move the counter */
			refuse_data(t, why == 2 ? mg_error_code_busy : mg_error_code_invalid_value, 0);
			if (why == 0 && keys != NULL && r == MGSEAL_E_TAG) {
				end_on_bad_tag(t);
			}
			return;
		}
	}
#endif
#if MGDS_DATA
	if (rx_kept(t) != NULL) {
		data_done(t, dget(t));
		return;
	}
#endif
	STAT(t, data_in);
	drop_data(t, 0); /* handled by dropping it: every byte goes back as credit */
}

#if MGDS_DATA
/* ---- the data path: re-asks, the port's messages --------------------------- */

static uint8_t ra_step(const mgds_t *t, unsigned k)
{
	return (uint8_t)((t->ra_steps >> (3u * k)) & 7u);
}

/* Series k's next delay (0 closed, 1 no_network): at once, then 1, 2, 4 ...
 * s, capped at 60 s (closed) or 15 s (no_network). */
static uint32_t ask_delay(const mgds_t *t, unsigned k)
{
	uint32_t cap = k ? MGDS_ASK_NONET_MS : MGDS_ASK_CLOSED_MS, d = MGDS_ASK_FIRST_MS;
	uint8_t s = ra_step(t, k);

	if (s == 0) {
		return 0;
	}
	while (--s != 0 && d < cap) {
		d *= 2u;
	}
	return d < cap ? d : cap;
}

/* A closed (k 0) or no_network (k 1) the device didn't ask for: the next ask
 * of that kind's series, at once or after its step. An ask of the same kind
 * already waiting stands (the series doesn't advance); one of the other kind
 * waiting: the earlier of the two goes. */
static void ask_schedule(mgds_t *t, unsigned k, uint32_t now)
{
	uint32_t due = now + ask_delay(t, k);
	bool pending = (t->ra & RA_PENDING) != 0;
	uint8_t s = ra_step(t, k);

	if (pending && ((t->ra & RA_KIND_NONET) != 0) == (k != 0)) {
		return;
	}
	if (!pending || (int32_t)(due - ASK_DUE(t)) < 0) {
		ASK_DUE(t) = due;
	}
	t->ra = (uint8_t)((t->ra & ~RA_KIND_NONET) | RA_PENDING | (k ? RA_KIND_NONET : 0u));
	if (s < RA_STEP_MAX) {
		t->ra_steps = (uint8_t)((t->ra_steps & ~(7u << (3u * k))) | (unsigned)(s + 1u) << (3u * k));
	}
}

/* The port's message and its abort owed, dropped (d attached). */
static void tx_drop(mgds_data_t *d)
{
	d->tx_msg = NULL;
	d->tx_len = d->tx_off = 0;
	d->flags &= (uint8_t)~(DF_TAGGED | DF_OWE_ABORT);
	mgseal_wipe(d->tx_tag, sizeof(d->tx_tag));
}

/* A state the client reported (after on_state's own handling): the port's
 * message and a client message in flight end, the re-ask rules (mgds_t), the
 * port hears it (and may attach or detach), then a lent state takes it:
 * credit is the window again on both sides, and an open makes it live. */
static void data_state(mgds_t *t, uint8_t st, uint16_t epoch, bool was_open, bool was_kept, uint32_t now)
{
	const mgds_port_t *p = t->p;
	mgds_data_t *d = dget(t);
	bool dropped = d != NULL && d->tx_msg != NULL, pieces = was_kept && d != NULL && d->buf == NULL;

	/* A session that stayed open MGDS_ASK_STABLE_MS restarts both series at
	 * its end; after a shorter one (or one whose open time isn't known: no
	 * lent state saw it open) the same kind of end continues its series, so a
	 * server that keeps ending at once is never asked faster. */
	if (was_open && d != NULL && (d->flags & DF_LIVE) && now - d->opened_at >= MGDS_ASK_STABLE_MS) {
		t->ra_steps &= RS_KEEP; /* both series start over */
	}
	if (d != NULL) {
		if (d->tx_msg != NULL && (d->flags & DF_TAGGED) && d->tx_off == 0) {
			/* Reserved, never sent: the client never saw this counter, so it
			 * isn't used (the next message takes it). Nothing later took a
			 * counter: pump_saved waits for a reserved first fragment. */
			t->tx_ctr--;
		}
		tx_drop(d); /* the session's end drops it on both sides (its counter, if its first fragment went, used) */
		d->flags &= (uint8_t)~(DF_LIVE | DF_RX_KEEP);
		d->credit_owed = 0;
		d->data_cred = 0;
	}
	switch (st) {
	case mg_session_state_open:
		t->ra &= (uint8_t)~(RA_BLOCK | RA_OWN0 | RA_PENDING | RA_OWE1);
		break;
	case mg_session_state_connecting:
		t->ra &= (uint8_t)~RA_PENDING; /* the client is opening */
		break;
	case mg_session_state_closed:
	case mg_session_state_no_network:
		/* Not asked for (the port's [0] blocks, and a closed after connecting
		 * with no session open counts too: a failed open is never dropped). */
		if (!(t->ra & (RA_BLOCK | RA_OWN0 | RA_OWE1)) && !ASK_QUIET(t)) {
			ask_schedule(t, st == mg_session_state_no_network ? 1u : 0u, now);
		}
		break;
	default: /* not_enrolled, auth_failed, too_large: asking again can't fix them */
		t->ra = (uint8_t)((t->ra & ~(RA_PENDING | RA_OWE1)) | RA_BLOCK);
		break;
	}
	t->ra &= (uint8_t)~(RA_ENDED | RA_ENDED_NONET);
	if (st != mg_session_state_open && st != mg_session_state_connecting) {
		t->ra |= (uint8_t)(RA_ENDED | (st == mg_session_state_no_network ? RA_ENDED_NONET : 0u));
	}
	if (pieces && p->data_end != NULL) {
		p->data_end(p->ctx, false);
	}
	if (dropped && p->data_sent != NULL) {
		p->data_sent(p->ctx, false);
	}
	if (p->data_state != NULL) {
		p->data_state(p->ctx, st, epoch);
	}
	d = dget(t); /* attached, detached or replaced by the callback */
	if (d != NULL) {
		d->vm_state = st;
		d->epoch = epoch;
		d->send_credit = d->client_window;
		if (st == mg_session_state_open && t->state != S_IDLE) {
			d->flags |= DF_LIVE;
			d->opened_at = now;
		}
	}
}

/* A re-ask that came due, then open_request owed: after the credentials and
 * any credentials_saved going out (a save's ask goes right behind its answer). */
static void ask_poll(mgds_t *t, uint32_t now)
{
	if ((t->ra & RA_PENDING) && (int32_t)(now - ASK_DUE(t)) >= 0) {
		t->ra &= (uint8_t)~RA_PENDING;
		if (!(t->flags & F_VM_OPEN) && !(t->ra & (RA_BLOCK | RA_OWN0)) && !ASK_QUIET(t)) {
			t->ra = (uint8_t)((t->ra & ~RA_OWE0) | RA_OWE1);
		}
	}
	if ((t->ra & (RA_OWE1 | RA_OWE0)) && t->state == S_OPEN && !saved_owed(t)) {
		const uint8_t m[3] = {mg_command_session, mg_session_open_request, (t->ra & RA_OWE1) ? 1u : 0u};
		int r = send(t, m, sizeof(m));

		if (r == MGDS_SENT) {
			t->ra &= (uint8_t)~(RA_OWE1 | RA_OWE0);
		} else if (r < 0) {
			end_session(t, false);
		}
	}
}

/* The lent state when data flows: attached, it saw this VM session open, the
 * credentials are out. */
static mgds_data_t *live(const mgds_t *t)
{
	mgds_data_t *d = dget(t);

	return d != NULL && (d->flags & DF_LIVE) && (t->flags & F_VM_OPEN) && t->state == S_OPEN ? d : NULL;
}

static int msg_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	const mgds_data_t *d = ctx;

	memcpy(buf, &d->tx_msg[off], n);
	return 0;
}

/* Sends an abort owed for a message dropped part-way. false: not yet (the
 * stack had no buffer, or the link went). */
static bool tx_abort_owed(mgds_t *t, mgds_data_t *d)
{
	const uint8_t m[2] = {mg_command_session, mg_session_abort};
	int r;

	if (!(d->flags & DF_OWE_ABORT)) {
		return true;
	}
	r = send(t, m, sizeof(m));
	if (r == MGDS_AGAIN) {
		return false;
	}
	if (r < 0) {
		end_session(t, false);
		return false;
	}
	STAT(t, aborted);
	d->flags &= (uint8_t)~DF_OWE_ABORT;
	return true;
}

/* The port's message, as far as the client's credit goes: sealed in a sealed
 * session (counter || ciphertext || tag, the counter reserved and the tag
 * made from the port's bytes as its first fragment is made, each fragment
 * then encrypted again from them: no buffer), fragmented to the MTU and
 * MGDS_FRAME_MAX. */
static void pump_data(mgds_t *t)
{
	uint8_t f[MGDS_FRAME_MAX];

	for (int guard = 0; guard < 256; guard++) {
		mgds_data_t *d = dget(t); /* again each time: data_sent() may detach it */
		bool sealed = (t->flags & F_SEALED) != 0, first;
		uint32_t plain, wire, hdr, room, take, pos, end;
		uint16_t mtu;
		mgseal_keys_t tmp;
		const mgseal_keys_t *keys = NULL;
		int r = 0;

		if (d == NULL || !tx_abort_owed(t, d) || dget(t) != d) {
			break;
		}
		first = d->tx_off == 0;
		if (d->tx_msg == NULL || live(t) != d || d->send_credit == 0 || (sealed && first && d->send_credit < 4u)) {
			break; /* a sealed first fragment carries the whole counter */
		}
		plain = d->tx_len;
		wire = plain + (sealed ? MGSEAL_OVERHEAD : 0u);
		hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
		mtu = t->p->mtu(t->p->ctx);
		room = mtu > 3u ? (uint32_t)mtu - 3u : 0u;
		if (room > MGDS_FRAME_MAX) {
			room = MGDS_FRAME_MAX;
		}
		if (room <= hdr) {
			break;
		}
		if (sealed && !(d->flags & DF_TAGGED)) {
			if (t->state == S_OPEN && saved_owed(t)) {
				break; /* a credentials_saved owed goes first, under the next counter (data_flush) */
			}
			if (clr_taken(t)) {
				break; /* a taken clear's answer goes next (clear_now), then the session ends */
			}
			if (tx_spent(t)) {
				/* No counter left: dropped (the client re-proves into a new session). */
				tx_drop(d);
				if (t->p->data_sent != NULL) {
					t->p->data_sent(t->p->ctx, false);
				}
				break;
			}
			keys = session_keys(t, &tmp);
			r = keys != NULL ? mgseal_tag_stream(keys, MGSEAL_DIR_DEVICE, t->tx_ctr, plain, msg_read, d, d->tx_tag)
					 : MGSEAL_E_CRYPTO;
			if (r != MGSEAL_OK) {
				/* Can't seal it: dropped, no counter used. */
				mgseal_wipe(&tmp, sizeof(tmp));
				tx_drop(d);
				if (t->p->data_sent != NULL) {
					t->p->data_sent(t->p->ctx, false);
				}
				continue;
			}
			d->tx_msg_ctr = t->tx_ctr++;
			t->tx_back = 0;
			d->flags |= DF_TAGGED;
		} else if (sealed) {
			keys = session_keys(t, &tmp);
			if (keys == NULL) {
				end_session(t, true);
				break;
			}
		}
		take = wire - d->tx_off;
		if (take > room - hdr) {
			take = room - hdr;
		}
		if (take > d->send_credit) {
			take = d->send_credit;
		}
		f[0] = mg_command_session;
		f[1] = mg_session_data;
		f[2] = (uint8_t)((first ? mg_session_flag_first : 0) | (d->tx_off + take == wire ? mg_session_flag_last : 0) |
				 (sealed ? mg_session_flag_sealed : 0));
		if (first) {
			put32le(&f[3], wire);
		}
		pos = d->tx_off;
		end = d->tx_off + take;
		while (pos < end && r == 0) {
			uint8_t *o = &f[hdr + (pos - d->tx_off)];
			uint32_t m, po;

			if (sealed && pos < 4u) {
				*o = (uint8_t)(d->tx_msg_ctr >> (24u - 8u * pos)); /* the counter, big-endian */
				pos++;
				continue;
			}
			if (sealed && pos >= 4u + plain) {
				m = end - pos;
				memcpy(o, &d->tx_tag[pos - 4u - plain], m);
				pos += m;
				continue;
			}
			po = sealed ? pos - 4u : pos;
			m = plain - po;
			if (m > end - pos) {
				m = end - pos;
			}
			memcpy(o, &d->tx_msg[po], m);
			if (sealed) {
				r = mgseal_xor_at(keys, MGSEAL_DIR_DEVICE, d->tx_msg_ctr, po, o, m);
			}
			pos += m;
		}
		if (keys != NULL) {
			mgseal_wipe(&tmp, sizeof(tmp));
		}
		if (r != 0) {
			mgseal_wipe(f, sizeof(f));
			end_session(t, true);
			break;
		}
		r = send(t, f, hdr + take);
		mgseal_wipe(f, hdr + take);
		if (r == MGDS_AGAIN) {
			break; /* the same fragment from the next poll */
		}
		if (r < 0) {
			end_session(t, false);
			break;
		}
		d->tx_off += take;
		d->send_credit -= take;
		if (d->tx_off == wire) {
			tx_drop(d);
			if (t->p->data_sent != NULL) {
				t->p->data_sent(t->p->ctx, true); /* it may hand over the next one: the loop sends it */
			}
		}
	}
}

/* What the data path owes, after every write and poll: credit the stack had
 * no buffer for, a re-ask come due or open_request owed, the port's message,
 * then a credentials_saved that waited for its first fragment. */
static void data_flush(mgds_t *t, uint32_t now)
{
	if (dget(t) != NULL && dget(t)->credit_owed != 0 && t->state != S_IDLE) {
		uint8_t m[6] = {mg_command_session, mg_session_credit, 0, 0, 0, 0};
		int r;

		put32le(&m[2], dget(t)->credit_owed);
		r = send(t, m, sizeof(m));
		if (r == MGDS_SENT) {
			dget(t)->credit_owed = 0;
		} else if (r < 0) {
			end_session(t, false);
		}
	}
	ask_poll(t, now);
	pump_data(t);
	if (t->state == S_OPEN && saved_owed(t)) {
		pump_saved(t);
		pump_data(t); /* the port's message that waited for it */
	}
}

void mgds_data_attach(mgds_t *t, uint8_t *buf, uint32_t cap)
{
	mgds_data_t *d = t->p->data;
	uint32_t owed = 0;
	uint8_t owe_abort = 0;

	if (d == NULL) {
		return;
	}
	if (dget(t) != NULL && !mgds_data_detach(t) && dget(t) == d) {
		/* Attached again while the detach still owes an abort or credit (no buffer): the fresh state carries
		 * them, and the next poll sends them. */
		owed = d->credit_owed;
		owe_abort = (uint8_t)(d->flags & DF_OWE_ABORT);
	}
	mgseal_wipe(d, sizeof(*d));
	d->credit_owed = owed;
	d->flags = owe_abort;
	d->buf = buf;
	d->cap = buf != NULL ? cap : 0;
	d->vm_state = (t->flags & F_VM_OPEN) ? mg_session_state_open : mg_session_state_closed;
	if (t->state != S_IDLE) {
		/* The hello was answered before: its limits as mgds_t keeps them
		 * (rounded down: never more than the client allows). */
		d->client_max = (uint32_t)1u << t->lim_max;
		d->client_window = (uint32_t)1u << t->lim_win;
	}
	t->ra_steps |= RS_LENT; /* not live until the next state it sees */
}

bool mgds_data_detach(mgds_t *t)
{
	mgds_data_t *d = dget(t);

	if (d == NULL) {
		return true;
	}
	if (d->tx_msg != NULL) {
		if (d->tx_off != 0) {
			d->flags |= DF_OWE_ABORT; /* its counter is used on both sides */
		} else if (d->flags & DF_TAGGED) {
			t->tx_ctr--; /* reserved, never sent: nothing later took a counter */
		}
		d->tx_msg = NULL;
		d->tx_len = d->tx_off = 0;
		d->flags &= (uint8_t)~DF_TAGGED;
	}
	if (rx_kept(t) != NULL) {
		drop_data(t, 0); /* its bytes back (owed if the stack has no buffer), data_end(false) */
	}
	if (dget(t) != d || !tx_abort_owed(t, d) || dget(t) != d) {
		return dget(t) == NULL;
	}
	if (d->credit_owed != 0) {
		uint8_t m[6] = {mg_command_session, mg_session_credit, 0, 0, 0, 0};
		int r;

		put32le(&m[2], d->credit_owed);
		r = send(t, m, sizeof(m));
		if (r == MGDS_AGAIN) {
			return false;
		}
		d->credit_owed = 0;
	}
	if (dget(t) == d) {
		mgseal_wipe(d, sizeof(*d));
		t->ra_steps &= (uint8_t)~RS_LENT;
	}
	return true;
}

uint32_t mgds_send_max(const mgds_t *t)
{
	const mgds_data_t *d = live(t);

	if (d == NULL || d->client_max == 0) {
		return 0;
	}
	return d->client_max - ((t->flags & F_SEALED) ? MGSEAL_OVERHEAD : 0u);
}

bool mgds_can_send(const mgds_t *t, uint32_t len)
{
	const mgds_data_t *d = live(t);

	return d != NULL && d->tx_msg == NULL && !(d->flags & DF_OWE_ABORT) && !clr_taken(t) && !tx_spent(t) &&
	       len != 0 && len <= mgds_send_max(t);
}

int mgds_send(mgds_t *t, const uint8_t *msg, uint32_t len)
{
	if (msg == NULL || !mgds_can_send(t, len)) {
		return MGDS_FAIL;
	}
	dget(t)->tx_msg = msg;
	dget(t)->tx_len = len;
	dget(t)->tx_off = 0;
	return MGDS_OK;
}

void mgds_send_abort(mgds_t *t)
{
	mgds_data_t *d = dget(t);

	if (d == NULL || d->tx_msg == NULL) {
		return;
	}
	if (d->tx_off != 0) {
		d->flags |= DF_OWE_ABORT; /* from the next poll; its counter is used on both sides */
	} else if (d->flags & DF_TAGGED) {
		t->tx_ctr--; /* reserved, never sent: nothing later took a counter (pump_saved waits for it) */
	}
	d->tx_msg = NULL;
	d->tx_len = d->tx_off = 0;
	d->flags &= (uint8_t)~DF_TAGGED;
	mgseal_wipe(d->tx_tag, sizeof(d->tx_tag));
}

void mgds_open_request(mgds_t *t, bool open)
{
	if (open) {
		t->ra_steps &= (uint8_t)~RS_QUIET; /* a task: layer 2's own asks again (even before a hello, or while open) */
	}
	if (!(t->flags & F_HELLO_DONE)) {
		return;
	}
	if (open) {
		if (t->flags & F_VM_OPEN) {
			return;
		}
		t->ra = (uint8_t)((t->ra & ~(RA_OWE0 | RA_OWN0 | RA_PENDING)) | RA_OWE1);
	} else {
		t->ra = (uint8_t)((t->ra & ~(RA_OWE1 | RA_PENDING)) | RA_OWE0 | RA_OWN0);
	}
}

void mgds_ask_hold(mgds_t *t)
{
	t->ra &= (uint8_t)~RA_PENDING;
}

bool mgds_ask_pending(const mgds_t *t)
{
	return (t->ra & RA_PENDING) != 0;
}

void mgds_ask_quiet(mgds_t *t, bool on)
{
	if (on) {
		t->ra_steps |= RS_QUIET;
		t->ra &= (uint8_t)~(RA_PENDING | RA_OWE1); /* a waiting re-ask and layer 2's owed [1]; the port's [0] stays */
	} else {
		t->ra_steps &= (uint8_t)~RS_QUIET;
	}
}

/* A task starts (a wake included): quiet off, and with the VM session down the ended kind's series asks, at once
 * if it hasn't asked yet, else at its next step (an ask of that kind already waiting stands). */
void mgds_task_start(mgds_t *t, uint32_t now)
{
	t->ra_steps &= (uint8_t)~RS_QUIET;
	if ((t->flags & (F_HELLO_DONE | F_VM_OPEN)) == F_HELLO_DONE && !(t->ra & (RA_BLOCK | RA_OWN0 | RA_OWE1))) {
		ask_schedule(t, (t->ra & RA_ENDED_NONET) ? 1u : 0u, now);
	}
}

uint8_t mgds_vm_state(const mgds_t *t, uint16_t *epoch)
{
	if (dget(t) == NULL) {
		if (epoch != NULL) {
			*epoch = 0;
		}
		return (t->flags & F_VM_OPEN) ? mg_session_state_open : mg_session_state_closed;
	}
	if (epoch != NULL) {
		*epoch = dget(t)->epoch;
	}
	return dget(t)->vm_state;
}
#endif

#if MGDS_DATA
static void on_state(mgds_t *t, const uint8_t *p, size_t n, uint32_t now)
#else
static void on_state(mgds_t *t, const uint8_t *p, size_t n)
#endif
{
	if (n < 3) {
		refuse(t, mg_error_code_invalid_length);
		return;
	}
	if (!(t->flags & F_HELLO_DONE)) {
		refuse(t, mg_error_code_invalid_value);
		return;
	}
	if (p[0] > mg_session_state_too_large) {
		refuse(t, mg_error_code_invalid_value);
		return;
	}
#if MGDS_DATA
	bool was_open = (t->flags & F_VM_OPEN) != 0, was_kept = rx_kept(t) != NULL;

	if (was_kept && dget(t)->buf != NULL) {
		mgseal_wipe(dget(t)->buf, t->data_got);
	}
#endif
	/* Any state ends the client's VM session: credit resets to the window. A
	 * sealed message in flight keeps its counter used; the counters restart
	 * only at a sealed hello. */
	t->flags &= (uint8_t)~(F_DATA_IN | F_DATA_SEALED | F_VM_OPEN);
	t->data_total = t->data_got = 0;
#if MGDS_SEALED_DATA
	dtag_put(t);
#endif
	if (p[0] == mg_session_state_open) {
		t->flags |= F_VM_OPEN;
	}
#if MGDS_DATA
	data_state(t, p[0], (uint16_t)(p[1] | p[2] << 8), was_open, was_kept, now);
#endif
}

#if MGDS_STALL
/* Frames the device owes the client (its hello and
 * credentials going out, a sealed credentials_saved going out or a give-up
 * [0] waiting, a sealed clear's answer going out), or a sealed clear part way
 * in (the lent storage held); the clock starts when owing does. */
static void owe_track(mgds_t *t, uint32_t now)
{
	bool owes = t->state == S_SEND_HELLO || t->state == S_SEND_CREDS || saved_owed(t) || t->state == S_CLEARING ||
		    clr_in(t);

	if (!owes) {
		t->owing = 0;
	} else if (!t->owing) {
		t->owing = 1;
		t->owe_at = now;
	}
}

/* Owing MGDS_STALL_MS since the owing began or the last client write
 * (owe_at), or an update part-way (storage lent, or its fragments being
 * discarded for want of it) with no session frame for MGDS_UPDATE_IDLE_MS
 * (rx_at): the session ends with the link, never silently. */
static void stall_check(mgds_t *t, uint32_t now)
{
	const mgds_port_t *p = t->p;

	owe_track(t, now);
	if (p->end_link == NULL) {
		return;
	}
	if ((t->owing && now - t->owe_at >= MGDS_STALL_MS) ||
	    ((t->flags & (F_UPDATE_IN | F_UPDATE_DROP)) && now - t->rx_at >= MGDS_UPDATE_IDLE_MS)) {
		end_session(t, true);
		p->end_link(p->ctx);
	}
}

void mgds_client_frame(mgds_t *t, uint32_t now)
{
	t->owe_at = now; /* any client write restarts the 35 s clock (not the update's 2 min) */
}

void mgds_woke(mgds_t *t, uint32_t now)
{
	t->owe_at = now; /* asleep, not stalled: both clocks from here */
	t->rx_at = now;
}
#endif

void mgds_rx(mgds_t *t, const uint8_t *frame, size_t len, uint32_t now)
{
	const uint8_t *p;
	size_t n;

#if MGDS_STALL
	t->rx_at = now; /* a session frame restarts both clocks */
	t->owe_at = now;
#endif

	if (len < 2) {
		refuse(t, mg_error_code_invalid_length);
		return;
	}
	p = &frame[2];
	n = len - 2;
	if ((t->state == S_CLEARING && frame[1] != mg_session_hello) || t->state == S_CLEAR_WAIT) {
		refuse(t, mg_error_code_invalid_value); /* the session ends once the clear's answer is out */
		return;
	}
	switch (frame[1]) {
	case mg_session_hello:
		on_hello(t, p, n, now);
		break;
	case mg_session_state:
#if MGDS_DATA
		on_state(t, p, n, now);
#else
		on_state(t, p, n);
#endif
		break;
	case mg_session_data:
		on_data(t, p, n);
		break;
	case mg_session_credit:
		if (n < 4) {
			refuse(t, mg_error_code_invalid_length);
		} else if (!(t->flags & F_HELLO_DONE)) {
			refuse(t, mg_error_code_invalid_value);
		}
#if MGDS_DATA
		else if (dget(t) != NULL) {
			uint32_t more = get32le(p);

			dget(t)->send_credit = more > UINT32_MAX - dget(t)->send_credit ? UINT32_MAX : dget(t)->send_credit + more;
		}
		break; /* the port's message goes on (data_flush) */
#else
		break; /* the device sends no data: nothing waits for credit */
#endif
	case mg_session_abort:
		/* The client drops the message it was sending: a data message's bytes
		 * go back as credit, a credentials_update is discarded (no answer).
		 * A sealed one keeps its counter used. */
		if (!(t->flags & F_HELLO_DONE)) {
			refuse(t, mg_error_code_invalid_value);
		} else if (t->flags & F_DATA_IN) {
			drop_data(t, 0);
		} else if (t->flags & (F_UPDATE_IN | F_UPDATE_DROP)) {
			t->flags &= (uint8_t)~(F_UPDATE_IN | F_UPDATE_DROP);
			drop_storage(t);
			t->in_total = t->in_got = 0;
		} else if (clr_in(t)) {
			drop_storage(t); /* a sealed clear in fragments: dropped, its counter used */
			t->in_total = t->in_got = 0;
		}
		break;
	case mg_session_credentials_update:
		on_update(t, p, n, now);
		break;
	case mg_session_clear:
		on_clear(t, p, n);
		break;
#if MGDS_DATA
	case mg_session_wake:
		if (t->flags & F_HELLO_DONE) {
			mgds_task_start(t, now); /* a task of its own; before the hello it is ignored */
		}
		break;
#endif
	default: /* open_request, credentials, credentials_saved: device -> client */
		refuse(t, mg_error_code_unsupported);
		break;
	}
#if MGDS_DATA
	data_flush(t, now);
#endif
	clear_now(t);
#if MGDS_STALL
	owe_track(t, now);
#endif
}

void mgds_poll(mgds_t *t, uint32_t now)
{
	if (t->state == S_DEFER_HELLO) {
		if (!t->p->busy(t->p->ctx)) {
			answer_hello(t);
		} else if (now - t->defer_at >= MGDS_HELLO_DEFER_MS) {
			/* Still busy at the end of its window: busy, never silence; the client's busy
			 * retry (250 / 500 / 1000 ms) asks again. The proof isn't spent. */
			t->state = S_IDLE;
			t->flags = 0;
			t->saved_sealed = 0;
			STAT(t, dropped);
			refuse(t, mg_error_code_busy);
		}
	}
	if (t->state == S_SEND_HELLO || t->state == S_SEND_CREDS) {
		pump(t);
	} else if (saved_owed(t)) {
		pump_saved(t);
	}
	if (t->flags & F_UPDATE_WAIT) {
		if (!t->p->busy(t->p->ctx) && t->state != S_SEND_HELLO && t->state != S_SEND_CREDS &&
		    !saved_owed(t)) {
			commit_update(t);
		} else if (now - t->defer_at >= MGDS_DEFER_MS) {
			t->flags &= (uint8_t)~F_UPDATE_WAIT;
			STAT(t, dropped);
			drop_storage(t);
			t->in_total = t->in_got = 0;
			give_up(t); /* sealed [0] while the link is up, never silence */
		}
	}
	nudge_flush(t); /* before the credit flush: the keepalive takes the credit owed with it */
#if MGDS_DATA
	data_flush(t, now);
#else
	credit_flush(t);
#endif
	clear_now(t);
	pump_clear(t);
#if MGDS_STALL
	stall_check(t, now);
#endif
}
