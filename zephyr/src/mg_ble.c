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

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include "mg_adv_policy.h"
#include "mg_evt_policy.h"
#include "mg_app.h"
#include "mg_connparam.h"
#include "mg_identity.h"
#include "mg_session.h"
#include "mg_setup.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#include "mgcommands.h"
#include "mg_health.h"
#if defined(CONFIG_MG_DFU)
#include "mg_ota.h"
#endif
#include "mg_tx_progress.h"
#include "mg_reach_policy.h"

#include "mg_zephyr_version.h"

MG_ZEPHYR_WRAPS_CHECK(); /* the -Wl,--wrap functions below */

static void txp_conn_edge(void);
static void tx_dead_give_back(void);
/* The reachability check's completed-ATT-exchange signals (defined with the
 * reach state below). */
static void att_event(enum mg_att_event e, const char *how);
static atomic_t att_peer_req; /* a peer Write Request seen on this link */

/* The TX path's progress (below, with the notification queue). */
#define TXP_SAMPLE_MS 100
static void txp_peer_spoke(void);
static struct k_work_delayable txp_sample_work;

LOG_MODULE_REGISTER(mg_ble, CONFIG_MG_LOG_LEVEL);

#define MAX_ATT 244 /* ATT payload with the largest MTU we accept */

/* ------------------------------------------------------------------------
 * Events from the BT stack, handled in order on the app work queue.
 * ---------------------------------------------------------------------- */

enum evt_type {
	EVT_CCC,
	EVT_WRITE,
};

/* Connects and disconnects aren't queued, so a full queue can't lose
 * one: evt_fn compares the link's generation with the last one it handled
 * first. A queued event of an earlier connection is dropped. */
struct evt {
	uint32_t gen; /* conn_gen when it was posted */
	uint8_t type;
	uint8_t ch;
	uint16_t len;
	uint8_t data[MAX_ATT];
};

/* Eight deep, as before the device session: post() waits up to 100 ms for
 * room, and nothing on the app work queue holds it for long while a session
 * runs (its senders never wait for the notification queue). A Control
 * write still dropped ends the session (control_lost). */
K_MSGQ_DEFINE(evt_q, sizeof(struct evt), 8, 4);
static struct k_work evt_work;
/* Control writes dropped on a full evt_q, not yet reported. A write without
 * response can't be refused, so the client never learns of it: the session
 * would lose those bytes (and their credit) for good. */
static atomic_t control_lost;

/* Since when peer events have been waiting or running without the queue
 * draining (0: drained); mg_transport_evt_saturated(). */
static atomic_t evt_busy_since;
/* When evt_fn last took a peer event (0: never); mg_transport_evt_saturated(). */
static atomic_t evt_last_took;

static void post(uint8_t type, uint8_t ch, const void *data, uint16_t len)
{
	struct evt e = {
		.gen = mg_transport_conn_gen(), .type = type, .ch = ch, .len = MIN(len, MAX_ATT)};
	uint32_t now = k_uptime_get_32();

	(void)atomic_cas(&evt_busy_since, 0, (atomic_val_t)(now ? now : 1));

	if (data) {
		memcpy(e.data, data, e.len);
	}
	if (k_msgq_put(&evt_q, &e, K_MSEC(100)) != 0) {
		LOG_WRN("event queue full, dropped event %u", type);
		if (type != EVT_WRITE || ch != MG_ATT_CONTROL) {
			return;
		}
		atomic_inc(&control_lost);
	}
	k_work_submit_to_queue(mg_app_wq(), &evt_work);
}

static bool link_now(uint32_t *gen);

static uint32_t seen_gen; /* the connection evt_fn last handled */
static bool seen_up;

/* Tells setup and the session about every link change since the last
 * call, in order: the old connection ends before a new one starts. */
static void link_sync(void)
{
	uint32_t gen;
	bool up = link_now(&gen);

	if (gen == seen_gen) {
		return;
	}
	if (seen_up) {
		if (!mg_health_safe()) {
			mg_setup_disconnected();
		}
		mg_session_disconnected();
	}
	seen_gen = gen;
	seen_up = up;
	if (up) {
		if (!mg_health_safe()) {
			mg_setup_connected();
		}
		mg_session_connected();
	}
}

static void evt_fn(struct k_work *w)
{
	static struct evt e;
	int64_t t0 = k_uptime_get();
	unsigned int done = 0;

	ARG_UNUSED(w);
	(void)atomic_cas(&evt_busy_since, 0, (atomic_val_t)((uint32_t)t0 ? (uint32_t)t0 : 1));
	link_sync();
	for (;;) {
		/* At most a few events, or 50 ms, per run: then behind whatever
		 * else waits on the app work queue (the watchdog's ping, the
		 * healthy mark; mg_evt_policy.h). */
		if (!mg_evt_run_more(done, k_uptime_get() - t0)) {
			k_work_submit_to_queue(mg_app_wq(), &evt_work);
			return;
		}
		if (k_msgq_get(&evt_q, &e, K_NO_WAIT) != 0) {
			break;
		}
		{
			uint32_t took = k_uptime_get_32();

			atomic_set(&evt_last_took, (atomic_val_t)(took ? took : 1));
		}
		link_sync();
		if (e.gen != seen_gen) {
			continue; /* an earlier connection's */
		}
		done++;
		mg_session_reply_budget(k_uptime_get() + MG_REPLY_BUDGET_MS);
		switch (e.type) {
		case EVT_CCC:
			if (e.ch != MG_ATT_SETUP) {
				mg_session_subscribed(e.ch, e.data[0]);
			}
			break;
		case EVT_WRITE:
			mg_connparam_busy(MG_XFER_WRITE); /* any GATT write */
			if (e.ch == MG_ATT_SETUP) {
				if (!mg_health_safe()) { /* no Link setup in SAFE mode */
					mg_setup_rx(e.data, e.len);
				}
			} else {
				mg_session_rx(e.ch, e.data, e.len);
			}
			break;
		}
		mg_session_reply_budget(0);
	}
	link_sync();
	atomic_set(&evt_busy_since, 0);
	if (atomic_set(&control_lost, 0)) {
		mg_session_control_lost();
	}
}

/* ------------------------------------------------------------------------
 * GATT
 * ---------------------------------------------------------------------- */

static const struct bt_uuid_128 mg_svc_uuid = BT_UUID_INIT_128(MG_SERVICE_UUID_LE_BYTES);
static const struct bt_uuid_128 mg_ctrl_uuid = BT_UUID_INIT_128(MG_CONTROL_UUID_LE_BYTES);
static const struct bt_uuid_128 mg_data_uuid = BT_UUID_INIT_128(MG_DATA_UUID_LE_BYTES);

static ssize_t on_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	txp_peer_spoke(); /* the peer is alive (the TX clock's 10 s case) */
	if (!(flags & BT_GATT_WRITE_FLAG_CMD)) {
		/* A Write Request: ATT allows one at a time, so the previous
		 * one's response got through (TX progress); our callback
		 * answering it is a completed exchange on the gadget's own
		 * path once the next one comes (the reachability check). */
		mg_transport_rx_request();
		att_event(atomic_set(&att_peer_req, 1) ? MG_ATT_PEER_NEXT_REQUEST
						       : MG_ATT_PEER_FIRST_REQUEST,
			  "a second peer Write Request (our answer arrived)");
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len > MAX_ATT) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	post(EVT_WRITE, (uint8_t)(uintptr_t)attr->user_data, buf, len);
	return len;
}

#define CCC_HANDLER(name, chan)                                                                    \
	static void name(const struct bt_gatt_attr *attr, uint16_t value)                          \
	{                                                                                          \
		uint8_t on = value == BT_GATT_CCC_NOTIFY;                                          \
		ARG_UNUSED(attr);                                                                  \
		post(EVT_CCC, chan, &on, 1);                                                       \
	}

CCC_HANDLER(ccc_ctrl, MG_ATT_CONTROL)
CCC_HANDLER(ccc_data, MG_ATT_DATA)
CCC_HANDLER(ccc_setup, MG_ATT_SETUP)

