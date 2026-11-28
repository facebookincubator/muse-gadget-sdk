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

/* A gadget for test_session.py: mg_device_session over a fake port, driven
 * line by line on stdin. Commands:
 *   mtu N | enc 0|1 | recovery 0|1 | busy 0|1 | storage 0|1 | commitfail 0|1
 *   clearcb 0|1        port.clear set (default) or NULL; clearfail 0|1: it fails. "CL ok" each call (ok: the pair
 *                      is erased: no token from then on)
 *   clearpend 0|1      port.clear answers MGDS_CLEAR_PENDING ("CL pend"); cleardone 0|1: the erase ends (1: the
 *                      pair erased), mgds_clear_done()
 *   again N            the next N sends answer MGDS_AGAIN
 *   budget N           only N more sends go through, then MGDS_AGAIN (-1: no limit)
 *   hello MAX WINDOW   this device's hello (default 16384 4096, the exchange vectors')
 *   linkdown           every send fails (< 0) from now on
 *   endlink 0|1        (MGDS_STALL builds) port.end_link set (default) or NULL; "L" each time it is called
 *   sleep MS           (MGDS_STALL builds) time += MS without a poll (a connected sleep)
 *   woke | cframe      (MGDS_STALL builds) mgds_woke / mgds_client_frame now
 *   k HEX | node STR | access STR | refresh STR | nopair
 *   expiry 0|1         the board keeps access_expires_at (expires + commit_expires)
 *   exp N              the stored access_expires_at (0: none)
 *   proof CN DN        a new token proof matched (the session reset at its challenge):
 *                      nonces available once
 *   unproof            no matching proof (a new challenge)
 *   nudge              mgds_nudge (the keepalive)
 *   counters RX TX     the next client -> device and device -> client counters jump there (- keeps one): a long
 *                      session's stand-in
 *   w HEX              one Control write
 *   poll MS            time += MS, then mgds_poll
 *   pump               mgds_poll until it sends nothing more (MGDS_AGAIN off)
 *   reset 0|1 | release
 *   taglend 0|1        (MGDS_TAG_LEND builds) the tag state can be lent (default 1)
 *   show               prints the stored pair, the takes, the stats
 * MGDS_DATA builds (the data path):
 *   dmode buf|piece    client messages reassembled into the port's buffer (default) or in pieces (attaches the
 *                      lent state again)
 *   dattach 0|1        mgds_data_detach ("DT 1|0") / mgds_data_attach now (attached from the start by default)
 *   dnull              the port has no mgds_data_t (port.data NULL)
 *   dlazy 0|1          1: detached now, attached only inside the data_state() callback for open, detached at
 *                      the callback for any other state ("DT ..." each time)
 *   send HEX           mgds_send() of these bytes (kept until SENT), "Q rc", then mgds_poll
 *   sendnp HEX         the same without the poll (it goes from the next write's or poll's flush)
 *   sendnext HEX       the port's next message, given to mgds_send() from inside data_sent() (back to back)
 *   cansend N | sendmax | sendabort | openreq 0|1 | vmstate | askhold | askpending | askquiet 0|1
 *   task 0|1           a task ends (mgds_ask_quiet on) / starts (mgds_task_start, then mgds_poll)
 * Data output: "ST state epoch" (data_state), "D HEX" (data_message), "P off HEX len"
 * (data_piece), "PE ok" (data_end), "SENT ok" (data_sent), "CS 0|1", "SM n",
 * "VS state epoch".
 * Output: "N HEX" per frame sent (MGDS_SENT), "A" per MGDS_AGAIN, "C al rl ok"
 * per commit (then "X at" with expiry on), "T ok" per take_nonces, "S ..." for
 * show. Test tokens only. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_device_session.h"

static struct {
	uint16_t mtu;
	int enc, recovery, busy, storage, commitfail, clearfail, clearpend, again, budget, linkdown, pair, proven, nonces_left;
	uint8_t k[32], cn[16], dn[16];
	char node[80], access[2100], refresh[2100];
	uint8_t store[2100];
	int borrowed, takes, sends, agains;
	unsigned long long exp;
} g;

/* "credswait 1": the stack has no room for the credentials until "credswait 0" (the session vectors'
 * credentials_wait device and its buffer event); every other frame goes. */
