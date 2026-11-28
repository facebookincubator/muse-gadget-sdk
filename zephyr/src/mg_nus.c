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

#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_MG_AUTOMATION)
#include <hal/nrf_gpio.h>
#endif
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/sys/ring_buffer.h>

#include "mg_app.h"
#include "mg_audio.h"
#include "mg_connparam.h"
#include "mg_core.h"
#include "mg_health.h"
#include "mg_nus_policy.h"
#include "mg_session.h"
#include "mg_setup_store.h"
#include "mg_settings.h"
#include "mg_owner_reads.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_DFU)
#include "mg_ota.h"
#endif
#if defined(CONFIG_MG_BENCH_MODE)
#include <zephyr/sys/reboot.h>
#include "mg_boot.h"
#include "mg_setup.h"
#endif
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_queue.h"
#endif

#if defined(CONFIG_MG_WATCHDOG)
static void tx_dead_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	(void)mg_ble_tx_dead_test();
}
static K_WORK_DELAYABLE_DEFINE(tx_dead_work, tx_dead_fn);
#endif

#if defined(CONFIG_MG_BENCH_MODE)
static void bench_reboot_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	sys_reboot(SYS_REBOOT_WARM); /* warm: the boot record keeps the request */
}
static K_WORK_DELAYABLE_DEFINE(bench_reboot_work, bench_reboot_fn);
#endif

/*
 * Nordic UART Service: TX mirrors the log, RX takes console commands: reads,
 * and the state-changing bench, listen and mode, which any client can reach
 * only in an automation build (in a release build they need this
 * connection's token proof, mg_nus_policy.h); so do a board's own words
 * (mg_board_nus_word()). Nothing here starts capture, presses the button or
 * touches setup.
 *
 * Never blocks anything (a gadget once froze
 * mid-call): the log goes into a bounded ring under a spinlock (no wait), a
 * full ring drops and counts; the ring drains from the system work queue
 * with at most NUS_INFLIGHT notifications in flight (counted by their sent
 * callback), so a chatty subscribed console never takes the ACL buffers the
 * data path's notifications need; and nothing goes while the data path has
 * notifications waiting.
 */

RING_BUF_DECLARE(tx_ring, 2048);
static struct k_spinlock ring_lock;
/* At most this many console notifications in the stack at once (one ACL
 * buffer of the 12, beside the data path's 10). */
#define NUS_INFLIGHT 1
static atomic_t inflight;
static int64_t inflight_since; /* when the oldest one went (a lost callback: reset after 1 s) */
static atomic_t dropped_bytes; /* the ring was full: console output dropped */
static atomic_t log_drops;     /* the log subsystem's own drops (messages) */
static const struct bt_gatt_attr *tx_attr;

unsigned int mg_nus_dropped(void)
{
	return (unsigned int)atomic_get(&dropped_bytes);
}
/* Delayable, and never sleeping: it runs on the system work queue, which
 * the Bluetooth host also needs. */
static struct k_work_delayable tx_work;
static struct k_work rx_work;
static atomic_t enabled;
/* Bumped (under ring_lock) at every subscription change: a notification
 * peeked for one subscription is never sent to, or consumed from the ring
 * of, the next. */
static uint32_t ring_gen;
/* A line is assembled here on the Bluetooth RX thread only; each whole
 * line is copied into rx_q, which the app work queue drains, so a line
 * arriving before the last one ran can't change it. */
static char rx_line[64];
static uint32_t reply_gen; /* the subscription the line now running came on */
static size_t rx_len;
static uint32_t rx_gen; /* the connection rx_line's bytes came from */
/* A queued line runs only on the connection it came from, and its replies
 * go only into the ring of the subscription it came on. */
struct rx_item {
	uint32_t gen;
	uint32_t ring;
	char text[sizeof(rx_line)];
};
K_MSGQ_DEFINE(rx_q, sizeof(struct rx_item), 4, 4);

static uint32_t ring_gen_now(void)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	uint32_t g = ring_gen;

	k_spin_unlock(&ring_lock, key);
	return g;
}

/* Only into the ring of the subscription the producer decided for (gen,
 * snapshotted with its authorization): a line decided for one subscription
 * never lands in the next one's ring. */