#define MG_CHRC(u, chan, ccc)                                                                      \
	BT_GATT_CHARACTERISTIC(&(u).uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY, \
			       BT_GATT_PERM_WRITE, NULL, on_write, (void *)(uintptr_t)(chan)),      \
		BT_GATT_CCC(ccc, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)

BT_GATT_SERVICE_DEFINE(mg_svc,
	BT_GATT_PRIMARY_SERVICE(&mg_svc_uuid.uuid),
	MG_CHRC(mg_ctrl_uuid, MG_ATT_CONTROL, ccc_ctrl),
	MG_CHRC(mg_data_uuid, MG_ATT_DATA, ccc_data),
);

/* Muse Link setup (mg_setup.h): RX takes the app's (chunked) JSON, TX
 * notifies the replies and reads back the last plaintext status. */
static const struct bt_uuid_128 setup_svc_uuid = BT_UUID_INIT_128(MG_SETUP_SERVICE_UUID_LE);
static const struct bt_uuid_128 setup_rx_uuid = BT_UUID_INIT_128(MG_SETUP_RX_UUID_LE);
static const struct bt_uuid_128 setup_tx_uuid = BT_UUID_INIT_128(MG_SETUP_TX_UUID_LE);

static ssize_t setup_tx_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			     uint16_t len, uint16_t offset)
{
	char st[64];
	size_t n = mg_setup_read_status(st, sizeof(st));

	return bt_gatt_attr_read(conn, attr, buf, len, offset, st, n);
}

BT_GATT_SERVICE_DEFINE(setup_svc,
	BT_GATT_PRIMARY_SERVICE(&setup_svc_uuid.uuid),
	BT_GATT_CHARACTERISTIC(&setup_rx_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, on_write, (void *)(uintptr_t)MG_ATT_SETUP),
	BT_GATT_CHARACTERISTIC(&setup_tx_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, setup_tx_read, NULL, NULL),
	BT_GATT_CCC(ccc_setup, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* Value attribute of each notifying characteristic: in the mg service,
 * service then (decl, value, CCC) per characteristic; in the setup service,
 * service, RX (decl, value), TX (decl, value, CCC). */
static const struct bt_gatt_attr *value_attr(enum mg_att_chan ch)
{
	return ch == MG_ATT_SETUP ? &setup_svc.attrs[4] : &mg_svc.attrs[2 + 3 * ch];
}

/* ------------------------------------------------------------------------
 * Connection
 * ---------------------------------------------------------------------- */

/* The reachability check's plain state: connectable advertising running, a link up. */
static atomic_t adv_on;
static atomic_t link_on;

/* Peer events backed up with a link up for half the watchdog's staleness:
 * a stale app work queue is the peer's doing (mg_evt_policy.h). */
bool mg_transport_evt_saturated(uint32_t stale_ms)
{
	return mg_evt_saturated(atomic_get(&link_on) != 0,
				(int64_t)(uint32_t)atomic_get(&evt_busy_since),
				(int64_t)(uint32_t)atomic_get(&evt_last_took),
				(int64_t)(uint32_t)k_uptime_get_32(), stale_ms);
}

/* A link is reachability only once an ATT exchange completed on it through
 * the gadget's own path (mg_reach_policy.h, mg_reach_att_counts()). Per link:
 * att_ok once one did; the gadget's own requests (the MTU exchange, the
 * central's name read) pending or failed. */
static atomic_t att_ok;
static atomic_t att_gated; /* att_ok on a link past the gate: it moves recovery state */
static atomic_t att_own_failed;
static atomic_t att_own_pending;
static atomic_t att_dead_test;  /* the bench row's stand-in (mg_ble_att_dead_test()) */
static int64_t att_asked_ms;

/* The link's ATT works and it passed the gate (mg_reach_exchange_counts()):
 * now, or at this connection's proof (mg_transport_gate_passed()). */
static void att_gate_maybe(void)
{
	if (!mg_reach_exchange_counts(atomic_get(&att_ok) != 0, mg_setup_store_complete(),
				      mg_token_proof_matched()) ||
	    atomic_set(&att_gated, 1)) {
		return;
	}
	LOG_DBG("mg.ble: ATT works on a link past the gate");
	mg_health_att_exchange(); /* clears the dead-link run (not the restart bound) */
#if defined(CONFIG_MG_DFU)
	mg_ota_link_proven(); /* an updated image may now keep itself */
#endif
}

static void att_event(enum mg_att_event e, const char *how)
{
	if (!mg_reach_att_counts(e) || atomic_get(&att_dead_test)) {
		return; /* the stack's own answer, or the bench row's stand-in */
	}
	if (!atomic_set(&att_ok, 1)) {
		LOG_DBG("mg.ble: ATT works on this link (%s)", how);
	}
	att_gate_maybe();
}

void mg_transport_gate_passed(void)
{
	att_gate_maybe();
}

/* A board's own update path live on this link (weak, none): a board whose
 * update goes through its own loader, with its own prompt. */
__weak bool mg_board_update_live(void)
{
	return false;
}

bool mg_transport_reachable(void)
{
	bool update_live = atomic_get(&link_on) && mg_board_update_live();

#if defined(CONFIG_MG_DFU)
	update_live = update_live || (atomic_get(&link_on) && mg_ota_update_live());
#endif
	return atomic_get(&adv_on) ||
	       (atomic_get(&link_on) &&
		mg_reach_link_counts(atomic_get(&att_gated) != 0, update_live));
}

bool mg_transport_link_up(void)
{
	return atomic_get(&link_on) != 0;
}

/* A connection edge since each reader's last call, one bit per reader: the
 * reachability sampler's, so a link wholly between two samples still marks
 * the stretch as one a peer could cause, and the trial clock's, so a link
 * wholly inside one tick still pauses it. */
static atomic_t link_seen;

bool mg_transport_link_seen_take(unsigned int who)
{
	return atomic_test_and_clear_bit(&link_seen, who);
}

bool mg_transport_att_ok(void)
{
	return atomic_get(&link_on) && atomic_get(&att_gated);
}

/* The bench test for ATT-dead links (bench builds only): from now until the
 * next boot (RAM only) every link counts as one where no ATT exchange
 * completes and the gadget's own requests fail, and is dropped 6 s after it
 * forms, so three links in a row restart the gadget as a peer loss. It stands
 * in at the signals the check reads (the host stack's ATT still runs). */
static void att_dead_drop_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	mg_transport_disconnect();
}
static K_WORK_DELAYABLE_DEFINE(att_dead_drop_work, att_dead_drop_fn);

int mg_ble_att_dead_test(void)
{
	if (!IS_ENABLED(CONFIG_MG_AUTOMATION) && !IS_ENABLED(CONFIG_MG_BENCH_MODE)) {
		return -EPERM; /* a bench word: refused in release builds */
	}
	if (!IS_ENABLED(CONFIG_MG_REACH_RESET)) {
		return -ENOTSUP;
	}
	atomic_set(&att_dead_test, 1);
	atomic_clear(&att_ok);
	atomic_clear(&att_gated);
	LOG_WRN("att dead test: every link until the next boot counts as ATT-dead and drops "
		"after 6 s; %d in a row restart the gadget", MG_REACH_DEAD_LINKS);
	k_work_reschedule(&att_dead_drop_work, K_MSEC(200));
	return 0;
}

static struct bt_conn *cur_conn;
/* Counts connections and disconnections (under conn_lock): a notification
 * queued for one connection never goes out on the next. */
static uint32_t conn_gen;
static K_MUTEX_DEFINE(conn_lock);
static struct k_work adv_work;
static struct bt_gatt_exchange_params mtu_params;

/* The 2M PHY asked once more if no PHY update came within 3 s of the
 * connection (on a Galaxy S22 link the gadget's request collided and
 * the link stayed on 1M, ~24 kB/s against the iPhone's ~94). */
#define PHY_RETRY_MS 3000
static bool phy_seen;
static uint32_t phy_gen;
static struct k_work_delayable phy_retry_work;

static void phy_retry_fn(struct k_work *w)
{
	struct bt_conn *conn = NULL;

	ARG_UNUSED(w);
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn && phy_gen == conn_gen && !phy_seen) {
		conn = bt_conn_ref(cur_conn);
	}
	k_mutex_unlock(&conn_lock);
	if (conn) {
		int e = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);

		LOG_INF("mg.ble: no PHY update yet: 2M asked again%s", e ? " (refused)" : "");
		bt_conn_unref(conn);
	}
}

