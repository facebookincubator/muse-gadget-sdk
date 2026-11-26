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


#include "mg_connparam.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_core.h"
#include "mg_transport.h"
#include "mgcommands.h"

LOG_MODULE_REGISTER(mg_connparam, CONFIG_MG_LOG_LEVEL);

#define READY_WAIT_MS 2000 /* PHY and data length updates, at most */
/* Never ask at connect: after the central's own first update, or this
 * long after connecting if none came (a request colliding with the central's
 * own update can go unanswered, and the controller then drops the link on its
 * 40 s procedure timer). */
#define FIRST_REQ_MS 1000
#define GRANT_WAIT_MS 5000 /* for the central to apply a request */
#define IDLE_ITVL_WIDE 24  /* idle's floor once 15 ms was granted or nothing came back */
#define IDLE_NO_REPLY_MS 2000 /* an idle request allowing 15 ms with no update this long: not granted */

static const char *const XFER_NAMES[] = {"dfu",   "tokens",     "stream",    "throughput test",
					 "mic",   "talk press", "GATT write", "console",
					 "clip download"};
#define CONNECT_IDLE INT64_MIN / 2 /* last_action at connect: no activity yet */

struct params {
	uint16_t min, max, latency, timeout;
};

static struct k_spinlock lock;
static struct {
	bool connected;
	bool phy, dle;
	bool central_updated; /* the central's own first update came */
	int64_t conn_at;
	int64_t last_action;
	unsigned int holds;  /* MG_CONNPARAM_HOLD_*: activity that lasts */
	const char *action_why;
	uint8_t mode;        /* the effective mode (mg_link_mode_t) */
	bool mode_set;       /* the first mode (ready) taken on this connection */
	uint8_t client_mode; /* what the client wrote (ready at connect) */
	int64_t client_active_at; /* when it wrote active */
	int64_t ready_since; /* when the effective mode became ready */
	bool auto_sleep;     /* asleep by itself (10 min in ready): until activity or a write */
	bool force;          /* a real update owed even if the link matches */
	bool idle_wide;      /* a central gave 15 ms for 12/24: idle asks 24/24 */
	bool have_cur;
	uint16_t interval, latency, timeout; /* in force */
	struct params asked;
	uint8_t asked_mode;  /* the mode the request out is for */
	/* The switch out (an experiment): from the request to its
	 * grant, timed, and heard at every event when switch_listen is on. */
	bool sw_open;
	int64_t sw_at;
	uint8_t sw_from;     /* the mode before */
	uint8_t mode_before; /* the effective mode before the last change */
	uint16_t sw_from_lat;
	uint32_t sw_cancels;
	bool pending;
	int64_t req_at;
	bool requested;      /* a request went out on this connection */
	int64_t last_req_at; /* when (CONFIG_MG_CONN_REQ_GAP_MS apart at least) */
	uint8_t retries;
	bool gave_up;
} s;
static struct k_work_delayable work;
static struct k_work_delayable listen_work;
static bool switch_listen = IS_ENABLED(CONFIG_MG_CONN_SWITCH_LISTEN);
#define SWITCH_LISTEN_MAX_MS 15000 /* stop listening at every event after this */

static void schedule_at(int64_t at, int64_t now)
{
	k_work_reschedule_for_queue(mg_app_wq(), &work, K_MSEC(MAX(at - now, 1)));
}

static const char *const MODE_NAMES[] = {"sleep", "ready", "active"};

static struct params mode_params(uint8_t mode)
{
	if (mode == mg_link_mode_active) {
		return (struct params){CONFIG_MG_CONN_INTERVAL_MIN, CONFIG_MG_CONN_INTERVAL_MAX,
				       CONFIG_MG_CONN_LATENCY, CONFIG_MG_CONN_TIMEOUT};
	}
	if (mode == mg_link_mode_sleep) {
		return (struct params){CONFIG_MG_CONN_SLEEP_INTERVAL_MIN,
				       CONFIG_MG_CONN_SLEEP_INTERVAL_MAX, CONFIG_MG_CONN_SLEEP_LATENCY,
				       CONFIG_MG_CONN_SLEEP_TIMEOUT};
	}
	return (struct params){s.idle_wide ? IDLE_ITVL_WIDE : CONFIG_MG_CONN_IDLE_INTERVAL_MIN,
			       CONFIG_MG_CONN_IDLE_INTERVAL_MAX, CONFIG_MG_CONN_IDLE_LATENCY,
			       CONFIG_MG_CONN_TIMEOUT};
}

/* The effective mode now (Link modes, mgcommands.h), and when it next
 * changes by itself (0: not by itself). */
