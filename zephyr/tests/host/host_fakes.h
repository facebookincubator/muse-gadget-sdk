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

#ifndef HOST_FAKES_H_
#define HOST_FAKES_H_

#include <stdbool.h>
#include <stdint.h>

/* Every notification the modules under test sent, in order. */
struct host_note {
	uint8_t ch; /* enum mg_att_chan */
	uint16_t len;
	uint8_t data[520]; /* an ATT value (512) and its header */
};

#define HOST_MAX_NOTES 128
extern struct host_note host_notes[HOST_MAX_NOTES];
extern int host_nnotes;
extern uint16_t host_mtu;
extern int host_disconnects;
extern int host_adv_refreshes;
extern int host_link_refreshes;
extern bool host_pair_led;
extern int host_led_events[4]; /* per enum mg_led_event */
extern int host_log_verbose;
/* Everything the firmware logged since the last host_log_clear(). */
extern char host_log_buf[];
void host_log_clear(void);
/* The link is encrypted (mg_transport_encrypted). */
extern bool host_encrypted;
extern uint32_t host_conn_gen; /* mg_transport_conn_gen */
/* After host_notify_skip more Control notifications, the next
 * host_notify_fail fail with -ENOMEM (a full notification queue). */
extern int host_notify_skip;
extern int host_notify_fail;
extern int host_adv_boosts; /* mg_transport_adv_boost */
/* Control notifications fail with -ENOTCONN (the client unsubscribed). */
extern bool host_notify_unsubscribed;

void host_notes_clear(void);

/* Link setup's hold on the connection parameters (MG_CONNPARAM_HOLD_*). */
extern unsigned int host_connparam_holds;

#endif
