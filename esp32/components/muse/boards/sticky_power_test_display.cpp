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
#include "sticky_power_test_display.h"
#include <FreeInkDisplay.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
freeink::FreeInkDisplay display(0, 0, 0, 0, 0, 0);
constexpr int64_t ASSERT_US = 100000;
constexpr int64_t REFRESH_US = 8000000;
constexpr int64_t REST_US = 500000;
bool ready, asleep, active, pending, saw_busy, black;
int64_t started_us, next_us;
uint32_t completed;
esp_err_t fault;
freeink::FreeInkDisplay::RefreshMode refresh_mode;

gpio_num_t busy_pin() { return static_cast<gpio_num_t>(BoardConfig::ACTIVE.display.busy); }
bool busy() { return gpio_get_level(busy_pin()) != 0; }

esp_err_t fail(esp_err_t error)
{
    /* A failed refresh cannot be followed by more SPI commands or another run. */
    fault = error;
    active = false;
    gpio_set_level(static_cast<gpio_num_t>(BoardConfig::ACTIVE.display.rst), 0);
    gpio_set_level(static_cast<gpio_num_t>(BoardConfig::ACTIVE.display.powerEnable), 0);
    return error;
}

esp_err_t launch(freeink::FreeInkDisplay::RefreshMode mode, uint8_t fill)
{
    if (busy()) { return fail(ESP_ERR_INVALID_STATE); }
    display.clearScreen(fill);
    started_us = esp_timer_get_time();
    display.displayBufferAsync(mode);
    pending = display.isRefreshPending();
    saw_busy = busy();
    /* Refuse the library's blocking fallback: it has no observable BUSY edge. */
    if (!pending) { return fail(ESP_ERR_NOT_SUPPORTED); }
    return ESP_OK;
}

esp_err_t poll_pending()
{
    if (!pending) { return ESP_OK; }
    int64_t now = esp_timer_get_time();
    bool high = busy();
    saw_busy |= high;
    if (now - started_us >= REFRESH_US) { return fail(ESP_ERR_TIMEOUT); }
    if (high) { return ESP_OK; }
    if (!saw_busy) {
        return now - started_us >= ASSERT_US ? fail(ESP_ERR_TIMEOUT) : ESP_OK;
    }
    /* BUSY has asserted and deasserted; finish only the driver's deferred sync. */
    display.waitRefreshComplete();
    if (busy()) { return fail(ESP_ERR_INVALID_STATE); }
    pending = false;
    ++completed;
    next_us = esp_timer_get_time() + REST_US;
    return ESP_OK;
}

esp_err_t drain()
{
    while (pending) {
        esp_err_t e = poll_pending();
        if (e) { return e; }
        if (pending) { vTaskDelay(pdMS_TO_TICKS(10)); }
    }
    return ESP_OK;
}
} // namespace

esp_err_t muse_sticky_ptest_display_init(void)
{
    ready = active = pending = false;
    fault = ESP_OK;
    display.begin();
    if (busy()) { return fail(ESP_ERR_TIMEOUT); }
    display.deepSleep();
    asleep = true;
    if (!display.getFrameBuffer()) { return ESP_ERR_NO_MEM; }
    if (!display.supportsAsyncRefresh()) { return ESP_ERR_NOT_SUPPORTED; }
    ready = true;
    return ESP_OK;
}

esp_err_t muse_sticky_ptest_display_start(bool fast)
{
    if (!ready || fault || active || pending) { return ESP_ERR_INVALID_STATE; }
    if (asleep) {
        display.begin();
        asleep = false;
    }
    if (!display.getFrameBuffer()) { return fail(ESP_ERR_NO_MEM); }
    /* Seed a completed white FULL before either row. FAST then has a known
     * differential baseline, and the seed is outside settling/capture. */
    esp_err_t e = launch(freeink::FreeInkDisplay::FULL_REFRESH, 0xff);
    if (!e) { e = drain(); }
    if (e) { return e; }
    completed = 0;
    refresh_mode = fast ? freeink::FreeInkDisplay::FAST_REFRESH : freeink::FreeInkDisplay::FULL_REFRESH;
    black = true;
    active = true;
    return ESP_OK;
}

esp_err_t muse_sticky_ptest_display_service(void)
{
    if (!ready || !active || fault) { return ESP_ERR_INVALID_STATE; }
    esp_err_t e = poll_pending();
    if (e || pending || esp_timer_get_time() < next_us) { return e; }
    e = launch(refresh_mode, black ? 0x00 : 0xff);
    if (!e) { black = !black; }
    return e;
}

esp_err_t muse_sticky_ptest_display_stop(void)
{
    active = false;
    if (fault) { return fail(fault); }
    if (!ready || asleep) { return ESP_OK; }
    esp_err_t e = drain();
    if (e) { return e; }
    display.deepSleep();
    /* SSD1677 holds BUSY high in deep sleep; completion was checked above. */
    asleep = true;
    return ESP_OK;
}

uint32_t muse_sticky_ptest_display_frames(void) { return completed; }
