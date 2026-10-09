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
#include "muse_watcher_power_test.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MUSE_PTEST_HOST
#include "esp_attr.h"
#else
#define RTC_NOINIT_ATTR
#endif

/* The core below also runs against a deterministic host fake. The only
 * platform boundary is these operations; tests execute this exact FSM/parser. */
static int64_t fw_now(void);
static esp_err_t fw_power(muse_ptest_readback_t *out);
static esp_err_t fw_apply(const muse_ptest_state_t *state);
static esp_err_t fw_readback(muse_ptest_readback_t *out);
static esp_err_t fw_service(muse_ptest_load_t load);
static void fw_write(const char *buf, size_t n);
static int64_t fw_timer_now(void);
static int64_t fw_rtc_now(void);
static bool fw_timer_reset(int reset_reason);
static esp_err_t fw_prepare_deep(const muse_ptest_state_t *state, int64_t us);
static void fw_enter_deep(void);
static void fw_codec_regs(muse_ptest_codec_regs_t *out);
static void fw_dryrun_mode(bool on);
static esp_err_t fw_flush(void);
static bool s_dryrun;
static char s_last_error_log[MUSE_PTEST_ERROR_LOG_MAX + 1];
static char s_error_log_fragment[MUSE_PTEST_ERROR_LOG_MAX + 1];
static size_t s_error_log_used;
static unsigned s_error_log_escape;
static void log_reset(void)
{
    s_last_error_log[0] = s_error_log_fragment[0] = 0;
    s_error_log_used = s_error_log_escape = 0;
}
static int capture_error_log(const char *format, va_list args)
{
    char rendered[192] = {0};
    int n = vsnprintf(rendered, sizeof(rendered), format, args);
    /* V1 is one callback; V2 can split prefix/body/newline. Never let a
     * framing-only newline erase the body, and never forward to UART. */
    for (size_t i = 0; rendered[i]; i++) {
        unsigned char c = (unsigned char)rendered[i];
        if (s_error_log_escape) {
            if (s_error_log_escape == 1) { s_error_log_escape = c == '[' ? 2 : 0; }
            else if (c >= 0x40 && c <= 0x7e) { s_error_log_escape = 0; }
            continue;
        }
        if (c == 27) { s_error_log_escape = 1; continue; } /* ANSI framing, including post-newline reset */
        if (c == '\r' || c == '\n') {
            if (s_error_log_used) { memcpy(s_last_error_log, s_error_log_fragment, s_error_log_used + 1); }
            s_error_log_used = 0;
            s_error_log_fragment[0] = 0;
            continue;
        }
        if (s_error_log_used < MUSE_PTEST_ERROR_LOG_MAX) {
            s_error_log_fragment[s_error_log_used++] = c >= 32 && c <= 126 && c != '"' && c != '\\' ? (char)c : '_';
            s_error_log_fragment[s_error_log_used] = 0;
        }
    }
    if (s_error_log_used) { memcpy(s_last_error_log, s_error_log_fragment, s_error_log_used + 1); }
    return n;
}

