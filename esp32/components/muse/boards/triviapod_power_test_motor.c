/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* Power-test motor worker: M5IOE1 IO9/PWM1 at 2 kHz and 60 % duty, 30 and
 * 70 ms pulses. It runs independently of the sweep task. Failed stops retry every 10ms;
 * any failed write/readback, overlong pulse or guard expiry invalidates a run.
 * Software cannot guarantee a stop if the MCU or I2C bus no longer works. */
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "triviapod_power_test_motor.h"
#include "muse_triviapod_power.h"
#include "boards/muse_power_test_board.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
static esp_err_t s_error;
static bool s_on, s_valid, s_stop_pending;
static uint8_t s_mode, s_pulses;
static uint16_t s_max_on_ms;
static int64_t s_start_us, s_on_us, s_off_us, s_next_us;
static unsigned s_phase;
/* Bounded by the 302s capture + <=60s settle: at most 146 pulse starts. */
static struct { int64_t start_us, stop_us; } s_edges[160];
static unsigned s_edges_count;
static int64_t s_trace_start_us;

static void emit_edges(void)
{
    char line[240];
    for (unsigned i = 0; i < s_edges_count; i++) {
        int n = snprintf(line, sizeof(line), "PTEST {\"type\":\"motor_pulse\",\"state_start_us\":%" PRId64 ",\"index\":%u,\"requested_ms\":%u,\"write_start_us\":%" PRId64 ",\"stop_verified_us\":%" PRId64 "}\n",
            s_trace_start_us, i, i & 1 ? 70 : 30, s_edges[i].start_us, s_edges[i].stop_us);
        if (n > 0 && (size_t)n < sizeof(line)) { muse_ptest_board_console_write(line, (size_t)n); }
    }
    s_edges_count = 0;
}

uint32_t muse_ptest_board_capture_ms(const muse_ptest_state_t *state, uint32_t settle_ms, uint32_t requested_ms)
{
    if (!strcmp(state->id, "motor_on_60pct") && settle_ms + requested_ms > 25000u) { return 0; }
    if (!strcmp(state->id, "motor_idle_between") || !strcmp(state->id, "motor_pulses_30_70ms")
        || !strcmp(state->id, "motor_idle_post")) { return 302000; }
    return requested_ms;
}

/* Caller owns s_lock, or runs during boot before worker allocation. */
static esp_err_t motor_set(bool on)
{
    int64_t before = esp_timer_get_time();
    /* A failed enable may still energize the motor: remember stop first. */
    if (on) { s_stop_pending = true; s_on_us = before; }
    esp_err_t err = muse_triviapod_motor(on);
    s_valid = !err;
    if (err) { if (!s_error) { s_error = err; } s_stop_pending = true; return err; }
    if (!on && s_on) {
        uint64_t ms = (esp_timer_get_time() - s_on_us + 999) / 1000;
        if (ms > s_max_on_ms) { s_max_on_ms = ms > UINT16_MAX ? UINT16_MAX : ms; }
        if (s_mode == 2 && ms > 120 && !s_error) { s_error = ESP_ERR_TIMEOUT; }
    }
    if (s_mode == 2) {
        if (on && s_edges_count < sizeof(s_edges) / sizeof(s_edges[0])) {
            s_edges[s_edges_count].start_us = before;
            s_edges[s_edges_count++].stop_us = 0;
        } else if (on) { s_error = ESP_ERR_INVALID_STATE; }
        else if (s_on && s_edges_count) { s_edges[s_edges_count - 1].stop_us = esp_timer_get_time(); }
    }
    s_on = on;
    s_stop_pending = on;
    return ESP_OK;
}

/* One scheduling step is exposed to the host harness by including this file.
 * Deadlines use absolute time; missed edges never become a burst of catch-up
 * pulses. Matched idle states execute the same schedule and off readbacks. */