static uint8_t effective(int64_t now, int64_t *change_at)
{
	*change_at = 0;
	/* The device's own activity, and 5 s after it. */
	if (s.holds != 0) {
		return mg_link_mode_active;
	}
	if (now - s.last_action < CONFIG_MG_CONN_IDLE_AFTER_MS) {
		*change_at = s.last_action + CONFIG_MG_CONN_IDLE_AFTER_MS;
		return mg_link_mode_active;
	}
	/* A client's active, until 30 s without activity. */
	if (s.client_mode == mg_link_mode_active) {
		int64_t from = MAX(s.client_active_at, s.last_action);

		if (now - from < CONFIG_MG_CONN_CLIENT_ACTIVE_MS) {
			*change_at = from + CONFIG_MG_CONN_CLIENT_ACTIVE_MS;
			return mg_link_mode_active;
		}
		s.client_mode = mg_link_mode_ready;
	}
	if (s.client_mode == mg_link_mode_sleep) {
		return mg_link_mode_sleep;
	}
	/* Ready; asleep by itself after 10 min in ready without activity. */
	if (s.auto_sleep) {
		return mg_link_mode_sleep;
	}
	if (s.mode == mg_link_mode_ready && s.mode_set) {
		if (now - s.ready_since >= CONFIG_MG_CONN_SLEEP_AFTER_MS) {
			s.auto_sleep = true;
			return mg_link_mode_sleep;
		}
		*change_at = s.ready_since + CONFIG_MG_CONN_SLEEP_AFTER_MS;
	}
	return mg_link_mode_ready;
}

static bool granted(const struct params *w)
{
	return s.have_cur && s.interval >= w->min && s.interval <= w->max &&
	       s.latency == w->latency && s.timeout == w->timeout;
}

static int64_t at_most(int64_t next, int64_t t)
{
	return next ? MIN(next, t) : t;
}

static void eval(struct k_work *w)
{
	ARG_UNUSED(w);
	k_spinlock_key_t key = k_spin_lock(&lock);
	int64_t now = k_uptime_get();
	int64_t next = 0;
	bool request = false, log_mode = false, log_gave_up = false, notify = false;
	struct params want = {0};
	const char *why = NULL;

	if (!s.connected) {
		goto out;
	}
	if (!(s.phy && s.dle) && now - s.conn_at < READY_WAIT_MS) {
		next = s.conn_at + READY_WAIT_MS;
		goto out;
	}
	if (!s.central_updated && now - s.conn_at < FIRST_REQ_MS) {
		next = s.conn_at + FIRST_REQ_MS;
		goto out;
	}
	int64_t change_at;
	uint8_t mode = effective(now, &change_at);

	uint8_t prev_mode = s.mode_set ? s.mode : mg_link_mode_ready;

	if (!s.mode_set || mode != s.mode) {
		notify = s.mode_set;
		s.mode_before = prev_mode;
		if (mode == mg_link_mode_ready) {
			s.ready_since = now;
			change_at = now + CONFIG_MG_CONN_SLEEP_AFTER_MS;
		}
		s.mode_set = true;
		s.mode = mode;
		s.force = true;
		s.retries = 0;
		s.pending = false;
		s.gave_up = false;
		log_mode = true;
	}
	if (change_at) {
		next = change_at;
	}
	/* An idle set that allows 15 ms, and no update within 2 s (a Mac on a
	 * 15 ms link sends none): not granted, 24 and up. */
	if (mode == mg_link_mode_ready && !s.idle_wide && s.pending &&
	    s.asked.min < IDLE_ITVL_WIDE &&
	    s.asked.latency == CONFIG_MG_CONN_IDLE_LATENCY && !granted(&s.asked)) {
		if (now - s.req_at >= IDLE_NO_REPLY_MS) {
			s.idle_wide = true;
			s.force = true;
			s.retries = 0;
		} else {
			next = at_most(next, s.req_at + IDLE_NO_REPLY_MS);
		}
	}
	want = mode_params(mode);
	/* Never re-send the set the link is in (a Mac rejects it; the
	 * open controller drops it unseen), a mode change included. */
	if (granted(&want)) {
		s.force = false;
		s.pending = false;
		goto out;
	}
	if (!s.force && s.gave_up) {
		goto out;
	}
	if (!s.force && s.pending && now - s.req_at < GRANT_WAIT_MS) {
		next = at_most(next, s.req_at + GRANT_WAIT_MS);
		goto out;
	}
	if (!s.force && s.pending) {
		if (s.retries >= 1) {
			s.pending = false;
			s.gave_up = true;
			log_gave_up = true;
			goto out;
		}
		s.retries++;
	}
	if (s.requested && now - s.last_req_at < CONFIG_MG_CONN_REQ_GAP_MS) {
		/* At most one request per CONFIG_MG_CONN_REQ_GAP_MS. */
		next = at_most(next, s.last_req_at + CONFIG_MG_CONN_REQ_GAP_MS);
		goto out;
	}
	s.force = false;
	s.pending = true;
	s.req_at = now;
	s.requested = true;
	s.last_req_at = now;
	s.asked = want;
	request = true;
	if (!s.sw_open || s.asked_mode != mode) {
		s.sw_from = s.mode_before;
		s.sw_open = true;
		s.sw_at = now;
		s.sw_from_lat = s.have_cur ? s.latency : 0;
		s.sw_cancels = 0;
	}
	s.asked_mode = mode;
	next = at_most(next, now + GRANT_WAIT_MS);
out:
	if (next) {
		schedule_at(next, now);
	}
	uint8_t mode_now = s.mode;
	uint8_t retries = s.retries;

	why = s.action_why;
	k_spin_unlock(&lock, key);

	if (log_mode) {
		LOG_INF("mg.ble: link %s (%s)", MODE_NAMES[mode_now],
			mode_now == mg_link_mode_active ? (why ? why : "the client")
			: notify ? "activity over" : "connected");
	}
	if (notify) {
		/* Every effective change: [get_settings, link_mode, mode]. */
		const uint8_t m[] = {mg_command_get_settings, mg_setting_parameter_link_mode,
				     mode_now};

		mg_core_send_unsolicited(m, sizeof(m));
	}
	if (log_gave_up) {
		LOG_WRN("mg.ble: the central didn't grant the %s set; keeping what it chose",
			MODE_NAMES[mode_now]);
	}
	if (request) {
		int err = mg_transport_conn_params(want.min, want.max, want.latency, want.timeout);

		if (switch_listen) {
			k_work_reschedule_for_queue(mg_app_wq(), &listen_work, K_NO_WAIT);
		}

		LOG_INF("mg.ble: asking for %u-%u x 1.25 ms, latency %u, timeout %u ms (%s)%s (%d)",
			want.min, want.max, want.latency, want.timeout * 10U, MODE_NAMES[mode_now],
			retries ? ", again" : "", err);
	}
}