#define STATE(id, note, load, mhz, bl, amp) { id, note, load, mhz, bl, amp, 0, 1000, MUSE_PTEST_CODEC_KEEP, "other", "" }
static const muse_ptest_state_t s_matrix[] = {
    STATE("baseline_pre", "radio-off, codecs uninitialized/closed, LCD+ADC rail off, DFS + automatic light sleep", MUSE_PTEST_IDLE, 0, -1, false),
    STATE("cpu_fixed_40_idle", "fixed CPU clock, idle (not a busy loop)", MUSE_PTEST_IDLE, 40, -1, false),
    STATE("cpu_fixed_80_idle", "fixed CPU clock, idle", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("cpu_fixed_160_idle", "fixed CPU clock, idle", MUSE_PTEST_IDLE, 160, -1, false),
    STATE("cpu_fixed_240_idle", "fixed CPU clock, idle", MUSE_PTEST_IDLE, 240, -1, false),
    STATE("baseline_cpu_post", "paired DFS baseline after fixed clocks", MUSE_PTEST_IDLE, 0, -1, false),
    STATE("lcd_rail_off_pre", "LCD and touch rail off, fixed 80MHz", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("lcd_black_bl_0", "LCD rail on, static black, backlight 0%, no LVGL/touch task", MUSE_PTEST_LCD, 80, 0, false),
    STATE("lcd_black_bl_25", "LCD rail on, static black, backlight 25%", MUSE_PTEST_LCD, 80, 25, false),
    STATE("lcd_black_bl_50", "LCD rail on, static black, backlight 50%", MUSE_PTEST_LCD, 80, 50, false),
    STATE("lcd_black_bl_100", "LCD rail on, static black, backlight 100%", MUSE_PTEST_LCD, 80, 100, false),
    STATE("lcd_rail_off_post", "paired LCD rail-off after brightness sequence", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("codecs_closed_pre", "always-powered codec rail, no initialized I2S yet or codecs closed", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("codecs_open_amp_off", "16kHz stereo-slot codecs open, I2S clocks on, no drain, amp off", MUSE_PTEST_CODECS_IDLE, 80, -1, false),
    STATE("codecs_open_amp_on", "same codecs and I2S, amp enabled, no playback", MUSE_PTEST_CODECS_IDLE, 80, -1, true),
    STATE("mic_capture_16k", "16kHz stereo I2S drained/discarded, right-slot mic, 0dB gain, amp off", MUSE_PTEST_MIC, 80, -1, false),
    STATE("sine_1k_minus18dbfs", "1kHz sine -18dBFS peak, volume 25/100, 16kHz stereo slots", MUSE_PTEST_SINE, 80, -1, true),
    STATE("codecs_closed_post", "paired baseline after codec close and I2S stop", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("adc_divider_rail_on", "battery divider enabled, no ADC sampling", MUSE_PTEST_ADC_RAIL, 80, -1, false),
    STATE("adc_sample_1hz", "battery divider enabled, calibrated 8-sample reading once/second", MUSE_PTEST_ADC_SAMPLE, 80, -1, false),
    STATE("adc_divider_off_post", "paired ADC rail-off baseline", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("wifi_scanning", "unassociated active scans, no credentials/NVS; completed scans counted", MUSE_PTEST_WIFI_SCAN, 80, -1, false),
    STATE("wifi_associated_idle", "skipped: no known AP credentials; never load production NVS", MUSE_PTEST_UNSUPPORTED, 80, -1, false),
    STATE("ble_advertising", "skipped: standalone NimBLE lifecycle not characterized; controller never initialized", MUSE_PTEST_UNSUPPORTED, 80, -1, false),
    STATE("camera_capture", "skipped: coprocessor/SSCMA lifecycle excluded from minimum sweep; AI rail stays off", MUSE_PTEST_UNSUPPORTED, 80, -1, false),
    STATE("deep_sleep_timer", "skipped: no deep-sleep reset or auto-resume in this sweep; use measured automatic light sleep", MUSE_PTEST_UNSUPPORTED, 0, -1, false),
    STATE("baseline_post", "radio-off resting, LCD+ADC+amp+AI off, codecs closed, DFS + automatic light sleep", MUSE_PTEST_IDLE, 0, -1, false),
};
#define MATRIX_COUNT (sizeof(s_matrix) / sizeof(s_matrix[0]))
_Static_assert(MATRIX_COUNT * MUSE_PTEST_MAX_REPEATS <= MUSE_PTEST_MAX_RECORDS, "result capacity");

#define SLEEP_STATE(id, note, knobs, codec, role, group, poll, load) \
    { id, note, load, 0, -1, false, knobs, poll, codec, role, group }
#define BEST_KNOBS (MUSE_PTEST_I2S_LOW | MUSE_PTEST_UART_HIZ | MUSE_PTEST_RGB_LOW | MUSE_PTEST_PULLS_OFF | MUSE_PTEST_UNUSED_HIZ | MUSE_PTEST_GPIO_ISOLATE | MUSE_PTEST_CPU_PD)
static const muse_ptest_state_t s_sleep_matrix[] = {
    SLEEP_STATE("cold_ref", "untouched codec and I2S state; cold means no audio constructor has run", 0, MUSE_PTEST_CODEC_COLD, "other", "", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("cold_i2s_low", "cold codec, I2S outputs low and DIN pulldown; reference for hiz", MUSE_PTEST_I2S_LOW, MUSE_PTEST_CODEC_COLD, "ref", "cold", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("cold_i2s_hiz", "cold codec, I2S pads floating", MUSE_PTEST_I2S_HIZ, MUSE_PTEST_CODEC_COLD, "variant", "cold", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("cold_i2s_low_b", "paired cold I2S-low reference", MUSE_PTEST_I2S_LOW, MUSE_PTEST_CODEC_COLD, "ref", "cold", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("codec_initialized", "audio constructors only, never stream-opened; ADC constructor powers ADC; I2S stopped", 0, MUSE_PTEST_CODEC_INITIALIZED, "other", "", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("codec_suspended", "16kHz codecs opened then closed; shutdown registers checked; always-on codec rail", 0, MUSE_PTEST_CODEC_SUSPENDED, "other", "", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref", "warm baseline_post policy; no parking knobs", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("uart_hiz", "UART0 floating only without VBUS; rerouted immediately on USB", MUSE_PTEST_UART_HIZ, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_2", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("rgb_low", "GPIO40 DIN driven low; powered LED is not rail-off", MUSE_PTEST_RGB_LOW, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_3", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("pulls_off", "internal I2C0/EXP_INT pulls off; external pulls retained; knob pads hiz, no wake", MUSE_PTEST_PULLS_OFF, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_4", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("unused_hiz", "unowned Himax/shared SD pads floating, both rails off", MUSE_PTEST_UNUSED_HIZ, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_5", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("gpio_isolate", "sleep-pad isolation, LCD/touch rail-off outputs stay low", MUSE_PTEST_GPIO_ISOLATE, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_6", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("cpu_pd", "CPU retention power-down permitted; other vetoes/eligibility still apply", MUSE_PTEST_CPU_PD, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_7", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("best_combined", "all safe sleep knobs, I2S low", BEST_KNOBS, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_8", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("best_combined_b", "repeat best-combined variant", BEST_KNOBS, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("best_poll_5s", "best-combined with five-second VBUS poll; boundary timers remain", BEST_KNOBS, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 5000, MUSE_PTEST_IDLE),
    /* Final A closes both late B variants; explicitly requested for A/B/A. */
    SLEEP_STATE("warm_ref_9", "final paired warm reference for best-combined-b and five-second polling", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("deep_sleep_timer", "timer-only reset, sleep-pad isolation; no held I2S/RGB outputs", MUSE_PTEST_GPIO_ISOLATE, MUSE_PTEST_CODEC_SUSPENDED, "other", "", 0, MUSE_PTEST_DEEP_SLEEP),
    SLEEP_STATE("deep_sleep_timer_held", "timer-only reset; I2S outputs and RGB low held across deep sleep", MUSE_PTEST_GPIO_ISOLATE | MUSE_PTEST_I2S_LOW | MUSE_PTEST_RGB_LOW | MUSE_PTEST_GPIO_HOLD, MUSE_PTEST_CODEC_SUSPENDED, "other", "", 0, MUSE_PTEST_DEEP_SLEEP),
};
#define SLEEP_MATRIX_COUNT (sizeof(s_sleep_matrix) / sizeof(s_sleep_matrix[0]))
typedef enum { MATRIX_PERIPHERAL, MATRIX_SLEEP } matrix_t;
static const muse_ptest_state_t *matrix_states(matrix_t m) { return m == MATRIX_SLEEP ? s_sleep_matrix : s_matrix; }
static size_t matrix_count(matrix_t m) { return m == MATRIX_SLEEP ? SLEEP_MATRIX_COUNT : MATRIX_COUNT; }
static const char *matrix_name(matrix_t m) { return m == MATRIX_SLEEP ? "sleep" : "peripheral"; }
static unsigned matrix_version(matrix_t m) { return m == MATRIX_SLEEP ? 2 : 1; }

typedef enum { RUN_IDLE, RUN_ARMED, RUN_RUNNING, RUN_COMPLETE, RUN_ABORTED, RUN_ERROR, RUN_REBOOT } run_status_t;
typedef enum { REC_PENDING, REC_SETTLE, REC_CAPTURE, REC_OK, REC_SKIPPED, REC_ERROR, REC_ABORTED, REC_SLEEP_SKIPPED, REC_DEEP_PENDING } rec_status_t;
static const char *const s_run_names[] = { "idle", "armed", "running", "complete", "aborted", "error", "incomplete_reboot" };
static const char *const s_rec_names[] = { "pending", "settling", "capturing", "ok", "skipped", "error", "aborted", "skipped", "deep_sleep_pending" };
typedef struct {
    int64_t enter_us, applied_us, measure_start_us, end_us;
    int64_t timeline_uncertainty_us;
    muse_ptest_readback_t actual;
    uint32_t boot_id;
    int error, cleanup_error;
    uint16_t state_index, repeat;
    uint8_t status, codec_slot;
} record_t;
typedef struct {
    int64_t programmed_us, entry_rtc_us, rtc_slept_us;
    uint32_t entry_boot_id, resume_boot_id;
    uint16_t record_pos;
    bool resumed;
} deep_info_t;
typedef struct {
    char run_id[MUSE_PTEST_RUN_ID_MAX + 1];
    uint32_t boot_id;
    int reset_reason;
    run_status_t status;
    matrix_t matrix;
    uint32_t settle_ms, capture_ms, repeats, count;
    int64_t ack_us, last_usb_present_us, vbus_removed_us, sequence_start_us, finished_us;
    int64_t timeline_offset_us, timeline_uncertainty_us;
    int64_t checkpoint_virtual_us, checkpoint_rtc_us;
    uint32_t resume_count, deep_count;
    uint8_t codec_history;
    int error, restore_error;
    uint8_t codec_count;
    uint16_t error_record;
    char last_error_log[MUSE_PTEST_ERROR_LOG_MAX + 1];
    muse_ptest_codec_regs_t codec_regs[MUSE_PTEST_CODEC_SLOTS];
    deep_info_t deep[2]; /* sleep matrix is one repeat: cold cannot be recreated */
    record_t records[MUSE_PTEST_MAX_RECORDS];
} run_t;

#define SAVED_MAGIC 0x50544553u
#define SAVED_VERSION 3u
static RTC_NOINIT_ATTR struct {
    uint32_t magic, version, size, checksum;
    run_t run;
} s_saved;
/* ESP32-S3 RTC slow memory is 8KiB; leave >=512B for SDK metadata. */
_Static_assert(sizeof(s_saved) + MUSE_PTEST_RTC_RECOVERY_BYTES <= 7680, "diagnostic RTC results exceed budget");
static run_t s_run;
static uint32_t s_boot_id;
static int s_reset_reason;
static bool s_restored, s_resume_pending, s_release_deep_holds;
static int64_t s_timeline_offset_us, s_timeline_uncertainty_us;
static int64_t s_next_power_us, s_capture_slept_us;
static uint32_t s_capture_sleeps, s_capture_frames, s_capture_adc, s_capture_scans;
static esp_err_t s_init_error;
static unsigned fw_codec_history(void);
static void remember_error(void)
{
    if (!s_run.last_error_log[0]) {
        memcpy(s_run.last_error_log, s_last_error_log, sizeof(s_run.last_error_log));
        s_run.error_record = s_run.count ? (uint16_t)(s_run.count - 1) : UINT16_MAX;
    }
}
static void capture_codec_record(record_t *r)
{
    muse_ptest_codec_regs_t snapshot = {0};
    fw_codec_regs(&snapshot);
    for (unsigned i = 0; i < s_run.codec_count; i++) {
        if (!memcmp(&snapshot, &s_run.codec_regs[i], sizeof(snapshot))) { r->codec_slot = (uint8_t)i; return; }
    }
    for (unsigned i = 0; i < MUSE_PTEST_CODEC_SLOTS; i++) {
        bool used = false;
        for (unsigned q = 0; q < s_run.count; q++) {
            if (&s_run.records[q] != r && s_run.records[q].codec_slot == i) { used = true; break; }
        }
        if (!used) {
            s_run.codec_regs[i] = snapshot;
            r->codec_slot = (uint8_t)i;
            if (s_run.codec_count <= i) { s_run.codec_count = (uint8_t)(i + 1); }
            return;
        }
    }
    r->codec_slot = UINT8_MAX; /* v1 >26 distinct snapshots: explicitly unavailable */
}
static void start_next(void);
static int64_t fw_now(void) { return fw_timer_now() + s_timeline_offset_us; }
static int64_t rtc_uncertainty(int64_t us) { return (us + 99) / 100 + 50000; }
static const muse_ptest_state_t *run_state(unsigned index) { return &matrix_states(s_run.matrix)[index]; }
static const muse_ptest_state_t *resting_state(void) { return &s_matrix[MATRIX_COUNT - 1]; }
static deep_info_t *deep_for(unsigned pos)
{
    for (unsigned i = 0; i < s_run.deep_count; i++) { if (s_run.deep[i].record_pos == pos) { return &s_run.deep[i]; } }
    return NULL;
}

static uint32_t checksum(const void *ptr, size_t n)
{
    const uint8_t *p = ptr;
    uint32_t h = 2166136261u;
    while (n--) { h = (h ^ *p++) * 16777619u; }
    return h;
}

/* Only boundary writes: never NVS, never a periodic flash flood. A reset
 * during a copy invalidates the checksum instead of passing partial data.
 * RTC checkpoints also allow a retained completed journal to be retrieved
 * after a non-deep USB reset without pretending esp_timer kept running. */
static void save_run(void)
{
    unsigned history = fw_codec_history();
    if (history > s_run.codec_history) { s_run.codec_history = (uint8_t)history; }
    s_run.timeline_offset_us = s_timeline_offset_us;
    s_run.timeline_uncertainty_us = s_timeline_uncertainty_us;
    s_run.checkpoint_virtual_us = fw_now();
    s_run.checkpoint_rtc_us = fw_rtc_now();
    s_saved.magic = 0;
    s_saved.run = s_run;
    s_saved.version = SAVED_VERSION;
    s_saved.size = sizeof(run_t);
    s_saved.checksum = checksum(&s_saved.run, sizeof(run_t));
    s_saved.magic = SAVED_MAGIC;
}

static void boot_results(uint32_t boot_id, int reset_reason)
{
    s_boot_id = boot_id;
    s_reset_reason = reset_reason;
    s_restored = s_resume_pending = s_release_deep_holds = false;
    s_timeline_offset_us = s_timeline_uncertainty_us = 0;
    memset(&s_run, 0, sizeof(s_run));
    if (s_saved.magic == SAVED_MAGIC && s_saved.version == SAVED_VERSION && s_saved.size == sizeof(run_t)
        && s_saved.checksum == checksum(&s_saved.run, sizeof(run_t))
        && s_saved.run.count <= MUSE_PTEST_MAX_RECORDS
        && (unsigned)s_saved.run.status <= RUN_REBOOT
        && (unsigned)s_saved.run.matrix <= MATRIX_SLEEP
        && s_saved.run.settle_ms >= 1000 && s_saved.run.settle_ms <= 60000
        && s_saved.run.capture_ms >= 1000 && s_saved.run.capture_ms <= 120000
        && s_saved.run.repeats >= 1 && s_saved.run.repeats <= MUSE_PTEST_MAX_REPEATS
        && (s_saved.run.matrix != MATRIX_SLEEP || s_saved.run.repeats == 1)
        && s_saved.run.count <= matrix_count(s_saved.run.matrix) * s_saved.run.repeats
        && memchr(s_saved.run.run_id, 0, sizeof(s_saved.run.run_id))
        && s_saved.run.codec_history <= MUSE_PTEST_CODEC_SUSPENDED
        && s_saved.run.codec_count <= MUSE_PTEST_CODEC_SLOTS
        && memchr(s_saved.run.last_error_log, 0, sizeof(s_saved.run.last_error_log))
        && (s_saved.run.error_record == UINT16_MAX || s_saved.run.error_record < s_saved.run.count)
        && s_saved.run.timeline_uncertainty_us >= 0 && s_saved.run.deep_count <= 2
        && s_saved.run.resume_count <= s_saved.run.deep_count) {
        s_run = s_saved.run;
        bool valid = true;
        for (const unsigned char *p = (const unsigned char *)s_run.last_error_log; *p; p++) {
            valid &= *p >= 32 && *p <= 126 && *p != '"' && *p != '\\';
        }
        for (unsigned i = 0; i < s_run.codec_count; i++) {
            const muse_ptest_codec_regs_t *c = &s_run.codec_regs[i];
            valid &= c->adc_variant <= 2 && c->dac_mask <= 0x7f
                && c->adc_mask <= (c->adc_variant == 1 ? 7 : c->adc_variant == 2 ? 15 : 0)
                && ((const uint8_t *)&c->expected)[0] <= 1;
        }
        for (unsigned i = 0; i < s_run.count; i++) {
            valid &= s_run.records[i].state_index == i % matrix_count(s_run.matrix)
                && s_run.records[i].repeat == i / matrix_count(s_run.matrix)
                && (unsigned)s_run.records[i].status <= REC_DEEP_PENDING
                && (s_run.records[i].codec_slot == UINT8_MAX || s_run.records[i].codec_slot < s_run.codec_count)
                && s_run.records[i].timeline_uncertainty_us >= 0;
        }
        unsigned resumed = 0;
        for (unsigned i = 0; i < s_run.deep_count; i++) {
            deep_info_t *d = &s_run.deep[i];
            valid &= d->record_pos < s_run.count
                && d->programmed_us == (int64_t)(s_run.settle_ms + s_run.capture_ms) * 1000
                && d->rtc_slept_us >= 0;
            if (valid && d->record_pos < s_run.count) {
                valid &= run_state(s_run.records[d->record_pos].state_index)->load == MUSE_PTEST_DEEP_SLEEP;
            }
            if (i) { valid &= d->record_pos > s_run.deep[i - 1].record_pos; }
            resumed += d->resumed;
        }
        valid &= resumed == s_run.resume_count;
        if (!valid) { memset(&s_run, 0, sizeof(s_run)); s_next_power_us = 0; return; }
        s_restored = true;
        int64_t rtc = fw_rtc_now();
        /* RTC continuity is not assumed on power-on reset (reason 1). */
        if (reset_reason != 1 && s_run.checkpoint_rtc_us > 0 && rtc >= s_run.checkpoint_rtc_us) {
            int64_t delta = rtc - s_run.checkpoint_rtc_us;
            s_timeline_offset_us = s_run.checkpoint_virtual_us + delta - fw_timer_now();
            s_timeline_uncertainty_us = s_run.timeline_uncertainty_us + rtc_uncertainty(delta);
        }
        if (s_run.count) {
            record_t *r = &s_run.records[s_run.count - 1];
            deep_info_t *d = deep_for(s_run.count - 1);
            s_release_deep_holds = d && (run_state(r->state_index)->knobs & MUSE_PTEST_GPIO_HOLD);
            if (s_run.status == RUN_RUNNING && s_run.matrix == MATRIX_SLEEP && r->status == REC_DEEP_PENDING
                && d && !d->resumed && d->entry_boot_id == r->boot_id && d->entry_rtc_us > 0
                && d->programmed_us == (int64_t)(s_run.settle_ms + s_run.capture_ms) * 1000
                && run_state(r->state_index)->load == MUSE_PTEST_DEEP_SLEEP && fw_timer_reset(reset_reason)) {
                int64_t delta = rtc - d->entry_rtc_us;
                int64_t uncertainty = delta > 0 ? rtc_uncertainty(delta) : 0;
                if (delta > 0 && delta + uncertainty >= d->programmed_us && delta <= d->programmed_us + 10000000) {
                    d->rtc_slept_us = delta;
                    d->resume_boot_id = boot_id;
                    s_timeline_offset_us = r->enter_us + delta - fw_timer_now();
                    s_timeline_uncertainty_us = s_run.timeline_uncertainty_us + uncertainty;
                    s_resume_pending = true; /* VBUS and initialization must still pass. */
                }
            }
        }
        if ((s_run.status == RUN_ARMED || s_run.status == RUN_RUNNING) && !s_resume_pending) {
            s_run.status = RUN_REBOOT;
            s_run.error = ESP_ERR_INVALID_STATE;
            if (s_run.count) {
                record_t *r = &s_run.records[s_run.count - 1];
                if (r->status <= REC_CAPTURE || r->status == REC_DEEP_PENDING) {
                    r->status = REC_ABORTED;
                    r->error = ESP_ERR_INVALID_STATE;
                    /* end_us remains 0: no matched wake survived the reboot. */
                }
            }
            save_run();
        }
    }
    s_next_power_us = 0;
}

static void emit(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0 && n < (int)sizeof(buf)) { fw_write(buf, (size_t)n); }
}

static void emit_actual(const muse_ptest_readback_t *a)
{
    emit("{\"usb\":%s,\"charging\":%s,\"expander_input\":%u,\"expander_output\":%u,\"expander_direction\":%u,",
         a->usb ? "true" : "false", a->charging ? "true" : "false", a->input, a->output, a->direction);
    emit("\"backlight_duty_1023\":%d,\"codecs_open\":%s,\"configured_volume\":%d,\"battery_mv\":%d,\"io_frames\":%" PRIu32 ",\"adc_samples\":%" PRIu32 ",",
         a->backlight_duty, a->codecs_open ? "true" : "false", a->configured_volume, a->battery_mv, a->io_frames, a->adc_samples);
    emit("\"wifi_started\":%s,\"wifi_scans\":%" PRIu32 ",\"pm_min_mhz\":%d,\"pm_max_mhz\":%d,\"auto_light_sleep\":%s,\"cpu_readback_mhz\":%d,\"slept_us\":%" PRId64 ",\"sleep_count\":%" PRIu32 ",",
         a->wifi_started ? "true" : "false", a->wifi_scans, a->pm_min_mhz, a->pm_max_mhz,
         a->auto_light_sleep ? "true" : "false", a->cpu_readback_mhz, a->slept_us, a->sleep_count);
    static const char *const codec_names[] = { "unknown", "cold", "initialized", "suspended" };
    emit("\"knobs_applied\":%u,\"codec_state\":\"%s\"}", a->knobs_applied,
         a->codec_state <= MUSE_PTEST_CODEC_SUSPENDED ? codec_names[a->codec_state] : "unknown");
}

static void emit_timeline(void)
{
    emit("\"matrix\":\"%s\",\"matrix_version\":%u,\"resume_count\":%" PRIu32 ",\"timeline_offset_us\":%" PRId64 ",\"timeline_uncertainty_us\":%" PRId64 ",",
         matrix_name(s_run.matrix), matrix_version(s_run.matrix), s_run.resume_count,
         s_timeline_offset_us, s_timeline_uncertainty_us);
}

static void emit_codec_regs(const muse_ptest_codec_regs_t *regs);
static void emit_codec_live(void)
{
    muse_ptest_codec_regs_t regs = {0};
    fw_codec_regs(&regs);
    emit_codec_regs(&regs);
}
static void emit_status_request(const char *type, const char *request_id)
{
    muse_ptest_readback_t p = {0};
    esp_err_t e = fw_power(&p);
    emit("PTEST {\"schema\":1,\"type\":\"%s\",\"request_id\":\"%s\",\"boot_id\":%" PRIu32 ",\"now_us\":%" PRId64 ",\"reset_reason\":%d,\"run_id\":\"%s\",\"run_boot_id\":%" PRIu32 ",\"state\":\"%s\",\"status\":\"%s\",",
         type, request_id, s_boot_id, fw_now(), s_reset_reason, s_run.run_id, s_run.boot_id, s_run_names[s_run.status], s_run_names[s_run.status]);
    emit_timeline();
    emit_codec_live();
    emit(",\"last_error_log\":\"%s\",", (s_init_error || s_run.error || s_run.restore_error) ? (s_last_error_log[0] ? s_last_error_log : s_run.last_error_log) : "");
    emit("\"restored_from_rtc\":%s,\"record_count\":%" PRIu32 ",\"power_error\":%d,\"init_error\":%d,\"usb\":%s,\"charging\":%s,\"expander_input\":%u,\"sequence_start_us\":%" PRId64 ",\"error\":%d,\"restore_error\":%d}\n",
         s_restored ? "true" : "false", s_run.count, e, s_init_error, p.usb ? "true" : "false", p.charging ? "true" : "false", p.input, s_run.sequence_start_us, s_run.error, s_run.restore_error);
}

static void emit_status(const char *type) { emit_status_request(type, ""); }

static void emit_error(const char *message, esp_err_t error)
{
    emit("PTEST {\"schema\":1,\"type\":\"error\",\"error\":%d,\"message\":\"%s\"}\n", error, message);
}

static void emit_knobs(const muse_ptest_state_t *s)
{
    static const char *const names[] = { "i2s_low", "i2s_hiz", "uart_hiz", "rgb_low", "pulls_off", "unused_hiz", "gpio_isolate", "cpu_pd", "gpio_hold" };
    emit("[");
    bool comma = false;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (s->knobs & (1u << i)) { emit("%s\"%s\"", comma ? "," : "", names[i]); comma = true; }
    }
    if (s->codec_policy != MUSE_PTEST_CODEC_KEEP) {
        static const char *const codec[] = { "", "codec_cold", "codec_initialized", "codec_suspended" };
        emit("%s\"%s\"", comma ? "," : "", codec[s->codec_policy]);
    }
    emit("]");
}

static void emit_plan(void)
{
    emit("[");
    for (size_t i = 0; i < matrix_count(s_run.matrix); i++) {
        const muse_ptest_state_t *s = run_state(i);
        emit("%s{\"index\":%u,\"id\":\"%s\",\"timed\":%s,\"cpu_mhz\":%d,\"backlight_pct\":%d,\"amplifier\":%s,\"note\":\"%s\",",
             i ? "," : "", (unsigned)i, s->id, s->load == MUSE_PTEST_UNSUPPORTED ? "false" : "true", s->cpu_mhz, s->backlight_pct, s->amplifier ? "true" : "false", s->note);
        emit("\"deep_sleep\":%s,\"poll_ms\":%u,\"role\":\"%s\",\"ref_group\":\"%s\",\"knobs\":",
             s->load == MUSE_PTEST_DEEP_SLEEP ? "true" : "false", s->poll_ms, s->role, s->ref_group);
        emit_knobs(s);
        emit("}");
    }
    emit("]");
}

static void emit_codec_regs(const muse_ptest_codec_regs_t *regs)
{
    const muse_ptest_codec_regs_t empty = {0};
    bool available = regs != NULL;
    if (!regs) { regs = &empty; }
    static const uint8_t dac_keys[] = {0x00,0x01,0x02,0x0d,0x0e,0x12,0x14};
    static const uint8_t old_keys[] = {0x00,0x05,0x06}, new_keys[] = {0x00,0x01,0x04,0xf9};
    emit("\"codec_regs_available\":%s,\"codec_regs\":{\"dac\":{", available ? "true" : "false");
    for (unsigned i = 0; i < 7; i++) {
        emit("%s\"%02x\":", i ? "," : "", (unsigned)dac_keys[i]);
        if (regs->dac_mask & (1u << i)) { emit("%u", (unsigned)regs->dac[i]); } else { emit("null"); }
    }
    emit("},\"adc\":{");
    unsigned n = regs->adc_variant == 1 ? 3 : regs->adc_variant == 2 ? 4 : 0;
    for (unsigned i = 0; i < n; i++) {
        emit("%s\"%02x\":", i ? "," : "", (unsigned)(regs->adc_variant == 1 ? old_keys[i] : new_keys[i]));
        if (regs->adc_mask & (1u << i)) { emit("%u", (unsigned)regs->adc[i]); } else { emit("null"); }
    }
    emit("},\"adc_variant\":\"%s\"},\"codec_regs_expected\":%s", regs->adc_variant == 1 ? "es7243" : regs->adc_variant == 2 ? "es7243e" : "unknown", regs->expected ? "true" : "false");
}

static void emit_results(void)
{
    muse_ptest_readback_t power = {0};
    esp_err_t power_error = fw_power(&power);
    emit("PTEST {\"schema\":1,\"type\":\"results\",\"run_id\":\"%s\",\"boot_id\":%" PRIu32 ",\"run_boot_id\":%" PRIu32 ",\"retrieval_boot_id\":%" PRIu32 ",\"now_us\":%" PRId64 ",",
         s_run.run_id, s_run.boot_id, s_run.boot_id, s_boot_id, fw_now());
    emit_timeline();
    emit_codec_live();
    emit(",\"last_error_log\":\"%s\",", (s_init_error || s_run.error || s_run.restore_error) ? (s_last_error_log[0] ? s_last_error_log : s_run.last_error_log) : "");
    emit("\"reset_reason\":%d,\"retrieval_reset_reason\":%d,\"state\":\"%s\",\"status\":\"%s\",\"complete\":%s,\"usb\":%s,\"power_error\":%d,",
         s_run.reset_reason, s_reset_reason, s_run_names[s_run.status], s_run_names[s_run.status],
         s_run.status == RUN_COMPLETE ? "true" : "false", power.usb ? "true" : "false", power_error);
    emit("\"scope\":\"isolated_BSP_loads_not_production\",\"timestamp_unit\":\"us_since_run_boot\",\"ack_us\":%" PRId64 ",\"last_usb_present_us\":%" PRId64 ",\"vbus_removed_us\":%" PRId64 ",\"sequence_start_us\":%" PRId64 ",\"finished_us\":%" PRId64 ",",
         s_run.ack_us, s_run.last_usb_present_us, s_run.vbus_removed_us, s_run.sequence_start_us, s_run.finished_us);
    emit("\"settle_ms\":%" PRIu32 ",\"capture_ms\":%" PRIu32 ",\"repeats\":%" PRIu32 ",\"usb_abort_poll_ms\":%u,\"error\":%d,\"restore_error\":%d,\"records\":[",
         s_run.settle_ms, s_run.capture_ms, s_run.repeats, s_run.matrix == MATRIX_SLEEP ? 5000 : 1000, s_run.error, s_run.restore_error);
    for (uint32_t i = 0; i < s_run.count; i++) {
        const record_t *r = &s_run.records[i];
        const muse_ptest_state_t *s = run_state(r->state_index);
        emit("%s{\"index\":%u,\"repeat\":%u,\"name\":\"%s\",\"id\":\"%s\",\"status\":\"%s\",\"valid\":%s,\"apply_err\":%d,\"error\":%d,\"cleanup_error\":%d,",
             i ? "," : "", r->state_index, r->repeat, s->id, s->id, s_rec_names[r->status], r->status == REC_OK ? "true" : "false", r->error, r->error, r->cleanup_error);
        emit("\"boot_id\":%" PRIu32 ",\"timeline_uncertainty_us\":%" PRId64 ",\"poll_ms\":%u,\"role\":\"%s\",\"ref_group\":\"%s\",",
             r->boot_id, r->timeline_uncertainty_us, s->poll_ms, s->role, s->ref_group);
        emit("\"reason\":\"%s\",\"enter_us\":%" PRId64 ",\"applied_us\":%" PRId64 ",\"measure_start_us\":%" PRId64 ",\"end_us\":%" PRId64 ",\"note\":\"%s\",\"actual\":",
             r->status == REC_SLEEP_SKIPPED ? "automatic_light_sleep_not_observed" : "", r->enter_us, r->applied_us, r->measure_start_us, r->end_us, s->note);
        emit_actual(&r->actual);
        emit(",");
        emit_codec_regs(r->codec_slot < s_run.codec_count ? &s_run.codec_regs[r->codec_slot] : NULL);
        if (r->error) { emit(",\"last_error_log\":\"%s\"", i == s_run.error_record ? s_run.last_error_log : ""); }
        deep_info_t *d = deep_for(i);
        if (d) {
            emit(",\"deep_sleep\":{\"entry_boot_id\":%" PRIu32 ",\"resume_boot_id\":%" PRIu32 ",\"programmed_us\":%" PRId64 ",\"rtc_slept_us\":%" PRId64 ",\"wake_cause\":\"%s\",\"resume_reset_reason\":\"%s\"}",
                 d->entry_boot_id, d->resume_boot_id, d->programmed_us, d->rtc_slept_us,
                 d->resumed ? "timer" : "none", d->resumed ? "deepsleep" : "none");
        }
        emit("}");
    }
    emit("]}\n");
}

/* Strict flat JSON: four required keys and optional matrix, each once;
 * no trailing junk, escapes, floats, signs, unknown or duplicate keys. */
typedef struct { char run_id[MUSE_PTEST_RUN_ID_MAX + 1]; uint32_t settle_ms, capture_ms, repeats; matrix_t matrix; } arm_t;
static void ws(const char **p) { while (isspace((unsigned char)**p)) { (*p)++; } }
static bool quoted(const char **p, char *out, size_t capacity)
{
    ws(p);
    if (*(*p)++ != '"') { return false; }
    size_t n = 0;
    while (**p && **p != '"') {
        unsigned char c = (unsigned char)*(*p)++;
        if (!(isalnum(c) || c == '_' || c == '-') || n + 1 >= capacity) { return false; }
        out[n++] = (char)c;
    }
    if (**p != '"') { return false; }
    (*p)++;
    out[n] = 0;
    return n > 0;
}
static bool number(const char **p, uint32_t *out)
{
    ws(p);
    if (!isdigit((unsigned char)**p)) { return false; }
    uint32_t v = 0;
    if (**p == '0' && isdigit((unsigned char)(*p)[1])) { return false; }
    while (isdigit((unsigned char)**p)) {
        unsigned digit = (unsigned)*(*p)++ - '0';
        if (v > (UINT32_MAX - digit) / 10) { return false; }
        v = v * 10 + digit;
    }
    *out = v;
    return true;
}
static bool parse_arm(const char *p, arm_t *a)
{
    memset(a, 0, sizeof(*a));
    unsigned seen = 0;
    ws(&p);
    if (*p++ != '{') { return false; }
    for (;;) {
        char key[24];
        if (!quoted(&p, key, sizeof(key))) { return false; }
        ws(&p);
        if (*p++ != ':') { return false; }
        unsigned bit;
        if (!strcmp(key, "run_id")) {
            bit = 1;
            if (!quoted(&p, a->run_id, sizeof(a->run_id))) { return false; }
        } else if (!strcmp(key, "matrix")) {
            char matrix[16];
            bit = 16;
            if (!quoted(&p, matrix, sizeof(matrix))) { return false; }
            if (!strcmp(matrix, "peripheral")) { a->matrix = MATRIX_PERIPHERAL; }
            else if (!strcmp(matrix, "sleep")) { a->matrix = MATRIX_SLEEP; }
            else { return false; }
        } else {
            uint32_t *dst;
            if (!strcmp(key, "settle_ms")) { bit = 2; dst = &a->settle_ms; }
            else if (!strcmp(key, "capture_ms")) { bit = 4; dst = &a->capture_ms; }
            else if (!strcmp(key, "repeats")) { bit = 8; dst = &a->repeats; }
            else { return false; }
            if (!number(&p, dst)) { return false; }
        }
        if (seen & bit) { return false; }
        seen |= bit;
        ws(&p);
        if (*p == '}') { p++; break; }
        if (*p++ != ',') { return false; }
    }
    ws(&p);
    return !*p && (seen == 15 || seen == 31) && a->settle_ms >= 1000 && a->settle_ms <= 60000
        && a->capture_ms >= 1000 && a->capture_ms <= 120000
        && a->repeats >= 1 && a->repeats <= MUSE_PTEST_MAX_REPEATS;
}

static void finish_run(run_status_t status, esp_err_t error)
{
    s_run.status = status;
    s_run.error = error;
    s_run.finished_us = fw_now();
    if (error) { remember_error(); save_run(); } /* checkpoint before reset-prone restoration */
    /* One safe resting state, even on abort. Never drops EXP_PWR_SYSTEM. */
    s_run.restore_error = fw_apply(resting_state());
    if (!s_run.restore_error) {
        muse_ptest_readback_t restored = {0};
        s_run.restore_error = fw_readback(&restored); /* USB allowed outside RUN_RUNNING */
    }
    if (!s_run.restore_error) { log_reset(); } /* ignored component errors are not record failures */
    if (s_run.restore_error) { remember_error(); }
    if (s_run.restore_error && status == RUN_COMPLETE) { s_run.status = RUN_ERROR; }
    save_run();
}

static void abort_run(esp_err_t error, bool power_error)
{
    if (s_run.status != RUN_ARMED && s_run.status != RUN_RUNNING) { return; }
    if (s_run.count) {
        record_t *r = &s_run.records[s_run.count - 1];
        if (r->status == REC_PENDING || r->status == REC_CAPTURE || r->status == REC_SETTLE || r->status == REC_DEEP_PENDING) {
            r->end_us = fw_now();
            r->error = error;
            r->status = power_error ? REC_ERROR : REC_ABORTED;
        }
    }
    finish_run(power_error ? RUN_ERROR : RUN_ABORTED, error);
}

static void dryrun(const char *json)
{
    const char *p = json;
    char key[24], matrix[16];
    ws(&p);
    bool valid = *p++ == '{' && quoted(&p, key, sizeof(key)) && !strcmp(key, "matrix");
    if (valid) { ws(&p); valid = *p++ == ':' && quoted(&p, matrix, sizeof(matrix)) && !strcmp(matrix, "sleep"); }
    if (valid) { ws(&p); valid = *p == '}'; if (valid) { p++; ws(&p); valid = !*p; } }
    if (!valid) { emit_error("invalid_dryrun_json_sleep_only", ESP_ERR_INVALID_ARG); return; }
    if (s_run.status == RUN_ARMED || s_run.status == RUN_RUNNING) { emit_error("dryrun_requires_no_active_run", ESP_ERR_INVALID_STATE); return; }
    if (s_init_error) { emit_error("board_initialization_failed", s_init_error); return; }
    muse_ptest_readback_t power = {0};
    esp_err_t e = fw_power(&power);
    if (e || !power.usb) { emit_error("dryrun_requires_verified_usb_vbus", e ? e : ESP_ERR_INVALID_STATE); return; }
    s_dryrun = true;
    fw_dryrun_mode(true);
    unsigned errors = 0, states = 0;
    esp_err_t tx_error = ESP_OK;
    char tx_log[MUSE_PTEST_ERROR_LOG_MAX + 1] = {0};
    for (unsigned i = 0; i < SLEEP_MATRIX_COUNT; i++) {
        const muse_ptest_state_t *state = &s_sleep_matrix[i];
        if (state->load == MUSE_PTEST_DEEP_SLEEP) { continue; }
        log_reset();
        muse_ptest_readback_t actual = {0}, before = {0};
        e = fw_power(&before);
        if (!e && !before.usb) { e = ESP_ERR_INVALID_STATE; }
        bool usb_ok = !e && before.usb;
        if (usb_ok) {
            e = fw_apply(state);
            esp_err_t read_error = fw_readback(&actual); /* evidence even after apply failure */
            if (!e) { e = read_error; }
            muse_ptest_readback_t after = {0};
            esp_err_t power_error = fw_power(&after);
            usb_ok = !power_error && after.usb;
            actual.usb = usb_ok;
            if (!e && !usb_ok) { e = power_error ? power_error : ESP_ERR_INVALID_STATE; }
        }
        muse_ptest_codec_regs_t regs = {0};
        fw_codec_regs(&regs);
        if (!e) { log_reset(); }
        if (e) { errors++; }
        states++;
        emit("PTEST {\"schema\":1,\"type\":\"dryrun_state\",\"matrix\":\"sleep\",\"matrix_version\":2,\"boot_id\":%" PRIu32 ",\"usb\":%s,\"pm_exercised\":false,\"index\":%u,\"id\":\"%s\",\"error\":%d,\"last_error_log\":\"%s\",\"knobs_applied\":%u,\"codec_state\":\"%s\",",
             s_boot_id, actual.usb ? "true" : "false", i, state->id, e, s_last_error_log, actual.knobs_applied,
             actual.codec_state == MUSE_PTEST_CODEC_COLD ? "cold" : actual.codec_state == MUSE_PTEST_CODEC_INITIALIZED ? "initialized" : actual.codec_state == MUSE_PTEST_CODEC_SUSPENDED ? "suspended" : "unknown");
        emit_codec_regs(&regs);
        emit("}\n");
        tx_error = fw_flush(); /* never park the next state's UART over a FIFO tail */
        if (tx_error) { memcpy(tx_log, s_last_error_log, sizeof(tx_log)); break; }
        if (!usb_ok || !actual.usb) { break; }
    }
    log_reset();
    esp_err_t restore = fw_apply(resting_state());
    muse_ptest_readback_t restored = {0};
    esp_err_t read_error = fw_readback(&restored);
    if (!restore) { restore = read_error; }
    muse_ptest_readback_t final_power = {0};
    esp_err_t power_error = fw_power(&final_power);
    restored.usb = !power_error && final_power.usb;
    if (!restore && !restored.usb) { restore = power_error ? power_error : ESP_ERR_INVALID_STATE; }
    if (!restore && tx_error) { restore = tx_error; memcpy(s_last_error_log, tx_log, sizeof(tx_log)); }
    if (!restore) { log_reset(); }
    if (restore) { errors++; }
    fw_dryrun_mode(false);
    s_dryrun = false;
    emit("PTEST {\"schema\":1,\"type\":\"dryrun_done\",\"matrix\":\"sleep\",\"matrix_version\":2,\"boot_id\":%" PRIu32 ",\"usb\":%s,\"pm_exercised\":false,\"states\":%u,\"errors\":%u,\"restore_error\":%d,\"last_error_log\":\"%s\"}\n",
         s_boot_id, restored.usb ? "true" : "false", states, errors, restore, s_last_error_log);
    esp_err_t done_tx_error = fw_flush(); /* finish final line before returning to RX */
    if (done_tx_error) { emit_error("dryrun_done_tx_failed", done_tx_error); return; }
    /* No save_run, run mutation, timing counters or deep entry. Sticky warm
     * hardware-history markers necessarily remain; power-cycle before cold. */
}

static void command(const char *line)
{
    if (!strcmp(line, ">ptest.status")) { emit_status("status"); return; }
    if (!strncmp(line, ">ptest.status=", 14)) {
        const char *request_id = line + 14;
        size_t n = strlen(request_id);
        bool valid = n > 0 && n <= MUSE_PTEST_RUN_ID_MAX;
        for (size_t i = 0; valid && i < n; i++) {
            unsigned char c = (unsigned char)request_id[i];
            valid = isalnum(c) || c == '_' || c == '-';
        }
        if (!valid) { emit_error("invalid_status_request_id", ESP_ERR_INVALID_ARG); }
        else { emit_status_request("status", request_id); }
        return;
    }
    if (!strcmp(line, ">ptest.results")) { emit_results(); return; }
    if (!strncmp(line, ">ptest.dryrun=", 14)) { dryrun(line + 14); return; }
    if (!strcmp(line, ">ptest.abort")) { abort_run(ESP_ERR_INVALID_STATE, false); emit_status("abort_ack"); return; }
    if (strncmp(line, ">ptest.arm=", 11)) { emit_error("unknown_command", ESP_ERR_INVALID_ARG); return; }
    arm_t a;
    if (!parse_arm(line + 11, &a)) { emit_error("invalid_arm_json_or_bounds", ESP_ERR_INVALID_ARG); return; }
    unsigned timed = 0, deep = 0;
    if (a.matrix == MATRIX_SLEEP && a.repeats != 1) { emit_error("sleep_requires_one_repeat_cold_not_reversible", ESP_ERR_INVALID_ARG); return; }
    for (size_t i = 0; i < matrix_count(a.matrix); i++) {
        timed += matrix_states(a.matrix)[i].load != MUSE_PTEST_UNSUPPORTED;
        deep += matrix_states(a.matrix)[i].load == MUSE_PTEST_DEEP_SLEEP;
    }
    uint32_t bound = a.repeats * (timed * (a.settle_ms + a.capture_ms + 10000u) + deep * 10000u) + 10000u;
    if (bound > 3600000u) { emit_error("arm_total_duration_exceeds_one_hour", ESP_ERR_INVALID_ARG); return; }
    if (s_run.status == RUN_ARMED || s_run.status == RUN_RUNNING) { emit_error("already_active", ESP_ERR_INVALID_STATE); return; }
    if (s_init_error) { emit_error("board_initialization_failed", s_init_error); return; }
    muse_ptest_readback_t p = {0};
    esp_err_t e = fw_power(&p);
    if (e || !p.usb) { emit_error("arm_requires_verified_usb_vbus", e ? e : ESP_ERR_INVALID_STATE); return; }
    memset(&s_run, 0, sizeof(s_run));
    s_run.error_record = UINT16_MAX;
    log_reset();
    strcpy(s_run.run_id, a.run_id);
    s_run.boot_id = s_boot_id;
    s_run.reset_reason = s_reset_reason;
    s_run.status = RUN_ARMED;
    s_run.matrix = a.matrix;
    s_run.timeline_offset_us = s_timeline_offset_us;
    s_run.timeline_uncertainty_us = s_timeline_uncertainty_us;
    s_run.settle_ms = a.settle_ms;
    s_run.capture_ms = a.capture_ms;
    s_run.repeats = a.repeats;
    s_run.ack_us = fw_now();
    s_run.last_usb_present_us = s_run.ack_us;
    s_restored = false;
    s_next_power_us = 0;
    save_run();
    emit("PTEST {\"schema\":1,\"type\":\"arm_ack\",\"status\":\"armed\",\"run_id\":\"%s\",\"boot_id\":%" PRIu32 ",\"ack_us\":%" PRId64 ",\"now_us\":%" PRId64 ",\"usb\":true,\"charging\":%s,\"expander_input\":%u,",
         a.run_id, s_boot_id, s_run.ack_us, fw_now(), p.charging ? "true" : "false", p.input);
    emit_timeline();
    emit("\"settle_ms\":%" PRIu32 ",\"capture_ms\":%" PRIu32 ",\"repeats\":%" PRIu32 ",\"gate_poll_ms\":100,\"usb_abort_poll_ms\":%u,\"max_duration_ms\":%" PRIu32 ",\"rtc_bytes\":%u,\"plan\":",
         a.settle_ms, a.capture_ms, a.repeats, a.matrix == MATRIX_SLEEP ? 5000 : 1000, bound, (unsigned)(sizeof(s_saved) + MUSE_PTEST_RTC_RECOVERY_BYTES));
    emit_plan();
    emit("}\n");
}

static esp_err_t sample(record_t *r)
{
    memset(&r->actual, 0, sizeof(r->actual));
    esp_err_t e = fw_readback(&r->actual);
    if (!e) { log_reset(); }
    capture_codec_record(r);
    return !e && r->actual.usb ? ESP_ERR_INVALID_STATE : e;
}

static void start_next(void)
{
    while (s_run.count < matrix_count(s_run.matrix) * s_run.repeats) {
        uint32_t pos = s_run.count++;
        record_t *r = &s_run.records[pos];
        r->codec_slot = UINT8_MAX;
        log_reset();
        r->state_index = pos % matrix_count(s_run.matrix);
        r->repeat = pos / matrix_count(s_run.matrix);
        r->boot_id = s_boot_id;
        r->timeline_uncertainty_us = s_timeline_uncertainty_us;
        const muse_ptest_state_t *s = run_state(r->state_index);
        r->enter_us = fw_now();
        if (s->load == MUSE_PTEST_UNSUPPORTED) {
            r->status = REC_SKIPPED;
            r->error = ESP_ERR_NOT_SUPPORTED;
            r->end_us = r->enter_us;
            save_run();
            continue;
        }
        /* Save 'pending' BEFORE setup, so a setup-time reset is incomplete. */
        save_run();
        esp_err_t e = fw_apply(s);
        r->applied_us = fw_now();
        if (!e) { e = sample(r); }
        if (e && r->actual.usb) { abort_run(e, false); return; }
        if (e) {
            r->status = e == ESP_ERR_NOT_SUPPORTED ? REC_SKIPPED : REC_ERROR;
            r->error = e;
            r->end_us = fw_now();
            remember_error();
            if (e != ESP_ERR_NOT_SUPPORTED) {
                s_run.status = RUN_ERROR;
                s_run.error = e;
                s_run.finished_us = r->end_us;
            }
            save_run(); /* first failure/log survive a reset inside cleanup */
            r->cleanup_error = fw_apply(resting_state());
            /* Unknown partial hardware state is not a safe basis to continue. */
            if (r->cleanup_error || e != ESP_ERR_NOT_SUPPORTED) { finish_run(RUN_ERROR, e); return; }
            save_run();
            continue;
        }
        if (s->load == MUSE_PTEST_DEEP_SLEEP) {
            if (s_run.deep_count >= 2) { finish_run(RUN_ERROR, ESP_ERR_INVALID_STATE); return; }
            deep_info_t *d = &s_run.deep[s_run.deep_count++];
            d->record_pos = (uint16_t)pos;
            d->entry_boot_id = s_boot_id;
            d->programmed_us = (int64_t)(s_run.settle_ms + s_run.capture_ms) * 1000;
            r->status = REC_DEEP_PENDING;
            /* Commit intent before holds/timer setup; any setup reset is
             * incomplete, never a successful timer resume. */
            save_run();
            e = fw_prepare_deep(s, d->programmed_us);
            if (!e) { e = sample(r); }
            if (e && r->actual.usb) { abort_run(e, false); return; }
            if (e) {
                r->status = REC_ERROR;
                r->error = e;
                r->end_us = fw_now();
                finish_run(RUN_ERROR, e);
                return;
            }
            r->enter_us = r->applied_us = fw_now();
            r->actual.slept_us = 0;
            r->actual.sleep_count = 0; /* deep sleep is evidenced by RTC, not LS callbacks */
            d->entry_rtc_us = fw_rtc_now();
            r->measure_start_us = r->enter_us + (int64_t)s_run.settle_ms * 1000;
            if (d->entry_rtc_us <= 0) { r->status = REC_ERROR; r->error = ESP_FAIL; finish_run(RUN_ERROR, ESP_FAIL); return; }
            save_run();
            fw_enter_deep(); /* Never returns on-device; fake waits for reset. */
            return;
        }
        r->status = REC_SETTLE;
        save_run();
        return;
    }
    finish_run(RUN_COMPLETE, ESP_OK);
}

static void resume_run(void)
{
    if (!s_resume_pending) { return; }
    s_resume_pending = false;
    record_t *r = &s_run.records[s_run.count - 1];
    deep_info_t *d = deep_for(s_run.count - 1);
    muse_ptest_readback_t p = {0};
    esp_err_t e = s_init_error ? s_init_error : fw_power(&p);
    if (e || p.usb) { abort_run(e ? e : ESP_ERR_INVALID_STATE, e != ESP_OK); return; }
    d->resumed = true;
    s_run.resume_count++;
    r->end_us = r->enter_us + d->rtc_slept_us;
    r->timeline_uncertainty_us = s_timeline_uncertainty_us;
    r->status = REC_OK;
    save_run();
    s_next_power_us = 0;
    start_next();
}

static unsigned running_poll_ms(void)
{
    if (s_run.status != RUN_RUNNING || !s_run.count) { return 1000; }
    unsigned ms = run_state(s_run.records[s_run.count - 1].state_index)->poll_ms;
    return ms ? ms : 1000;
}

/* Called frequently only for active audio. USB checks are independently
 * rate-limited, so baseline wakes once/second, never a busy loop. */
static void tick(void)
{
    if (s_run.status != RUN_ARMED && s_run.status != RUN_RUNNING) { return; }
    if (s_run.status == RUN_RUNNING && s_run.count && s_run.records[s_run.count - 1].status == REC_DEEP_PENDING) { return; }
    if (fw_now() >= s_next_power_us) {
        muse_ptest_readback_t p = {0};
        esp_err_t e = fw_power(&p);
        if (e) { abort_run(e, true); return; }
        int64_t at = fw_now();
        s_next_power_us = at + (int64_t)(s_run.status == RUN_ARMED ? 100 : running_poll_ms()) * 1000;
        if (s_run.status == RUN_ARMED) {
            if (p.usb) { s_run.last_usb_present_us = at; return; }
            s_run.vbus_removed_us = at;
            s_run.sequence_start_us = at;
            s_run.status = RUN_RUNNING;
            save_run();
            start_next();
            return;
        }
        if (p.usb) { abort_run(ESP_ERR_INVALID_STATE, false); return; }
    }
    if (s_run.status != RUN_RUNNING || !s_run.count) { return; }
    record_t *r = &s_run.records[s_run.count - 1];
    const muse_ptest_state_t *s = run_state(r->state_index);
    esp_err_t e = fw_service(s->load);
    if (e) {
        r->status = REC_ERROR;
        r->error = e;
        r->end_us = fw_now();
        finish_run(RUN_ERROR, e);
        return;
    }
    int64_t now = fw_now();
    if (r->status == REC_SETTLE && now - r->applied_us >= (int64_t)s_run.settle_ms * 1000) {
        e = sample(r);
        if (e && r->actual.usb) { abort_run(e, false); return; }
        if (!e) {
            r->measure_start_us = fw_now();
            s_capture_slept_us = r->actual.slept_us;
            s_capture_sleeps = r->actual.sleep_count;
            s_capture_frames = r->actual.io_frames;
            s_capture_adc = r->actual.adc_samples;
            s_capture_scans = r->actual.wifi_scans;
            r->status = REC_CAPTURE;
            save_run();
        }
    } else if (r->status == REC_CAPTURE && now - r->measure_start_us >= (int64_t)s_run.capture_ms * 1000) {
        e = sample(r);
        if (e && r->actual.usb) { abort_run(e, false); return; }
        r->end_us = fw_now();
        if (!e) {
            r->actual.slept_us -= s_capture_slept_us;
            r->actual.sleep_count -= s_capture_sleeps;
            r->actual.io_frames -= s_capture_frames;
            r->actual.adc_samples -= s_capture_adc;
            r->actual.wifi_scans -= s_capture_scans;
            if ((s->load == MUSE_PTEST_MIC || s->load == MUSE_PTEST_SINE) && !r->actual.io_frames) { e = ESP_FAIL; }
            if (s->load == MUSE_PTEST_ADC_SAMPLE && !r->actual.adc_samples) { e = ESP_FAIL; }
            if (s->load == MUSE_PTEST_WIFI_SCAN && !r->actual.wifi_scans) { e = ESP_FAIL; }
        }
        if (!e && !s->cpu_mhz && (r->actual.slept_us <= 0 || !r->actual.sleep_count)) {
            /* Enabled PM is configuration, not evidence the chip slept. Keep
             * the timestamps/counters but never label this window measured LS. */
            r->status = REC_SLEEP_SKIPPED;
            r->error = ESP_ERR_NOT_SUPPORTED;
            save_run();
            start_next();
            return;
        }
        r->status = e ? REC_ERROR : REC_OK;
        r->error = e;
        if (e) { remember_error(); }
        save_run();
        if (!e) { start_next(); return; }
    }
    if (e) {
        r->status = REC_ERROR;
        r->error = e;
        r->end_us = fw_now();
        finish_run(RUN_ERROR, e);
    }
}

#ifndef MUSE_PTEST_HOST
#include "boards/board_sensecap_watcher_power_test.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_random.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static esp_pm_config_t s_pm;
static esp_pm_lock_handle_t s_cpu_lock, s_usb_lock;
static bool s_usb_lock_held;
static bool s_cpu_lock_held, s_wifi_initialized, s_wifi_started;
static volatile bool s_scan_done;
static volatile uint32_t s_scans;
static esp_event_handler_instance_t s_scan_handler;
static bool s_net_ready;
static int64_t s_sleep_us;
static uint32_t s_sleep_count;
static portMUX_TYPE s_sleep_mux = portMUX_INITIALIZER_UNLOCKED;

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
static esp_err_t IRAM_ATTR slept(int64_t us, void *arg)
{
    (void)arg;
    if (us > 0) {
        portENTER_CRITICAL_ISR(&s_sleep_mux);
        s_sleep_us += us;
        s_sleep_count++;
        portEXIT_CRITICAL_ISR(&s_sleep_mux);
    }
    return ESP_OK;
}
#endif

static void scan_done(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const wifi_event_sta_scan_done_t *event = data;
    if (event && event->status == 0) { s_scans++; }
    s_scan_done = true;
}

static esp_err_t wifi_off(void)
{
    esp_err_t first = ESP_OK;
    if (s_wifi_started) {
        esp_err_t e = esp_wifi_scan_stop();
        if (e && e != ESP_ERR_WIFI_NOT_STARTED) { first = e; }
        e = esp_wifi_stop();
        if (!first && e) { first = e; }
        if (!e) { s_wifi_started = false; }
    }
    if (s_wifi_initialized && !s_wifi_started) {
        esp_err_t e = esp_wifi_deinit();
        if (!first && e) { first = e; }
        if (!e) { s_wifi_initialized = false; }
    }
    return first;
}

static esp_err_t wifi_scan(void)
{
    esp_err_t e;
    if (!s_net_ready) {
        if ((e = esp_netif_init())) { return e; }
        e = esp_event_loop_create_default();
        if (e && e != ESP_ERR_INVALID_STATE) { return e; }
        if ((e = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_done, NULL, &s_scan_handler))) { return e; }
        s_net_ready = true;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false; /* Do not touch pairing/Wi-Fi NVS or credentials. */
    if ((e = esp_wifi_init(&cfg))) { return e; }
    s_wifi_initialized = true;
    if ((e = esp_wifi_set_storage(WIFI_STORAGE_RAM)) || (e = esp_wifi_set_mode(WIFI_MODE_STA)) || (e = esp_wifi_start())) { return e; }
    s_wifi_started = true;
    s_scans = 0;
    s_scan_done = false;
    wifi_scan_config_t scan = { .scan_type = WIFI_SCAN_TYPE_ACTIVE, .scan_time.active = { .min = 20, .max = 40 } };
    return esp_wifi_scan_start(&scan, false);
}

static int64_t fw_timer_now(void) { return esp_timer_get_time(); }
static int64_t fw_rtc_now(void) { return esp_rtc_get_time_us(); }
static bool fw_timer_reset(int reason) { return reason == ESP_RST_DEEPSLEEP && esp_sleep_get_wakeup_causes() == (UINT32_C(1) << ESP_SLEEP_WAKEUP_TIMER); }
static unsigned fw_codec_history(void) { return muse_watcher_ptest_codec_history(); }
static bool s_cpu_pd_veto_held;

static esp_err_t fw_prepare_deep(const muse_ptest_state_t *state, int64_t us)
{
    esp_err_t e;
    /* RTC_NOINIT is SLOW; SDK RTC-time state is FAST. On S3/IDF 6.0.1
     * these domains have no PD capability, so sleep_modes.c never sets their
     * PD flags. On SDK/SoC variants exposing them, retain each once per boot. */
#if SOC_PM_SUPPORT_RTC_SLOW_MEM_PD
    static bool slow_held;
    if (!slow_held) {
        if ((e = esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_ON))) { return e; }
        slow_held = true;
    }
#endif
#if SOC_PM_SUPPORT_RTC_FAST_MEM_PD
    static bool fast_held;
    if (!fast_held) {
        if ((e = esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_ON))) { return e; }
        fast_held = true;
    }
#endif
    if ((e = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL))) { return e; }
    if ((e = muse_watcher_ptest_prepare_deep((state->knobs & MUSE_PTEST_GPIO_HOLD) != 0))) { return e; }
    return esp_sleep_enable_timer_wakeup((uint64_t)us);
}
static void fw_enter_deep(void) { esp_deep_sleep_start(); }
static esp_err_t fw_power(muse_ptest_readback_t *out)
{
    esp_err_t e = muse_watcher_ptest_power(out);
    if (s_dryrun || e || out->usb) {
        esp_err_t uart_error = muse_watcher_ptest_uart_restore();
        if (!e) { e = uart_error; }
    }
    /* VBUS, not UART 'connection' (the CH342 has no connection API). Keep
     * console RX awake on USB, release before the first USB-free state.
     * On uncertain power keep awake so the error/results remain readable. */
    bool awake = s_dryrun || e || out->usb;
    if (s_usb_lock && awake != s_usb_lock_held) {
        esp_err_t lock_error = awake ? esp_pm_lock_acquire(s_usb_lock) : esp_pm_lock_release(s_usb_lock);
        if (!lock_error) { s_usb_lock_held = awake; }
        if (!e) { e = lock_error; }
    }
    return e;
}
static esp_err_t fw_apply(const muse_ptest_state_t *state)
{
    esp_err_t e = wifi_off();
    if (e) { return e; }
    if (s_dryrun) { return muse_watcher_ptest_apply(state); } /* USB lock held; no PM changes */
    bool veto = !(state->knobs & MUSE_PTEST_CPU_PD);
    if (veto != s_cpu_pd_veto_held) {
        /* IDF v6.0.1 sleep_modes.c uses ON refs. OFF releases ours; AUTO
         * would erase other owners' refs. ON vetoes RTC_SLEEP_PD_CPU even
         * with PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y. OFF still needs retention. */
        if ((e = esp_sleep_pd_config(ESP_PD_DOMAIN_CPU, veto ? ESP_PD_OPTION_ON : ESP_PD_OPTION_OFF))) { return e; }
        s_cpu_pd_veto_held = veto;
    }
    if (s_cpu_lock_held) {
        if ((e = esp_pm_lock_release(s_cpu_lock))) { return e; }
        s_cpu_lock_held = false;
    }
    const esp_pm_config_t pm = {
        .min_freq_mhz = state->cpu_mhz ? state->cpu_mhz : 40,
        .max_freq_mhz = state->cpu_mhz ? state->cpu_mhz : 240,
        .light_sleep_enable = state->cpu_mhz == 0,
    };
    if ((e = esp_pm_configure(&pm))) { return e; }
    s_pm = pm;
    if (state->cpu_mhz) {
        if ((e = esp_pm_lock_acquire(s_cpu_lock))) { return e; }
        s_cpu_lock_held = true;
    }
    if ((e = muse_watcher_ptest_apply(state))) { return e; }
    return state->load == MUSE_PTEST_WIFI_SCAN ? wifi_scan() : ESP_OK;
}
static esp_err_t fw_readback(muse_ptest_readback_t *out)
{
    if (s_run.status == RUN_RUNNING && s_usb_lock_held) { return ESP_ERR_INVALID_STATE; }
    esp_err_t e = muse_watcher_ptest_readback(out);
    if (out->usb) {
        muse_ptest_readback_t p = {0};
        esp_err_t power_error = fw_power(&p); /* UART restore/USB lock immediately. */
        if (!e) { e = power_error; }
        if (s_run.status == RUN_RUNNING) { return ESP_ERR_INVALID_STATE; }
    }
    if (e) { return e; }
    if (s_dryrun) { out->auto_light_sleep = false; return ESP_OK; }
    esp_pm_config_t actual;
    if ((e = esp_pm_get_configuration(&actual))) { return e; }
    out->pm_min_mhz = actual.min_freq_mhz;
    out->pm_max_mhz = actual.max_freq_mhz;
    out->auto_light_sleep = actual.light_sleep_enable;
    if (!s_cpu_pd_veto_held) { out->knobs_applied |= MUSE_PTEST_CPU_PD; }
    out->cpu_readback_mhz = esp_clk_cpu_freq() / 1000000;
    out->wifi_started = s_wifi_started;
    out->wifi_scans = s_scans;
    portENTER_CRITICAL(&s_sleep_mux);
    out->slept_us = s_sleep_us;
    out->sleep_count = s_sleep_count;
    portEXIT_CRITICAL(&s_sleep_mux);
    if (actual.min_freq_mhz != s_pm.min_freq_mhz || actual.max_freq_mhz != s_pm.max_freq_mhz
        || actual.light_sleep_enable != s_pm.light_sleep_enable) { return ESP_FAIL; }
    if (s_cpu_lock_held && out->cpu_readback_mhz != s_pm.max_freq_mhz) { return ESP_FAIL; }
    return ESP_OK;
}
static esp_err_t fw_service(muse_ptest_load_t load)
{
    if (load == MUSE_PTEST_WIFI_SCAN && s_scan_done) {
        /* Free driver-owned scan records before the next scan. */
        esp_err_t e = esp_wifi_clear_ap_list();
        if (e) { return e; }
        s_scan_done = false;
        wifi_scan_config_t scan = { .scan_type = WIFI_SCAN_TYPE_ACTIVE, .scan_time.active = { .min = 20, .max = 40 } };
        return esp_wifi_scan_start(&scan, false);
    }
    return muse_watcher_ptest_service(load);
}
static void fw_codec_regs(muse_ptest_codec_regs_t *out) { muse_watcher_ptest_codec_regs(out); }
static void fw_dryrun_mode(bool on) { muse_watcher_ptest_usb_dryrun(on); }
static esp_err_t fw_flush(void)
{
    esp_err_t e = uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(1000));
    if (e) { ESP_LOGE("watcher_ptest", "UART TX drain failed: %d", e); }
    return e;
}
static void fw_write(const char *buf, size_t n) { uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf, n); }

void muse_watcher_power_test_run(void)
{
    boot_results(esp_random(), esp_reset_reason());
    /* No muse_console RX wake lock/timer, no LVGL, voice, reconnect, BLE,
     * credentials, NVS, or production app initialization. UART is the bridge
     * console selected by the Watcher overlay; no serial-JTAG assumption. */
    esp_log_set_vprintf(capture_error_log);
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_level_set("board", ESP_LOG_ERROR); /* BSP returns use capture-only hook */
    esp_log_level_set("watcher_ptest", ESP_LOG_ERROR);
    bool held_recovery = s_release_deep_holds || muse_watcher_ptest_deep_holds_owned();
    s_init_error = held_recovery ? muse_watcher_ptest_release_deep_holds() : ESP_OK;
    /* Recover owned pads even if UART allocation subsequently fails. */
    esp_err_t uart_error = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 1024, 0, 0, NULL, 0);
    if (!s_init_error) { s_init_error = uart_error; }
    if (!s_init_error) { s_init_error = muse_watcher_ptest_init(); }
    muse_watcher_ptest_restore_codec_history((held_recovery || muse_watcher_ptest_warm_seen()) ? MUSE_PTEST_CODEC_SUSPENDED : s_run.codec_history);
    if (!s_init_error) { s_init_error = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ptest_fixed", &s_cpu_lock); }
    if (!s_init_error) { s_init_error = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ptest_usb", &s_usb_lock); }
    if (!s_init_error) {
        muse_ptest_readback_t p = {0};
        s_init_error = fw_power(&p);
    }
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    if (!s_init_error) {
        esp_pm_sleep_cbs_register_config_t cbs = { .exit_cb = slept };
        s_init_error = esp_pm_light_sleep_register_cbs(&cbs);
    }
#endif
    if (!s_init_error) { s_init_error = fw_apply(resting_state()); }
    /* ROM/EARLY/DRAM printf bypasses vprintf. Disconnect both ROM sinks;
     * uart_write_bytes uses the driver/HAL and remains the protocol writer. */
    esp_rom_install_channel_putc(1, NULL);
    esp_rom_install_channel_putc(2, NULL);
    if (!s_init_error) { s_init_error = fw_flush(); }
    if (!s_init_error) { log_reset(); }
    resume_run();
    if (s_run.status != RUN_RUNNING) { emit_status("ready"); }
    char line[256];
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        if (s_run.status != RUN_RUNNING) {
            /* Idle/armed/complete: detect VBUS every 100ms before UART RX.
             * Running: tick() alone checks VBUS once/second, no extra poll. */
            muse_ptest_readback_t p = {0};
            fw_power(&p);
            uint8_t c;
            while (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, 0) == 1) {
                if (c == '\r') { continue; }
                if (c == '\n') {
                    line[used] = 0;
                    if (overflow) { emit_error("line_too_long", ESP_ERR_INVALID_ARG); }
                    else if (used) { command(line); }
                    used = 0;
                    overflow = false;
                } else if (used + 1 < sizeof(line)) { line[used++] = (char)c; }
                else { overflow = true; }
            }
        }
        tick();
        uint32_t delay_ms = 100;
        if (s_run.status == RUN_RUNNING && s_run.count) {
            record_t *r = &s_run.records[s_run.count - 1];
            muse_ptest_load_t load = run_state(r->state_index)->load;
            delay_ms = (load == MUSE_PTEST_MIC || load == MUSE_PTEST_SINE) ? 1 : running_poll_ms();
            if (s_run.matrix == MATRIX_SLEEP && (r->status == REC_SETTLE || r->status == REC_CAPTURE)) {
                int64_t deadline = r->status == REC_SETTLE ? r->applied_us + (int64_t)s_run.settle_ms * 1000 : r->measure_start_us + (int64_t)s_run.capture_ms * 1000;
                int64_t remaining = deadline - fw_now();
                uint32_t boundary_ms = remaining > 0 ? (uint32_t)((remaining + 999) / 1000) : 1;
                if (boundary_ms < delay_ms) { delay_ms = boundary_ms; }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms) > 0 ? pdMS_TO_TICKS(delay_ms) : 1);
    }
}
#endif