static int g_creds_wait; /* mg_session_credentials is 6 (mgcommands.h) */

static int p_send(void *ctx, const uint8_t *f, size_t n)
{
	(void)ctx;
	if (g.linkdown) {
		return -1;
	}
	if (g.again > 0 || g.budget == 0 || (g_creds_wait && n > 1 && f[1] == 6u)) {
		if (g.again > 0) {
			g.again--;
		}
		printf("A\n");
		g.agains++;
		return MGDS_AGAIN;
	}
	if (g.budget > 0) {
		g.budget--;
	}
	if (n + 3 > g.mtu) {
		printf("E frame %zu over mtu %u\n", n, g.mtu);
		exit(3);
	}
	printf("N ");
	for (size_t i = 0; i < n; i++) {
		printf("%02x", f[i]);
	}
	printf("\n");
	g.sends++;
	return MGDS_SENT;
}

static uint16_t p_mtu(void *ctx) { (void)ctx; return g.mtu; }
static bool p_enc(void *ctx) { (void)ctx; return g.enc != 0; }
static bool p_recovery(void *ctx) { (void)ctx; return g.recovery != 0; }
static bool p_proven(void *ctx) { (void)ctx; return g.proven != 0; }

static bool p_take(void *ctx, uint8_t cn[16], uint8_t dn[16])
{
	(void)ctx;
	if (!g.proven || !g.nonces_left) {
		printf("T 0\n");
		return false;
	}
	memcpy(cn, g.cn, 16);
	memcpy(dn, g.dn, 16);
	g.nonces_left = 0;
	g.takes++;
	printf("T 1\n");
	return true;
}

static int p_get_k(void *ctx, uint8_t k[32])
{
	(void)ctx;
	if (!g.pair) {
		return MGDS_NONE;
	}
	memcpy(k, g.k, 32);
	return MGDS_OK;
}

static int p_node(void *ctx, char node[MGDS_NODE_MAX], uint8_t *len)
{
	size_t n = strlen(g.node);

	(void)ctx;
	if (n > MGDS_NODE_MAX) {
		return MGDS_FAIL;
	}
	memcpy(node, g.node, n);
	*len = (uint8_t)n;
	return MGDS_OK;
}

static int p_token_len(void *ctx, int which, uint16_t *len)
{
	(void)ctx;
	if (!g.pair) {
		return MGDS_NONE;
	}
	*len = (uint16_t)strlen(which ? g.refresh : g.access);
	return MGDS_OK;
}

static int p_token_read(void *ctx, int which, uint16_t off, uint8_t *buf, size_t n)
{
	const char *t = which ? g.refresh : g.access;

	(void)ctx;
	if (n > 32 || off + n > strlen(t)) {
		printf("E token_read %d %u %zu\n", which, off, n);
		exit(3);
	}
	memcpy(buf, t + off, n);
	return MGDS_OK;
}

static bool p_busy(void *ctx) { (void)ctx; return g.busy != 0; }

static uint8_t *p_borrow(void *ctx, size_t need)
{
	(void)ctx;
	if (!g.storage || g.borrowed || need > sizeof(g.store)) {
		return NULL;
	}
	g.borrowed = 1;
	memset(g.store, 0xEE, sizeof(g.store));
	return g.store;
}

static void p_release(void *ctx)
{
	(void)ctx;
	if (!g.borrowed) {
		printf("E release without borrow\n");
		exit(3);
	}
	for (size_t i = 0; i < sizeof(g.store); i++) {
		if (g.store[i] != 0 && g.store[i] != 0xEE) {
			printf("E storage not wiped at %zu\n", i);
			exit(3);
		}
	}
	g.borrowed = 0;
}

static int p_commit(void *ctx, const char *a, uint16_t al, const char *r, uint16_t rl)
{
	(void)ctx;
	printf("C %u %u %d\n", al, rl, !g.commitfail);
	if (g.commitfail) {
		return MGDS_FAIL;
	}
	memcpy(g.access, a, al);
	g.access[al] = 0;
	memcpy(g.refresh, r, rl);
	g.refresh[rl] = 0;
	return MGDS_OK;
}

