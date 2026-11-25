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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Call once at boot, before any task that can send. If the previous boot ended
// in a panic, watchdog, brownout, power-glitch, CPU-lockup or eFuse reset,
// queues one summary of it in place of any still queued, then arms the
// retained record for this boot. Both live in retained RAM only: a queued
// report survives restarts until it is sent, nothing is written to flash, and
// power loss discards it.
void error_report_init(void);

// The queued link.error_report message as heap JSON the caller frees, or NULL
// when nothing is queued. It stays queued until error_report_mark_sent().
char *error_report_pending_json(void);
void error_report_mark_sent(void);

// Drops a queued report unsent. A factory reset calls it, since the device may
// change hands; nothing may be sending the report at the time.
void error_report_discard(void);

// Panic-handler entry: stores into retained RAM without locking, logging or
// allocating. `address` is the faulting instruction and `return_address` the
// return-address register at the fault; either may be NULL when unknown.
// Before error_report_init() in this boot it notes only that the boot crashed:
// the next report has the reset and its count, but no version, binary or
// fault.
void error_report_note_panic(const void *address, const void *return_address,
                             const char *cause, const char *detail);

#ifdef __cplusplus
}
#endif
