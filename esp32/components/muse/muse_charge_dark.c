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

/* Charge dark on the device: see muse_charge_dark.h. */

#include "muse_charge_dark.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_board.h"
#include "sdkconfig.h"

static const char *TAG = "muse_charge";

/* Set at the first hold check of a power-up. RTC memory keeps it across every
 * ESP32 reset (a flash, a crash, a deep-sleep wake) and loses it with power,
 * so only a power-up the power chip started can hold. */
#define CD_SEEN_MAGIC 0x4D434447u   /* "MCDG" */
static RTC_NOINIT_ATTR uint32_t s_seen[2];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void muse_charge_dark_hold(void)
{
#if CONFIG_MUSE_CHARGE_DARK
    bool fresh = !(s_seen[0] == CD_SEEN_MAGIC && s_seen[1] == ~CD_SEEN_MAGIC);
    s_seen[0] = CD_SEEN_MAGIC;
    s_seen[1] = ~CD_SEEN_MAGIC;
    const muse_board_t *b = muse_board_get();
    if (!b || !b->early_power || b->battery_shutdown_mv <= 0) {
        return;
    }
    muse_cd_reading_t r = { 0 };
    esp_err_t err = b->early_power(&r);   /* every boot: it logs the power-on source once */
    if (!fresh) {
        return;   /* an ESP32 reset, not a power-up */
    }
    int healthy_mv = muse_cd_healthy_mv(b->battery_shutdown_mv);
#if CONFIG_MUSE_CHARGE_DARK_TEST_MV > 0
    healthy_mv = CONFIG_MUSE_CHARGE_DARK_TEST_MV;
    ESP_LOGW(TAG, "bench: charge dark until %d mV (the board's %d mV)", healthy_mv,
             muse_cd_healthy_mv(b->battery_shutdown_mv));
#endif
    muse_cd_state_t st = { 0 };
    muse_cd_why_t why;
    muse_cd_action_t a = muse_cd_decide(&st, err == ESP_OK, &r, healthy_mv, now_ms(), &why);
    if (a != MUSE_CD_WAIT) {
        ESP_LOGI(TAG, "power-up: USB %s, %d mV: booting (%s)", r.usb ? (r.usb_on ? "turned it on" : "present") : "absent",
                 r.battery_mv, muse_cd_why_name(why));
        return;
    }
    ESP_LOGW(TAG, "charge dark: USB turned it on with a low cell (%d mV); no display or radio until it reads %d mV",
             r.battery_mv, healthy_mv);
    for (int n = 1;; n++) {
        vTaskDelay(pdMS_TO_TICKS(MUSE_CD_POLL_MS));
        r = (muse_cd_reading_t){ 0 };
        err = b->early_power(&r);
        a = muse_cd_decide(&st, err == ESP_OK, &r, healthy_mv, now_ms(), &why);
        if (a == MUSE_CD_WAIT) {
            if (n % MUSE_CD_LOG_EVERY == 0) {
                ESP_LOGI(TAG, "charge dark: %d mV, USB %s, %lld s", r.battery_mv, r.usb ? "on" : "off",
                         (long long)((now_ms() - st.since_ms) / 1000));
            }
            continue;
        }
        if (a == MUSE_CD_OFF) {
            ESP_LOGW(TAG, "charge dark: USB gone at %d mV: powering off", r.battery_mv);
            if (b->early_power_off) {
                b->early_power_off();
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            ESP_LOGE(TAG, "charge dark: power-off failed: booting");
            return;
        }
        ESP_LOGI(TAG, "charge dark over (%s) at %d mV: booting", muse_cd_why_name(why), r.battery_mv);
        return;
    }
#endif
}
