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
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "esp_err.h"

static int64_t clock_us, launched_us;
static bool buffer = true, async_supported = true, hw_pending, never_busy, stuck_busy, blocking;
static bool powered = true, reset_high = true, sleeping;
static unsigned begins, sleeps, finishes;
static uint8_t fill;
static std::vector<int> modes, colors;
namespace BoardConfig {
struct Display { int busy, rst, powerEnable; };
struct Profile { Display display; };
static const Profile ACTIVE = {{18, 17, 47}};
}
using gpio_num_t = int;
static int gpio_get_level(gpio_num_t pin) {
    assert(pin == 18);
    return powered && (sleeping || (hw_pending && !never_busy && clock_us-launched_us >= 20000 &&
        (stuck_busy || clock_us-launched_us < 200000)));
}
static esp_err_t gpio_set_level(gpio_num_t pin, int level) {
    if (pin == 47) powered = level;
    else { assert(pin == 17); reset_high = level; }
    return ESP_OK;
}
static int64_t esp_timer_get_time() { return clock_us; }
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(unsigned ms) { clock_us += ms * 1000; }
namespace freeink {
class FreeInkDisplay {
public:
    enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
    FreeInkDisplay(int,int,int,int,int,int) {}
    void begin() { ++begins; powered = reset_high = true; sleeping = false; }
    void deepSleep() { assert(!hw_pending); ++sleeps; sleeping = true; }
    uint8_t *getFrameBuffer() { return buffer ? &fill : nullptr; }
    bool supportsAsyncRefresh() { return async_supported; }
    void clearScreen(uint8_t color) { assert(!hw_pending); fill = color; }
    void displayBufferAsync(RefreshMode mode) {
        assert(!hw_pending); modes.push_back(mode); colors.push_back(fill);
        hw_pending = !blocking; launched_us = clock_us;
    }
    bool isRefreshPending() { return hw_pending; }
    void waitRefreshComplete() {
        assert(hw_pending && !gpio_get_level(18) && clock_us-launched_us >= 200000);
        ++finishes; hw_pending = false;
    }
};
}
#include "../components/muse/boards/sticky_power_test_display.cpp"

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *scenario = argv[1];
    if (!strcmp(scenario,"no_buffer")) buffer = false;
    if (!strcmp(scenario,"no_async")) async_supported = false;
    esp_err_t e = muse_sticky_ptest_display_init();
    assert(begins == 1 && sleeps == 1);
    if (!buffer || !async_supported) {
        assert(e == (!buffer ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED));
        assert(muse_sticky_ptest_display_start(false) == ESP_ERR_INVALID_STATE);
        return 0;
    }
    assert(e == ESP_OK);
    if (!strcmp(scenario,"seed_failure")) never_busy = true;
    bool fast = !strcmp(scenario,"fast");
    e = muse_sticky_ptest_display_start(fast);
    if (never_busy) {
        assert(e == ESP_ERR_TIMEOUT && !powered && !reset_high);
        assert(muse_sticky_ptest_display_stop() == ESP_ERR_TIMEOUT);
        return 0;
    }
    assert(e == ESP_OK && begins == 2 && finishes == 1);
    assert(modes.size() == 1 && modes[0] == freeink::FreeInkDisplay::FULL_REFRESH && colors[0] == 255);
    assert(muse_sticky_ptest_display_frames() == 0);
    assert(muse_sticky_ptest_display_start(false) == ESP_ERR_INVALID_STATE);
    if (!strcmp(scenario,"missing_busy")) never_busy = true;
    if (!strcmp(scenario,"stuck_busy")) stuck_busy = true;
    if (!strcmp(scenario,"blocking_fallback")) blocking = true;
    clock_us += 500000;
    e = muse_sticky_ptest_display_service();
    if (blocking) {
        assert(e == ESP_ERR_NOT_SUPPORTED && !powered && !reset_high);
        return 0;
    }
    assert(e == ESP_OK && hw_pending && colors.back() == 0);
    assert(modes.back() == (fast ? freeink::FreeInkDisplay::FAST_REFRESH : freeink::FreeInkDisplay::FULL_REFRESH));
    if (!strcmp(scenario,"abort") || stuck_busy || never_busy) {
        int64_t before = clock_us;
        e = muse_sticky_ptest_display_stop();
        if (stuck_busy || never_busy) {
            assert(e == ESP_ERR_TIMEOUT && !powered && !reset_high);
            assert(clock_us-before <= 8000000);
            assert(muse_sticky_ptest_display_start(false) == ESP_ERR_INVALID_STATE);
        } else assert(e == ESP_OK && !hw_pending && sleeps == 2);
        return 0;
    }
    for (unsigned i=0; i<10; ++i) { clock_us += 20000; assert(muse_sticky_ptest_display_service() == ESP_OK); }
    assert(!hw_pending && muse_sticky_ptest_display_frames() == 1);
    clock_us += 500000;
    assert(muse_sticky_ptest_display_service() == ESP_OK && hw_pending && colors.back() == 255);
    for (unsigned i=0; i<10; ++i) { clock_us += 20000; assert(muse_sticky_ptest_display_service() == ESP_OK); }
    assert(muse_sticky_ptest_display_frames() == 2);
    assert(muse_sticky_ptest_display_stop() == ESP_OK && sleeps == 2 && !hw_pending);
    assert(muse_sticky_ptest_display_stop() == ESP_OK && sleeps == 2);
    assert(gpio_get_level(18) == 1);
    assert(muse_sticky_ptest_display_start(true) == ESP_OK && begins == 3);
    assert(muse_sticky_ptest_display_stop() == ESP_OK && sleeps == 3);
}
