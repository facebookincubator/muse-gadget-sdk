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

#define STATE(id, note, load, mhz, bl, amp) { id, note, load, mhz, bl, amp }
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

typedef enum { RUN_IDLE, RUN_ARMED, RUN_RUNNING, RUN_COMPLETE, RUN_ABORTED, RUN_ERROR, RUN_REBOOT } run_status_t;
typedef enum { REC_PENDING, REC_SETTLE, REC_CAPTURE, REC_OK, REC_SKIPPED, REC_ERROR, REC_ABORTED, REC_SLEEP_SKIPPED } rec_status_t;
static const char *const s_run_names[] = { "idle", "armed", "running", "complete", "aborted", "error", "incomplete_reboot" };
static const char *const s_rec_names[] = { "pending", "settling", "capturing", "ok", "skipped", "error", "aborted", "skipped" };
typedef struct {
    int64_t enter_us, applied_us, measure_start_us, end_us;
    muse_ptest_readback_t actual;
    int error, cleanup_error;
    uint16_t state_index, repeat;
    rec_status_t status;
} record_t;
typedef struct {
    char run_id[MUSE_PTEST_RUN_ID_MAX + 1];
    uint32_t boot_id;
    int reset_reason;
    run_status_t status;
    uint32_t settle_ms, capture_ms, repeats, count;
    int64_t ack_us, last_usb_present_us, vbus_removed_us, sequence_start_us, finished_us;
    int error, restore_error;
    record_t records[MUSE_PTEST_MAX_RECORDS];
} run_t;

#define SAVED_MAGIC 0x50544553u
#define SAVED_VERSION 1u
static RTC_NOINIT_ATTR struct {
    uint32_t magic, version, size, checksum;
    run_t run;
} s_saved;
/* ESP32-S3 RTC slow memory is 8KiB. Leave space for IDF RTC metadata. */
_Static_assert(sizeof(s_saved) <= 7680, "diagnostic RTC results exceed budget");
static run_t s_run;
static uint32_t s_boot_id;
static int s_reset_reason;
static bool s_restored;
static int64_t s_next_power_us, s_capture_slept_us;
static uint32_t s_capture_sleeps, s_capture_frames, s_capture_adc, s_capture_scans;
static esp_err_t s_init_error;

static uint32_t checksum(const void *ptr, size_t n)
{
    const uint8_t *p = ptr;
    uint32_t h = 2166136261u;
    while (n--) { h = (h ^ *p++) * 16777619u; }
    return h;
}

/* Only boundary writes: never NVS, never a periodic flash flood. A reset
 * during a copy invalidates the checksum instead of passing partial data. */