/* The connected device's own name, read once per
 * connection from its GAP Device Name (0x2A00) after the MTU exchange; the
 * client's client_name setting is only the fallback. Printable ASCII kept,
 * anything else '?'; empty until read (or when the central has none). */
#define PEER_NAME_MAX 32
static char peer_name[PEER_NAME_MAX + 1];
static uint32_t peer_name_gen;
static struct bt_gatt_read_params name_params;

/* The host reports a request it ended itself as BT_ATT_ERR_UNLIKELY: ATT's
 * 30 s timeout on a usable link (our request failed), or the link coming
 * down (ATT cancels what's outstanding before the app's disconnected
 * callback): that one isn't a failure, and MG_REACH_ATT_WAIT_MS decides as
 * for a request still pending. */
static bool host_ended_on_usable_link(struct bt_conn *conn)
{
	struct bt_conn_info info;

	return bt_conn_get_info(conn, &info) == 0 && info.state == BT_CONN_STATE_CONNECTED;
}

static uint8_t name_read_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *p,
			    const void *data, uint16_t len)
{
	ARG_UNUSED(p);
	if (err == BT_ATT_ERR_UNLIKELY && !host_ended_on_usable_link(conn)) {
		return BT_GATT_ITER_STOP; /* the link's end: still pending */
	}
	atomic_clear(&att_own_pending);
	if (err || data == NULL) {
		if (err == BT_ATT_ERR_UNLIKELY) {
			/* ATT's timeout on a usable link: our request failed. */
			atomic_set(&att_own_failed, 1);
		} else if (err) {
			/* The peer's ATT error response is still an answer. */
			att_event(MG_ATT_OWN_ANSWERED, "the name read answered with an error");
		}
		if (err) {
			LOG_INF("mg.ble: the central's device name isn't readable (0x%02x)", err);
		}
		return BT_GATT_ITER_STOP;
	}
	att_event(MG_ATT_OWN_ANSWERED, "the name read answered");
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (peer_name_gen == conn_gen) {
		size_t n = MIN(len, PEER_NAME_MAX);
		const uint8_t *b = data;

		for (size_t i = 0; i < n; i++) {
			peer_name[i] = b[i] >= 0x20 && b[i] < 0x7f ? (char)b[i] : '?';
		}
		peer_name[n] = '\0';
	}
	k_mutex_unlock(&conn_lock);
	/* Logged by length only: the phone's name is the user's, and this log
	 * is mirrored on the Nordic UART. */
	LOG_INF("mg.ble: the central's device name read (%u B)", len);
	return BT_GATT_ITER_STOP;
}

static void read_peer_name(struct bt_conn *conn)
{
	static const struct bt_uuid_16 uuid = BT_UUID_INIT_16(BT_UUID_GAP_DEVICE_NAME_VAL);

	name_params = (struct bt_gatt_read_params){
		.func = name_read_cb,
		.handle_count = 0,
		.by_uuid.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE,
		.by_uuid.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE,
		.by_uuid.uuid = &uuid.uuid,
	};
	if (bt_gatt_read(conn, &name_params)) {
		/* Nothing asked: nothing pending either. */
		atomic_clear(&att_own_pending);
		LOG_INF("mg.ble: the central's device name not asked");
	}
}

size_t mg_transport_peer_name(char *out, size_t cap)
{
	size_t n;

	k_mutex_lock(&conn_lock, K_FOREVER);
	n = cur_conn && peer_name_gen == conn_gen ? strlen(peer_name) : 0;
	n = MIN(n, cap ? cap - 1 : 0);
	memcpy(out, peer_name, n);
	if (cap) {
		out[n] = '\0';
	}
	k_mutex_unlock(&conn_lock);
	return n;
}

static void mtu_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *p)
{
	ARG_UNUSED(p);
	LOG_INF("ATT MTU %u%s", bt_gatt_get_mtu(conn), err ? " (exchange failed)" : "");
	if (err == BT_ATT_ERR_UNLIKELY) {
		if (host_ended_on_usable_link(conn)) {
			/* ATT's timeout on a usable link: our request failed */
			atomic_set(&att_own_failed, 1);
			atomic_clear(&att_own_pending);
		} /* else the link's end: still pending (host_ended_on_usable_link()) */
		return;
	}
	/* Answered, by an MTU response or the peer's ATT error. */
	att_event(MG_ATT_OWN_ANSWERED, "our MTU exchange answered");
	atomic_set(&att_own_pending, 1); /* the name read, next */
	att_asked_ms = k_uptime_get();
	read_peer_name(conn);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connection failed (0x%02x)", err);
		k_work_submit(&adv_work);
		return;
	}
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		k_mutex_unlock(&conn_lock);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
		return;
	}
	mg_transport_peer_wrote(); /* the link's start counts as a write */
	cur_conn = bt_conn_ref(conn);
	conn_gen++;
	atomic_clear(&att_ok);
	atomic_clear(&att_gated);
	atomic_clear(&att_own_failed);
	atomic_clear(&att_peer_req);
	atomic_set(&att_own_pending, 1); /* the MTU exchange, below */
	att_asked_ms = k_uptime_get();
	txp_conn_edge(); /* nothing owed from an earlier link */
	atomic_set(&link_seen, BIT(MG_LINK_SEEN_REACH) | BIT(MG_LINK_SEEN_TRIAL));
	atomic_set(&link_on, 1);
	if (atomic_get(&att_dead_test)) {
		k_work_reschedule(&att_dead_drop_work, K_SECONDS(6));
	}
	atomic_set(&adv_on, 0); /* a connection ends legacy advertising */
	peer_name[0] = '\0';
	peer_name_gen = conn_gen;
	k_mutex_unlock(&conn_lock);
	LOG_INF("connected");

	/* Ask for what audio needs; the client may settle on less: the largest
	 * MTU, 251-byte link-layer PDUs and the 2M PHY, then (mg_connparam.c)
	 * the connection interval and latency. */
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) == 0) {
		mg_connparam_connected(info.le.interval_us / 1250U, info.le.latency,
				       info.le.timeout);
	} else {
		mg_connparam_connected(0, 0, 0);
	}
	mtu_params.func = mtu_cb;
	if (bt_gatt_exchange_mtu(conn, &mtu_params)) {
		/* The central exchanged first (-EALREADY): the host answered
		 * it, which proves nothing about our path; our own
		 * request is the name read. */
		read_peer_name(conn);
	}
	/* A central running its own update at the same time makes the
	 * controller refuse ours; the result still comes to the callbacks
	 * below. */
	int e = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);

	if (e) {
		LOG_INF("mg.ble: data length update already under way (%d)", e);
	}
	e = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
	if (e) {
		LOG_INF("mg.ble: 2M PHY update already under way (%d)", e);
	}
	k_mutex_lock(&conn_lock, K_FOREVER);
	phy_seen = false;
	phy_gen = conn_gen;
	k_mutex_unlock(&conn_lock);
	k_work_reschedule(&phy_retry_work, K_MSEC(PHY_RETRY_MS));
	k_work_reschedule(&txp_sample_work, K_MSEC(TXP_SAMPLE_MS));
	k_work_submit_to_queue(mg_app_wq(), &evt_work);
}

static void tx_flush(void);