static void put_gen(const uint8_t *data, size_t len, uint32_t gen)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	if (gen != ring_gen) {
		k_spin_unlock(&ring_lock, key);
		return; /* stale: its subscription is gone */
	}
	uint32_t put_n = ring_buf_put(&tx_ring, data, len); /* drops what doesn't fit */

	k_spin_unlock(&ring_lock, key);
	if (put_n < len) {
		atomic_add(&dropped_bytes, (atomic_val_t)(len - put_n));
	}
	if (atomic_get(&enabled)) {
		k_work_schedule(&tx_work, K_NO_WAIT);
	}
}

static void sent_cb(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(user_data);
	if (atomic_dec(&inflight) <= 0) {
		atomic_set(&inflight, 0);
	}
	if (atomic_get(&enabled)) {
		k_work_reschedule(&tx_work, K_NO_WAIT);
	}
}

static void tx_fn(struct k_work *w)
{
	static uint8_t buf[64]; /* the stack copies it at the notify */

	ARG_UNUSED(w);
	if (!atomic_get(&enabled) || tx_attr == NULL) {
		return;
	}
	if (atomic_get(&inflight) >= NUS_INFLIGHT) {
		if (k_uptime_get() - inflight_since < 1000) {
			k_work_reschedule(&tx_work, K_MSEC(20)); /* its callback reschedules too */
			return;
		}
		atomic_set(&inflight, 0); /* a callback lost with the link */
	}
	/* The data path first: nothing while its notifications wait. */
	if (mg_transport_tx_pending()) {
		k_work_reschedule(&tx_work, K_MSEC(20));
		return;
	}
	size_t room = MIN(sizeof(buf), (size_t)(mg_transport_att_mtu() - 3));
	/* The subscriber's own connection (bound before the peek): the bytes
	 * go to it, never to whoever subscribes next. */
	struct bt_conn *conn = mg_transport_conn_ref();

	if (conn == NULL) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	uint32_t gen = ring_gen;
	uint32_t n = ring_buf_peek(&tx_ring, buf, room);

	k_spin_unlock(&ring_lock, key);
	if (n == 0) {
		bt_conn_unref(conn);
		return;
	}
	struct bt_gatt_notify_params p = {
		.attr = tx_attr,
		.data = buf,
		.len = (uint16_t)n,
		.func = sent_cb,
	};

	key = k_spin_lock(&ring_lock);
	bool same = gen == ring_gen;

	k_spin_unlock(&ring_lock, key);
	if (!same) {
		bt_conn_unref(conn); /* the subscription changed: drop the peek */
		k_work_reschedule(&tx_work, K_NO_WAIT);
		return;
	}
	atomic_inc(&inflight);
	inflight_since = k_uptime_get();
	int err = bt_gatt_notify_cb(conn, &p);

	bt_conn_unref(conn);
	if (err != 0) {
		atomic_dec(&inflight);
		/* No buffers now (or not subscribed); try again shortly. */
		k_work_reschedule(&tx_work, K_MSEC(20));
		return;
	}
	key = k_spin_lock(&ring_lock);
	if (gen == ring_gen) {
		ring_buf_get(&tx_ring, NULL, n); /* else the new ring isn't ours to consume */
	}
	k_spin_unlock(&ring_lock, key);
}

static void reply(const char *fmt, ...)
{
	char line[176];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintk(line, sizeof(line) - 2, fmt, ap);

	va_end(ap);
	n = MIN(n, (int)sizeof(line) - 3);
	line[n++] = '\r';
	line[n++] = '\n';
	put_gen((const uint8_t *)line, n, reply_gen);
}

static void run_cmd(const char *cmd);

static void rx_fn(struct k_work *w)
{
	struct rx_item it;

	ARG_UNUSED(w);
	while (k_msgq_get(&rx_q, &it, K_NO_WAIT) == 0) {
		if (it.gen == mg_transport_conn_gen()) {
			reply_gen = it.ring; /* its replies go only to its subscription */
			run_cmd(it.text);
		}
	}
}