static void save_run(void)
{
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
    s_restored = false;
    memset(&s_run, 0, sizeof(s_run));
    if (s_saved.magic == SAVED_MAGIC && s_saved.version == SAVED_VERSION && s_saved.size == sizeof(run_t)
        && s_saved.checksum == checksum(&s_saved.run, sizeof(run_t))
        && s_saved.run.count <= MUSE_PTEST_MAX_RECORDS && s_saved.run.status <= RUN_REBOOT) {
        s_run = s_saved.run;
        s_restored = true;
        if (s_run.status == RUN_ARMED || s_run.status == RUN_RUNNING) {
            s_run.status = RUN_REBOOT;
            s_run.error = ESP_ERR_INVALID_STATE;
            if (s_run.count && s_run.records[s_run.count - 1].status <= REC_CAPTURE) {
                s_run.records[s_run.count - 1].status = REC_ABORTED;
                s_run.records[s_run.count - 1].error = ESP_ERR_INVALID_STATE;
                /* end_us remains 0: no device-evidenced end survived the reboot. */
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
    emit("\"wifi_started\":%s,\"wifi_scans\":%" PRIu32 ",\"pm_min_mhz\":%d,\"pm_max_mhz\":%d,\"auto_light_sleep\":%s,\"cpu_readback_mhz\":%d,\"slept_us\":%" PRId64 ",\"sleep_count\":%" PRIu32 "}",
         a->wifi_started ? "true" : "false", a->wifi_scans, a->pm_min_mhz, a->pm_max_mhz,
         a->auto_light_sleep ? "true" : "false", a->cpu_readback_mhz, a->slept_us, a->sleep_count);
}

static void emit_status_request(const char *type, const char *request_id)
{
    muse_ptest_readback_t p = {0};
    esp_err_t e = fw_power(&p);
    emit("PTEST {\"schema\":1,\"type\":\"%s\",\"request_id\":\"%s\",\"boot_id\":%" PRIu32 ",\"now_us\":%" PRId64 ",\"reset_reason\":%d,\"run_id\":\"%s\",\"run_boot_id\":%" PRIu32 ",\"state\":\"%s\",\"status\":\"%s\",",
         type, request_id, s_boot_id, fw_now(), s_reset_reason, s_run.run_id, s_run.boot_id, s_run_names[s_run.status], s_run_names[s_run.status]);
    emit("\"restored_from_rtc\":%s,\"record_count\":%" PRIu32 ",\"power_error\":%d,\"init_error\":%d,\"usb\":%s,\"charging\":%s,\"expander_input\":%u,\"sequence_start_us\":%" PRId64 ",\"error\":%d,\"restore_error\":%d}\n",
         s_restored ? "true" : "false", s_run.count, e, s_init_error, p.usb ? "true" : "false", p.charging ? "true" : "false", p.input, s_run.sequence_start_us, s_run.error, s_run.restore_error);
}

static void emit_status(const char *type) { emit_status_request(type, ""); }

static void emit_error(const char *message, esp_err_t error)
{
    emit("PTEST {\"schema\":1,\"type\":\"error\",\"error\":%d,\"message\":\"%s\"}\n", error, message);
}

static void emit_plan(void)
{
    emit("[");
    for (size_t i = 0; i < MATRIX_COUNT; i++) {
        const muse_ptest_state_t *s = &s_matrix[i];
        emit("%s{\"index\":%u,\"id\":\"%s\",\"timed\":%s,\"cpu_mhz\":%d,\"backlight_pct\":%d,\"amplifier\":%s,\"note\":\"%s\"}",
             i ? "," : "", (unsigned)i, s->id, s->load == MUSE_PTEST_UNSUPPORTED ? "false" : "true", s->cpu_mhz, s->backlight_pct, s->amplifier ? "true" : "false", s->note);
    }
    emit("]");
}

static void emit_results(void)
{
    muse_ptest_readback_t power = {0};
    esp_err_t power_error = fw_power(&power);
    emit("PTEST {\"schema\":1,\"matrix_version\":1,\"type\":\"results\",\"run_id\":\"%s\",\"boot_id\":%" PRIu32 ",\"run_boot_id\":%" PRIu32 ",\"retrieval_boot_id\":%" PRIu32 ",\"now_us\":%" PRId64 ",",
         s_run.run_id, s_run.boot_id, s_run.boot_id, s_boot_id, fw_now());
    emit("\"reset_reason\":%d,\"retrieval_reset_reason\":%d,\"state\":\"%s\",\"status\":\"%s\",\"complete\":%s,\"usb\":%s,\"power_error\":%d,",
         s_run.reset_reason, s_reset_reason, s_run_names[s_run.status], s_run_names[s_run.status],
         s_run.status == RUN_COMPLETE ? "true" : "false", power.usb ? "true" : "false", power_error);
    emit("\"scope\":\"isolated_BSP_loads_not_production\",\"timestamp_unit\":\"us_since_run_boot\",\"ack_us\":%" PRId64 ",\"last_usb_present_us\":%" PRId64 ",\"vbus_removed_us\":%" PRId64 ",\"sequence_start_us\":%" PRId64 ",\"finished_us\":%" PRId64 ",",
         s_run.ack_us, s_run.last_usb_present_us, s_run.vbus_removed_us, s_run.sequence_start_us, s_run.finished_us);
    emit("\"settle_ms\":%" PRIu32 ",\"capture_ms\":%" PRIu32 ",\"repeats\":%" PRIu32 ",\"usb_abort_poll_ms\":1000,\"error\":%d,\"restore_error\":%d,\"records\":[",
         s_run.settle_ms, s_run.capture_ms, s_run.repeats, s_run.error, s_run.restore_error);
    for (uint32_t i = 0; i < s_run.count; i++) {
        const record_t *r = &s_run.records[i];
        const muse_ptest_state_t *s = &s_matrix[r->state_index];
        emit("%s{\"index\":%u,\"repeat\":%u,\"name\":\"%s\",\"id\":\"%s\",\"status\":\"%s\",\"valid\":%s,\"apply_err\":%d,\"error\":%d,\"cleanup_error\":%d,",
             i ? "," : "", r->state_index, r->repeat, s->id, s->id, s_rec_names[r->status], r->status == REC_OK ? "true" : "false", r->error, r->error, r->cleanup_error);
        emit("\"reason\":\"%s\",\"enter_us\":%" PRId64 ",\"applied_us\":%" PRId64 ",\"measure_start_us\":%" PRId64 ",\"end_us\":%" PRId64 ",\"note\":\"%s\",\"actual\":",
             r->status == REC_SLEEP_SKIPPED ? "automatic_light_sleep_not_observed" : "", r->enter_us, r->applied_us, r->measure_start_us, r->end_us, s->note);
        emit_actual(&r->actual);
        emit("}");
    }
    emit("]}\n");
}

/* Strict flat JSON, no arbitrary cJSON allocations or credentials. Only
 * these four keys, each once; no trailing junk, escapes, floats, or signs.
 * Order/whitespace are free. Reject rather than silently default a typo. */
typedef struct { char run_id[MUSE_PTEST_RUN_ID_MAX + 1]; uint32_t settle_ms, capture_ms, repeats; } arm_t;
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
    return !*p && seen == 15 && a->settle_ms >= 1000 && a->settle_ms <= 60000
        && a->capture_ms >= 1000 && a->capture_ms <= 120000
        && a->repeats >= 1 && a->repeats <= MUSE_PTEST_MAX_REPEATS;
}

static void finish_run(run_status_t status, esp_err_t error)
{
    s_run.status = status;
    s_run.error = error;
    s_run.finished_us = fw_now();
    /* One safe resting state, even on abort. Never drops EXP_PWR_SYSTEM. */
    s_run.restore_error = fw_apply(&s_matrix[MATRIX_COUNT - 1]);
    if (s_run.restore_error && status == RUN_COMPLETE) { s_run.status = RUN_ERROR; }
    save_run();
}

static void abort_run(esp_err_t error, bool power_error)
{
    if (s_run.status != RUN_ARMED && s_run.status != RUN_RUNNING) { return; }
    if (s_run.count) {
        record_t *r = &s_run.records[s_run.count - 1];
        if (r->status == REC_PENDING || r->status == REC_CAPTURE || r->status == REC_SETTLE) {
            r->end_us = fw_now();
            r->error = error;
            r->status = power_error ? REC_ERROR : REC_ABORTED;
        }
    }
    finish_run(power_error ? RUN_ERROR : RUN_ABORTED, error);
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
    if (!strcmp(line, ">ptest.abort")) { abort_run(ESP_ERR_INVALID_STATE, false); emit_status("abort_ack"); return; }
    if (strncmp(line, ">ptest.arm=", 11)) { emit_error("unknown_command", ESP_ERR_INVALID_ARG); return; }
    arm_t a;
    if (!parse_arm(line + 11, &a)) { emit_error("invalid_arm_json_or_bounds", ESP_ERR_INVALID_ARG); return; }
    unsigned timed = 0;
    for (size_t i = 0; i < MATRIX_COUNT; i++) { timed += s_matrix[i].load != MUSE_PTEST_UNSUPPORTED; }
    uint32_t bound = a.repeats * timed * (a.settle_ms + a.capture_ms + 10000u) + 10000u;
    if (bound > 3600000u) { emit_error("arm_total_duration_exceeds_one_hour", ESP_ERR_INVALID_ARG); return; }
    if (s_run.status == RUN_ARMED || s_run.status == RUN_RUNNING) { emit_error("already_active", ESP_ERR_INVALID_STATE); return; }
    if (s_init_error) { emit_error("board_initialization_failed", s_init_error); return; }
    muse_ptest_readback_t p = {0};
    esp_err_t e = fw_power(&p);
    if (e || !p.usb) { emit_error("arm_requires_verified_usb_vbus", e ? e : ESP_ERR_INVALID_STATE); return; }
    memset(&s_run, 0, sizeof(s_run));
    strcpy(s_run.run_id, a.run_id);
    s_run.boot_id = s_boot_id;
    s_run.reset_reason = s_reset_reason;
    s_run.status = RUN_ARMED;
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
    emit("\"settle_ms\":%" PRIu32 ",\"capture_ms\":%" PRIu32 ",\"repeats\":%" PRIu32 ",\"gate_poll_ms\":100,\"usb_abort_poll_ms\":1000,\"max_duration_ms\":%" PRIu32 ",\"rtc_bytes\":%u,\"plan\":",
         a.settle_ms, a.capture_ms, a.repeats, bound, (unsigned)sizeof(s_saved));
    emit_plan();
    emit("}\n");
}

static esp_err_t sample(record_t *r)
{
    memset(&r->actual, 0, sizeof(r->actual));
    esp_err_t e = fw_readback(&r->actual);
    return !e && r->actual.usb ? ESP_ERR_INVALID_STATE : e;
}

static void start_next(void)
{
    while (s_run.count < MATRIX_COUNT * s_run.repeats) {
        uint32_t pos = s_run.count++;
        record_t *r = &s_run.records[pos];
        r->state_index = pos % MATRIX_COUNT;
        r->repeat = pos / MATRIX_COUNT;
        const muse_ptest_state_t *s = &s_matrix[r->state_index];
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
            r->cleanup_error = fw_apply(&s_matrix[MATRIX_COUNT - 1]);
            /* Unknown partial hardware state is not a safe basis to continue. */
            if (r->cleanup_error || e != ESP_ERR_NOT_SUPPORTED) { finish_run(RUN_ERROR, e); return; }
            save_run();
            continue;
        }
        r->status = REC_SETTLE;
        save_run();
        return;
    }
    finish_run(RUN_COMPLETE, ESP_OK);
}

/* Called frequently only for active audio. USB checks are independently
 * rate-limited, so baseline wakes once/second, never a busy loop. */
static void tick(void)
{
    if (s_run.status != RUN_ARMED && s_run.status != RUN_RUNNING) { return; }
    if (fw_now() >= s_next_power_us) {
        muse_ptest_readback_t p = {0};
        esp_err_t e = fw_power(&p);
        if (e) { abort_run(e, true); return; }
        int64_t at = fw_now();
        s_next_power_us = at + (s_run.status == RUN_ARMED ? 100000 : 1000000);
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
    const muse_ptest_state_t *s = &s_matrix[r->state_index];
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
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_random.h"
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

static int64_t fw_now(void) { return esp_timer_get_time(); }
static esp_err_t fw_power(muse_ptest_readback_t *out)
{
    esp_err_t e = muse_watcher_ptest_power(out);
    /* VBUS, not UART 'connection' (the CH342 has no connection API). Keep
     * console RX awake on USB, release before the first USB-free state.
     * On uncertain power keep awake so the error/results remain readable. */
    bool awake = e || out->usb;
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
        fw_power(&p); /* Reacquire the USB console lock immediately. */
        return ESP_ERR_INVALID_STATE;
    }
    if (e) { return e; }
    esp_pm_config_t actual;
    if ((e = esp_pm_get_configuration(&actual))) { return e; }
    out->pm_min_mhz = actual.min_freq_mhz;
    out->pm_max_mhz = actual.max_freq_mhz;
    out->auto_light_sleep = actual.light_sleep_enable;
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
static void fw_write(const char *buf, size_t n) { uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf, n); }

void muse_watcher_power_test_run(void)
{
    boot_results(esp_random(), esp_reset_reason());
    /* No muse_console RX wake lock/timer, no LVGL, voice, reconnect, BLE,
     * credentials, NVS, or production app initialization. UART is the bridge
     * console selected by the Watcher overlay; no serial-JTAG assumption. */
    s_init_error = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 1024, 0, 0, NULL, 0);
    if (!s_init_error) { s_init_error = muse_watcher_ptest_init(); }
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
    if (!s_init_error) { s_init_error = fw_apply(&s_matrix[MATRIX_COUNT - 1]); }
    /* The default UART log backend can otherwise emit setup messages while
     * detached and distort the capture. Protocol writes remain explicit. */
    esp_log_level_set("*", ESP_LOG_NONE);
    emit_status("ready");
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
            muse_ptest_load_t load = s_matrix[s_run.records[s_run.count - 1].state_index].load;
            delay_ms = (load == MUSE_PTEST_MIC || load == MUSE_PTEST_SINE) ? 1 : 1000;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms) > 0 ? pdMS_TO_TICKS(delay_ms) : 1);
    }
}
#endif