static int64_t fast_until; /* advertise fast until (mg_ble_start_advertising) */

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (conn != cur_conn) {
		k_mutex_unlock(&conn_lock);
		return;
	}
	bt_conn_unref(cur_conn);
	cur_conn = NULL;
	conn_gen++;
	atomic_set(&link_on, 0);
	atomic_or(&link_seen, BIT(MG_LINK_SEEN_TRIAL)); /* the trial tick it ends in pauses too */
	txp_conn_edge();
	tx_dead_give_back(); /* under conn_lock, as the bench check's take */
	k_mutex_unlock(&conn_lock);
	LOG_INF("disconnected (0x%02x)", reason);
	{
		/* Only a link past the gate clears the run; a stranger's link
		 * whose ATT works neither clears it nor counts as dead. */
		bool ok = atomic_get(&att_gated) != 0;
		bool failed = atomic_get(&att_dead_test) ||
			      (!atomic_get(&att_ok) &&
			       mg_reach_own_failed(atomic_get(&att_own_failed) != 0,
						   atomic_get(&att_own_pending) != 0, att_asked_ms,
						   k_uptime_get()));

		if (mg_health_link_ended(ok, failed)) {
			LOG_ERR("mg.ble: %d links in a row ended with ATT dead",
				MG_REACH_DEAD_LINKS);
		} else if (!ok && failed) {
			LOG_WRN("mg.ble: a link ended with ATT dead (%u in a row)",
				mg_health_att_dead_run());
		}
		atomic_clear(&att_ok);
		atomic_clear(&att_gated);
	}
	/* Fast for CONFIG_MG_ADV_FAST_SECS from the link's loss. */
	fast_until = k_uptime_get() + CONFIG_MG_ADV_FAST_SECS * 1000LL;
	mg_connparam_disconnected();
	tx_flush();
	k_work_submit_to_queue(mg_app_wq(), &evt_work);
}

static void recycled(void)
{
	k_work_submit(&adv_work);
}

static const char *phy_name(uint8_t phy)
{
	return phy == BT_GAP_LE_PHY_2M ? "2M" : phy == BT_GAP_LE_PHY_1M ? "1M" : "coded";
}

static void phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *p)
{
	phy_seen = true;
	ARG_UNUSED(conn);
	LOG_INF("mg.ble: phy tx %s rx %s", phy_name(p->tx_phy), phy_name(p->rx_phy));
	mg_connparam_phy_done();
}

static void data_len_updated(struct bt_conn *conn, struct bt_conn_le_data_len_info *d)
{
	ARG_UNUSED(conn);
	LOG_INF("mg.ble: data length tx %u B / %u us, rx %u B / %u us", d->tx_max_len,
		d->tx_max_time, d->rx_max_len, d->rx_max_time);
	mg_connparam_data_len_done();
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	ARG_UNUSED(conn);
	uint32_t us = interval * 1250U;

	LOG_INF("mg.ble: conn %u.%02u ms latency %u timeout %u ms", us / 1000, (us % 1000) / 10,
		latency, timeout * 10U);
	mg_connparam_updated(interval, latency, timeout);
}

static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *p)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(p);
	return true; /* the central decides; ours follow (mg_connparam.c) */
}

#if defined(CONFIG_BT_SMP)
static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	ARG_UNUSED(conn);
	if (err) {
		LOG_WRN("mg.ble: encryption failed (%d)", err);
	} else {
		LOG_INF("mg.ble: link security level %d", level);
	}
}
#endif

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.le_param_req = le_param_req,
	.le_param_updated = le_param_updated,
	.le_phy_updated = phy_updated,
	.le_data_len_updated = data_len_updated,
#if defined(CONFIG_BT_SMP)
	.security_changed = security_changed,
#endif
};

int mg_transport_conn_params(uint16_t interval_min, uint16_t interval_max, uint16_t latency,
			     uint16_t timeout)
{
	const struct bt_le_conn_param p = {
		.interval_min = interval_min,
		.interval_max = interval_max,
		.latency = latency,
		.timeout = timeout,
	};
	int err = -ENOTCONN;

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		err = bt_conn_le_param_update(cur_conn, &p);
	}
	k_mutex_unlock(&conn_lock);
	return err;
}

#if defined(CONFIG_BT_LL_SW_SPLIT)
/* Zephyr's open controller (ll_sw): the call its own TX path makes to hear
 * the next connection event despite peripheral latency
 * (ull_peripheral.c). Opaque here. */
struct ll_conn;
struct ll_conn *ll_connected_get(uint16_t handle);
void ull_periph_latency_cancel(struct ll_conn *conn, uint16_t handle);
#endif

#if defined(CONFIG_BT_LL_SOFTDEVICE)
/* The nRF Connect SDK's SoftDevice Controller (a board built with NCS)
 * has no one-event cancel, but a vendor command that sets the peripheral's
 * latency mode per connection: DISABLE wakes on every connection event,
 * ENABLE skips again. A cancel disables it and holds it for three intervals
 * (at least 100 ms); each further cancel (mg_connparam's listen, twice an
 * interval) pushes that on, and when they stop latency is enabled again. */
#include <bluetooth/hci_vs_sdc.h>

static bool lat_off;
static uint16_t lat_handle;
static uint32_t lat_conn_gen;
static struct k_work_delayable lat_restore_work;
static bool lat_work_ready;

static int lat_mode(uint16_t handle, uint8_t mode)
{
	const sdc_hci_cmd_vs_peripheral_latency_mode_set_t p = {
		.conn_handle = handle,
		.mode = mode,
	};

	return hci_vs_sdc_peripheral_latency_mode_set(&p);
}

static void lat_restore_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (lat_off && cur_conn && conn_gen == lat_conn_gen) {
		int err = lat_mode(lat_handle, SDC_HCI_VS_PERIPHERAL_LATENCY_MODE_ENABLE);

		if (err) {
			LOG_WRN("mg.ble: peripheral latency back on: %d", err);
		}
	}
	lat_off = false;
	k_mutex_unlock(&conn_lock);
}
#endif

int mg_transport_latency_cancel(void)
{
#if defined(CONFIG_BT_LL_SOFTDEVICE)
	uint16_t handle;
	struct bt_conn_info info;
	int err = -ENOTCONN;

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (!lat_work_ready) {
		k_work_init_delayable(&lat_restore_work, lat_restore_fn);
		lat_work_ready = true;
	}
	if (cur_conn && bt_hci_get_conn_handle(cur_conn, &handle) == 0 &&
	    bt_conn_get_info(cur_conn, &info) == 0) {
		err = 0;
		if (!lat_off || lat_conn_gen != conn_gen || lat_handle != handle) {
			err = lat_mode(handle, SDC_HCI_VS_PERIPHERAL_LATENCY_MODE_DISABLE);
			if (!err) {
				lat_off = true;
				lat_handle = handle;
				lat_conn_gen = conn_gen;
			}
		}
		if (!err) {
			uint32_t itvl_ms = info.le.interval * 5U / 4U;

			k_work_reschedule_for_queue(mg_app_wq(), &lat_restore_work,
						    K_MSEC(MAX(3U * itvl_ms, 100U)));
		}
	}
	k_mutex_unlock(&conn_lock);
	return err;
#elif defined(CONFIG_BT_LL_SW_SPLIT)
	uint16_t handle;
	int err = -ENOTCONN;

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn && bt_hci_get_conn_handle(cur_conn, &handle) == 0) {
		/* The controller's thread-context ticker calls come from the
		 * (cooperative) Bluetooth TX thread too: nothing may preempt
		 * this one. */
		k_sched_lock();
		struct ll_conn *lc = ll_connected_get(handle);

		if (lc) {
			ull_periph_latency_cancel(lc, handle);
			err = 0;
		}
		k_sched_unlock();
	}
	k_mutex_unlock(&conn_lock);
	return err;
#else
	return -ENOTSUP;
#endif
}

struct bt_conn *mg_transport_conn_ref(void)
{
	struct bt_conn *c;

	k_mutex_lock(&conn_lock, K_FOREVER);
	c = cur_conn ? bt_conn_ref(cur_conn) : NULL;
	k_mutex_unlock(&conn_lock);
	return c;
}

uint16_t mg_transport_att_mtu(void)
{
	uint16_t mtu = 23;

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		mtu = bt_gatt_get_mtu(cur_conn);
	}
	k_mutex_unlock(&conn_lock);
	return MIN(mtu, MAX_ATT + 3);
}

