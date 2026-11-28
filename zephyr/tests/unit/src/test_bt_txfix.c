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

/* mg_bt_txfix.c with the kernel: a disconnect the host asks for on the thread
 * that sends its ACL data (the TX processor, conn.c's bt_send()) goes out
 * from the system work queue, never inline there (a peer gone mid-send,
 * then bt_hci_disconnect() waiting on its own thread);
 * any other thread's goes out inline as before. */

#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

int __wrap_bt_send(struct net_buf *buf);
int __wrap_bt_hci_disconnect(uint16_t handle, uint8_t reason);

static int real_disc_calls;
static k_tid_t real_disc_thread;
static uint16_t real_disc_handle;
static K_SEM_DEFINE(real_disc_sem, 0, 4);

int __real_bt_send(struct net_buf *buf)
{
	ARG_UNUSED(buf);
	return 0;
}

int __real_bt_hci_disconnect(uint16_t handle, uint8_t reason)
{
	ARG_UNUSED(reason);
	real_disc_calls++;
	real_disc_thread = k_current_get();
	real_disc_handle = handle;
	k_sem_give(&real_disc_sem);
	return -EIO; /* "unknown connection": the link is already gone */
}

#define STACK 2048
static K_THREAD_STACK_DEFINE(tx_stack, STACK);
static K_THREAD_STACK_DEFINE(other_stack, STACK);
static struct k_thread tx_t, other_t;
static int tx_ret = 1, other_ret = 1;
static int calls_seen_inline = -1;

/* The TX processor: an ACL send fails, and the host disconnects right there. */
static void tx_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	(void)__wrap_bt_send(NULL);
	tx_ret = __wrap_bt_hci_disconnect(0x0000, 0x13);
	calls_seen_inline = real_disc_calls; /* nothing sent on this thread */
}

static void other_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	other_ret = __wrap_bt_hci_disconnect(0x0001, 0x13);
}

ZTEST(bt_txfix, test_tx_thread_disconnect_moves_to_sys_workq)
{
	int calls0 = real_disc_calls;

	k_sem_reset(&real_disc_sem);
	/* Cooperative, as the host's TX processor (its work queue's priority). */
	k_thread_create(&tx_t, tx_stack, STACK, tx_fn, NULL, NULL, NULL, K_PRIO_COOP(1), 0,
			K_NO_WAIT);
	k_thread_join(&tx_t, K_SECONDS(1));
	zassert_equal(tx_ret, 0, "the TX thread's disconnect returns at once");
	zassert_equal(calls_seen_inline, calls0, "not sent on the TX thread");
	zassert_ok(k_sem_take(&real_disc_sem, K_SECONDS(2)), "sent later");
	zassert_equal(real_disc_calls, calls0 + 1, "sent once");
	zassert_equal(real_disc_thread, &k_sys_work_q.thread, "from the system work queue");
	zassert_equal(real_disc_handle, 0x0000);

	/* Any other thread: inline, its result passed back, nothing moved. */
	k_thread_create(&other_t, other_stack, STACK, other_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(5),
			0, K_NO_WAIT);
	k_thread_join(&other_t, K_SECONDS(1));
	zassert_equal(other_ret, -EIO, "inline, the controller's answer");
	zassert_equal(real_disc_calls, calls0 + 2);
	zassert_equal(real_disc_thread, &other_t);
	zassert_equal(real_disc_handle, 0x0001);
}

/* Two links' disconnects asked on the TX thread before the work
 * runs both go out. */
static void tx_two_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	(void)__wrap_bt_send(NULL);
	tx_ret = __wrap_bt_hci_disconnect(0x0010, 0x13);
	other_ret = __wrap_bt_hci_disconnect(0x0011, 0x13);
}

ZTEST(bt_txfix, test_two_links_both_sent)
{
	int before = real_disc_calls;

	k_sem_reset(&real_disc_sem);
	k_thread_create(&tx_t, tx_stack, STACK, tx_two_fn, NULL, NULL, NULL, K_PRIO_COOP(1), 0,
			K_NO_WAIT);
	k_thread_join(&tx_t, K_SECONDS(1));
	zassert_equal(tx_ret, 0);
	zassert_equal(other_ret, 0);
	zassert_ok(k_sem_take(&real_disc_sem, K_SECONDS(2)));
	zassert_ok(k_sem_take(&real_disc_sem, K_SECONDS(2)));
	zassert_equal(real_disc_calls, before + 2, "both links' disconnects sent");
}

ZTEST_SUITE(bt_txfix, NULL, NULL, NULL, NULL, NULL);
