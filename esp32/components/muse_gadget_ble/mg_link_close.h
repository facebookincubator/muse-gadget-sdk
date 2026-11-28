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

#include <stdint.h>

/*
 * Which link closes our code asked for (two Live call drops once read as
 * "the gadget closed it" with nothing in the log). Every ble_gap_terminate()
 * in this firmware goes through a linker wrap (-Wl,--wrap=ble_gap_terminate,
 * mg_link_close.c) that notes when. NimBLE's own closes don't: its L2CAP
 * reassembly timer (BLE_L2CAP_RX_FRAG_TIMEOUT, 30 s in IDF's NimBLE: part of
 * a packet came and the rest never did) calls ble_gap_terminate_with_conn()
 * directly. So a disconnect with reason 0x216 (local host) and no ask within
 * MG_LINK_CLOSE_ASK_US before it was the host's own (mg_ble logs it once and
 * counts it in the status line's host_closed). The timeout itself is
 * unchanged.
 */
#define MG_LINK_CLOSE_ASK_US 5000000   /* an ask this recent explains a 0x216 disconnect */

/* esp_timer time of the last ble_gap_terminate() our code made, or -1. */
int64_t mg_link_close_asked_us(void);