static void run_cmd(const char *cmd)
{
	const struct mg_settings *s = mg_settings_get();

	if (mg_nus_word_locked(cmd, IS_ENABLED(CONFIG_MG_AUTOMATION), mg_token_proof_matched(),
			       IS_ENABLED(CONFIG_MG_BENCH_MODE))) {
		reply("locked");
		return;
	}
	if (strcmp(cmd, "status") == 0) {
		char who[96];

		/* Never the phone's name here (relayed into the app's logs). */
		(void)mg_core_client_line(who, sizeof(who), false);
		struct mg_audio_gate_stats gs;

		/* A held capture isn't visible here (audio 0). */
		reply("ready %d audio %d set_up %d %s dropped %u console dropped %u B %u logs",
		      mg_core_is_ready(), mg_audio_output_held() ? 0 : mg_audio_activity(),
		      mg_setup_store_complete(), who, mg_session_replies_dropped(),
		      mg_nus_dropped(), (unsigned int)atomic_get(&log_drops));
		mg_audio_gate_stats(&gs);
		reply("output: %s held=%u releases=%u discards=%u stale=%u full=%u turns=%u "
		      "queued=%u start_refused=%u silent=%u",
		      mg_core_bench()         ? "bench"
		      : mg_core_output_ok()   ? "proven"
					      : "unproven",
		      gs.held, gs.releases, gs.discards, gs.stale, gs.full, gs.turns, gs.queued,
		      mg_core_start_refused(), gs.silent);
#if defined(CONFIG_MG_DFU)
		reply("update: %s", mg_ota_prompt_state()); /* for a web updater */
#endif
	} else if (strcmp(cmd, "version") == 0) {
		reply("%s mg%d", mg_app_version(), MG_SPEC_VERSION);
	} else if (strcmp(cmd, "settings") == 0) {
		reply("ptt %u haptics %u buzz %u Hz %u ms %u%% codec %u queue %u",
		      s->push_to_talk_enabled, s->haptics_enabled, s->ptt_buzz_freq_hz,
		      s->ptt_buzz_duration_ms, s->ptt_buzz_volume_percent, s->audio_codec,
		      s->audio_queue_enabled);
#if defined(CONFIG_MG_AUDIO_QUEUE)
	} else if (strcmp(cmd, "queue") == 0) {
		struct mg_queue_status st;

		mg_queue_get_status(&st);
		if (!mg_core_clips_visible()) {
			st.clip_count = 0;
			st.used_bytes = 0;
		}
		reply("clips %u used %u of %u", st.clip_count, st.used_bytes, st.capacity_bytes);
#endif
	} else if (strcmp(cmd, "battery") == 0) {
		reply("battery %u%% %d mV usb %d", mg_app_battery_percent(), mg_app_battery_mv(),
		      mg_app_usb_present());
#if defined(CONFIG_MG_AUTOMATION)
	} else if (strcmp(cmd, "pins") == 0) {
		/* Bench: every port's input levels (a probe for board signals). */
		uint32_t in[3] = {0};

		for (uint32_t port = 0; port < 3; port++) {
			for (uint32_t i = 0; i < 32; i++) {
				uint32_t pin = NRF_GPIO_PIN_MAP(port, i);

				if (nrf_gpio_pin_present_check(pin) && nrf_gpio_pin_read(pin)) {
					in[port] |= BIT(i);
				}
			}
		}
		reply("pins p0 %08x p1 %08x p2 %08x", in[0], in[1], in[2]);
#endif
	} else if (strcmp(cmd, "listen 0") == 0 || strcmp(cmd, "listen 1") == 0) {
		/* Link modes' switch-listen A/B over the air (boards without a
		 * UART console). */
		mg_connparam_switch_listen(cmd[7] == '1');
		reply("listen %c", cmd[7]);
	} else if (strcmp(cmd, "mode 0") == 0 || strcmp(cmd, "mode 1") == 0 ||
		   strcmp(cmd, "mode 2") == 0) {
		/* The client's link mode, as a set_settings write would set it
		 * (the gadget logs each switch's time). */
		mg_connparam_client_mode((uint8_t)(cmd[5] - '0'));
		reply("mode %c", cmd[5]);
	} else if (strcmp(cmd, "bench 1") == 0 || strcmp(cmd, "bench 0") == 0) {
		/* The bench switch over Bluetooth:
		 * this connection only, off at every boot, logged, the LED's bench
		 * mark while on. A release build runs it only on a proven
		 * connection (above). */
		bool on = cmd[6] == '1';

		int r = mg_core_set_bench(on, "Nordic UART", false);

		if (r == -EACCES) {
			reply("bench refused (needs this connection's token proof)");
		} else if (r == -EPERM) {
			reply("bench refused (a set-up gadget in a release build)");
		} else if (r) {
			reply("bench none (no connection)");
		} else {
			reply("bench %c", cmd[6]);
		}
	} else if (strcmp(cmd, "reach test") == 0) {
		/* The reachability check's bench test (bench builds only):
		 * unreachable until the check restarts the gadget. */
		int err = mg_ble_reach_test();

		if (err == -EPERM) {
			reply("reach test disabled (release build)");
		} else if (err) {
			reply("reach test unavailable (no reachability restart in this build)");
		} else {
			reply("reach test: advertising off until the next boot");
		}
	} else if (strcmp(cmd, "att dead test") == 0) {
		/* The ATT-dead bench test for boards without a console UART
		 * (bench builds only): the link drops 200 ms later, so the reply
		 * goes first. */
		int err = mg_ble_att_dead_test();

		if (err == -EPERM) {
			reply("att dead test disabled (release build)");
		} else if (err) {
			reply("att dead test unavailable (no reachability restart in this build)");
		} else {
			reply("att dead test: every link ATT-dead until the next boot");
		}
	} else if (strcmp(cmd, "tx dead test") == 0) {
		/* The TX path's bench test for boards without a console UART
		 * (bench builds only): the reply first, then the link's controller
		 * credits held 300 ms later, so the gadget restarts as a peer loss
		 * about 10 s after. */
#if defined(CONFIG_MG_WATCHDOG)
		if (!IS_ENABLED(CONFIG_MG_AUTOMATION) && !IS_ENABLED(CONFIG_MG_BENCH_MODE)) {
			reply("tx dead test disabled (release build)");
		} else {
			reply("tx dead test: credits held in 300 ms; a peer restart in about 10 s");
			k_work_reschedule(&tx_dead_work, K_MSEC(300));
		}
#else
		reply("tx dead test unavailable (no watchdog in this build)");
#endif
#if defined(CONFIG_MG_DFU)
	} else if (strcmp(cmd, "update window") == 0) {
		/* The update prompt's press, scripted (automation and bench-mode
		 * builds; a release refuses it, proven or not): this connection's
		 * next transfer is admitted for 60 s. */
		if (!IS_ENABLED(CONFIG_MG_AUTOMATION) && !IS_ENABLED(CONFIG_MG_BENCH_MODE)) {
			reply("update window disabled (release build)");
		} else {
			mg_ota_window_open(mg_transport_conn_gen());
			reply("update window open for this connection");
		}
#endif
#if defined(CONFIG_MG_BENCH_MODE)
	} else if (strcmp(cmd, "talk down") == 0 || strcmp(cmd, "talk up") == 0) {
		/* The button, scripted (bench mode counts it as the button's for
		 * every human gate). */
		mg_core_button_from(strcmp(cmd, "talk down") == 0, k_uptime_get(), false);
		reply("%s", cmd);
	} else if (strcmp(cmd, "pair confirm") == 0) {
		/* The bench console's >pair.confirm, for boards without one. */
		bool confirmed = mg_setup_confirm_pending() && mg_setup_button(false);

		reply("pair confirm %s", confirmed ? "confirmed" : "none");
	} else if (strcmp(cmd, "safe next") == 0 || strcmp(cmd, "reset next") == 0) {
		/* SAFE (reason bench) or the setup reset hold at the next boot,
		 * as the button's boot hold would; restarts now. */
		mg_health_bench_request(strcmp(cmd, "safe next") == 0 ? MG_BOOT_F_BENCH_SAFE
								      : MG_BOOT_F_BENCH_RESET);
		reply("%s: restarting", cmd);
		k_work_schedule(&bench_reboot_work, K_MSEC(300));
#endif
	} else if (strcmp(cmd, "reset") == 0) {
		/* The last reset's reason and crash breadcrumb. */
		char line[160];

		mg_health_status(line, sizeof(line));
		reply("%s", line);
	} else {
		char out[160];

		/* A board's own words count as changing state (mg_nus_policy.h). */
		if (!mg_nus_board_word_locked(IS_ENABLED(CONFIG_MG_AUTOMATION),
					      mg_token_proof_matched()) &&
		    mg_board_nus_word(cmd, out, sizeof(out))) {
			reply("%s", out);
		} else {
			/* The bench checks only where they run (release builds
			 * answer them "disabled"). */
			reply("commands: status version settings queue battery bench reset%s%s",
			      IS_ENABLED(CONFIG_MG_AUTOMATION) || IS_ENABLED(CONFIG_MG_BENCH_MODE) ?
				      ", reach test, tx dead test, att dead test, update window" : "",
			      IS_ENABLED(CONFIG_MG_BENCH_MODE) ?
				      ", talk down|up, pair confirm, safe next, reset next" : "");
		}
	}
}

