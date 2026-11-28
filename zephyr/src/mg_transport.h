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

#ifndef MG_TRANSPORT_H_
#define MG_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/* The musegadgets characteristics that carry notifications. */
enum mg_att_chan {
	MG_ATT_CONTROL,
	MG_ATT_DATA,
	/* Muse Link setup's TX characteristic (notifications), and its RX
	 * characteristic for writes (mg_setup.h). */
	MG_ATT_SETUP,
	MG_ATT_CHAN_COUNT,
};

/*
 * Queues one notification. Notifications go out in the order they are
 * queued, across all characteristics. Returns -ENOMEM if no buffer freed up
 * within wait, -ENOTCONN without a subscribed client. Implemented by
 * mg_ble.c (and by fakes in the unit tests).
 */
int mg_transport_notify(enum mg_att_chan ch, const uint8_t *buf, size_t len, k_timeout_t wait);
/* Ahead of everything queued (the token proof's answers):
 * at the queue's head, from a buffer held back for it when the
 * others are taken; waits at most 800 ms for one when that's in use too. */
int mg_transport_notify_urgent(enum mg_att_chan ch, const uint8_t *buf, size_t len);
void mg_transport_disconnect(void);
/* The link's connection with a reference the caller drops (bt_conn_unref),
 * or NULL without one. */
struct bt_conn *mg_transport_conn_ref(void);
/* Notifications queued and not yet handed to the stack (the NUS console
 * waits for the data path). */
bool mg_transport_tx_pending(void);
/* The TX path is dead (src/mg_tx_progress.h): a link up, something owed
 * (ours in flight or waiting for a buffer, or packets the controller holds)
 * and nothing completed for 10 s with the peer alive, or for the
 * supervision timeout plus 2 s (at most 30 s) with it silent: the detection
 * threshold; the supervisor then restarts the gadget as a peer loss within a
 * second (mg_health_peer_loss()). */
bool mg_transport_tx_dead(void);
/* Peer events backed up with a link up long enough that a stale app work
 * queue is the peer's doing (mg_evt_policy.h). */
bool mg_transport_evt_saturated(uint32_t stale_ms);
/* A request from the peer that implies our earlier answers got through (an
 * ATT Write Request, an SMP request): progress for the TX path's ACL side. */
void mg_transport_rx_request(void);
/* Connectable advertising is running, or a link is up on which a completed
 * ATT exchange proved the gadget's own path (plain flags the BLE callbacks
 * set; safe from a timer). */
bool mg_transport_reachable(void);
/* A connection formed since this reader's last call (one bit per reader:
 * the reachability sampler's and the trial clock's). */
#define MG_LINK_SEEN_REACH 0
#define MG_LINK_SEEN_TRIAL 1
bool mg_transport_link_seen_take(unsigned int who);
/* A link is up (with or without a completed ATT exchange). */
bool mg_transport_link_up(void);
/* The central's own writes (mg characteristics, the Nordic UART, SMP), and
 * the link's start: SAFE's idle-link drop (mg_safe_policy.h). */
void mg_transport_peer_wrote(void);
uint32_t mg_transport_peer_write_ms(void);
/* A link is up and a completed ATT exchange proved it this connection. */
bool mg_transport_att_ok(void);
/* This connection passed the gate (its token proof matched): ATT evidence
 * seen on it before now moves recovery state (mg_reach_exchange_counts()). */
void mg_transport_gate_passed(void);
/* Notifications the stack refused since boot (too long for the link's ATT
 * MTU, or another error; counted, never silent). */
unsigned int mg_transport_notify_dropped(void);
/* The link has completed no notification for TX_STALL_MS with every slot
 * taken (a central that stopped acknowledging): queued notifications are
 * dropped (counted) until one completes; the link stays up. */
bool mg_transport_tx_stalled(void);
/* The connected central's GAP Device Name (0x2A00), as read once per
 * connection: its length (0 when none or not read yet). */
size_t mg_transport_peer_name(char *out, size_t cap);
/* Advertise fast again for CONFIG_MG_ADV_FAST_SECS (a press with no
 * app connected). */
void mg_transport_adv_boost(void);
uint16_t mg_transport_att_mtu(void);
/* Changes at every connect and disconnect, in the stack's own callbacks:
 * what was granted to one connection can be checked against it from any
 * thread, ahead of the app work queue's connection events. */
uint32_t mg_transport_conn_gen(void);
/* The generation of the connection the SMP request being handled came on,
 * stamped where the transport took it (src/zephyr/smp.c change 3 with
 * CONFIG_MG_DFU, src/mg_smp_req_gen.c with CONFIG_MG_SMP_GATE_BOARD); 0
 * outside a request. */
uint32_t mg_smp_request_gen(void);
/* The connection a request just completed began on: its first fragment's
 * with SMP reassembly (the stamp above takes it). */
uint32_t mg_smp_rx_source_gen(void);
/* The link is encrypted (LE security level 2 or higher). */
bool mg_transport_encrypted(void);
/* The advertised name, e.g. MuseGadget-1A2B3C (set once BLE is up). */
const char *mg_transport_name(void);

/* Asks the central for connection parameters (units: 1.25 ms, events,
 * 10 ms). 0, or a negative errno if it couldn't be sent. */
/* Hear the next connection event despite peripheral latency (Zephyr's open
 * controller; -ENOTSUP elsewhere). For a mode switch (mg_connparam.c). */
int mg_transport_latency_cancel(void);
int mg_transport_conn_params(uint16_t interval_min, uint16_t interval_max, uint16_t latency,
			     uint16_t timeout);

/* The app work queue: all protocol state lives on it. */
struct k_work_q *mg_app_wq(void);

#endif
