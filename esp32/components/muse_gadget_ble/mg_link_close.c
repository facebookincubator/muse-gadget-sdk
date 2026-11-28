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
/* The link closes our code asks for (mg_link_close.h). */
#include "mg_link_close.h"

#include "esp_timer.h"

int __real_ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason);

static volatile int64_t s_asked_us = -1;

int __wrap_ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason)
{
    s_asked_us = esp_timer_get_time();   /* before the call: its disconnect event may come at once */
    return __real_ble_gap_terminate(conn_handle, hci_reason);
}

int64_t mg_link_close_asked_us(void)
{
    return s_asked_us;
}
