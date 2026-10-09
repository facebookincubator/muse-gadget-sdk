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
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Diagnostic BSP loads, NOT a model of the normal application's power.
 * USB bridge UART: 115200 baud, newline-delimited commands/replies.
 *   >ptest.arm={"run_id":"bench-1","settle_ms":5000,"capture_ms":20000,"repeats":1}
 *   >ptest.status[=<request_id>]    >ptest.results    >ptest.abort
 * Optional request_id: 1..32 alphanumeric/_/- characters, echoed in status
 * so a host can reject queued replies during clock/RTT synchronization.
 * Replies: one JSON object per line after "PTEST ", schema=1.
 * Arm ACK is not the run start. First absent VBUS read starts the sweep;
 * last_usb_present_us..vbus_removed_us brackets that edge (100ms gate poll).
 * All *_us timestamps are esp_timer_get_time(), microseconds since this boot.
 * Firmware labels only successful device-evidenced windows. Skips have no dwell.
 * Reconnecting VBUS aborts an active sweep within ~1s plus bounded I/O and
 * restores radio-off resting. No BLE, credentials, NVS, or deep sleep required.
 */
#define MUSE_PTEST_RUN_ID_MAX 32
#define MUSE_PTEST_MAX_REPEATS 2
#define MUSE_PTEST_MAX_RECORDS 56

typedef enum {
    MUSE_PTEST_IDLE,
    MUSE_PTEST_LCD,
    MUSE_PTEST_CODECS_IDLE,
    MUSE_PTEST_MIC,
    MUSE_PTEST_SINE,
    MUSE_PTEST_ADC_RAIL,
    MUSE_PTEST_ADC_SAMPLE,
    MUSE_PTEST_WIFI_SCAN,
    MUSE_PTEST_UNSUPPORTED,
} muse_ptest_load_t;

typedef struct {
    const char *id;
    const char *note;
    muse_ptest_load_t load;
    int cpu_mhz;             /* 0 = DFS 40..240 + automatic light sleep */
    int backlight_pct;       /* -1 = rail off, 0 = panel black + rail on */
    bool amplifier;
} muse_ptest_state_t;

typedef struct {
    bool usb, charging;
    uint16_t input, output, direction;
    int backlight_duty;      /* -1 if no PWM has been initialized */
    bool codecs_open;
    int configured_volume;  /* API programmed, not an analog measurement */
    int battery_mv;         /* 0 if unavailable or divider rail off */
    uint32_t io_frames;      /* successful I2S frames during this state */
    uint32_t adc_samples;
    uint32_t wifi_scans;
    bool wifi_started;
    int pm_min_mhz, pm_max_mhz;
    bool auto_light_sleep;
    int cpu_readback_mhz;    /* instantaneous clock at the boundary */
    int64_t slept_us;        /* measured automatic light sleep, if enabled */
    uint32_t sleep_count;
} muse_ptest_readback_t;

/* Never returns to app_run/muse_glue. Compiled only in opt-in Watcher builds. */
void muse_watcher_power_test_run(void);
