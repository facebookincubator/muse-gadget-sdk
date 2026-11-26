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
 * Bench console (CONFIG_MG_BENCH): single-key commands on the console UART,
 * like the ESP32 bench, so the gadget can be driven from a test rig:
 *
 *   d / u   talk button down / up, through the real button path; in a
 *           release build a d while a setup waits for its confirmation is
 *           refused ("@pair.confirm disabled"; the button alone confirms)
 *   s       status: link, push-to-talk, battery, image, offline clips
 *   l       a test control message on an open device session
 *   ?       this list
 *
 * and line commands:
 *
 *   >pair.confirm   confirm a pending Muse Link setup as the button would,
 *                   answering "@pair.confirm confirmed" or "none" (never
 *                   push-to-talk; as on the ESP32's console); "@pair.pending"
 *                   says one is waiting. Automation builds and bench mode
 *                   only: otherwise "@pair.confirm disabled"
 *   >bench=0|1      the bench switch (mic output without a proof) for this
 *                   connection
 *   >listen=0|1     hear every connection event during a link mode switch
 *   >mode=0|1|2     the client's link mode, as if written
 *   >expire         the stored pair's access_expires_at set in the past
 *   >keyauth=0|1    get_device_info's device_key_auth
 *   >bench.safe     bench mode only: SAFE (recovery reason bench) at the next
 *                   boot, as the button's boot hold; restarts now
 *   >bench.reset    bench mode only: the setup reset hold at the next boot;
 *                   restarts now
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/console/console.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include "mg_boot.h"
#include "mg_health.h"
#include "mg_app.h"
#include "mg_audio.h"
#include "mg_cap_level.h"
#include "mg_connparam.h"
#include "mg_button.h"
#include "mg_core.h"
#include "mg_session.h"
#include "mg_settings.h"
#if defined(CONFIG_MG_VM_SESSION)
#include <errno.h>
#include "mg_vm_link.h"
#endif
#include "mg_psa.h"
#include "mg_setup.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_queue.h"
#endif
#if defined(CONFIG_MG_DFU)
#include "mg_ota.h"
#endif

static void status(void)
{
	const struct mg_settings *st = mg_settings_get();
	const char *link = !mg_core_is_connected() ? "disconnected"
			   : !mg_core_is_ready()    ? "connecting"
			   : st->push_to_talk_enabled ? "ready"
						     : "app connected";

	printk("bench: link %s, ptt %s, setup %s, token proof %s\n", link,
	       st->push_to_talk_enabled ? "on" : "off", mg_setup_store_complete() ? "done" : "none",
	       mg_token_proof_matched() ? "matched" : "none");
	char who[96];

	(void)mg_core_client_line(who, sizeof(who), true); /* USB only */
#if defined(CONFIG_MG_NUS)
	printk("bench: %s; control replies dropped since boot: %u; NUS console dropped %u B\n",
	       who, mg_session_replies_dropped(), mg_nus_dropped());
#else
	printk("bench: %s; control replies dropped since boot: %u; no NUS console\n", who,
	       mg_session_replies_dropped());
#endif
	printk("bench: battery %d mV, %u%%, usb %d\n", mg_app_battery_mv(), mg_app_battery_percent(),
	       mg_app_usb_present());
	{
		struct mg_audio_gate_stats gs;

		mg_audio_gate_stats(&gs);
		printk("bench: talk turns dropped as silent (nothing above room noise): %u\n",
		       gs.silent);
	}
	char health[160];

	mg_health_status(health, sizeof(health));
	printk("bench: %s\n", health);
#if defined(CONFIG_MG_DFU)
	printk("bench: image %s, %s%s\n", mg_app_version(),
	       mg_ota_confirmed() ? "confirmed" : "on test (not confirmed)",
	       mg_ota_upload_active() ? ", upload in progress" : "");
#else
	printk("bench: image %s\n", mg_app_version());
#endif
	{
		/* The last turns' capture levels, newest first. */
		struct mg_cap_record lv[4];
		size_t nl = mg_audio_levels(lv, ARRAY_SIZE(lv));

		for (size_t i = 0; i < nl; i++) {
			printk("bench: level %u: %s %u ms, peak %s%d.%d, rms %s%d.%d, floor %s%d.%d "
			       "dBFS, %u clipped, gain level %u (%s%d.%d dB)\n",
			       (unsigned int)i, lv[i].live ? "live" : "clip", lv[i].ms,
			       MG_DDB(lv[i].peak_ddb), MG_DDB(lv[i].rms_ddb), MG_DDB(lv[i].floor_ddb),
			       lv[i].clipped, lv[i].gain_level, MG_DDB(lv[i].gain_ddb));
		}
	}
#if defined(CONFIG_MG_AUDIO_QUEUE)
	struct mg_queue_status q;

	mg_queue_get_status(&q);
	printk("bench: queue %s (%s), %u clips, %u of %u bytes\n",
	       st->audio_queue_enabled ? "on" : "off", mg_queue_owner(), q.clip_count, q.used_bytes,
	       q.capacity_bytes);
#endif
}

#define BENCH_LINES "; lines >pair.confirm, >bench=0|1, >listen=0|1, >mode=0|1|2, >expire, " \
		    ">keyauth=0|1\n"
#if defined(CONFIG_MG_VM_SESSION)
#define BENCH_HELP "bench: keys d (talk down), u (talk up), s (status), l (send a test " \
		   "link-control message)" BENCH_LINES
#else
#define BENCH_HELP "bench: keys d (talk down), u (talk up), s (status)" BENCH_LINES
#endif

#if defined(CONFIG_MG_VM_SESSION)
/* The device session's state lives on the app work queue too. */
static void link_test_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	int err = mg_vm_link_send_test();

	printk("bench: link test %s\n", err == 1 ? "waits for the session (10 s)"
					 : err ? "not sent" : "sent");
}
static K_WORK_DEFINE(link_test_work, link_test_fn);
#endif

