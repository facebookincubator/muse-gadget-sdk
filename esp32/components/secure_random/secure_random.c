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

/* The security draws' one source: include/secure_random.h, README.md. */

#include "secure_random.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"

#if CONFIG_BT_CONTROLLER_ENABLED
#include "esp_bt.h"
#endif

/* Wi-Fi counts only where it's linked: a BLE-only image keeps the Wi-Fi stack
 * out (esp32/AGENTS.md, BLE only), so it never names a Wi-Fi symbol. */
#if CONFIG_ESP_WIFI_ENABLED && !CONFIG_MUSE_GADGET_BLE_STANDALONE
#define SECURE_RANDOM_WIFI 1
#include "esp_wifi.h"
#endif

/* The ESP32-P4 has no radio of its own (its Wi-Fi is a coprocessor's):
 * ESP-IDF keeps its SAR ADC entropy source on from the application's start
 * (ESP-IDF's random.rst, Startup), so every draw is a true RNG's. */
#if CONFIG_IDF_TARGET_ESP32P4
#define SECURE_RANDOM_SAR_ADC 1
#endif

/* Never a build whose every security draw is refused without a word: it needs
 * a radio this file can ask about, or a chip whose entropy needs none. */
#if !CONFIG_BT_CONTROLLER_ENABLED && !SECURE_RANDOM_WIFI && !SECURE_RANDOM_SAR_ADC
#error "secure_random: neither this chip's BT controller nor Wi-Fi in the build: no radio to vouch for a draw"
#endif

static const char *TAG = "secure_random";
static atomic_bool s_refusal_logged;

bool secure_random_radio_on(void)
{
#if SECURE_RANDOM_SAR_ADC
    return true;
#else
#if CONFIG_BT_CONTROLLER_ENABLED
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        return true;
    }
#endif
#if SECURE_RANDOM_WIFI
    /* A read-only getter that answers only once esp_wifi_start() ran:
     * ESP_ERR_WIFI_NOT_INIT before esp_wifi_init(), ESP_ERR_WIFI_NOT_STARTED
     * before the start or after esp_wifi_stop(). */
    int8_t power;
    if (esp_wifi_get_max_tx_power(&power) == ESP_OK) {
        return true;
    }
#endif
    return false;
#endif
}

bool secure_random_ready(void)
{
    if (secure_random_radio_on()) {
        atomic_store(&s_refusal_logged, false);
        return true;
    }
    if (!atomic_exchange(&s_refusal_logged, true)) {
        ESP_LOGE(TAG, "no radio on: refusing a security draw");
    }
    return false;
}

bool secure_random(void *out, size_t n)
{
    if (n == 0) {
        return true;
    }
    if (out == NULL) {
        return false;
    }
    if (!secure_random_ready()) {
        memset(out, 0, n);
        return false;
    }
    esp_fill_random(out, n);
    return true;
}