__weak bool mg_board_nus_word(const char *cmd, char *out, size_t len)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(out);
	ARG_UNUSED(len);
	return false;
}

static void received(struct bt_conn *conn, const void *data, uint16_t len, void *ctx)
{
	const char *p = data;
	uint32_t gen = mg_transport_conn_gen();

	mg_transport_peer_wrote(); /* SAFE's idle-link drop */
	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);
	if (gen != rx_gen) {
		/* A partial line from an earlier connection never completes on
		 * this one. */
		rx_len = 0;
		rx_gen = gen;
	}
	for (uint16_t i = 0; i < len; i++) {
		if (p[i] == '\r' || p[i] == '\n') {
			if (rx_len == 0) {
				continue;
			}
			struct rx_item it = {.gen = gen, .ring = ring_gen_now()};

			memcpy(it.text, rx_line, rx_len);
			it.text[rx_len] = '\0';
			rx_len = 0;
			if (k_msgq_put(&rx_q, &it, K_NO_WAIT) != 0) {
				reply("busy: line dropped");
				continue;
			}
			k_work_submit_to_queue(mg_app_wq(), &rx_work);
			mg_connparam_busy(MG_XFER_CONSOLE);
		} else if (rx_len < sizeof(rx_line) - 1) {
			rx_line[rx_len++] = p[i];
		}
	}
}