static bool link_now(uint32_t *gen)
{
	k_mutex_lock(&conn_lock, K_FOREVER);
	bool up = cur_conn != NULL;

	*gen = conn_gen;
	k_mutex_unlock(&conn_lock);
	return up;
}

uint32_t mg_transport_conn_gen(void)
{
	k_mutex_lock(&conn_lock, K_FOREVER);
	uint32_t gen = conn_gen;

	k_mutex_unlock(&conn_lock);
	return gen;
}

bool mg_transport_encrypted(void)
{
	bool enc = false;

#if defined(CONFIG_BT_SMP)
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		enc = bt_conn_get_security(cur_conn) >= BT_SECURITY_L2;
	}
	k_mutex_unlock(&conn_lock);
#endif
	return enc;
}

void mg_transport_disconnect(void)
{
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		(void)bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		/* The link is going: its TX debt with it, so the TX check doesn't
		 * fire again each second until disconnected(). */
		txp_conn_edge();
	}
	k_mutex_unlock(&conn_lock);
}

/* ------------------------------------------------------------------------
 * Notification queue: one FIFO for every characteristic keeps Control and
 * Data in order. A TX thread feeds the stack with a bounded number in
 * flight.
 * ---------------------------------------------------------------------- */

#define TX_BUFS     24
/* Notifications handed to the stack at once: enough to fill a 15 ms
 * connection event at 2M PHY (about ten 251-byte PDUs), within
 * CONFIG_BT_CONN_TX_MAX and CONFIG_BT_BUF_ACL_TX_COUNT. */
#define TX_INFLIGHT 10

struct tx_item {
	void *fifo_reserved;
	uint32_t gen; /* conn_gen when it was queued */
	uint8_t ch;
	uint16_t len;
	uint8_t data[MAX_ATT];
};

K_MEM_SLAB_DEFINE_STATIC(tx_slab, sizeof(struct tx_item), TX_BUFS, 4);
static K_FIFO_DEFINE(tx_fifo);
/* Held back for an urgent notification when the slab is empty. */
static struct tx_item urgent_item;

bool mg_transport_tx_pending(void)
{
	return !k_fifo_is_empty(&tx_fifo);
}
static atomic_t urgent_busy;

static void tx_free(struct tx_item *it)
{
	if (it == &urgent_item) {
		atomic_clear(&urgent_busy);
	} else {
		k_mem_slab_free(&tx_slab, it);
	}
}
/* Slots for notifications handed to the stack. They belong to one
 * connection (inflight_gen): a completion from an earlier one gives nothing
 * back, and the slots are refilled only when the next connection's first
 * notification goes, never on a live link (the stack's ATT buffers are
 * shared, and taking one blocks the thread that asks). */
static K_SEM_DEFINE(inflight, TX_INFLIGHT, TX_INFLIGHT);
static atomic_t inflight_gen;
/* Ours in flight never exhaust the ATT pool: bt_gatt_notify_cb() from this
 * thread waits for an ATT buffer with no timeout, so with the pool empty on
 * a stalled link it would block for good and the watchdog would reset the
 * gadget. One buffer stays for the Nordic UART mirror and one for an SMP or
 * GATT response. */
BUILD_ASSERT(TX_INFLIGHT + 2 <= CONFIG_BT_ATT_TX_COUNT, "TX_INFLIGHT within the ATT pool");
/* The recovery tier's answers (SMP over GATT) and the urgent token-proof
 * answer share the host's fixed TX pools with mg's notifications: mg's own
 * never take the last two of any (a way back never shares a fixed table
 * that optional entries can fill first). */
BUILD_ASSERT(TX_INFLIGHT + 2 <= CONFIG_BT_CONN_TX_MAX, "TX_INFLIGHT within the TX contexts");
BUILD_ASSERT(TX_INFLIGHT + 2 <= CONFIG_BT_L2CAP_TX_BUF_COUNT, "TX_INFLIGHT within the L2CAP pool");
/* A link that completes nothing for this long with every slot taken is
 * stalled: queued notifications are dropped (counted) until one completes,
 * and the link stays up. */
#define TX_STALL_MS 2000
static atomic_t tx_stalled;
static atomic_t notify_dropped;

bool mg_transport_tx_stalled(void)
{
	return atomic_get(&tx_stalled) != 0;
}

/* The TX path is progress (src/mg_tx_progress.h). Notifications:
 * ours in flight or waiting for a buffer, against their sent callbacks. ACL
 * credits: packets the controller holds (the connection's credit semaphore
 * below its limit), sampled every 100 ms, against credits coming back; a
 * notification completing, a Write Request (ATT: one at a time) or an SMP
 * request (a client sends more only once answered) is progress there too.
 * bt_conn_get_pkts() is the Zephyr host's own (conn_internal.h), the same
 * with the open controller and the SoftDevice Controller. */
struct k_sem *bt_conn_get_pkts(struct bt_conn *conn);
static struct k_spinlock txp_lock;
static struct mg_txp txp_notes;
static struct mg_txp txp_acl;
static unsigned int acl_last_done;
/* Packets the controller reported completed (Number of Completed Packets),
 * any connection, any sender (mg, the Nordic UART, ATT responses, SMP): the
 * host calls bt_conn_tx_notify(conn, false) once per completed packet from
 * hci_num_completed_packets() (its other callers pass true). Wrapped at link
 * time (CMakeLists.txt), the same on the open controller and the SoftDevice
 * Controller. */
static atomic_t acl_completions;

/* No OS bond unless a feature requires one: the Muse link authenticates by
 * the token proof and the sealed session, so nothing here needs a bond, and a
 * stranger's pairing or an unbonded peer's CCC write must store nothing. With
 * CONFIG_BT_SMP=n (prj.conf) there is no pairing at all; a board that turns
 * SMP on keeps bonding off. A feature that needs a bond (BLE encryption,
 * ANCS, AMS, HID, classic key exchange) brings its own option with it (LE
 * Secure Connections only, bondable only inside setup or an owner action);
 * none exists yet. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_SMP) || !IS_ENABLED(CONFIG_BT_BONDABLE),
	     "Muse Gadgets store no OS bonds: CONFIG_BT_BONDABLE=n with CONFIG_BT_SMP");
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_SETTINGS),
	     "no Bluetooth settings storage (bonds, CCC, the GATT hash): CONFIG_BT_SETTINGS=n");
void __real_bt_conn_tx_notify(struct bt_conn *conn, bool wait_for_completion);
void __wrap_bt_conn_tx_notify(struct bt_conn *conn, bool wait_for_completion)
{
	if (!wait_for_completion) {
		atomic_inc(&acl_completions);
	}
	__real_bt_conn_tx_notify(conn, wait_for_completion);
}
static atomic_t tx_dead_logged;
static atomic_t last_rx_ms;        /* the peer's latest write or SMP request */
static atomic_t sup_timeout_ms = ATOMIC_INIT(4000); /* the link's, from bt_conn_get_info() */

/* A link's start or end: its TX-path debt goes with it (the controller's
 * credits come back at the disconnect; sent callbacks of notifications in
 * flight never do), so a new link starts with nothing owed. */
static void txp_conn_edge(void)
{
	k_spinlock_key_t key = k_spin_lock(&txp_lock);

	mg_txp_idle(&txp_notes);
	mg_txp_idle(&txp_acl);
	acl_last_done = (unsigned int)atomic_get(&acl_completions);
	k_spin_unlock(&txp_lock, key);
	atomic_clear(&tx_dead_logged);
}

static void txp_owe_note(void)
{
	k_spinlock_key_t key = k_spin_lock(&txp_lock);

	mg_txp_owe(&txp_notes, k_uptime_get_32());
	k_spin_unlock(&txp_lock, key);
}

void mg_transport_rx_request(void)
{
	uint32_t now = k_uptime_get_32();

	mg_transport_peer_wrote();
	k_spinlock_key_t key = k_spin_lock(&txp_lock);

	mg_txp_done(&txp_acl, now, txp_acl.owed);
	k_spin_unlock(&txp_lock, key);
	atomic_set(&last_rx_ms, (atomic_val_t)now);
}