/* While a switch is out, hear every connection event (the controller's
 * latency skip cancelled twice an interval) until the new set is granted. */
static void listen_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool go = switch_listen && s.connected && s.sw_open && s.have_cur && s.latency > 0 &&
		  k_uptime_get() - s.sw_at < SWITCH_LISTEN_MAX_MS;
	uint32_t itvl_ms = s.have_cur ? MAX(s.interval * 5U / 4U, 8U) : 30U;

	if (go) {
		s.sw_cancels++;
	}
	k_spin_unlock(&lock, key);
	if (!go) {
		return;
	}
	(void)mg_transport_latency_cancel();
	/* A cancel holds for the next event only. */
	k_work_reschedule_for_queue(mg_app_wq(), &listen_work, K_MSEC(itvl_ms / 2));
}

void mg_connparam_switch_listen(bool on)
{
	switch_listen = on;
	LOG_INF("mg.ble: switch listen %s", on ? "on (every event until the grant)" : "off");
}

static void kick(void)
{
	k_work_reschedule_for_queue(mg_app_wq(), &work, K_NO_WAIT);
}

void mg_connparam_init(void)
{
	k_work_init_delayable(&work, eval);
	k_work_init_delayable(&listen_work, listen_fn);
}

void mg_connparam_connected(uint16_t interval, uint16_t latency, uint16_t timeout)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool wide = s.idle_wide; /* the same central, likely: keep what it taught */

	memset(&s, 0, sizeof(s));
	s.connected = true;
	if (interval) {
		/* What the central connected with: a link already in the fast set
		 * gets no request. */
		s.have_cur = true;
		s.interval = interval;
		s.latency = latency;
		s.timeout = timeout;
	}
	s.conn_at = k_uptime_get();
	s.last_action = CONNECT_IDLE; /* ready at connect */
	s.client_mode = mg_link_mode_ready;
	s.idle_wide = wide;
	k_spin_unlock(&lock, key);
	kick();
}

void mg_connparam_disconnected(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool wide = s.idle_wide;

	memset(&s, 0, sizeof(s));
	s.idle_wide = wide;
	k_spin_unlock(&lock, key);
	(void)k_work_cancel_delayable(&work);
}

void mg_connparam_phy_done(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	s.phy = true;
	k_spin_unlock(&lock, key);
	kick();
}

void mg_connparam_data_len_done(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	s.dle = true;
	k_spin_unlock(&lock, key);
	kick();
}

