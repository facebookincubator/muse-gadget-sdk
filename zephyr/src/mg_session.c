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

#include "mg_evt_policy.h"
#include "mg_session.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_core.h"
#include "mg_throughput.h"
#include "mgcommands.h"
#if defined(CONFIG_MG_VM_SESSION)
#include "mg_vm_session.h"
#endif

LOG_MODULE_REGISTER(mg_session, CONFIG_MG_LOG_LEVEL);

/* One sender at a time on Control and Data. */
static K_MUTEX_DEFINE(tx_lock);

static struct {
	bool connected;
	bool sub[MG_ATT_CHAN_COUNT];
	bool ready;
	bool status_seen; /* request_status on this connection */
} s;

static void update_ready(void);

/* ------------------------------------------------------------------------
 * Control and Data, in plaintext.
 * ---------------------------------------------------------------------- */

void mg_session_init(void)
{
}

static bool authorized(void)
{
	return s.connected;
}

void mg_session_rx(enum mg_att_chan ch, const uint8_t *buf, size_t len)
{
	if (ch == MG_ATT_CONTROL && len > 0) {
		mg_core_control_rx(buf, len);
	} else if (ch == MG_ATT_DATA && s.connected) {
		/* Client Data is only the receive throughput test's. */
		mg_tp_data_rx(buf, len);
	}
}

void mg_session_control_lost(void)
{
	LOG_ERR("a Control write was dropped");
#if defined(CONFIG_MG_VM_SESSION)
	mg_vm_session_write_lost();
#endif
}

static atomic_t replies_dropped;
/* The replies one event sends wait for buffers together at most
 * MG_REPLY_BUDGET_MS (mg_evt_policy.h); 0: no event's budget runs. The
 * budget belongs to the thread handling the event (the app work queue):
 * another thread's sends (the audio thread's change_data_type, the
 * throughput report) keep their own MG_REPLY_EACH_MS. */
static struct k_spinlock budget_lock;
static int64_t reply_deadline;
static k_tid_t reply_owner;

void mg_session_reply_budget(int64_t deadline)
{
	k_spinlock_key_t key = k_spin_lock(&budget_lock);

	reply_deadline = deadline;
	reply_owner = deadline ? k_current_get() : NULL;
	k_spin_unlock(&budget_lock, key);
}

static int64_t my_reply_deadline(void)
{
	k_spinlock_key_t key = k_spin_lock(&budget_lock);
	int64_t d = reply_owner == k_current_get() ? reply_deadline : 0;

	k_spin_unlock(&budget_lock, key);
	return d;
}

int mg_session_send_control(const uint8_t *buf, size_t len)
{
	/* A turn's gestures and stop_mic are never dropped: they
	 * wait until they're queued, or the link is gone. Other replies wait
	 * 500 ms, and a dropped one is counted and logged. */
	bool must = len > 0 && (buf[0] == mg_command_gesture || buf[0] == mg_command_stop_mic);
	int err;

	k_mutex_lock(&tx_lock, K_FOREVER);
	do {
		int32_t wait = must ? MG_REPLY_EACH_MS
				    : mg_reply_wait_ms(k_uptime_get(), my_reply_deadline());

		err = mg_transport_notify(MG_ATT_CONTROL, buf, len, K_MSEC(wait));
	} while (must && err == -ENOMEM);
	k_mutex_unlock(&tx_lock);
	if (err && err != -ENOTCONN) {
		atomic_inc(&replies_dropped);
		LOG_WRN("control reply %u dropped (%d): %ld so far", len ? buf[0] : 0, err,
			(long)atomic_get(&replies_dropped));
	}
	return err;
}

unsigned int mg_session_replies_dropped(void)
{
	return (unsigned int)atomic_get(&replies_dropped) + mg_transport_notify_dropped();
}

void mg_session_count_drop(const char *what)
{
	atomic_inc(&replies_dropped);
	LOG_ERR("%s: cut short (every reply must fit its buffer)", what);
}

int mg_session_send_urgent(const uint8_t *buf, size_t len)
{
	/* No tx_lock: it orders Control with Data, and the audio thread may
	 * hold it while it waits for a buffer. Nothing else on Control
	 * depends on this frame's place. */
	return mg_transport_notify_urgent(MG_ATT_CONTROL, buf, len);
}

int mg_session_try_control(const uint8_t *buf, size_t len)
{
	/* Never waits: not for another sender (the audio thread may hold the
	 * lock while it waits for a buffer), nor for a buffer. */
	if (k_mutex_lock(&tx_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	int err = mg_transport_notify(MG_ATT_CONTROL, buf, len, K_NO_WAIT);

	k_mutex_unlock(&tx_lock);
	return err;
}

int mg_session_send_data(const uint8_t *buf, size_t len, k_timeout_t wait)
{
	k_mutex_lock(&tx_lock, K_FOREVER);
	int err = mg_transport_notify(MG_ATT_DATA, buf, len, wait);

	k_mutex_unlock(&tx_lock);
	return err;
}

size_t mg_session_data_room(void)
{
	uint16_t mtu = mg_transport_att_mtu();

	/* min(ATT_MTU - 3, MG_MAX_ATT_VALUE) (mgcommands.h): the XIAO's MTU
	 * tops out at CONFIG_BT_L2CAP_TX_MTU (247), so the cap never bites. */
#if defined(CONFIG_BT_L2CAP_TX_MTU)
	BUILD_ASSERT(CONFIG_BT_L2CAP_TX_MTU - 3 <= MG_MAX_ATT_VALUE, "an ATT value over 512");
#endif
	return mtu > 3 ? MIN((size_t)mtu - 3, (size_t)MG_MAX_ATT_VALUE) : 0;
}

static void update_ready(void)
{
	/* Ready for gestures and audio: Control and Data both subscribed and
	 * request_status received, in either order (mgcommands.h: a
	 * turn never goes to a client that hasn't read the features). */
	bool ready = authorized() && s.sub[MG_ATT_CONTROL] && s.sub[MG_ATT_DATA] && s.status_seen;

	if (ready != s.ready) {
		s.ready = ready;
		mg_core_set_ready(ready);
	}
}

void mg_session_connected(void)
{
	memset(&s, 0, sizeof(s));
	s.connected = true;
	mg_core_connected();
	update_ready();
}

void mg_session_disconnected(void)
{
	s.connected = false;
	update_ready();
	memset(&s, 0, sizeof(s));
	mg_core_disconnected();
}

void mg_session_status_requested(void)
{
	if (s.connected && !s.status_seen) {
		s.status_seen = true;
		update_ready();
	}
}

void mg_session_subscribed(enum mg_att_chan ch, bool enabled)
{
	if (ch < MG_ATT_CHAN_COUNT) {
		s.sub[ch] = enabled;
		update_ready();
	}
}
