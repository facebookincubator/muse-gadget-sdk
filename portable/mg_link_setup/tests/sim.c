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
 * A host gadget for the end-to-end tests: the setup core, the token proof
 * and the two-slot store on a RAM flash, driven line by line on stdin by
 * test_e2e.py (the phone). Commands, one per line:
 *
 *   connect T | disconnect | mtu N | rx HEX | poll T | simpress
 *   press (mgls_press: the talk key) | presskey other|talk | gesture (mgls_confirm_gesture) | dismiss
 *   promptgap MS|off (may_prompt) | setupmode 0|1 (a one-key board: setup_mode() set, answering this)
 *   setuphook off|on (the set_up hook NULL or set)
 *   badconfig (mgls_init() again without set_up: "R init RC"; the core then refuses every hello)
 *   nonce HEX (the next 16-byte random) | eph HEX (the next hello's key)
 *   again N (the next N notifications: no buffer) | notifyfail N
 *   flashfail N (the Nth flash operation from now fails, torn)
 *   deny N (refuse the next N workspace loans) | proof HEX | proofreset
 *   sdk TOKEN | nosdk | state | dump | read | abort | reboot | quit
 *   multiconn (notify_to: connection 0 as above, others print "O CONN HEX") | connect2 T | rx2 HEX | disconnect2
 *   (connection 2 through mgls_conn_*(); the plain commands are connection 0)
 *
 * Output: "N hex" a notification, "E name" an event, "D" a disconnect the
 * core asked for (the sim then delivers it), "P hex" a token proof frame,
 * "S ..." state, "T ..." the stored set, "R status", "L ..." logs,
 * "X problem" a harness check failing (workspace not wiped, notification
 * longer than the MTU allows), "OK" after every command.
 */

/* arc4random_buf: glibc 2.36 and later declare it only with _DEFAULT_SOURCE,
 * which -std=c99 leaves off (older glibc gets the stand-in below). */
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_link_setup.h"
#include "mg_token_proof.h"
#include "mgls_store.h"

#define SLOT 4096

static struct {
	uint8_t flash[2 * SLOT];
	int flash_fail; /* fail the Nth operation from now (0: off) */
	mglss_t store;
	mgls_t setup;
	mgtp_t proof;
	uint8_t nonce[16];
	bool nonce_set;
	int again, notifyfail, deny;
	bool disc_req;
	char sdk[256];
	bool sdk_set;
	void *ws;
	size_t ws_len;
	uint8_t addr[6];
	uint32_t now; /* the last poll's or connect's time */
} g;

static void hexout(const char *tag, const uint8_t *p, size_t n)
{
	printf("%s ", tag);
	for (size_t i = 0; i < n; i++) {
		printf("%02x", p[i]);
	}
	printf("\n");
}

static size_t unhex(const char *h, uint8_t *out, size_t cap)
{
	size_t n = strlen(h) / 2;

	if (n > cap) {
		n = cap;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned int b;

		sscanf(&h[2 * i], "%2x", &b);
		out[i] = (uint8_t)b;
	}
	return n;
}

/* ---- flash -------------------------------------------------------------- */

static bool flash_fault(void)
{
	if (g.flash_fail > 0 && --g.flash_fail == 0) {
		printf("L flash fault injected\n");
		return true;
	}
	return false;
}

static int fl_erase(void *ctx, uint32_t addr, uint32_t len)
{
	(void)ctx;
	if (flash_fault()) {
		memset(&g.flash[addr], 0xFF, len / 2);
		return -1;
	}
	memset(&g.flash[addr], 0xFF, len);
	return 0;
}

static int fl_write(void *ctx, uint32_t addr, const void *data, uint32_t len)
{
	const uint8_t *p = data;

	(void)ctx;
	if (flash_fault()) {
		len /= 2;
		for (uint32_t i = 0; i < len; i++) {
			g.flash[addr + i] &= p[i];
		}
		return -1;
	}
	for (uint32_t i = 0; i < len; i++) {
		g.flash[addr + i] &= p[i];
	}
	return 0;
}

static int fl_read(void *ctx, uint32_t addr, void *data, uint32_t len)
{
	(void)ctx;
	memcpy(data, &g.flash[addr], len);
	return 0;
}

static const mglss_flash_t FLASH = {NULL, {0, SLOT}, SLOT, fl_erase, fl_write, fl_read};

/* ---- platform ----------------------------------------------------------- */

#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 36)) /* no arc4random_buf before glibc 2.36 */
#include <sys/random.h>
static void arc4random_buf(void *buf, size_t n)
{
	for (uint8_t *p = buf; n;) {
		size_t k = n < 256 ? n : 256;
		if (getentropy(p, k) != 0)
			abort();
		p += k;
		n -= k;
	}
}
#endif

static int sim_random(void *ctx, uint8_t *out, size_t len)
{
	(void)ctx;
	if (len == 16 && g.nonce_set) {
		memcpy(out, g.nonce, 16);
		g.nonce_set = false;
		return 0;
	}
	arc4random_buf(out, len);
	return 0;
}

static void *sim_borrow(void *ctx, size_t need)
{
	(void)ctx;
	if (g.ws != NULL) {
		printf("X borrow while lent\n");
		return NULL;
	}
	if (g.deny > 0) {
		g.deny--;
		printf("E borrow_denied\n");
		return NULL;
	}
	/* Exactly the size asked for, so ASan sees any overrun, filled with
	 * garbage so nothing relies on zeroed memory. */
	g.ws = malloc(need);
	g.ws_len = need;
	memset(g.ws, 0xA5, need);
	printf("E borrow\n");
	return g.ws;
}

static void sim_release(void *ctx, void *ws)
{
	const uint8_t *p = ws;

	(void)ctx;
	if (ws != g.ws) {
		printf("X release of a foreign pointer\n");
		return;
	}
	for (size_t i = 0; i < g.ws_len; i++) {
		if (p[i] != 0) {
			printf("X workspace not wiped at %zu\n", i);
			break;
		}
	}
	free(ws);
	g.ws = NULL;
	printf("E release\n");
}

static int sim_notify(void *ctx, const uint8_t *data, size_t len)
{
	(void)ctx;
	if (g.again > 0) {
		g.again--;
		return MGLS_AGAIN;
	}
	if (g.notifyfail > 0) {
		g.notifyfail--;
		return -1;
	}
	if (len > (size_t)g.setup.mtu - 3) {
		printf("X notification of %zu bytes over MTU %u\n", len, g.setup.mtu);
	}
	hexout("N", data, len);
	return MGLS_OK;
}

/* notify_to ("multiconn"): connection 0 is the phone the tests drive (as notify), another prints "O". */
static int sim_notify_to(void *ctx, uint16_t conn, const uint8_t *data, size_t len)
{
	if (conn == 0) {
		return sim_notify(ctx, data, len);
	}
	printf("O %u ", (unsigned)conn);
	for (size_t i = 0; i < len; i++) {
		printf("%02x", data[i]);
	}
	printf("\n");
	return MGLS_OK;
}

static void sim_disconnect(void *ctx)
{
	(void)ctx;
	printf("D\n");
	g.disc_req = true;
}

static int sim_commit(void *ctx, const mgls_credentials_t *c)
{
	(void)ctx;
	return mglss_commit(&g.store, c);
}

static int sim_erase(void *ctx)
{
	(void)ctx;
	return mglss_erase(&g.store);
}

static const char *sim_sdk_token(void *ctx, size_t *len)
{
	(void)ctx;
	if (!g.sdk_set) {
		*len = 0;
		return NULL;
	}
	*len = strlen(g.sdk);
	return g.sdk;
}

static const char *const EVENTS[] = {
	"",          "session_started", "confirm_armed",  "confirmed", "no_sdk_token",
	"timeout",   "provisioned",     "storage_failed", "unpaired",  "session_ended",
};

/* "setupmode 0|1": a one-key board (setup_mode() set), in setup mode or not. As the board does, setup mode ends at
 * the confirmed re-setup or 5 min after it began. */
#define SIM_SETUP_MODE_MS (5u * 60u * 1000u)
static bool g_setup_mode;
static uint32_t g_setup_mode_at;

static void sim_event(void *ctx, enum mgls_event ev)
{
	(void)ctx;
	printf("E %s\n", (unsigned)ev < sizeof(EVENTS) / sizeof(EVENTS[0]) ? EVENTS[ev] : "?");
	if (ev == MGLS_EV_CONFIRMED && mglss_set_up(&g.store)) {
		g_setup_mode = false; /* setup mode ends at the confirmed re-setup */
	}
	if (ev == MGLS_EV_PROVISIONED) {
		/* Contract 7: proof state on a live connection resets at the commit. */
		mgtp_reset(&g.proof);
	}
}

static bool sim_set_up(void *ctx)
{
	(void)ctx;
	return mglss_set_up(&g.store);
}

/* A release build's rule (MGLS_RESETUP_PROMPT_MS), or the gap the test sets ("promptgap MS"; "promptgap off" is the
 * test build's hands-free default, MGLS_HOOK_TEST_DEFAULT(may_prompt), as at start). */
static uint32_t g_prompt_gap;


static bool sim_setup_mode(void *ctx)
{
	(void)ctx;
	return g_setup_mode && g.now - g_setup_mode_at < SIM_SETUP_MODE_MS;
}

static bool sim_may_prompt(void *ctx, uint32_t since_ms)
{
	bool ok = since_ms >= g_prompt_gap;

	(void)ctx;
	if (since_ms == MGLS_NO_PROMPT) {
		printf("M since=none ok=%d\n", ok);
	} else {
		printf("M since=%lu ok=%d\n", (unsigned long)since_ms, ok);
	}
	return ok;
}

static void sim_log(void *ctx, const char *fmt, long a, long b)
{
	(void)ctx;
	printf("L ");
	printf(fmt, a, b);
	printf("\n");
}

static mgls_config_t cfg = {
	.bd_addr = g.addr,
	.firmware_version = "1.0.0",
	.mgcommands = 1,
	.borrow = sim_borrow,
	.release = sim_release,
	.notify = sim_notify,
	.disconnect = sim_disconnect,
	.random = sim_random,
	.commit = sim_commit,
	.erase = sim_erase,
	.sdk_token = sim_sdk_token,
	.event = sim_event,
	.log = sim_log,
	.set_up = sim_set_up,
	.may_prompt = MGLS_HOOK_TEST_DEFAULT(may_prompt), /* "promptgap MS": the rate as a release build's */
};

/* ---- token proof platform ------------------------------------------------ */

static int tp_get_k(void *ctx, uint8_t k[32])
{
	(void)ctx;
	return mglss_get_k(&g.store, k);
}

static int tp_send(void *ctx, const uint8_t *frame, size_t len)
{
	(void)ctx;
	hexout("P", frame, len);
	return 0;
}

static int tp_clear(void *ctx)
{
	(void)ctx;
	printf("E cleared\n");
	return mglss_erase(&g.store);
}

static const mgtp_platform_t TP = {NULL, tp_get_k, sim_random, tp_send, tp_clear};

/* ---- main ---------------------------------------------------------------- */

static void after_call(void)
{
	if (g.disc_req) {
		g.disc_req = false;
		mgls_disconnected(&g.setup);
		mgtp_reset(&g.proof);
	}
}

static void boot(void)
{
	if (mglss_load(&g.store, &FLASH) != 0) {
		printf("L store load: flash error\n");
	}
	if (mgls_init(&g.setup, &cfg) != MGLS_OK) {
		printf("L mgls_init: MGLS_E_CONFIG\n");
	}
	mgtp_init(&g.proof, &TP);
}

static void usage(void)
{
	fprintf(stderr, "sim [--addr HEX12] [--node S] [--device S] [--mac S] [--fw S] "
			"[--no-mgcommands] [--sdk TOKEN]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	static char line[16384];

	memcpy(g.addr, "\x02\x00\x00\x00\x00\x01", 6);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
			unhex(argv[++i], g.addr, 6);
		} else if (strcmp(argv[i], "--node") == 0 && i + 1 < argc) {
			cfg.node_id = argv[++i];
		} else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			cfg.device_id = argv[++i];
		} else if (strcmp(argv[i], "--mac") == 0 && i + 1 < argc) {
			cfg.mac = argv[++i];
		} else if (strcmp(argv[i], "--fw") == 0 && i + 1 < argc) {
			cfg.firmware_version = argv[++i];
		} else if (strcmp(argv[i], "--no-mgcommands") == 0) {
			cfg.mgcommands = 0;
		} else if (strcmp(argv[i], "--sdk") == 0 && i + 1 < argc) {
			snprintf(g.sdk, sizeof(g.sdk), "%s", argv[++i]);
			g.sdk_set = true;
		} else {
			usage();
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	memset(g.flash, 0xFF, sizeof(g.flash));
	boot();

	while (fgets(line, sizeof(line), stdin)) {
		char *arg;
		static uint8_t buf[8192];

		line[strcspn(line, "\r\n")] = '\0';
		arg = strchr(line, ' ');
		if (arg) {
			*arg++ = '\0';
		} else {
			arg = line + strlen(line);
		}
		if (strcmp(line, "connect") == 0) {
			g.now = (uint32_t)strtoul(arg, NULL, 10);
			mgls_connected(&g.setup, g.now);
			mgtp_reset(&g.proof);
		} else if (strcmp(line, "disconnect") == 0) {
			mgls_disconnected(&g.setup);
			mgtp_reset(&g.proof);
		} else if (strcmp(line, "mtu") == 0) {
			mgls_mtu_changed(&g.setup, (uint16_t)atoi(arg));
		} else if (strcmp(line, "rx") == 0) {
			size_t n = unhex(arg, buf, sizeof(buf));

			mgls_rx(&g.setup, buf, n);
		} else if (strcmp(line, "poll") == 0) {
			g.now = (uint32_t)strtoul(arg, NULL, 10);
			mgls_poll(&g.setup, g.now);
		} else if (strcmp(line, "presskey") == 0) {
			printf("R press %d\n",
			       (int)mgls_press_key(&g.setup, strcmp(arg, "talk") == 0 ? MGLS_KEY_TALK : MGLS_KEY_OTHER));
		} else if (strcmp(line, "setupmode") == 0) {
			g_setup_mode = atoi(arg) != 0;
			g_setup_mode_at = g.now;
			cfg.setup_mode = sim_setup_mode;
		} else if (strcmp(line, "multiconn") == 0) {
			cfg.notify_to = sim_notify_to;
		} else if (strcmp(line, "connect2") == 0) {
			mgls_conn_connected(&g.setup, 2, (uint32_t)strtoul(arg, NULL, 10));
		} else if (strcmp(line, "rx2") == 0) {
			size_t n2 = unhex(arg, buf, sizeof(buf));

			mgls_conn_rx(&g.setup, 2, buf, n2);
		} else if (strcmp(line, "disconnect2") == 0) {
			mgls_conn_disconnected(&g.setup, 2);
		} else if (strcmp(line, "press") == 0) {
			printf("R press %d\n", (int)mgls_press(&g.setup));
		} else if (strcmp(line, "dismiss") == 0) {
			printf("R dismiss %d\n", mgls_dismiss(&g.setup) ? 1 : 0);
		} else if (strcmp(line, "gesture") == 0) {
			printf("R gesture %d\n", mgls_confirm_gesture(&g.setup) ? 1 : 0);
		} else if (strcmp(line, "simpress") == 0) {
#if defined(MGLS_ENABLE_SIMULATED_PRESS)
			printf("R press %d\n", (int)mgls_simulated_press(&g.setup));
#else
			printf("X simulated press not compiled in\n");
#endif
		} else if (strcmp(line, "nonce") == 0) {
			unhex(arg, g.nonce, 16);
			g.nonce_set = true;
		} else if (strcmp(line, "eph") == 0) {
			uint8_t d[32];

			unhex(arg, d, 32);
			mgls_test_set_ephemeral(d);
		} else if (strcmp(line, "again") == 0) {
			g.again = atoi(arg);
		} else if (strcmp(line, "setuphook") == 0) {
			cfg.set_up = strcmp(arg, "off") == 0 ? NULL : sim_set_up;
		} else if (strcmp(line, "promptgap") == 0) {
			if (strcmp(arg, "off") == 0) {
				cfg.may_prompt = MGLS_HOOK_TEST_DEFAULT(may_prompt);
			} else {
				g_prompt_gap = (uint32_t)strtoul(arg, NULL, 10);
				cfg.may_prompt = sim_may_prompt;
			}
		} else if (strcmp(line, "notifyfail") == 0) {
			g.notifyfail = atoi(arg);
		} else if (strcmp(line, "flashfail") == 0) {
			g.flash_fail = atoi(arg);
		} else if (strcmp(line, "deny") == 0) {
			g.deny = atoi(arg);
		} else if (strcmp(line, "proof") == 0) {
			size_t n = unhex(arg, buf, sizeof(buf));

			mgtp_rx(&g.proof, buf, n);
		} else if (strcmp(line, "proofreset") == 0) {
			mgtp_reset(&g.proof);
		} else if (strcmp(line, "sdk") == 0) {
			snprintf(g.sdk, sizeof(g.sdk), "%s", arg);
			g.sdk_set = true;
		} else if (strcmp(line, "nosdk") == 0) {
			g.sdk_set = false;
		} else if (strcmp(line, "state") == 0) {
			int blank = 1;
			int other = g.store.cur == 0 ? 1 : 0;

			for (int i = 0; i < SLOT; i++) {
				if (g.flash[other * SLOT + i] != 0xFF) {
					blank = 0;
				}
			}
			printf("S armed=%d active=%d idle=%d ws=%d setup=%d seq=%u wifi_skipped=%d "
			       "other_blank=%d matched=%d\n",
			       mgls_confirm_armed(&g.setup), mgls_session_active(&g.setup),
			       mgls_idle(&g.setup), g.ws != NULL, mglss_set_up(&g.store),
			       (unsigned)g.store.seq,
			       (g.store.flags & MGLS_CRED_WIFI_SKIPPED) != 0, blank,
			       mgtp_matched(&g.proof));
		} else if (strcmp(line, "dump") == 0) {
			static char tok[4096];
			uint8_t k[32];
			size_t n;

			if (!mglss_set_up(&g.store)) {
				printf("T none\n");
			} else {
				(void)mglss_get_k(&g.store, k);
				hexout("T k", k, 32);
				(void)mglss_read_token(&g.store, false, tok, sizeof(tok), &n);
				hexout("T access", (const uint8_t *)tok, n);
				(void)mglss_read_token(&g.store, true, tok, sizeof(tok), &n);
				hexout("T refresh", (const uint8_t *)tok, n);
			}
		} else if (strcmp(line, "read") == 0) {
			char st[80];
			size_t n = mgls_read_status(&g.setup, st, sizeof(st) - 1);

			st[n] = '\0';
			printf("R %s\n", st);
		} else if (strcmp(line, "abort") == 0) {
			mgls_abort(&g.setup);
		} else if (strcmp(line, "badconfig") == 0) {
			/* A board's glue without set_up(): mgls_init() refuses it and the core fails closed. */
			cfg.set_up = NULL;
			printf("R init %d\n", mgls_init(&g.setup, &cfg));
			cfg.set_up = sim_set_up;
		} else if (strcmp(line, "reboot") == 0) {
			if (g.ws != NULL) {
				mgls_abort(&g.setup);
			}
			boot();
		} else if (strcmp(line, "quit") == 0) {
			break;
		} else {
			printf("X unknown command %s\n", line);
		}
		after_call();
		printf("OK\n");
	}
	if (g.ws != NULL) {
		mgls_abort(&g.setup);
	}
	return 0;
}