static void erase_pair(void)
{
	g.pair = 0;
	memset(g.k, 0, sizeof(g.k));
	g.access[0] = g.refresh[0] = 0;
}

static int p_clear(void *ctx)
{
	(void)ctx;
	if (g.clearpend) {
		printf("CL pend\n");
		return MGDS_CLEAR_PENDING;
	}
	printf("CL %d\n", !g.clearfail);
	if (g.clearfail) {
		return MGDS_FAIL;
	}
	erase_pair();
	return MGDS_OK;
}

static bool p_expires(void *ctx, uint64_t *at)
{
	(void)ctx;
	*at = g.exp;
	return g.exp != 0;
}

static int p_commit_exp(void *ctx, const char *a, uint16_t al, const char *r, uint16_t rl, uint64_t at)
{
	int e = p_commit(ctx, a, al, r, rl);

	printf("X %llu\n", (unsigned long long)at);
	if (e == MGDS_OK) {
		g.exp = at;
	}
	return e;
}

#if MGDS_SEALED_DATA && MGDS_TAG_LEND
/* MGDS_TAG_LEND: the board's lent tag state (one at a time). */
static mgds_dtag_t lent_dtag;
static int taglend = 1, lent, lends, returns;

mgds_dtag_t *mgds_tag_lend(void *ctx)
{
	(void)ctx;
	if (!taglend || lent) {
		return NULL;
	}
	lent = 1;
	lends++;
	memset(&lent_dtag, 0xA5, sizeof(lent_dtag)); /* the lease's leftovers: the core must not trust them */
	return &lent_dtag;
}

void mgds_tag_return(void *ctx, mgds_dtag_t *d)
{
	const uint8_t *b = (const uint8_t *)d;

	(void)ctx;
	if (d != &lent_dtag || !lent) {
		fprintf(stderr, "E tag returned twice or not lent\n");
		exit(3);
	}
	for (size_t i = 0; i < sizeof(*d); i++) {
		if (b[i] != 0) {
			fprintf(stderr, "E tag returned unwiped\n");
			exit(3);
		}
	}
	lent = 0;
	returns++;
}
#endif

#if MGDS_DATA
static mgds_t *g_t;
static mgds_data_t g_d;
static int g_piece, g_lazy;
static uint8_t data_buf[70000];
static uint8_t tx_buf[70000], tx_copy[70000];
static uint32_t tx_len;
static int tx_out; /* the port's message is lent to layer 2 */
static uint8_t next_buf[4096];
static uint32_t next_len; /* sendnext: the port's next message, given from data_sent() */

static void attach(void)
{
	mgds_data_attach(g_t, g_piece ? NULL : data_buf, sizeof(data_buf));
}

static void p_data_state(void *ctx, uint8_t st, uint16_t epoch)
{
	(void)ctx;
	printf("ST %u %u\n", st, epoch);
	if (g_lazy) {
		if (st == 2) {
			attach();
		} else if (g_t->ra_steps & 0x40u) { /* attached */
			printf("DT %d\n", mgds_data_detach(g_t) ? 1 : 0);
			tx_out = 0;
		}
	}
}

static void hexline(const char *tag, const uint8_t *b, size_t n)
{
	printf("%s", tag);
	for (size_t i = 0; i < n; i++) {
		printf("%02x", b[i]);
	}
}

static void p_data_message(void *ctx, const uint8_t *msg, uint32_t len)
{
	(void)ctx;
	if (msg < data_buf || msg + len > data_buf + sizeof(data_buf) || g_piece) {
		printf("E message outside the port's buffer\n");
		exit(3);
	}
	hexline("D ", msg, len);
	printf("\n");
}

static void p_data_piece(void *ctx, uint32_t off, const uint8_t *piece, size_t n, uint32_t len)
{
	(void)ctx;
	if (n == 0 || n > 64 || off + n > len) {
		printf("E piece %u %zu %u\n", off, n, len);
		exit(3);
	}
	printf("P %u ", off);
	hexline("", piece, n);
	printf(" %u\n", len);
}

