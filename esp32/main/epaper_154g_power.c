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

#include "epaper_154g_power.h"
#include "sdkconfig.h"
#include "epaper_154g_rotation.h"
#include "led_status.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#if CONFIG_HOMEHUB_VOICE
#include "voice_epaper_154g.h"
#endif

static bool s_button_ready;
static epaper_154g_power_button_t s_button;

#if CONFIG_PM_ENABLE
#include <stdatomic.h>
#include "noise_control.h"
#include "driver/usb_serial_jtag.h"
#include "esp_pm.h"
#include "esp_sleep.h"

static atomic_bool s_ready;
static bool s_low;

static esp_err_t update_power(void) {
    bool low = !usb_serial_jtag_is_connected();
    if (s_ready && low == s_low) return ESP_OK;
    const esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = low ? CONFIG_XTAL_FREQ : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = low,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err == ESP_OK) {
        s_low = low;
        noise_ctrl_set_power_save(low);
        ESP_LOGI("link.power", "CPU %d-%d MHz, automatic light sleep %s, BOOT wake",
                 pm.min_freq_mhz, pm.max_freq_mhz, low ? "on" : "off (USB host)");
        s_ready = true;
    }
    return err;
}

esp_err_t epaper_154g_power_init(void) {
    if (!s_button_ready) return ESP_ERR_INVALID_STATE;
    esp_err_t err = gpio_wakeup_enable(CONFIG_HOMEHUB_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) return err;
    err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) return err;
    return update_power();
}

bool epaper_154g_power_idle(void) {
    return s_ready && update_power() == ESP_OK && s_low;
}
#endif

esp_err_t epaper_154g_power_button_init(void) {
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << EPAPER_154G_POWER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&cfg);
#if CONFIG_PM_ENABLE
    if (err == ESP_OK) err = gpio_wakeup_enable(EPAPER_154G_POWER_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
#endif
    s_button_ready = err == ESP_OK;
    return err;
}

void epaper_154g_power_button_poll(bool boot_pressed) {
#if CONFIG_PM_ENABLE
    epaper_154g_power_idle();
#endif
    if (!s_button_ready) return;
    epaper_154g_power_action_t action = epaper_154g_power_button_update(
        &s_button, gpio_get_level(EPAPER_154G_POWER_BUTTON_GPIO) == 0,
        boot_pressed, esp_timer_get_time());
    if (action == EPAPER_154G_POWER_ROTATE) epaper_154g_status_rotate();
#if CONFIG_HOMEHUB_VOICE
    if (action == EPAPER_154G_POWER_PAGE) epaper_154g_status_next_page();
    if (action != EPAPER_154G_POWER_NONE) voice_epaper_154g_button_cue(action == EPAPER_154G_POWER_ROTATE);
#endif
}