/* >expire: the stored pair's access_expires_at set in the past (1), the
 * tokens kept, so the app refreshes at its next hello (a refused-refresh
 * test). The tokens are read into the malloc arena, rewritten through the
 * store's one commit and wiped; nothing is printed but the outcome. */
static void expire_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	char *at = malloc(MG_SETUP_TOKEN_MAX + 1);
	char *rt = malloc(MG_SETUP_TOKEN_MAX + 1);
	size_t al = 0, rl = 0;
	int err = !at || !rt ? -ENOMEM : !mg_setup_store_complete() ? -ENOENT : 0;

	err = err ?: mg_setup_store_get_tokens(at, MG_SETUP_TOKEN_MAX + 1, &al, rt,
					       MG_SETUP_TOKEN_MAX + 1, &rl);
	err = err ?: mg_setup_store_update_tokens_exp(at, al, rt, rl, 1);
	if (at) {
		mg_psa_wipe(at, MG_SETUP_TOKEN_MAX + 1);
		free(at);
	}
	if (rt) {
		mg_psa_wipe(rt, MG_SETUP_TOKEN_MAX + 1);
		free(rt);
	}
	printk("@expire %s\n", err == -ENOENT ? "none (not set up)" : err ? "failed" : "done");
}
static K_WORK_DEFINE(expire_work, expire_fn);

/* Setup state lives on the app work queue. */
static void confirm_fn(struct k_work *w)
{
	ARG_UNUSED(w);
#if defined(CONFIG_MG_AUTOMATION) || defined(CONFIG_MG_BENCH_MODE)
	bool confirmed = mg_setup_confirm_pending() && mg_setup_button(false);

	printk("@pair.confirm %s\n", confirmed ? "confirmed" : "none");
#else
	/* A release build is confirmed with the button alone, as on the
	 * ESP32. */
	printk("@pair.confirm disabled\n");
#endif
}

static K_WORK_DEFINE(confirm_work, confirm_fn);

#if !defined(CONFIG_MG_AUTOMATION) && !defined(CONFIG_MG_BENCH_MODE)
/* d and u in order on the app work queue, where setup state lives: a d
 * that would confirm a waiting setup is refused, and its u with it. */
K_MSGQ_DEFINE(key_q, 1, 8, 1);

static void key_fn(struct k_work *w)
{
	static bool refused;
	char k;

	ARG_UNUSED(w);
	while (k_msgq_get(&key_q, &k, K_NO_WAIT) == 0) {
		if (k == 'd' && mg_setup_confirm_pending()) {
			refused = true;
			printk("@pair.confirm disabled\n");
		} else if (k == 'd') {
			refused = false;
			mg_button_inject(true);
		} else if (refused) {
			refused = false;
		} else {
			mg_button_inject(false);
		}
	}
}

static K_WORK_DEFINE(key_work, key_fn);
#endif

static void talk_key(char k)
{
#if defined(CONFIG_MG_AUTOMATION) || defined(CONFIG_MG_BENCH_MODE)
	mg_button_inject(k == 'd');
#else
	if (k_msgq_put(&key_q, &k, K_NO_WAIT) == 0) {
		k_work_submit_to_queue(mg_app_wq(), &key_work);
	}
#endif
}

#if defined(CONFIG_MG_BENCH_MODE)
static void bench_reboot_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	sys_reboot(SYS_REBOOT_WARM); /* warm: the boot record keeps the request */
}
static K_WORK_DELAYABLE_DEFINE(bench_reboot_work, bench_reboot_fn);
#endif

/* >bench=1|0: the bench switch for the connection there is now. */
static atomic_t bench_want;

static void bench_switch_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	bool on = atomic_get(&bench_want) != 0;

	if (mg_core_set_bench(on, "USB console", true)) {
		printk("@bench none (no connection)\n");
	} else {
		printk("@bench %d\n", on);
	}
}

static K_WORK_DEFINE(bench_switch_work, bench_switch_fn);

