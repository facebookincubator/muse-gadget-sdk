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


#ifndef MG_CONNPARAM_H_
#define MG_CONNPARAM_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Link modes (mgcommands.h, settings link_mode 16 and link_params
 * 17): several gadgets share one phone's radio, so
 * only the gadget in use holds a short interval. Three modes, each a real
 * connection parameter update:
 *
 * - sleep  CONFIG_MG_CONN_SLEEP_* (100/160/4/600, 125-200 ms, 6 s)
 * - ready  CONFIG_MG_CONN_IDLE_* (24/36/5/400, 30-45 ms; never 15 ms)
 * - active CONFIG_MG_CONN_INTERVAL_* / _LATENCY (12/12/0/400, 15 ms; Apple
 *   grants 15 ms only for exactly 12/12); all with CONFIG_MG_CONN_TIMEOUT.
 *
 * - Ready at connect: once the PHY and data length updates are done (or
 *   after 2 s) and, never at connect, after the central's own first update
 *   or 1 s after connecting if none came; nothing asked when the central
 *   connected with it.
 * - Active on its own activity: a talk press (at touch-down), the mic (a
 *   client's start_mic too), a clip download, any SMP request (an update),
 *   a throughput test, set_tokens (mg_connparam_busy, called again while
 *   it lasts), and Link setup from pairing_client_hello to its end
 *   (mg_connparam_hold). Not activity: a GATT write alone, NUS console
 *   input, assistant_state.
 * - CONFIG_MG_CONN_IDLE_AFTER_MS (5 s) after the last activity: back to the
 *   client's mode (ready unless it wrote sleep); 10 min in ready without
 *   activity (CONFIG_MG_CONN_SLEEP_AFTER_MS): sleep, until activity or a
 *   write. A client's active lasts until CONFIG_MG_CONN_CLIENT_ACTIVE_MS
 *   (30 s) without activity, then ready; its ready or sleep takes effect
 *   once the device's own activity ends.
 * - Every effective change notifies [get_settings, link_mode, mode]
 *   (mg_core_send_unsolicited).
 * - The set the link is already in is never asked again (a Mac rejects it;
 *   Zephyr's controller completes it without telling either host). At most
 *   one request per CONFIG_MG_CONN_REQ_GAP_MS (2 s), so a mode change never
 *   follows the previous grant back to back (>= 500 ms); a
 *   request not granted within 5 s is sent once more, then the central's
 *   choice stands until the next change.
 *
 * Zephyr's open controller (BT_LL_SW_SPLIT) has no local latency switch;
 * a local latency switch wouldn't count as idle anyway.
 * Logged: "mg.ble: link active (talk press)", "mg.ble: link ready (activity
 * over)", each request, and each grant (mg_ble.c).
 */

enum mg_xfer {
	MG_XFER_DFU,
	MG_XFER_TOKENS,
	MG_XFER_STREAM,
	MG_XFER_BENCH,
	MG_XFER_MIC,
	MG_XFER_PRESS,
	MG_XFER_WRITE,
	MG_XFER_CONSOLE,
	MG_XFER_CLIP,
};

void mg_connparam_init(void);
/* The parameters the central connected with (interval 0: unknown). */
void mg_connparam_connected(uint16_t interval, uint16_t latency, uint16_t timeout);
void mg_connparam_disconnected(void);
/* The PHY or data length update finished (either order). */
void mg_connparam_phy_done(void);
void mg_connparam_data_len_done(void);
/* The parameters in force now (le_param_updated). */
void mg_connparam_updated(uint16_t interval, uint16_t latency, uint16_t timeout);
/* Activity (all but MG_XFER_WRITE and MG_XFER_CONSOLE): fast now, and idle
 * only CONFIG_MG_CONN_IDLE_AFTER_MS after the last one. Ignored while
 * disconnected. */
void mg_connparam_busy(enum mg_xfer what);
/* Activity that lasts (a Link setup session): fast while on, idle
 * CONFIG_MG_CONN_IDLE_AFTER_MS after it goes off. Cleared at a disconnect. */
#define MG_CONNPARAM_HOLD_SETUP 0x1u
void mg_connparam_hold(unsigned int bit, bool on);
/* Setting link_mode 16 (mg_link_mode_t): the client's write, and the
 * effective mode (ready when not connected). */
void mg_connparam_client_mode(uint8_t mode);
/* A user's wake or touch on the device (a press): from sleep (by itself or
 * the client's) back to ready, not active, and the 10 minutes in ready start
 * again (mgcommands.h, Link modes). A talk press is activity too
 * (mg_connparam_busy): active, then ready. */
void mg_connparam_user_wake(void);
/* Experiment: hear every connection event from a mode
 * request until its grant (CONFIG_MG_CONN_SWITCH_LISTEN is the default;
 * the bench console's ">listen=0|1" sets it). Each switch logs
 * "mg.ble: switch A -> B applied in N ms". */
void mg_connparam_switch_listen(bool on);
uint8_t mg_connparam_mode(void);
/* Setting link_params 17: what the controller last reported (0s if none). */
void mg_connparam_link_params(uint16_t *interval, uint16_t *latency, uint16_t *timeout);
/* The link is in its active mode (12/12/0). */
bool mg_connparam_fast(void);

#endif
