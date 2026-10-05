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

// Host harness for components/muse/muse_hw.c: touches and the wheel in,
// events out; input capture; the light's effects. The runner compiles a copy
// of muse_hw.c against tests/muse_hw_fakes.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse_board.h"
#include "muse_hw.h"

#define MS(x) ((x) * 1000LL)

// ---- Fakes -----------------------------------------------------------------

static int64_t s_now = MS(1000);
static bool s_asleep = true;
static void (*s_timer_cb)(void *);
static void *s_timer_arg;
static bool s_timer_running;
static int s_led_calls;
static uint8_t s_led[3];

int64_t esp_timer_get_time(void) { return s_now; }
void vTaskDelay(TickType_t ticks) { s_now += MS(ticks); }
void muse_state_set_asleep(bool asleep) { s_asleep = asleep; }

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    s_timer_cb = args->callback;
    s_timer_arg = args->arg;
    *out = (esp_timer_handle_t)&s_timer_cb;
    return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) {
    (void)t;
    (void)us;
    s_timer_running = true;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t t) {
    (void)t;
    s_timer_running = false;
    return ESP_OK;
}

static int s_mutex;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_mutex; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }

static esp_err_t set_led(uint8_t r, uint8_t g, uint8_t b) {
    s_led_calls++;
    s_led[0] = r;
    s_led[1] = g;
    s_led[2] = b;
    return ESP_OK;
}
static const muse_board_t s_board = { .set_led = set_led };
const muse_board_t *muse_board;

// ---- Helpers ---------------------------------------------------------------

// A finger down at (x0, y0), moved to (x1, y1) over ms, then lifted.
static void touch(int x0, int y0, int x1, int y1, int ms) {
    muse_hw_touch(true, x0, y0);
    for (int t = 10; t <= ms; t += 10) {
        s_now += MS(10);
        muse_hw_touch(true, x0 + (x1 - x0) * t / ms, y0 + (y1 - y0) * t / ms);
    }
    muse_hw_touch(false, 0, 0);
}

static muse_hw_event_t last_event(void) {
    muse_hw_event_t ev[1];
    uint32_t seq = muse_hw_last_seq();
    assert(seq > 0);
    assert(muse_hw_events(seq - 1, ev, 1, 0) == 1);
    assert(ev[0].seq == seq);
    return ev[0];
}

// ---- Tests -----------------------------------------------------------------

static void test_gestures(void) {
    uint32_t before = muse_hw_last_seq();
    touch(100, 120, 103, 118, 120);
    muse_hw_event_t ev = last_event();
    assert(ev.seq == before + 1);
    assert(ev.type == MUSE_HW_TAP && ev.x == 100 && ev.y == 120 && ev.value == 120);
    assert(strcmp(muse_hw_event_name(ev.type), "tap") == 0);

    touch(50, 60, 52, 61, 700);
    ev = last_event();
    assert(ev.type == MUSE_HW_LONG_PRESS && ev.x == 50 && ev.y == 60 && ev.value == 700);

    touch(100, 200, 220, 205, 200);
    ev = last_event();
    assert(ev.type == MUSE_HW_SWIPE && ev.x == 100 && ev.y == 200 && ev.x2 == 220 && ev.y2 == 205);
    assert(strcmp(muse_hw_swipe_dir(&ev), "right") == 0);
    touch(300, 200, 150, 190, 200);
    ev = last_event();
    assert(strcmp(muse_hw_swipe_dir(&ev), "left") == 0);
    touch(200, 300, 205, 150, 200);
    ev = last_event();
    assert(strcmp(muse_hw_swipe_dir(&ev), "up") == 0);
    touch(200, 100, 190, 300, 200);
    ev = last_event();
    assert(strcmp(muse_hw_swipe_dir(&ev), "down") == 0);

    // A lift without a touch, or a poll after the lift, adds nothing.
    uint32_t seq = muse_hw_last_seq();
    muse_hw_touch(false, 0, 0);
    assert(muse_hw_last_seq() == seq);
}

static void test_wheel(void) {
    uint32_t seq = muse_hw_last_seq();
    muse_hw_wheel_push(false);          // a release with no press (held at boot)
    muse_hw_wheel_turn(0);
    assert(muse_hw_last_seq() == seq);

    muse_hw_wheel_push(true);
    s_now += MS(250);
    muse_hw_wheel_push(false);
    muse_hw_event_t ev = last_event();
    assert(ev.type == MUSE_HW_WHEEL_CLICK && ev.value == 250);

    muse_hw_wheel_turn(-3);
    ev = last_event();
    assert(ev.type == MUSE_HW_WHEEL_TURN && ev.value == -3);
    assert(strcmp(muse_hw_event_name(ev.type), "wheel_turn") == 0);
}