static void notif_enabled(bool on, void *ctx)
{
	ARG_UNUSED(ctx);
	/* A subscriber gets only what was logged from its subscription on:
	 * lines queued for an earlier one (or none) are dropped, never handed
	 * to the next, possibly unproven, central. */
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	ring_buf_reset(&tx_ring);
	ring_gen++;
	k_spin_unlock(&ring_lock, key);
	atomic_set(&enabled, on);
	atomic_set(&inflight, 0);
	if (on) {
		k_work_schedule(&tx_work, K_NO_WAIT);
	}
}

static struct bt_nus_cb nus_cb = {
	.notif_enabled = notif_enabled,
	.received = received,
};

/* Log backend feeding the ring. Each message is formatted into a line
 * first: the clip queue's lines (clip counts and sizes) are the owner's, so
 * they go out only while the connection's token proof lets it see the clips
 * (mg_core_clips_visible()), like the queue's status. */

static uint8_t log_buf[128];
static char log_line[192]; /* the log thread's only */
static size_t log_line_len;

static uint32_t log_gen; /* the subscription this log line was decided for */

static int log_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	size_t room = sizeof(log_line) - 1 - log_line_len;
	size_t n = length < room ? length : room;

	memcpy(&log_line[log_line_len], data, n);
	log_line_len += n;
	return length;
}

LOG_OUTPUT_DEFINE(nus_log_output, log_out, log_buf, sizeof(log_buf));

static void log_process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	ARG_UNUSED(backend);
	if (log_msg_get_level(&msg->log) > LOG_LEVEL_INF) {
		return;
	}
	log_gen = ring_gen_now(); /* before the decision: a later subscription gets none of it */
	if (!mg_nus_log_allowed(IS_ENABLED(CONFIG_MG_BENCH_MODE), mg_setup_store_complete(),
				mg_token_proof_matched()) ||
	    !MG_OWNER_READ("log")) {
		return; /* a set-up gadget, or any release build: the proven central only */
	}
	log_format_func_t fmt = log_format_func_t_get(LOG_OUTPUT_TEXT);

	log_line_len = 0;
	fmt(&nus_log_output, &msg->log, LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_CRLF_NONE);
	log_line[log_line_len] = '\0';
#if defined(CONFIG_MG_AUDIO_QUEUE)
	if (strstr(log_line, " mg_queue: ") != NULL && !mg_core_clips_visible()) {
		return;
	}
#endif
	put_gen((const uint8_t *)log_line, log_line_len, log_gen);
	put_gen((const uint8_t *)"\r\n", 2, log_gen);
}

static void log_panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
}

static void log_dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);
	atomic_add(&log_drops, (atomic_val_t)cnt);
}

static const struct log_backend_api nus_log_api = {
	.process = log_process,
	.panic = log_panic,
	.dropped = log_dropped,
};

LOG_BACKEND_DEFINE(mg_nus_log, nus_log_api, true);

void mg_nus_init(void)
{
	k_work_init_delayable(&tx_work, tx_fn);
	k_work_init(&rx_work, rx_fn);
	(void)bt_nus_cb_register(&nus_cb, NULL);
	/* NUS TX's value attribute, notified directly so each notification's
	 * sent callback counts it (bt_nus_send has none). */
	tx_attr = bt_gatt_find_by_uuid(NULL, 0, BT_UUID_NUS_TX_CHAR);
}