/* The rest of a '>' line; an unknown or too long one is reported. */
static void line_command(void)
{
	char line[24];
	size_t len = 0;
	bool whole = true;

	for (;;) {
		int ch = console_getchar();

		if (ch == '\n') {
			break;
		}
		if (ch == '\r') {
			continue;
		}
		if (len < sizeof(line) - 1) {
			line[len++] = (char)ch;
		} else {
			whole = false;
		}
	}
	line[len] = '\0';
	if (whole && strcmp(line, "pair.confirm") == 0) {
		k_work_submit_to_queue(mg_app_wq(), &confirm_work);
#if defined(CONFIG_MG_BENCH_MODE)
	} else if (whole && (strcmp(line, "bench.safe") == 0 || strcmp(line, "bench.reset") == 0)) {
		/* SAFE (reason bench) or the setup reset hold at the next boot, as
		 * the button's boot hold would; restarts now. */
		bool safe_req = strcmp(line, "bench.safe") == 0;

		mg_health_bench_request(safe_req ? MG_BOOT_F_BENCH_SAFE : MG_BOOT_F_BENCH_RESET);
		printk("@%s restarting\n", line);
		k_work_schedule_for_queue(mg_app_wq(), &bench_reboot_work, K_MSEC(300));
#endif
	} else if (whole && (strcmp(line, "listen=0") == 0 || strcmp(line, "listen=1") == 0)) {
		mg_connparam_switch_listen(line[7] == '1');
		printk("@listen %c\n", line[7]);
	} else if (whole && (strcmp(line, "bench=0") == 0 || strcmp(line, "bench=1") == 0)) {
		atomic_set(&bench_want, line[6] == '1');
		k_work_submit_to_queue(mg_app_wq(), &bench_switch_work);
	} else if (whole && strcmp(line, "reach.test") == 0) {
		int err = mg_ble_reach_test();

		printk(err == -EPERM ? "@reach.test disabled\n"
		       : err	     ? "@reach.test unavailable\n"
				     : "@reach.test advertising off until the next boot\n");
	} else if (whole && strcmp(line, "attdead.test") == 0) {
		int err = mg_ble_att_dead_test();

		printk(err == -EPERM ? "@attdead.test disabled\n"
		       : err	     ? "@attdead.test unavailable\n"
				     : "@attdead.test every link ATT-dead until the next boot\n");
	} else if (whole && strcmp(line, "txdead.test") == 0) {
		int err = mg_ble_tx_dead_test();

		printk(err == -EPERM	 ? "@txdead.test disabled\n"
		       : err == -ENOTSUP ? "@txdead.test unavailable\n"
		       : err == -EALREADY ? "@txdead.test already holding them\n"
		       : err		 ? "@txdead.test no link\n"
					 : "@txdead.test credits held: a peer restart in about 10 s\n");
#if defined(CONFIG_MG_DFU)
	} else if (whole && strcmp(line, "update.window") == 0) {
		/* The local update window from the wired console (an owner
		 * action): update writes on this connection for 60 s. */
		if (!mg_transport_link_up()) {
			printk("@update.window no link\n");
		} else {
			mg_ota_window_open(mg_transport_conn_gen());
			printk("@update.window open for 60 s on this connection\n");
		}
#endif
	} else if (whole && strcmp(line, "expire") == 0) {
		k_work_submit_to_queue(mg_app_wq(), &expire_work);
	} else if (whole && (strcmp(line, "keyauth=0") == 0 || strcmp(line, "keyauth=1") == 0)) {
		mg_setup_set_device_key_auth(line[8] == '1');
		printk("@keyauth %c\n", line[8]);
	} else if (whole && strncmp(line, "mode=", 5) == 0 && line[5] >= '0' && line[5] <= '2' &&
		   line[6] == '\0') {
		/* The client's link mode, as a set_settings write would set it. */
		mg_connparam_client_mode((uint8_t)(line[5] - '0'));
		printk("@mode %c\n", line[5]);
	} else {
		printk("@error unknown command\n");
	}
}

static void bench_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	(void)console_init();
	printk(BENCH_HELP);
	for (;;) {
		int ch = console_getchar();

		switch (ch) {
		case 'd':
			printk("bench: talk down\n");
			talk_key('d');
			break;
		case 'u':
			printk("bench: talk up\n");
			talk_key('u');
			break;
		case 's':
			status();
			break;
		case '>':
			line_command();
			break;
#if defined(CONFIG_MG_VM_SESSION)
		case 'l':
			k_work_submit_to_queue(mg_app_wq(), &link_test_work);
			break;
#endif
		case '?':
		case 'h':
			printk(BENCH_HELP);
			break;
		default:
			break;
		}
	}
}

K_THREAD_STACK_DEFINE(bench_stack, 1536);
static struct k_thread bench_thread;

void mg_bench_init(void)
{
	k_thread_create(&bench_thread, bench_stack, K_THREAD_STACK_SIZEOF(bench_stack), bench_fn,
			NULL, NULL, NULL, K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
	k_thread_name_set(&bench_thread, "mg_bench");
}
