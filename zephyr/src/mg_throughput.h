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


#ifndef MG_THROUGHPUT_H_
#define MG_THROUGHPUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The throughput test (mgcommands.h, Throughput test): device_action
 * throughput_test. Send streams numbered Data notifications from a thread
 * of its own as fast as the stack takes them; receive counts the client's
 * Data writes. Either reports [device_action, throughput_test, 0, bytes,
 * packets, ms, gaps] at the end or on stop.
 */

struct mg_tp_count {
	uint32_t bytes;
	uint32_t packets;
	uint32_t gaps;
	uint32_t next_seq;
	int64_t first_ms; /* 0: nothing yet */
	int64_t last_ms;
};

/* A test packet: the sequence number, then (seq + i) & 0xFF. */
void mg_tp_fill(uint8_t *buf, size_t len, uint32_t seq);
/* Counts one received packet (false if it's too short to carry a sequence). */
bool mg_tp_account(struct mg_tp_count *c, const uint8_t *buf, size_t len, int64_t now_ms);
/* The report: [device_action, throughput_test, 0, bytes, packets, ms, gaps]. */
size_t mg_tp_report(const struct mg_tp_count *c, uint8_t *out);

/* device_action throughput_test (the bytes after the action byte). */
void mg_tp_command(const uint8_t *p, size_t len);
/* The client's change_data_type (it always switches): true if it selects the
 * receive test's Data, false for any other type (its Data is dropped). */
bool mg_tp_change_data_type(uint8_t type);
/* A client write on Data (plaintext, or opened from Encrypted Data). */
void mg_tp_data_rx(const uint8_t *buf, size_t len);
bool mg_tp_running(void);
/* Disconnected: stop without a report. */
void mg_tp_reset(void);
void mg_tp_init(void);

#endif
