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

#include "muse_restart.h"
#include "muse_bootguard.h"
#include "app.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
#include "esp_psram.h"
#endif
#if CONFIG_HOMEHUB_DMA_RESERVE_KB > 0
#include <stdlib.h>
#include <sys/param.h>
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_psram.h"
#endif
#include "diagnostic_log.h"
#if CONFIG_HOMEHUB_ERROR_REPORT
#include "error_report.h"
#endif
#include "link_events.h"
#if CONFIG_MUSE_POWER_TEST
#include "muse_power_test.h"
#endif
#if CONFIG_MUSE_ENABLED
#include "muse_charge_dark.h"
#include "muse_glue.h"
#else
#include "button.h"
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

#if CONFIG_HOMEHUB_DMA_RESERVE_KB > 0
// Internal RAM kept for DMA-capable and internal-only requests: its own heap,
// whose caps leave out MALLOC_CAP_DEFAULT so malloc() never takes from it. A
// constructor runs before main_task, while ESP-IDF still counts a new heap in
// heap_caps_get_minimum_free_size() (v6.0.3 counts later ones as empty).
__attribute__((constructor)) static void reserve_dma_pool(void) {
    if (!esp_psram_is_initialized()) {
        return;
    }
    static const uint32_t caps[] = {
        0, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT,
    };
    // Carve from the regular heaps only: with DEFAULT in the mask, a chunk
    // can't come out of the reserve registered on an earlier pass.
    const uint32_t src = MALLOC_CAP_DEFAULT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    // Pieces of at least 1 KB: a smaller one may not make a working heap, so a
    // remainder under 1 KB stays in the regular heap.
    const size_t min_piece = 1024;
    size_t left = (size_t)CONFIG_HOMEHUB_DMA_RESERVE_KB * 1024;
    while (left >= min_piece) {
        size_t n = MIN(heap_caps_get_largest_free_block(src), left);
        void *p = n >= min_piece ? heap_caps_malloc(n, src) : NULL;
        if (p && heap_caps_add_region_with_caps(caps, (intptr_t)p, (intptr_t)p + n - 1) != ESP_OK) {
            heap_caps_free(p);
            p = NULL;
        }
        if (!p) {
            ESP_EARLY_LOGE("link.main", "DMA reserve: %u bytes not reserved", (unsigned)left);
            abort();
        }
        left -= n;
    }
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
    if (!link_events_init()) {
        ESP_LOGW("link.main", "connectivity history disabled: PSRAM unavailable");
    }
#if CONFIG_HOMEHUB_ERROR_REPORT
    error_report_init();
#endif
    muse_boot_mark("app_main");
    ESP_LOGI("link.main", CONFIG_GADGET_PRODUCT_NAME " starting");
    muse_restart_log_boot("link.main");   /* how it began: an intentional restart, a crash, power-on */
    /* Before any app code: a crash loop escapes to the ROM's download
     * mode (USB host attached) or SAFE mode (no Muse UI or audio; BLE setup
     * and update stay). CONFIG_MUSE_BOOTGUARD, muse_bootguard.h. */
    if (muse_bootguard_begin() == MUSE_BOOT_SAFE) {
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        ESP_LOGW("link.main", "SAFE mode: Muse is off; BLE setup, musegadgets and the BLE update stay up");
#elif CONFIG_MUSE_ENABLED
        ESP_LOGW("link.main", "SAFE mode: Muse and the Link's commands are off; Wi-Fi, the Link session "
                              "(device.health) and the console stay up");
#else
        ESP_LOGW("link.main", "SAFE mode: the Link's commands are off; Wi-Fi, the Link session "
                              "(device.health), setup BLE, the console and the button stay up");
#endif
    }
#if CONFIG_MUSE_ENABLED
    /* USB turned a low-cell gadget on: charge dark (no display, radio
     * or advert) until the cell is healthy, before Link or BLE start. The
     * guard has run; USB recovery answers throughout. SAFE mode never holds:
     * recovery comes first. muse_charge_dark.h. */
    if (muse_bootguard_mode() != MUSE_BOOT_SAFE) {
        muse_charge_dark_hold();
    }
#endif
#if CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP && CONFIG_SPIRAM_IGNORE_NOTFOUND
    // The C5's TCP windows are sized for lwIP buffers in PSRAM. Its defaults
    // (sdkconfig.defaults.esp32c5) require PSRAM at boot; a config that lets
    // it boot without falls back to internal RAM.
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
    muse_boot_mark("guard");
#if CONFIG_MUSE_ENABLED
    muse_glue_start();   /* the board's SAFE key watch first (muse_bootguard_entry_watch) */
#else
    button_safe_entry_watch();   /* the Link button: the way into SAFE from the boot's start */
#endif
    muse_boot_mark("app_run");
    app_run();
}
