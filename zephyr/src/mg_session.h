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

#ifndef MG_SESSION_H_
#define MG_SESSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_transport.h"

/*
 * Sits between the BLE characteristics and mg_core: Control commands go to
 * mg_core, client Data to the throughput test, and a client is ready for
 * gestures and audio once it subscribes to Control and Data. (Commands 6,
 * 0x80-0x82 and errors 0x0100-0x01FF are reserved for a future security
 * extension, which would live here.)
 *
 * The rx and connection functions run on the app work queue; the send
 * functions may be called from any thread.
 */

void mg_session_init(void);
void mg_session_connected(void);
void mg_session_disconnected(void);
void mg_session_subscribed(enum mg_att_chan ch, bool enabled);
void mg_session_rx(enum mg_att_chan ch, const uint8_t *buf, size_t len);
/* A client write on Control was dropped (the event queue was full). */
void mg_session_control_lost(void);

/* Sends one Control notification (waits up to 500 ms for a buffer). */
int mg_session_send_control(const uint8_t *buf, size_t len);
/* The event handler sets the deadline the replies to one event share (0 when
 * none runs; mg_evt_policy.h). */
void mg_session_reply_budget(int64_t deadline);
/* Control replies dropped since boot (a full queue for 500 ms; a turn's
 * gestures and stop_mic never are). */
unsigned int mg_session_replies_dropped(void);
/* A reply that couldn't be built whole: counted with the drops, logged. */
void mg_session_count_drop(const char *what);
/* request_status arrived on this connection (part of readiness). */
void mg_session_status_requested(void);
/* On Control ahead of everything queued, not waiting for another sender
 * (mg_transport_notify_urgent): the token proof's answers. */
int mg_session_send_urgent(const uint8_t *buf, size_t len);
/* Queues one Control notification only if that needs no waiting: 0, or
 * -EBUSY / -ENOMEM (try again later), -ENOTCONN, -EMSGSIZE. For senders on
 * the app work queue that retry on their own: waiting there would hold up
 * the client's writes behind it. */
int mg_session_try_control(const uint8_t *buf, size_t len);
/* Sends one Data chunk. */
int mg_session_send_data(const uint8_t *buf, size_t len, k_timeout_t wait);
/* Largest Data chunk that fits one notification. */
size_t mg_session_data_room(void);

#endif