static void p_data_end(void *ctx, bool ok)
{
	(void)ctx;
	printf("PE %d\n", ok ? 1 : 0);
}

static void p_data_sent(void *ctx, bool ok)
{
	(void)ctx;
	if (!tx_out) {
		printf("E sent without a message\n");
		exit(3);
	}
	if (memcmp(tx_buf, tx_copy, tx_len) != 0) {
		printf("E the port's message changed\n");
		exit(3);
	}
	tx_out = 0;
	printf("SENT %d\n", ok ? 1 : 0);
	if (next_len != 0) {
		int rc;

		memcpy(tx_buf, next_buf, next_len);
		rc = mgds_send(g_t, tx_buf, next_len);
		if (rc == MGDS_OK) {
			tx_out = 1;
			tx_len = next_len;
			memcpy(tx_copy, tx_buf, next_len);
		}
		next_len = 0;
		printf("Q %d\n", rc);
	}
}
#endif

#if MGDS_STALL
static int g_links_ended;

static void p_end_link(void *ctx)
{
	(void)ctx;
	g_links_ended++;
	printf("L\n"); /* the board drops the link */
}
#endif

static mgds_port_t PORT = {NULL,      16384,      4096,   p_send,     p_mtu,
				 p_enc,     p_recovery, p_proven, p_take,   p_get_k,
				 p_node,    p_token_len, p_token_read, p_busy, p_borrow,
				 p_release, p_commit, NULL, NULL};

static size_t unhex(const char *h, uint8_t *out, size_t cap)
{
	size_t n = strlen(h) / 2;

	if (n > cap) {
		exit(2);
	}
	for (size_t i = 0; i < n; i++) {
		unsigned v;

		if (sscanf(h + 2 * i, "%2x", &v) != 1) {
			exit(2);
		}
		out[i] = (uint8_t)v;
	}
	return n;
}