void mg_connparam_updated(uint16_t interval, uint16_t latency, uint16_t timeout)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool sw_done = false;
	int64_t sw_ms = 0;
	uint8_t sw_from = 0, sw_to = 0;
	uint16_t sw_from_lat = 0;
	uint32_t sw_cancels = 0;

	s.have_cur = true;
	s.central_updated = true;
	s.interval = interval;
	s.latency = latency;
	s.timeout = timeout;
	if (s.sw_open && interval >= s.asked.min && interval <= s.asked.max &&
	    latency == s.asked.latency && timeout == s.asked.timeout) {
		sw_done = true;
		sw_ms = k_uptime_get() - s.sw_at;
		sw_from = s.sw_from;
		sw_to = s.asked_mode;
		sw_from_lat = s.sw_from_lat;
		sw_cancels = s.sw_cancels;
		s.sw_open = false;
	}
	/* An idle set that allows 15 ms, and the central chose 15 ms: 24 and up. */
	if (s.requested && !s.idle_wide && s.asked.max == CONFIG_MG_CONN_IDLE_INTERVAL_MAX &&
	    s.asked.min < IDLE_ITVL_WIDE && s.asked.latency == CONFIG_MG_CONN_IDLE_LATENCY &&
	    interval < 20) {
		s.idle_wide = true;
		if (s.mode == mg_link_mode_ready) {
			s.force = true;
		}
	}
	k_spin_unlock(&lock, key);
	if (sw_done) {
		LOG_INF("mg.ble: switch %s -> %s applied in %lld ms (latency %u before, listen %s, %u cancels)",
			MODE_NAMES[sw_from], MODE_NAMES[sw_to], sw_ms, sw_from_lat,
			switch_listen ? "on" : "off", sw_cancels);
	}
	kick();
}

void mg_connparam_busy(enum mg_xfer what)
{
	if (what == MG_XFER_WRITE || what == MG_XFER_CONSOLE) {
		/* A GATT write alone, or console input, isn't activity
		 * (background writes stay idle). */
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (!s.connected) {
		k_spin_unlock(&lock, key);
		return;
	}
	s.last_action = k_uptime_get();
	s.action_why = XFER_NAMES[what];
	s.auto_sleep = false;
	bool was_idle = s.mode_set && s.mode != mg_link_mode_active;

	k_spin_unlock(&lock, key);
	if (was_idle) {
		kick();
	}
}

void mg_connparam_client_mode(uint8_t mode)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (!s.connected) {
		k_spin_unlock(&lock, key);
		return;
	}
	s.client_mode = mode;
	s.auto_sleep = false;
	if (mode == mg_link_mode_active) {
		s.client_active_at = k_uptime_get();
		s.action_why = "the client";
	} else if (mode == mg_link_mode_ready && s.mode == mg_link_mode_ready) {
		s.ready_since = k_uptime_get();
	}
	k_spin_unlock(&lock, key);
	kick();
}

void mg_connparam_user_wake(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool kick_it = false;

	if (s.connected && (s.auto_sleep || s.client_mode == mg_link_mode_sleep ||
			    (s.mode_set && s.mode == mg_link_mode_sleep))) {
		/* Sleep -> ready (mgcommands.h, Link modes): a call from sleep
		 * would wait seconds for the central to schedule the update. */
		s.auto_sleep = false;
		if (s.client_mode == mg_link_mode_sleep) {
			s.client_mode = mg_link_mode_ready;
		}
		kick_it = true;
	}
	if (s.connected && s.mode_set && s.mode == mg_link_mode_ready) {
		s.ready_since = k_uptime_get(); /* the 10 minutes start again */
	}
	k_spin_unlock(&lock, key);
	if (kick_it) {
		kick();
	}
}

uint8_t mg_connparam_mode(void)
{
	return s.connected && s.mode_set ? s.mode : mg_link_mode_ready;
}

void mg_connparam_link_params(uint16_t *interval, uint16_t *latency, uint16_t *timeout)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*interval = s.have_cur ? s.interval : 0;
	*latency = s.have_cur ? s.latency : 0;
	*timeout = s.have_cur ? s.timeout : 0;
	k_spin_unlock(&lock, key);
}

void mg_connparam_hold(unsigned int bit, bool on)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	unsigned int was = s.holds;

	if (!s.connected) {
		k_spin_unlock(&lock, key);
		return;
	}
	s.holds = on ? (s.holds | bit) : (s.holds & ~bit);
	if (s.holds != was) {
		/* Idle CONFIG_MG_CONN_IDLE_AFTER_MS after it ends. */
		s.last_action = k_uptime_get();
		s.auto_sleep = false;
		if (on) {
			s.action_why = "Link setup";
		}
	}
	k_spin_unlock(&lock, key);
	if (s.holds != was) {
		kick();
	}
}

bool mg_connparam_fast(void)
{
	return s.connected && s.mode_set && s.mode == mg_link_mode_active;
}
