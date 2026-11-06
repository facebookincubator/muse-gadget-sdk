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

// See ble_coex.h. The BLE controller and the Wi-Fi driver both report their
// state to the coexistence scheduler through coex_schm_status_bit_set/clear
// (libcoexist). The linker wraps both (main/CMakeLists.txt): Wi-Fi bits pass
// straight through, and BLE bits in the withheld set are remembered but kept
// from the scheduler while the policy is active.

#include "sdkconfig.h"

#if CONFIG_HOMEHUB_BLE_COEX

#include "ble_coex.h"

#include <inttypes.h>

#include "esp_attr.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "private/esp_coexist_internal.h"

// coex_schm_status_bit_set/clear and the meaning of the BLE bits (ble_coex.h)
// are ESP-IDF's private coexistence interface, read from the v6.0 controller
// and confirmed on hardware. Another IDF may change either without notice:
// check the bits there (tools/coexist/README.md), then widen this range, or
// build with CONFIG_HOMEHUB_BLE_COEX off.
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0) || ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
#error "ble_coex.c is verified on ESP-IDF v6.0.x only: check the BLE coexistence status bits on this IDF, or turn off CONFIG_HOMEHUB_BLE_COEX"
#endif

#define KNOWN_BITS (BLE_COEX_ADV | BLE_COEX_SCAN | BLE_COEX_CONN)

static const char *TAG = "link.coex";

#if CONFIG_HOMEHUB_BLE_COEX_WIFI_FIRST_CONNECTED
#define DEFAULT_WITHHELD (BLE_COEX_ADV | BLE_COEX_CONN)
#else
#define DEFAULT_WITHHELD (BLE_COEX_ADV)
#endif

static DRAM_ATTR uint32_t s_controller;  // BLE bits the controller has set
static DRAM_ATTR uint32_t s_withheld = DEFAULT_WITHHELD;
static DRAM_ATTR bool s_active;
static DRAM_ATTR uint32_t s_unknown;     // BLE bits outside KNOWN_BITS seen
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void __real_coex_schm_status_bit_set(uint32_t type, uint32_t status);
void __real_coex_schm_status_bit_clear(uint32_t type, uint32_t status);

static inline uint32_t hidden_locked(void)
{
    return s_active ? s_withheld : 0;
}

IRAM_ATTR void __wrap_coex_schm_status_bit_set(uint32_t type, uint32_t status)
{
    if (type == COEX_SCHM_ST_TYPE_BLE) {
        portENTER_CRITICAL_SAFE(&s_lock);
        s_controller |= status;
        // Bits this file doesn't know pass through untouched; they are
        // reported (ble_coex_unknown_bits) as a sign the IDF changed them.
        s_unknown |= status & ~KNOWN_BITS;
        status &= ~hidden_locked();
        portEXIT_CRITICAL_SAFE(&s_lock);
        if (!status) return;
    }
    __real_coex_schm_status_bit_set(type, status);
}

IRAM_ATTR void __wrap_coex_schm_status_bit_clear(uint32_t type, uint32_t status)
{
    if (type == COEX_SCHM_ST_TYPE_BLE) {
        portENTER_CRITICAL_SAFE(&s_lock);
        s_controller &= ~status;
        portEXIT_CRITICAL_SAFE(&s_lock);
    }
    __real_coex_schm_status_bit_clear(type, status);
}

// Move from one hidden set to another: clear what is newly hidden, and hand
// back what the controller still wants and is no longer hidden.
static void apply(bool active, uint32_t withheld)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t before = hidden_locked();
    s_active = active;
    s_withheld = withheld;
    uint32_t after = hidden_locked();
    uint32_t controller = s_controller;
    portEXIT_CRITICAL(&s_lock);

    uint32_t hide = after & ~before;
    uint32_t restore = before & ~after & controller;
    if (hide) __real_coex_schm_status_bit_clear(COEX_SCHM_ST_TYPE_BLE, hide);
    if (restore) __real_coex_schm_status_bit_set(COEX_SCHM_ST_TYPE_BLE, restore);
    ESP_LOGI(TAG, "%s, BLE bits withheld 0x%" PRIx32 " (controller 0x%" PRIx32 ")",
             active ? "Wi-Fi first" : "balanced", after, controller);
    if (s_unknown) {
        ESP_LOGW(TAG, "the BLE controller set unknown coexistence bits 0x%" PRIx32
                 ": recheck ble_coex.h against this ESP-IDF", s_unknown);
    }
}

void ble_coex_set_active(bool active)
{
    apply(active, s_withheld);
}

void ble_coex_set_withheld(uint32_t bits)
{
    apply(s_active, bits & (BLE_COEX_ADV | BLE_COEX_SCAN | BLE_COEX_CONN));
}

uint32_t ble_coex_withheld(void)
{
    return s_withheld;
}

uint32_t ble_coex_controller_bits(void)
{
    return s_controller;
}

bool ble_coex_active(void)
{
    return s_active;
}

uint32_t ble_coex_unknown_bits(void)
{
    return s_unknown;
}

#endif  // CONFIG_HOMEHUB_BLE_COEX