int main(void)
{
	static char line[140000];
	static uint8_t frame[70000];
	mgds_t t;
	uint32_t now = 1000;

	memset(&g, 0, sizeof(g));
	g.mtu = 247;
	g.enc = 0;
	g.storage = 1;
	g.pair = 1;
	g.budget = -1;
	PORT.clear = p_clear;
#if MGDS_DATA
	PORT.data_state = p_data_state;
	PORT.data = &g_d;
	PORT.data_message = p_data_message;
	PORT.data_piece = p_data_piece;
	PORT.data_end = p_data_end;
	PORT.data_sent = p_data_sent;
#endif
#if MGDS_STALL
	PORT.end_link = p_end_link;
#endif
	mgds_init(&t, &PORT);
#if MGDS_DATA
	g_t = &t;
	attach();
#endif
	setvbuf(stdout, NULL, _IOLBF, 0);
	while (fgets(line, sizeof(line), stdin)) {
		static char a[140000], b[140000];
		char cmd[32] = {0};
		int n;

		a[0] = b[0] = 0;
		n = sscanf(line, "%31s %139999s %139999s", cmd, a, b);

		if (n < 1) {
			continue;
		}
		if (!strcmp(cmd, "caps")) {
			printf("caps %02x\n", (unsigned)MGDS_CAPS);
		} else if (!strcmp(cmd, "mtu")) {
			g.mtu = (uint16_t)atoi(a);
		} else if (!strcmp(cmd, "enc")) {
			g.enc = atoi(a);
		} else if (!strcmp(cmd, "recovery")) {
			g.recovery = atoi(a);
		} else if (!strcmp(cmd, "busy")) {
			g.busy = atoi(a);
		} else if (!strcmp(cmd, "storage")) {
			g.storage = atoi(a);
		} else if (!strcmp(cmd, "commitfail")) {
			g.commitfail = atoi(a);
		} else if (!strcmp(cmd, "clearfail")) {
			g.clearfail = atoi(a);
		} else if (!strcmp(cmd, "clearpend")) {
			g.clearpend = atoi(a);
		} else if (!strcmp(cmd, "cleardone")) {
			if (atoi(a)) {
				erase_pair();
			}
			mgds_clear_done(&t, atoi(a) != 0);
		} else if (!strcmp(cmd, "clearcb")) {
			PORT.clear = atoi(a) ? p_clear : NULL;
		} else if (!strcmp(cmd, "expiry")) {
			PORT.expires = atoi(a) ? p_expires : NULL;
			PORT.commit_expires = atoi(a) ? p_commit_exp : NULL;
		} else if (!strcmp(cmd, "exp")) {
			g.exp = strtoull(a, NULL, 10);
		} else if (!strcmp(cmd, "again")) {
			g.again = atoi(a);
		} else if (!strcmp(cmd, "budget")) {
			g.budget = atoi(a);
		} else if (!strcmp(cmd, "credswait")) {
			g_creds_wait = atoi(a);
		} else if (!strcmp(cmd, "hello")) {
			PORT.max_message = (uint32_t)atoi(a);
			PORT.window = (uint32_t)atoi(b);
		} else if (!strcmp(cmd, "linkdown")) {
			g.linkdown = 1;
#if MGDS_STALL
		} else if (!strcmp(cmd, "endlink")) {
			PORT.end_link = atoi(a) ? p_end_link : NULL;
		} else if (!strcmp(cmd, "sleep")) {
			now += (uint32_t)atoi(a); /* a connected sleep: time passes, no poll */
		} else if (!strcmp(cmd, "woke")) {
			mgds_woke(&t, now);
		} else if (!strcmp(cmd, "cframe")) {
			mgds_client_frame(&t, now);
#endif
		} else if (!strcmp(cmd, "k")) {
			unhex(a, g.k, 32);
		} else if (!strcmp(cmd, "node")) {
			snprintf(g.node, sizeof(g.node), "%s", a);
		} else if (!strcmp(cmd, "access")) {
			snprintf(g.access, sizeof(g.access), "%s", a);
		} else if (!strcmp(cmd, "refresh")) {
			snprintf(g.refresh, sizeof(g.refresh), "%s", a);
		} else if (!strcmp(cmd, "nopair")) {
			g.pair = 0;
		} else if (!strcmp(cmd, "proof")) {
			mgds_reset(&t, true); /* the board's rule: every challenge ends the session */
			unhex(a, g.cn, 16);
			unhex(b, g.dn, 16);
			g.proven = 1;
			g.nonces_left = 1;
		} else if (!strcmp(cmd, "counters")) {
			/* A stand-in for a long session (the vectors' counters event): the next client -> device and device ->
			 * client counters jump there (- leaves one as it is). */
			if (strcmp(a, "-") != 0) {
				t.rx_counter = (uint32_t)strtoul(a, NULL, 0);
				t.rx_hole = 0;
				t.rx_top = 0;
			}
			if (strcmp(b, "-") != 0) {
				t.tx_ctr = (mgds_txctr_t)strtoul(b, NULL, 0);
			}
		} else if (!strcmp(cmd, "nudge")) {
			mgds_nudge(&t);
		} else if (!strcmp(cmd, "unproof")) {
			g.proven = 0;
			g.nonces_left = 0;
		} else if (!strcmp(cmd, "w")) {
			size_t len = unhex(a, frame, sizeof(frame));

			if (len == 0 || frame[0] != 0x89) {
				exit(2);
			}
			mgds_rx(&t, frame, len, now);
		} else if (!strcmp(cmd, "poll")) {
			now += (uint32_t)atoi(a);
			mgds_poll(&t, now);
		} else if (!strcmp(cmd, "pump")) {
			for (int i = 0; i < 200 && mgds_busy(&t); i++) {
				int before = g.sends, ab = g.agains;

				mgds_poll(&t, now);
				if (g.sends == before && (g.agains == ab || g.budget == 0)) {
					break;
				}
			}
		} else if (!strcmp(cmd, "reset")) {
			mgds_reset(&t, atoi(a) != 0);
		} else if (!strcmp(cmd, "release")) {
			mgds_release_storage(&t);
		} else if (!strcmp(cmd, "taglend")) {
#if MGDS_SEALED_DATA && MGDS_TAG_LEND
			taglend = atoi(a);
#endif
#if MGDS_DATA
		} else if (!strcmp(cmd, "dmode")) {
			g_piece = !strcmp(a, "piece");
			attach();
		} else if (!strcmp(cmd, "dattach")) {
			if (atoi(a)) {
				attach();
			} else {
				printf("DT %d\n", mgds_data_detach(&t) ? 1 : 0);
				tx_out = 0; /* a detach drops the port's message (no data_sent) */
			}
		} else if (!strcmp(cmd, "dnull")) {
			PORT.data = NULL; /* a port without the data path's state */
		} else if (!strcmp(cmd, "dlazy")) {
			g_lazy = atoi(a);
			if (g_lazy && (t.ra_steps & 0x40u)) {
				printf("DT %d\n", mgds_data_detach(&t) ? 1 : 0);
				tx_out = 0;
			}
		} else if (!strcmp(cmd, "send") || !strcmp(cmd, "sendnp")) {
			uint32_t len = (uint32_t)unhex(a, tx_buf, sizeof(tx_buf));
			int rc;

			if (tx_out) {
				printf("E send while one is lent\n");
				exit(3);
			}
			rc = mgds_send(&t, tx_buf, len);
			if (rc == MGDS_OK) {
				tx_out = 1;
				tx_len = len;
				memcpy(tx_copy, tx_buf, len);
			}
			printf("Q %d\n", rc);
			if (!strcmp(cmd, "send")) {
				mgds_poll(&t, now);
			}
		} else if (!strcmp(cmd, "sendnext")) {
			next_len = (uint32_t)unhex(a, next_buf, sizeof(next_buf));
		} else if (!strcmp(cmd, "cansend")) {
			printf("CS %d\n", mgds_can_send(&t, (uint32_t)atoi(a)) ? 1 : 0);
		} else if (!strcmp(cmd, "sendmax")) {
			printf("SM %u\n", mgds_send_max(&t));
		} else if (!strcmp(cmd, "sendabort")) {
			mgds_send_abort(&t);
			tx_out = 0;
		} else if (!strcmp(cmd, "openreq")) {
			mgds_open_request(&t, atoi(a) != 0);
		} else if (!strcmp(cmd, "askhold")) {
			mgds_ask_hold(&t);
		} else if (!strcmp(cmd, "askpending")) {
			printf("AP %d\n", mgds_ask_pending(&t) ? 1 : 0);
		} else if (!strcmp(cmd, "askquiet")) {
			mgds_ask_quiet(&t, atoi(a) != 0);
		} else if (!strcmp(cmd, "task")) {
			if (atoi(a)) {
				mgds_task_start(&t, now);
				mgds_poll(&t, now);
			} else {
				mgds_ask_quiet(&t, true);
			}
		} else if (!strcmp(cmd, "vmstate")) {
			uint16_t ep = 0;
			uint8_t st = mgds_vm_state(&t, &ep);

			printf("VS %u %u\n", st, ep);
#endif
		} else if (!strcmp(cmd, "show")) {
			printf("S access=%s refresh=%s takes=%d borrowed=%d busy=%d\n", g.access, g.refresh,
			       g.takes, g.borrowed, mgds_busy(&t) ? 1 : 0);
#if MGDS_SEALED_DATA && MGDS_TAG_LEND
			printf("S lent=%d lends=%d returns=%d\n", lent, lends, returns);
#endif
#if MGDS_STATS
			printf("S stats hellos=%u sealed=%u refused=%u aborted=%u deferred=%u dropped=%u "
			       "saved=%u not_saved=%u bad=%u last=%u ctr_ends=%u\n",
			       t.st.hellos, t.st.sealed, t.st.refused, t.st.aborted, t.st.deferred,
			       t.st.dropped, t.st.saved, t.st.not_saved, t.st.bad_updates, t.st.last_refusal, t.st.ctr_ends);
#endif
		} else {
			fprintf(stderr, "unknown command %s\n", cmd);
			return 2;
		}
		printf(".\n"); /* end of this command's output */
	}
	return 0;
}