static void motor_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (s_error) { (void)motor_set(false); return; }
    if (s_mode == 1) {
        if (now - s_start_us >= 30000000) {
            s_error = ESP_ERR_TIMEOUT;
            (void)motor_set(false);
        }
        return;
    }
    if (s_mode != 2 && s_mode != 3) { return; }
    if (s_off_us && now >= s_off_us) {
        s_off_us = 0;
        if (motor_set(false) || s_error) { return; }
    }
    if (now < s_next_us) { return; }
    if (now - s_next_us > 20000) { s_error = ESP_ERR_TIMEOUT; (void)motor_set(false); return; }
    bool reply = s_phase++ & 1;
    unsigned width_ms = reply ? 70 : 30;
    if (motor_set(s_mode == 2)) { return; }
    if (s_mode == 2) { s_pulses++; }
    /* Hold for the requested width after the enabling I/O has completed,
     * just like the production write-then-delay sequence. Configuration and
     * readback latency must not shorten the physical enabled interval. */
    s_off_us = esp_timer_get_time() + width_ms * 1000;
    s_next_us += reply ? 4000000 : 1000000;
}

static void motor_worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        motor_tick();
        bool active = s_mode || s_stop_pending;
        xSemaphoreGive(s_lock);
        if (active) { vTaskDelay(pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1); }
        else { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); }
    }
}

esp_err_t muse_triviapod_ptest_motor_apply(const muse_ptest_state_t *state)
{
    if (s_lock) { xSemaphoreTake(s_lock, portMAX_DELAY); }
    esp_err_t err = motor_set(false);
    s_mode = 0;
    s_off_us = 0;
    /* Preserve a failed guard/stop as cleanup evidence, even if retry succeeds. */
    if (!err) { err = s_error; }
    bool continuous = state && !strcmp(state->id, "motor_on_60pct");
    bool pulses = state && !strcmp(state->id, "motor_pulses_30_70ms");
    bool matched = state && (!strcmp(state->id, "motor_idle_between") || !strcmp(state->id, "motor_idle_post"));
    if (s_lock) { xSemaphoreGive(s_lock); }
    /* Console output is outside every capture and after the motor is stopped.
     * No state is active and the worker cannot alter the completed trace. */
    emit_edges();
    if (err || (!continuous && !pulses && !matched)) { return err; }
    if (!s_lock) { s_lock = xSemaphoreCreateMutex(); }
    if (!s_lock) { return ESP_ERR_NO_MEM; }
    if (!s_worker && xTaskCreate(motor_worker, "ptest_motor", 4096, NULL, 4, &s_worker) != pdPASS) { return ESP_ERR_NO_MEM; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode = continuous ? 1 : pulses ? 2 : 3;
    s_pulses = 0;
    s_max_on_ms = 0;
    s_start_us = s_next_us = s_trace_start_us = esp_timer_get_time();
    s_phase = 0;
    if (continuous) { err = motor_set(true); }
    xTaskNotifyGive(s_worker);
    xSemaphoreGive(s_lock);
    return err;
}

void muse_triviapod_ptest_motor_readback(muse_ptest_readback_t *out)
{
    if (s_lock) { xSemaphoreTake(s_lock, portMAX_DELAY); }
    out->motor_valid = s_valid;
    out->motor_mode = s_mode;
    out->motor_duty = s_on ? 0x8999 : 0;
    out->motor_pulses = s_pulses;
    out->motor_max_on_ms = s_max_on_ms;
    if (s_lock) { xSemaphoreGive(s_lock); }
}
esp_err_t muse_triviapod_ptest_motor_service(void)
{
    if (s_lock) { xSemaphoreTake(s_lock, portMAX_DELAY); }
    esp_err_t err = s_error;
    if (s_lock) { xSemaphoreGive(s_lock); }
    return err;
}
void muse_triviapod_ptest_motor_retry(void)
{
    /* If allocation/early boot failed there is no worker to retry a stop. */
    if (!s_worker && s_stop_pending) { (void)motor_set(false); }
}