static atomic_t peer_write_ms;

void mg_transport_peer_wrote(void)
{
	atomic_set(&peer_write_ms, (atomic_val_t)k_uptime_get_32());
}

uint32_t mg_transport_peer_write_ms(void)
{
	return (uint32_t)atomic_get(&peer_write_ms);
}

static void txp_peer_spoke(void)
{
	atomic_set(&last_rx_ms, (atomic_val_t)k_uptime_get_32());
	mg_transport_peer_wrote();
}

static void txp_sample_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(txp_sample_work, txp_sample_fn);

static void txp_sample_fn(struct k_work *w)
{
	struct bt_conn *conn = NULL;

	ARG_UNUSED(w);
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		conn = bt_conn_ref(cur_conn);
	}
	k_mutex_unlock(&conn_lock);
	if (!conn) {
		return; /* restarted at the next connection */
	}
	struct k_sem *pkts = bt_conn_get_pkts(conn);
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) == 0 && info.le.timeout) {
		atomic_set(&sup_timeout_ms, (atomic_val_t)info.le.timeout * 10);
	}
	if (pkts) {
		unsigned int held = pkts->limit - k_sem_count_get(pkts);
		unsigned int done = (unsigned int)atomic_get(&acl_completions);
		k_spinlock_key_t key = k_spin_lock(&txp_lock);

		mg_txp_acl(&txp_acl, k_uptime_get_32(), held, done, &acl_last_done);
		k_spin_unlock(&txp_lock, key);
	}
	bt_conn_unref(conn);
	k_work_reschedule(&txp_sample_work, K_MSEC(TXP_SAMPLE_MS));
}

bool mg_transport_tx_dead(void)
{
	uint32_t now = k_uptime_get_32();
	bool link = atomic_get(&link_on) != 0;
	uint32_t rx = (uint32_t)atomic_get(&last_rx_ms);
	uint32_t sup = (uint32_t)atomic_get(&sup_timeout_ms);
	bool notes, acl;
	k_spinlock_key_t key = k_spin_lock(&txp_lock);

	notes = mg_txp_dead(&txp_notes, now, link, rx, sup);
	acl = mg_txp_dead(&txp_acl, now, link, rx, sup);
	k_spin_unlock(&txp_lock, key);
	if ((notes || acl) && atomic_cas(&tx_dead_logged, 0, 1)) {
		LOG_ERR("mg.ble: the TX path completed nothing with a link up (%s%s%s)",
			notes ? "notifications" : "",
			notes && acl ? ", " : "", acl ? "controller credits held" : "");
	}
	return notes || acl;
}

/* The TX path's bench test: the link's controller credits are taken and
 * held, so the host can send nothing on it (a controller that stopped
 * completing): the gadget restarts as a peer loss about 10 s later. RAM
 * only. The credits go back if the link ends before that (a central that
 * leaves first would otherwise strand them). */
static struct k_sem *tx_dead_pkts;
static atomic_t tx_dead_held;

static void tx_dead_give_back(void)
{
	atomic_val_t n = atomic_set(&tx_dead_held, 0);

	while (n-- > 0 && tx_dead_pkts) {
		k_sem_give(tx_dead_pkts);
	}
}

int mg_ble_tx_dead_test(void)
{
	struct bt_conn *conn = NULL;
	int n = 0;

	if (!IS_ENABLED(CONFIG_MG_AUTOMATION) && !IS_ENABLED(CONFIG_MG_BENCH_MODE)) {
		return -EPERM; /* a bench word: refused in release builds */
	}
	if (!IS_ENABLED(CONFIG_MG_WATCHDOG) || !mg_health_peer_restart_allowed()) {
		/* Nothing would give the credits back while the link lasts:
		 * only a peer restart does, and on test, in SAFE or past the
		 * rate bound the link is dropped instead. */
		return -ENOTSUP;
	}

	/* Taken and recorded under conn_lock, which disconnected() holds while
	 * it gives them back, and only once until then. */
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		conn = bt_conn_ref(cur_conn);
	}
	if (!conn || atomic_get(&tx_dead_held)) {
		k_mutex_unlock(&conn_lock);
		if (conn) {
			bt_conn_unref(conn);
		}
		return conn ? -EALREADY : -ENOTCONN;
	}
	struct k_sem *pkts = bt_conn_get_pkts(conn);

	while (pkts && k_sem_take(pkts, K_NO_WAIT) == 0) {
		n++;
	}
	tx_dead_pkts = pkts;
	atomic_set(&tx_dead_held, n);
	k_mutex_unlock(&conn_lock);
	bt_conn_unref(conn);
	LOG_WRN("tx dead test: holding the link's %d free controller credits (of %u): the "
		"watchdog resets the gadget in about %d s",
		n, pkts ? pkts->limit : 0, MG_TX_DEAD_MS / 1000);
	return 0;
}

unsigned int mg_transport_notify_dropped(void)
{
	return (unsigned int)atomic_get(&notify_dropped);
}

int mg_transport_notify(enum mg_att_chan ch, const uint8_t *buf, size_t len, k_timeout_t wait)
{
	struct tx_item *it;
	uint32_t gen;
	bool ok;

	if (len > MAX_ATT || ch >= MG_ATT_CHAN_COUNT) {
		return -EMSGSIZE;
	}
	k_mutex_lock(&conn_lock, K_FOREVER);
	ok = cur_conn && bt_gatt_is_subscribed(cur_conn, value_attr(ch), BT_GATT_CCC_NOTIFY);
	gen = conn_gen;
	k_mutex_unlock(&conn_lock);
	if (!ok) {
		return -ENOTCONN;
	}
	/* A disconnection while this waits flushes the queue before it's in:
	 * the generation keeps it off the next connection. */
	if (k_mem_slab_alloc(&tx_slab, (void **)&it, wait) != 0) {
		return -ENOMEM;
	}
	it->gen = gen;
	it->ch = ch;
	it->len = len;
	memcpy(it->data, buf, len);
	k_fifo_put(&tx_fifo, it);
	return 0;
}

int mg_transport_notify_urgent(enum mg_att_chan ch, const uint8_t *buf, size_t len)
{
	struct tx_item *it = NULL;
	uint32_t gen;
	bool ok;

	if (len > MAX_ATT || ch >= MG_ATT_CHAN_COUNT) {
		return -EMSGSIZE;
	}
	k_mutex_lock(&conn_lock, K_FOREVER);
	ok = cur_conn && bt_gatt_is_subscribed(cur_conn, value_attr(ch), BT_GATT_CCC_NOTIFY);
	gen = conn_gen;
	k_mutex_unlock(&conn_lock);
	if (!ok) {
		return -ENOTCONN;
	}
	if (k_mem_slab_alloc(&tx_slab, (void **)&it, K_NO_WAIT) != 0) {
		if (atomic_cas(&urgent_busy, 0, 1)) {
			it = &urgent_item;
		} else if (k_mem_slab_alloc(&tx_slab, (void **)&it, K_MSEC(800)) != 0) {
			return -ENOMEM;
		}
	}
	it->gen = gen;
	it->ch = ch;
	it->len = len;
	memcpy(it->data, buf, len);
	k_queue_prepend(&tx_fifo._queue, it);
	return 0;
}

static void tx_flush(void)
{
	struct tx_item *it;

	while ((it = k_fifo_get(&tx_fifo, K_NO_WAIT)) != NULL) {
		tx_free(it);
	}
}

static void sent_cb(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	if ((atomic_val_t)(uintptr_t)user_data == atomic_get(&inflight_gen)) {
		atomic_clear(&tx_stalled);
		k_sem_give(&inflight);
		att_event(MG_ATT_NOTIFY_COMPLETED, "a notification completed");

		uint32_t now = k_uptime_get_32();
		bool more = k_sem_count_get(&inflight) < TX_INFLIGHT;
		k_spinlock_key_t key = k_spin_lock(&txp_lock);

		mg_txp_done(&txp_notes, now, more);
		mg_txp_done(&txp_acl, now, txp_acl.owed);
		k_spin_unlock(&txp_lock, key);
	}
}

