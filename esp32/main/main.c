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

#include "app.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
#include "esp_psram.h"
#endif
#include "diagnostic_log.h"
#if CONFIG_MUSE_POWER_TEST
#include "muse_power_test.h"
#endif
#if CONFIG_MUSE_ENABLED
#include "muse_glue.h"
#endif
#if CONFIG_HOMEHUB_LED_BACKEND_STICKY_SSD1677 || CONFIG_MUSE_BOARD_RETERMINAL_STICKY
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static void sticky_power_hold(void) {
    const gpio_config_t output = {
        .pin_bit_mask = 1ULL << GPIO_NUM_45 | 1ULL << GPIO_NUM_46,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&output));
    ESP_ERROR_CHECK(gpio_hold_dis(GPIO_NUM_45));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_45, 1));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_46, 0));
    esp_rom_delay_us(10);
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_46, 1));
    esp_rom_delay_us(10);
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_46, 0));
}
#endif

void app_main(void) {
#if CONFIG_HOMEHUB_LED_BACKEND_STICKY_SSD1677 || CONFIG_MUSE_BOARD_RETERMINAL_STICKY
    sticky_power_hold();
#endif
#if CONFIG_MUSE_POWER_TEST
    /* Opt-in standalone BSP diagnostic: no support-log persistence, normal
     * UI/voice/radio/credential or reconnect initialization may run. */
    muse_power_test_run();
    return;
#endif
#if CONFIG_HOMEHUB_SUPPORT_BUG_REPORT
    if (!diagnostic_log_init()) {
        ESP_LOGW("link.main",
                 "support log capture disabled: PSRAM unavailable");
    }
#endif
    ESP_LOGI("link.main", CONFIG_GADGET_PRODUCT_NAME " starting");
#if CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    // The C5's TCP windows are sized for lwIP buffers in PSRAM
    // (sdkconfig.defaults.esp32c5). Without it they fall back to internal RAM.
    if (!esp_psram_is_initialized()) {
        ESP_LOGE("link.main", "PSRAM did not start: network buffers fall back to internal RAM");
    }
#endif
#if CONFIG_HOMEHUB_PIPELINE_BENCH
    // Bench builds measure the data path and BLE coexistence instead of
    // running the gadget.
    extern void pipeline_bench_main(void);
    pipeline_bench_main();
    return;
#endif
#if CONFIG_MUSE_ENABLED
    muse_glue_start();
#endif
    app_run();
}
