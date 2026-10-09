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
 * Optional fifth arm key: "matrix":"peripheral" (default, version 1) or
 * "sleep" (version 2, one repeat only: external cold codec state is irreversible).
 * Event timestamps use esp_timer_get_time() + timeline_offset_us, continuing
 * the run epoch across validated timer-deep resumes with RTC uncertainty.
 * ACK/status/results identify matrix/version/resumes/offset/uncertainty; plan
 * entries identify knobs, deep_sleep, poll_ms, role and ref_group.
 * Firmware labels only successful device-evidenced windows. Skips have no dwell.
 * VBUS abort polling is 1s, or 5s in best_poll_5s, plus bounded I/O; timer-deep
 * states cannot poll until wake. UART parking is undone on the next VBUS or
 * uncertain-power sample and on revert/finish/abort. No BLE/credentials/NVS.
 * Only a matching checksum-valid deep_sleep_pending journal with timer wake
 * and deep-sleep reset resumes. Other interrupted boots stay incomplete_reboot.
 */
#define MUSE_PTEST_RUN_ID_MAX 32
#define MUSE_PTEST_MAX_REPEATS 2
#define MUSE_PTEST_MAX_RECORDS 54
#define MUSE_PTEST_RTC_RECOVERY_BYTES 16 /* independent pad ownership + sticky warm history */
#define MUSE_PTEST_CODEC_SLOTS 26
#define MUSE_PTEST_ERROR_LOG_MAX 96

typedef struct {
    uint8_t dac[7], adc[4];
    uint8_t dac_mask, adc_mask, adc_variant; /* 0 unknown, 1 ES7243, 2 ES7243E */
    bool expected; /* value evidence only; transport failures are separate errors */
} muse_ptest_codec_regs_t;

typedef enum { MUSE_PTEST_CODEC_KEEP, MUSE_PTEST_CODEC_COLD, MUSE_PTEST_CODEC_INITIALIZED, MUSE_PTEST_CODEC_SUSPENDED } muse_ptest_codec_t;
enum {
    MUSE_PTEST_I2S_LOW = 1u << 0, MUSE_PTEST_I2S_HIZ = 1u << 1,
    MUSE_PTEST_UART_HIZ = 1u << 2, MUSE_PTEST_RGB_LOW = 1u << 3,
    MUSE_PTEST_PULLS_OFF = 1u << 4, MUSE_PTEST_UNUSED_HIZ = 1u << 5,
    MUSE_PTEST_GPIO_ISOLATE = 1u << 6, MUSE_PTEST_CPU_PD = 1u << 7,
    MUSE_PTEST_GPIO_HOLD = 1u << 8,
};

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
    MUSE_PTEST_DEEP_SLEEP,
} muse_ptest_load_t;

typedef struct {
    const char *id;
    const char *note;
    muse_ptest_load_t load;
    int cpu_mhz;             /* 0 = DFS 40..240 + automatic light sleep */
    int backlight_pct;       /* -1 = rail off, 0 = panel black + rail on */
    bool amplifier;
    uint16_t knobs;
    uint16_t poll_ms;
    muse_ptest_codec_t codec_policy;
    const char *role, *ref_group;
} muse_ptest_state_t;

typedef struct {
    bool usb, charging;
    uint16_t input, output, direction;
    int backlight_duty;      /* -1 if no PWM has been initialized */
    bool codecs_open;
    uint8_t codec_state;     /* driver sequence; register bytes are evidence only */
    uint16_t knobs_applied;  /* successful API/config readback, not measured savings */
    int configured_volume;  /* API programmed, not an analog measurement */
    int battery_mv;         /* 0 if unavailable or divider rail off */
    uint32_t io_frames;      /* successful I2S frames during this state */
    uint32_t adc_samples;
    uint32_t wifi_scans;
    bool wifi_started;
    int16_t pm_min_mhz, pm_max_mhz;
    bool auto_light_sleep;
    int16_t cpu_readback_mhz; /* instantaneous clock at the boundary */
    int64_t slept_us;        /* measured automatic light sleep, if enabled */
    uint32_t sleep_count;
} muse_ptest_readback_t;

/* Never returns to app_run/muse_glue. Compiled only in opt-in Watcher builds. */
void muse_watcher_power_test_run(void);