/* A new connection: every slot is free again, not stalled, nothing owed. */
static void tx_conn_reset(void)
{
	/* The TX legs belong to the connection edges (txp_conn_edge()): idling
	 * them here too would forgive a stall begun earlier on this link. */
	atomic_clear(&tx_stalled);
	k_sem_reset(&inflight);
	for (int i = 0; i < TX_INFLIGHT; i++) {
		k_sem_give(&inflight);
	}
}

static void tx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		mg_health_busy(MG_HEALTH_BLE_TX, false);
		struct tx_item *it = k_fifo_get(&tx_fifo, K_FOREVER);
		struct bt_conn *conn;

		mg_health_busy(MG_HEALTH_BLE_TX, true);
		k_mutex_lock(&conn_lock, K_FOREVER);
		/* Queued for an earlier connection: dropped. */
		conn = cur_conn && it->gen == conn_gen ? bt_conn_ref(cur_conn) : NULL;
		k_mutex_unlock(&conn_lock);

		if (conn && (atomic_val_t)it->gen != atomic_get(&inflight_gen)) {
			/* A new connection: every slot is free again, not stalled. */
			atomic_set(&inflight_gen, (atomic_val_t)it->gen);
			tx_conn_reset();
		}

		int64_t since = k_uptime_get();

		while (conn) {
			struct bt_gatt_notify_params p = {
				.attr = value_attr(it->ch),
				.data = it->data,
				.len = it->len,
				.func = sent_cb,
				.user_data = (void *)(uintptr_t)it->gen,
			};

			mg_health_beat(MG_HEALTH_BLE_TX);
			txp_owe_note(); /* waiting for a slot or a buffer is owing too */
			/* Never more than TX_INFLIGHT outstanding on one link and
			 * never a wait over 500 ms here (the watchdog wants a beat
			 * within 8 s): a stalled link drops what's queued, and
			 * stays up. */
			bool stalled = atomic_get(&tx_stalled) != 0;

			if (k_sem_take(&inflight, stalled ? K_NO_WAIT : K_MSEC(500)) != 0) {
				if (it->gen != mg_transport_conn_gen()) {
					break; /* the link is gone */
				}
				if (!stalled && k_uptime_get() - since < TX_STALL_MS) {
					continue;
				}
				if (!stalled) {
					atomic_set(&tx_stalled, 1);
					LOG_WRN("mg.ble: no notification completed for %d ms: "
						"dropping queued notifications until one does",
						TX_STALL_MS);
				}
				atomic_inc(&notify_dropped);
				break;
			}
			int err = it->len + 3 > bt_gatt_get_mtu(conn) ? -EMSGSIZE
								      : bt_gatt_notify_cb(conn, &p);

			if (err == -ENOMEM || err == -ENOBUFS) {
				k_sem_give(&inflight);
				if (k_uptime_get() - since >= TX_STALL_MS) {
					/* No buffer for this long: dropped and counted;
					 * the link stays up. */
					atomic_inc(&notify_dropped);
					LOG_WRN("mg.ble: no buffer for a notification for %d ms: "
						"dropped",
						TX_STALL_MS);
					break;
				}
				k_sleep(K_MSEC(5));
				continue;
			}
			if (err) {
				/* Never a silent drop: counted (console
				 * status lines) and logged. */
				k_sem_give(&inflight);
				atomic_inc(&notify_dropped);
				LOG_WRN("mg.ble: a %u B notification on %u dropped (%d; ATT MTU %u): %ld "
					"so far",
					it->len, it->ch, err, bt_gatt_get_mtu(conn),
					(long)atomic_get(&notify_dropped));
			}
			break;
		}
		if (conn) {
			bt_conn_unref(conn);
		}
		if (k_sem_count_get(&inflight) == TX_INFLIGHT) {
			/* Nothing of ours in flight (sent, or this one dropped):
			 * nothing owed on the notification side. */
			k_spinlock_key_t key = k_spin_lock(&txp_lock);

			mg_txp_idle(&txp_notes);
			k_spin_unlock(&txp_lock, key);
		}
		tx_free(it);
	}
}

K_THREAD_DEFINE(mg_tx, 2048, tx_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(3), 0, 0);

/* ------------------------------------------------------------------------
 * Advertising: one legacy advertiser on the identity address, so the Link
 * setup and the mg service are found at the same address (the apps key what
 * setup leaves them by it). Until setup is complete the payload takes turns
 * every ADV_TURN_MS between Link setup and mg (one 128-bit UUID fits), as
 * the ESP32's ble_server does; afterwards it is mg only.
 * ---------------------------------------------------------------------- */

#define ADV_TURN_MS 1500

static const uint8_t svc_uuid_le[] = {MG_SERVICE_UUID_LE_BYTES};
static const uint8_t setup_uuid_le[] = {MG_SETUP_SERVICE_UUID_LE};
/* Link setup's manufacturer data (protocols/README.md, Gadget setup v2): the
 * test company id 0xFFFF, then one flags byte. */
#define LINK_ADV_SET_UP     0x01 /* set up */
#define LINK_ADV_BLE_GADGET 0x02 /* a Muse Gadget over Bluetooth: serves mg, needs no Wi-Fi */
static uint8_t setup_mfg[] = {0xff, 0xff, LINK_ADV_BLE_GADGET};
static char name[CONFIG_BT_DEVICE_NAME_MAX];
/* The scan response is the name and the battery service UUID (4 bytes): the
 * name, MuseGadget-[<PRODUCT>-]XXXXXX, gets 25 of its 31. */
BUILD_ASSERT(sizeof(CONFIG_MG_DEVICE_NAME_PREFIX) - 1 +
		     (sizeof(CONFIG_MG_DEVICE_PRODUCT_CODE) > 1 ? sizeof(CONFIG_MG_DEVICE_PRODUCT_CODE) : 0) +
		     6 <= 25,
	     "CONFIG_MG_DEVICE_NAME_PREFIX and CONFIG_MG_DEVICE_PRODUCT_CODE make the name too long");
static K_MUTEX_DEFINE(adv_lock);
static bool ble_ready;
static bool adv_held; /* restarting: no more advertising (mg_ble_hold_for_reset) */
static bool link_turn; /* the Link setup payload is up */
static struct k_work_delayable adv_turn_work;

static void adv_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	(void)mg_ble_start_advertising();
}

static size_t build_ad(struct bt_data *ad, bool link)
{
	static const uint8_t flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;

	ad[0] = (struct bt_data)BT_DATA(BT_DATA_FLAGS, &flags, 1);
	if (mg_health_safe()) {
		/* SAFE: the name in the scan response, no mg or Link setup
		 * UUID (mgcommands.h, Recovery mode: a distinct identity). */
		return 1;
	}
	if (link) {
		/* Only advertised before setup today, so 0x02; 0x03 if it ever is
		 * after. */
		setup_mfg[2] = LINK_ADV_BLE_GADGET |
			       (mg_setup_store_complete() ? LINK_ADV_SET_UP : 0);
		ad[1] = (struct bt_data)BT_DATA(BT_DATA_UUID128_ALL, setup_uuid_le,
						sizeof(setup_uuid_le));
		ad[2] = (struct bt_data)BT_DATA(BT_DATA_MANUFACTURER_DATA, setup_mfg,
						sizeof(setup_mfg));
		return 3;
	}
	ad[1] = (struct bt_data)BT_DATA(BT_DATA_UUID128_ALL, svc_uuid_le, sizeof(svc_uuid_le));
	return 2;
}

/* The scan response, the same for both payloads: the name and BAS. */
static size_t build_sd(struct bt_data *sd)
{
	static const uint8_t bas[] = {BT_UUID_16_ENCODE(MG_BAS_SERVICE_UUID16)};

	sd[0] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name));
	sd[1] = (struct bt_data)BT_DATA(BT_DATA_UUID16_SOME, bas, sizeof(bas));
	return 2;
}

