/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "muse_power_test.h"

/* Board-neutral diagnostic porting surface. Only the isolated diagnostic may
 * call these hooks, never the production UI. Implement only board-owned rails,
 * pads and peripherals; never use blanket GPIO sweeps or production NVS.
 *
 * name: stable printable board name (1..80 bytes), for host report attribution.
 * matrix: immutable table for "peripheral" or "sleep"; unknown/unsupported name
 * returns NULL and sets *count to zero. count may be NULL. Peripheral must have
 * at least one entry and end in the safe resting configuration. Keep tables
 * within MUSE_PTEST_MAX_RECORDS; peripheral supports up to two repeats, sleep
 * is cold-once and has at most two timer-deep records. Matrix versions remain
 * peripheral=1 and sleep=2. No dynamic allocation or mutable descriptors.
 *
 * Codec evidence/history hooks may return zeroed/unknown evidence on boards
 * without reset-surviving codecs; the values never gate measurement acceptance.
 * Deep-hold ownership must be independently recoverable before UART setup.
 */
const char *muse_ptest_board_name(void);
const muse_ptest_state_t *muse_ptest_board_matrix(const char *name, size_t *count);
esp_err_t muse_ptest_board_init(void);
esp_err_t muse_ptest_board_power(muse_ptest_readback_t *out);
esp_err_t muse_ptest_board_apply(const muse_ptest_state_t *state);
esp_err_t muse_ptest_board_readback(muse_ptest_readback_t *out);
esp_err_t muse_ptest_board_service(muse_ptest_load_t load);
esp_err_t muse_ptest_board_uart_restore(void);
esp_err_t muse_ptest_board_prepare_deep(bool held);
esp_err_t muse_ptest_board_release_deep_holds(void);
bool muse_ptest_board_deep_holds_owned(void);
bool muse_ptest_board_warm_seen(void);
void muse_ptest_board_codec_regs(muse_ptest_codec_regs_t *out);
void muse_ptest_board_usb_dryrun(bool on);
unsigned muse_ptest_board_codec_history(void);
void muse_ptest_board_restore_codec_history(unsigned history);
