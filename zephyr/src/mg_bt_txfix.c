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

/* A disconnect the Bluetooth host asks for on its own TX processor goes out
 * from the system work queue instead.
 *
 * Zephyr v4.4.2's host (CONFIG_BT_TX_PROCESSOR_THREAD): when an ACL send
 * fails because the controller has already let the link go (a peer that
 * vanished: the controller logs "Invalid Tx Enqueue"), conn.c's
 * tx_processor() calls bt_conn_disconnect() right there, and
 * bt_hci_disconnect() waits in bt_hci_cmd_send_sync() for a command that
 * only that same thread would send. 10 s later the host asserts
 * "Controller unresponsive, command opcode 0x0406" and the gadget restarts
 * (the Muse app killed mid-link; 1 in 7 kill -9 drops of the bench
 * client). The wraps learn the TX processor's thread from its
 * ACL sends (conn.c's calls of bt_send(), the only ones that go through the
 * wrap) and hand a disconnect asked for on that thread to the system work
 * queue, which may wait for a command. The controller then answers it
 * (usually "unknown connection": the link is already gone) and the
 * disconnection proceeds as before. Every other caller is untouched. */

#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>

#include "mg_zephyr_version.h"

MG_ZEPHYR_WRAPS_CHECK(); /* the -Wl,--wrap functions below */

LOG_MODULE_REGISTER(mg_bt_txfix, CONFIG_MG_LOG_LEVEL);

int __real_bt_send(struct net_buf *buf);
int __real_bt_hci_disconnect(uint16_t handle, uint8_t reason);

static k_tid_t volatile tx_thread;

/* One slot per possible link: a disconnect for every handle asked on the TX
 * thread before the work runs goes out. */
#if defined(CONFIG_BT_MAX_CONN)
#define TXFIX_LINKS CONFIG_BT_MAX_CONN
#else
#define TXFIX_LINKS 2 /* the unit test, without Bluetooth */
#endif
static struct k_spinlock q_lock;
static struct {
	bool used;
	uint16_t handle;
	uint8_t reason;
} q[TXFIX_LINKS];

static void disconnect_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	for (;;) {
		uint16_t handle = 0;
		uint8_t reason = 0;
		bool any = false;
		k_spinlock_key_t key = k_spin_lock(&q_lock);

		for (size_t i = 0; i < ARRAY_SIZE(q) && !any; i++) {
			if (q[i].used) {
				handle = q[i].handle;
				reason = q[i].reason;
				q[i].used = false;
				any = true;
			}
		}
		k_spin_unlock(&q_lock, key);
		if (!any) {
			return;
		}
		int err = __real_bt_hci_disconnect(handle, reason);

		LOG_INF("mg.ble: the TX thread's disconnect (handle %u) sent from the system work "
			"queue: %d", handle, err);
	}
}

static K_WORK_DEFINE(disconnect_work, disconnect_fn);

int __wrap_bt_send(struct net_buf *buf)
{
	tx_thread = k_current_get();
	return __real_bt_send(buf);
}

int __wrap_bt_hci_disconnect(uint16_t handle, uint8_t reason)
{
	if (tx_thread == NULL || k_current_get() != tx_thread) {
		return __real_bt_hci_disconnect(handle, reason);
	}
	k_spinlock_key_t key = k_spin_lock(&q_lock);
	size_t slot = ARRAY_SIZE(q);

	for (size_t i = 0; i < ARRAY_SIZE(q); i++) {
		if (q[i].used && q[i].handle == handle) {
			k_spin_unlock(&q_lock, key);
			return 0; /* this link's is already on its way */
		}
		if (!q[i].used && slot == ARRAY_SIZE(q)) {
			slot = i;
		}
	}
	if (slot == ARRAY_SIZE(q)) {
		k_spin_unlock(&q_lock, key);
		return -ENOBUFS; /* more handles than links: can't happen */
	}
	q[slot].used = true;
	q[slot].handle = handle;
	q[slot].reason = reason;
	k_spin_unlock(&q_lock, key);
	LOG_WRN("mg.ble: disconnect asked on the Bluetooth TX thread (handle %u): "
		"moved to the system work queue", handle);
	k_work_submit(&disconnect_work);
	return 0;
}