static void adv_turn_fn(struct k_work *w)
{
	struct bt_data ad[3], sd[2];

	ARG_UNUSED(w);
	k_mutex_lock(&adv_lock, K_FOREVER);
	link_turn = mg_setup_advertise_link() && !mg_health_safe() && !link_turn;
	size_t n = build_ad(ad, link_turn);
	/* Both again: a NULL scan response would empty it. -EAGAIN when not
	 * advertising (connected): the next start resumes. */
	int err = bt_le_adv_update_data(ad, n, sd, build_sd(sd));

	if (err == 0 && mg_setup_advertise_link() && !mg_health_safe()) {
		k_work_reschedule(&adv_turn_work, K_MSEC(ADV_TURN_MS));
	}
	k_mutex_unlock(&adv_lock);
}

/*
 * Advertising interval: fast, 20-30 ms (Apple's first 30 s, within
 * 20-60 ms), for CONFIG_MG_ADV_FAST_SECS after boot, after a disconnect
 * (from the link's loss) and after a press with no app connected
 * (mg_transport_adv_boost); then slow, CONFIG_MG_ADV_SLOW_MS (Apple's
 * 1022.5 ms step). Before setup too (never fast
 * without end; mg_adv_policy.h).
 */
#define ADV_FAST_MIN 0x20 /* 20 ms, 0.625 ms units */
#define ADV_FAST_MAX 0x30 /* 30 ms */
static bool adv_slow; /* the advertiser runs at the slow interval */
static struct k_work_delayable adv_slow_work;

static bool want_fast(void)
{
	return mg_adv_want_fast(CONFIG_MG_ADV_SLOW_MS, k_uptime_get(), fast_until);
}

static bool connected_now(void)
{
	k_mutex_lock(&conn_lock, K_FOREVER);
	bool busy = cur_conn != NULL;

	k_mutex_unlock(&conn_lock);
	return busy;
}

/* Restarts the advertiser at the other interval when it should change. */
static void adv_slow_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (!ble_ready || connected_now()) {
		return;
	}
	bool fast = want_fast();

	if (fast == !adv_slow) {
		if (fast && CONFIG_MG_ADV_SLOW_MS) {
			/* A later boost: look again. */
			int64_t left = fast_until - k_uptime_get();

			k_work_reschedule(&adv_slow_work, K_MSEC(MAX(left, 10000)));
		}
		return;
	}
	if (fast) {
		LOG_INF("mg.ble: advertising fast for %d s", CONFIG_MG_ADV_FAST_SECS);
	} else {
		LOG_INF("mg.ble: advertising slowly (every %d ms)", CONFIG_MG_ADV_SLOW_MS);
	}
	atomic_set(&adv_on, 0);
	(void)bt_le_adv_stop();
	(void)mg_ble_start_advertising();
}

void mg_transport_adv_boost(void)
{
	fast_until = k_uptime_get() + CONFIG_MG_ADV_FAST_SECS * 1000LL;
	if (adv_slow) {
		k_work_reschedule(&adv_slow_work, K_NO_WAIT);
	}
}

int mg_ble_start_advertising(void)
{
	struct bt_data ad[3], sd[2];
	size_t sn = build_sd(sd);
	bool fast = want_fast();
	uint32_t slow = CONFIG_MG_ADV_SLOW_MS * 8 / 5; /* 0.625 ms units */
	const struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, fast ? ADV_FAST_MIN : slow,
				     fast ? ADV_FAST_MAX : slow + 16, NULL);

	k_mutex_lock(&conn_lock, K_FOREVER);
	bool busy = cur_conn != NULL;

	k_mutex_unlock(&conn_lock);
	if (busy) {
		return 0;
	}
	k_mutex_lock(&adv_lock, K_FOREVER);
	if (adv_held) {
		k_mutex_unlock(&adv_lock);
		return 0;
	}
	/* SAFE advertises its name alone (build_ad): no Link setup turns. */
	bool link = mg_setup_advertise_link() && !mg_health_safe();

	link_turn = link;
	size_t n = build_ad(ad, link_turn);
	int err = bt_le_adv_start(&param, ad, n, sd, sn);

	if (err == -EALREADY) {
		err = bt_le_adv_update_data(ad, n, sd, sn);
	}
	if (!err) {
		atomic_set(&adv_on, 1);
	}
	if (!err && link) {
		k_work_reschedule(&adv_turn_work, K_MSEC(ADV_TURN_MS));
	} else {
		(void)k_work_cancel_delayable(&adv_turn_work);
	}
	if (!err) {
		adv_slow = !fast;
		if (fast && CONFIG_MG_ADV_SLOW_MS) {
			int64_t left = fast_until - k_uptime_get();

			k_work_reschedule(&adv_slow_work, K_MSEC(MAX(left, 1000)));
		}
	}
	k_mutex_unlock(&adv_lock);
	if (err) {
		LOG_ERR("advertising failed (%d)", err);
		return err;
	}
	LOG_INF("advertising as %s%s", name,
		mg_health_safe() ? " (SAFE: the name alone, no service UUIDs)"
		: link	     ? " (Link setup and mg, taking turns)"
			     : "");
	return 0;
}

void mg_ble_hold_for_reset(void)
{
	k_mutex_lock(&adv_lock, K_FOREVER);
	adv_held = true;
	(void)k_work_cancel_delayable(&adv_turn_work);
	atomic_set(&adv_on, 0);
	(void)bt_le_adv_stop();
	k_mutex_unlock(&adv_lock);
	mg_transport_disconnect();
}

static void reach_test_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	mg_ble_hold_for_reset();
}
static K_WORK_DELAYABLE_DEFINE(reach_test_work, reach_test_fn);

int mg_ble_reach_test(void)
{
	if (!IS_ENABLED(CONFIG_MG_AUTOMATION) && !IS_ENABLED(CONFIG_MG_BENCH_MODE)) {
		return -EPERM; /* a bench word: refused in release builds */
	}
	if (!IS_ENABLED(CONFIG_MG_REACH_RESET)) {
		return -ENOTSUP;
	}
	LOG_WRN("reach test: advertising off until the next boot (the reachability check "
		"restarts it in 30 s)");
	k_work_reschedule(&reach_test_work, K_MSEC(200));
	return 0;
}

void mg_ble_refresh_advertising(void)
{
	if (ble_ready) {
		(void)mg_ble_start_advertising();
	}
}

const char *mg_transport_name(void)
{
	return name;
}

void mg_ble_set_battery(uint8_t percent)
{
	(void)bt_bas_set_battery_level(percent);
}

int mg_ble_init(void)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	int err;

	k_work_init(&evt_work, evt_fn);
	k_work_init(&adv_work, adv_fn);
	k_work_init_delayable(&adv_turn_work, adv_turn_fn);
	k_work_init_delayable(&phy_retry_work, phy_retry_fn);
	k_work_init_delayable(&adv_slow_work, adv_slow_fn);
	fast_until = k_uptime_get() + CONFIG_MG_ADV_FAST_SECS * 1000LL; /* after boot */
	mg_connparam_init();
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}
#if defined(CONFIG_BT_SETTINGS)
	/* Bonds (the device session's encrypted link), and with them the
	 * identity: the host finishes setting up once they're loaded. */
	(void)settings_load_subtree("bt");
#endif
	bt_id_get(addrs, &count);
	/* The identity address, most significant byte first. */
	uint8_t a[6];

	for (int i = 0; i < 6; i++) {
		a[i] = addrs[0].a.val[5 - i];
	}
	mg_identity_init(CONFIG_MG_DEVICE_NAME_PREFIX, CONFIG_MG_DEVICE_PRODUCT_CODE, a);
	/* SAFE mode advertises its own name and no mg UUID (mgcommands.h,
	 * Recovery mode: a distinct identity, so an app that doesn't read the
	 * declaration never takes it for the gadget; found by name for SMP
	 * recovery, as the ESP32's). */
	snprintf(name, sizeof(name), "%s",
		 mg_health_safe() ? mg_identity_safe_name() : mg_identity_name());
	(void)bt_set_name(name);
	ble_ready = true;
	return 0;
}
