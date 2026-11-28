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

#ifndef MG_VM_SESSION_H_
#define MG_VM_SESSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mgcommands.h"

struct mg_vm_link_mem;

/*
 * mg_command_session, the device side (mgcommands.h, Device session): the
 * gadget's own VM session, with the phone as its network.
 *
 *   hello      sealed only, after a matching token proof on this
 *              connection (else not_found / proof_required / insecure, on
 *              any link): the device's hello, then its credentials (node
 *              id, token pair, device key) sealed, as fragments
 *   state      open starts a session: mg_vm_link opens /link-control and
 *              registers; anything else ends it
 *   data       one message at a time each way, fragmented to ATT_MTU - 3,
 *              within the receiver's window; credit gives bytes back
 *   credentials_update
 *              the refreshed token pair, stored in one commit
 *              (mg_setup_store_update_tokens), answered credentials_saved
 *
 * The device takes messages up to CONFIG_MG_VM_SESSION_MAX_MESSAGE with a
 * window of CONFIG_MG_VM_SESSION_WINDOW, and gives a message's bytes back
 * once it has handled all of it (a message larger than the window: as its
 * fragments land). Everything goes out on Control, never waiting for the
 * notification queue (what doesn't fit is retried), and runs on the app
 * work queue, behind mg_core. protocols/test-vectors/mg-session-v1.json is
 * checked byte for byte by tests/host.
 */

/* An mg_command_session command (the bytes after the command byte). */
void mg_vm_session_rx(const uint8_t *p, size_t len);

/* Drops the session, its hello and everything queued: at every connection
 * and disconnection. */
void mg_vm_session_connection_reset(void);

/* The client reported the VM session open. */
bool mg_vm_session_is_open(void);

/* A client write on Control was lost (mg_session_control_lost): with a hello
 * on this connection, the device disconnects, as the client can't tell. */
void mg_vm_session_write_lost(void);

/* Asks the client for a new session (open_request 0, then 1), as a Wi-Fi
 * gadget reconnects its Noise session. Nothing if the session isn't open. */
void mg_vm_session_request_reopen(void);

struct mg_vm_iov {
	const void *p;
	size_t n;
};

/*
 * Queues one device -> client message (a ServiceRequestEnvelope), the
 * concatenation of iov, and sends what credit allows. -ENOTCONN without an
 * open session, -EMSGSIZE if it is larger than the client's max_message,
 * -ENOSPC if the send queue (CONFIG_MG_VM_SESSION_TX_QUEUE) is full.
 */
int mg_vm_session_sendv(const struct mg_vm_iov *iov, size_t n);
/* The largest message mg_vm_session_sendv takes now (0 if not open). */
size_t mg_vm_session_tx_room(void);

void mg_vm_session_init(void);

/* The link's buffers, inside the session's memory block: valid only while a
 * session is open (between a successful hello and the connection's end). */
struct mg_vm_link_mem *mg_vm_session_link_mem(void);
/* Whether the session's memory block is allocated (tests). */
bool mg_vm_session_mem_held(void);
#if defined(CONFIG_ZTEST) || defined(MG_HOST_TEST)
/* Test hook (host and unit tests only): the next client (rx) and device (tx)
 * counters, the vectors' stand-in for a long session; negative leaves one. */
void mg_vm_session_test_set_counters(int64_t rx, int64_t tx);
/* Test hook (host and unit tests only): an open sealed session's keys from
 * these inputs (K, the proof's nonces, the node id), for vectors made with
 * fixed nonces. */
int mg_vm_session_test_set_keys(const uint8_t *k, const uint8_t *cn, const uint8_t *dn,
				const char *node);
#endif

/* A user action on the gadget (a talk press, a tap that may lead to an
 * agent action): an active task for 2 min after it; with the
 * session down now, it asks at once. */
void mg_vm_session_user_action(void);
/* A task started (mg_vm_link: traffic owed while the session is down):
 * asks at once after an unasked end with no ask running. */
void mg_vm_session_reason(void);
/* The credit keepalive (mgcommands.h, Credit): the credit owed, or credit
 * [0] when none, while a session is open; for a board screen that waits on
 * the session (no caller on the XIAO yet). */
void mg_vm_session_nudge(void);
/* Whether the gadget has an active task: a user action in the last
 * 2 min, traffic owed, a board's wait (mg_vm_wait_active) or its own open request. */
bool mg_vm_session_task_active(void);
/* No task for CONFIG_MG_VM_IDLE_CLOSE_MS: the gadget closes its own
 * session (open_request [0]); a new task asks again (mg_vm_link's tick). */
void mg_vm_session_close_idle(void);
/* A board's own wait that keeps a task active (weak, false: none on this app). */
bool mg_vm_wait_active(void);

/* Sealed sessions ended by three counter refusals in a row, since boot. */
unsigned int mg_vm_session_ctr_ends(void);

/* A sealed receive counter given back opens a hole only within 255 of the
 * next expected counter, and the hole closes once the next counter moves it
 * further back (the reference receiver's limits). */
static inline bool mg_vm_session_hole_in_window(uint32_t next, uint32_t hole)
{
	return (uint32_t)(next - hole) <= 255u;
}

#endif