static void test_ring_and_waits(void) {
    muse_hw_event_t ev[64];
    uint32_t last = muse_hw_last_seq();

    // Nothing new: returns at once without waiting, or after the wait.
    int64_t t0 = s_now;
    assert(muse_hw_events(last, ev, 64, 0) == 0);
    assert(s_now == t0);
    assert(muse_hw_events(last, ev, 64, 100) == 0);
    assert(s_now - t0 >= MS(100) && s_now - t0 < MS(200));

    for (int i = 0; i < 40; i++) {
        muse_hw_wheel_turn(1);
    }
    // Only the last 32 are kept, oldest first, in order.
    int n = muse_hw_events(0, ev, 64, 0);
    assert(n == 32);
    assert(ev[0].seq == last + 40 - 31);
    for (int i = 1; i < n; i++) {
        assert(ev[i].seq == ev[i - 1].seq + 1);
    }
    assert(ev[n - 1].seq == muse_hw_last_seq());
    // max caps the copy, and `after` skips what was seen.
    assert(muse_hw_events(muse_hw_last_seq() - 5, ev, 3, 0) == 3);
    assert(ev[0].seq == muse_hw_last_seq() - 4);
}

static void test_capture(void) {
    assert(!muse_hw_captured());
    s_asleep = true;
    muse_hw_capture(s_now + MS(1000));
    assert(muse_hw_captured());
    assert(!s_asleep);                  // capturing wakes the screen
    s_now += MS(999);
    assert(muse_hw_captured());
    s_now += MS(2);
    assert(!muse_hw_captured());        // it lapses by itself
    muse_hw_capture(s_now + MS(5000));
    muse_hw_capture(0);
    assert(!muse_hw_captured());        // and 0 ends it
}

static void test_light(void) {
    muse_board = NULL;
    assert(!muse_hw_has_led());
    assert(!muse_hw_led(255, 0, 0, MUSE_HW_LED_SOLID, 1000));

    static const muse_board_t no_light = { 0 };
    muse_board = &no_light;
    assert(!muse_hw_has_led());
    assert(!muse_hw_led(255, 0, 0, MUSE_HW_LED_SOLID, 1000));

    muse_board = &s_board;
    assert(muse_hw_has_led());
    assert(muse_hw_led(255, 128, 0, MUSE_HW_LED_SOLID, 1000));
    assert(s_led_calls == 1 && s_led[0] == 255 && s_led[1] == 128 && s_led[2] == 0);
    assert(!s_timer_running);

    assert(muse_hw_led(9, 9, 9, MUSE_HW_LED_OFF, 1000));
    assert(s_led[0] == 0 && s_led[1] == 0 && s_led[2] == 0);

    // Blink: on for the first half of each period, off for the second.
    assert(muse_hw_led(0, 200, 100, MUSE_HW_LED_BLINK, 400));
    assert(s_timer_running);
    s_now += MS(100);
    s_timer_cb(s_timer_arg);
    assert(s_led[1] == 200 && s_led[2] == 100);
    s_now += MS(200);
    s_timer_cb(s_timer_arg);
    assert(s_led[1] == 0 && s_led[2] == 0);
    s_now += MS(200);   // 500 ms: the next period's first half
    s_timer_cb(s_timer_arg);
    assert(s_led[1] == 200);

    // Breathe: dark at the start of a breath, full at its middle.
    assert(muse_hw_led(100, 0, 0, MUSE_HW_LED_BREATHE, 1000));
    s_timer_cb(s_timer_arg);
    assert(s_led[0] == 0);
    s_now += MS(500);
    s_timer_cb(s_timer_arg);
    assert(s_led[0] >= 99);
    s_now += MS(250);
    s_timer_cb(s_timer_arg);
    assert(s_led[0] > 10 && s_led[0] < 60);

    // Solid again stops the effect.
    assert(muse_hw_led(1, 2, 3, MUSE_HW_LED_SOLID, 0));
    assert(!s_timer_running && s_led[0] == 1 && s_led[1] == 2 && s_led[2] == 3);
}

int main(void) {
    test_gestures();
    test_wheel();
    test_ring_and_waits();
    test_capture();
    test_light();
    printf("ok\n");
    return 0;
}
