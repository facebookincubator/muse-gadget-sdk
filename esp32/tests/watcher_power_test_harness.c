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
#include <assert.h>
#include <setjmp.h>
#define MUSE_PTEST_HOST 1
#include "../components/muse/muse_watcher_power_test.c"

static int64_t now_us = 1000000, rtc_us = 1000000;
static bool usb_present = true, deep_entered;
static int power_error, apply_error, readback_error, service_error, restore_error, timer_error;
static int apply_index = -1, readback_index = -1, service_index = -1, empty_index = -1;
static int no_sleep_index = -1, readback_usb_index = -1, readback_usb_nth;
static unsigned state_readbacks, codec_history = MUSE_PTEST_CODEC_COLD;
static unsigned wake_cause, deep_entries, holds_released, uart_restores, knob_reverts;
static uint16_t applied_knobs;
static bool uart_parked, codec_mismatch, reset_on_cleanup, pending_tx;
static unsigned flush_calls;
static int flush_error;
static unsigned fail_flush_nth;
static bool noise_log;
static void fake_log(const char *format, ...);
static esp_err_t fw_flush(void) { flush_calls++; if (flush_error && (!fail_flush_nth || flush_calls == fail_flush_nth)) { fake_log("E UART TX drain failed: %d\n", flush_error); return flush_error; } pending_tx = false; return ESP_OK; }
static int drop_usb_index = -1;
static jmp_buf reset_env;
static void fake_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    capture_error_log(format, args);
    va_end(args);
}
static void fw_dryrun_mode(bool on) { (void)on; }
static void fw_codec_regs(muse_ptest_codec_regs_t *out)
{
    memset(out, 0, sizeof(*out));
    out->dac_mask = 0x7f; out->adc_mask = 0x0f; out->adc_variant = 2;
    if (codec_history == MUSE_PTEST_CODEC_INITIALIZED) {
        out->dac[1] = 0x3f; out->dac[3] = 0xfa;
        out->adc[0] = 0x80; out->adc[1] = 0x3a; out->adc[2] = 1;
    } else if (codec_history == MUSE_PTEST_CODEC_SUSPENDED) {
        out->dac[3] = 0xfc; out->dac[4] = 0xff; out->dac[5] = 2;
        out->adc[0] = 0x1e; out->adc[2] = out->adc[3] = 1;
    }
    out->expected = codec_history >= MUSE_PTEST_CODEC_INITIALIZED && !codec_mismatch;
    if (codec_mismatch) { out->dac[3] = 0x99; }
}
static uint32_t power_reads, frames, adc_samples, scans;
static int64_t slept_us;
static uint32_t sleep_count;
static const muse_ptest_state_t *active;

static int64_t fw_timer_now(void) { return now_us; }
static int64_t fw_rtc_now(void) { return rtc_us; }
static bool fw_timer_reset(int reason) { return reason == 5 && wake_cause == 4; }
static unsigned fw_codec_history(void) { return codec_history; }
static void fw_write(const char *buf, size_t n) { assert(!s_dryrun || !uart_parked); if (s_dryrun && n && buf[n - 1] == '\n') { pending_tx = true; } fwrite(buf, 1, n, stdout); }
static esp_err_t fw_power(muse_ptest_readback_t *out)
{
    power_reads++;
    if (uart_parked && (s_dryrun || power_error || usb_present)) {
        uart_parked = false;
        applied_knobs &= ~MUSE_PTEST_UART_HIZ;
        uart_restores++;
    }
    if (power_error) { return power_error; }
    out->usb = usb_present;
    out->charging = usb_present;
    out->input = usb_present ? 0 : 4;
    return ESP_OK;
}
static int current_index(void)
{
    if (!active) { return -1; }
    for (unsigned i = 0; i < MATRIX_COUNT; i++) { if (active == &s_matrix[i]) { return (int)i; } }
    for (unsigned i = 0; i < SLEEP_MATRIX_COUNT; i++) { if (active == &s_sleep_matrix[i]) { return (int)i; } }
    return -1;
}
static bool audio_load(muse_ptest_load_t load)
{
    return load == MUSE_PTEST_CODECS_IDLE || load == MUSE_PTEST_MIC || load == MUSE_PTEST_SINE;
}
static esp_err_t fake_audio_open(void)
{
    if (applied_knobs & (MUSE_PTEST_I2S_LOW | MUSE_PTEST_I2S_HIZ)) { return ESP_ERR_INVALID_STATE; }
    if (codec_history < MUSE_PTEST_CODEC_INITIALIZED) { codec_history = MUSE_PTEST_CODEC_INITIALIZED; }
    return ESP_OK;
}
static esp_err_t fw_apply(const muse_ptest_state_t *state)
{
    if (reset_on_cleanup && state == resting_state() && s_run.count && s_run.records[s_run.count - 1].status == REC_ERROR) {
        reset_on_cleanup = false;
        longjmp(reset_env, 1); /* reset inside the real core's first cleanup call */
    }
    if (s_dryrun && (state->knobs & MUSE_PTEST_UART_HIZ)) { assert(!pending_tx); }
    if (audio_load(state->load) && (state->knobs & (MUSE_PTEST_I2S_LOW | MUSE_PTEST_I2S_HIZ))) { return ESP_ERR_INVALID_STATE; }
    if (applied_knobs) { knob_reverts++; }
    if (uart_parked) { uart_restores++; }
    applied_knobs = 0;
    uart_parked = false;
    bool was_audio = active && audio_load(active->load);
    active = state;
    state_readbacks = 0;
    frames = adc_samples = scans = 0;
    now_us += 25000;
    rtc_us += 25000; /* 25ms setup, never nominal host timestamps. */
    if (was_audio && !audio_load(state->load)) { codec_history = MUSE_PTEST_CODEC_SUSPENDED; }
    if (restore_error && state == resting_state() && s_run.status != RUN_RUNNING) { return restore_error; }
    if (current_index() == apply_index) { return apply_error; }
    if (state->codec_policy == MUSE_PTEST_CODEC_COLD && codec_history != MUSE_PTEST_CODEC_COLD) { return ESP_ERR_INVALID_STATE; }
    if (state->codec_policy == MUSE_PTEST_CODEC_INITIALIZED) {
        if (codec_history == MUSE_PTEST_CODEC_SUSPENDED) { return ESP_ERR_INVALID_STATE; }
        codec_history = MUSE_PTEST_CODEC_INITIALIZED;
    }
    if (state->codec_policy == MUSE_PTEST_CODEC_SUSPENDED) { codec_history = MUSE_PTEST_CODEC_SUSPENDED; }
    if (audio_load(state->load)) {
        esp_err_t e = fake_audio_open();
        if (e) { return e; }
    }
    if ((state->knobs & MUSE_PTEST_UART_HIZ) && usb_present && !s_dryrun) { return ESP_ERR_INVALID_STATE; }
    applied_knobs = state->knobs & ~(MUSE_PTEST_GPIO_HOLD | (s_dryrun ? MUSE_PTEST_CPU_PD : 0));
    uart_parked = (applied_knobs & MUSE_PTEST_UART_HIZ) != 0;
    return ESP_OK;
}
static esp_err_t fw_readback(muse_ptest_readback_t *out)
{
    if (current_index() == drop_usb_index) { usb_present = false; }
    out->codec_state = (uint8_t)codec_history;
    if (current_index() == readback_index && readback_error) {
        fake_log("E fake readback: error %d \"transport\"\\\n", readback_error);
        return readback_error;
    }
    esp_err_t e = fw_power(out);
    if (e) { return e; }
    if (noise_log) { fake_log("E ignored I2S disable on stopped channel\n"); }
    state_readbacks++;
    if (current_index() == readback_usb_index && state_readbacks == (unsigned)readback_usb_nth) { out->usb = true; }
    out->output = 1024;
    out->direction = 0x20ff;
    out->backlight_duty = active && active->backlight_pct >= 0 ? active->backlight_pct * 1023 / 100 : 0;
    out->codecs_open = active && audio_load(active->load);
    out->codec_state = (uint8_t)codec_history;
    out->knobs_applied = applied_knobs;
    out->configured_volume = out->codecs_open ? 25 : 0;
    out->battery_mv = adc_samples ? 3700 : 0;
    out->io_frames = frames;
    out->adc_samples = adc_samples;
    out->wifi_scans = scans;
    out->wifi_started = active && active->load == MUSE_PTEST_WIFI_SCAN;
    out->pm_min_mhz = active && active->cpu_mhz ? active->cpu_mhz : 40;
    out->pm_max_mhz = active && active->cpu_mhz ? active->cpu_mhz : 240;
    out->auto_light_sleep = active && !active->cpu_mhz;
    out->cpu_readback_mhz = out->pm_max_mhz;
    out->slept_us = slept_us;
    out->sleep_count = sleep_count;
    return ESP_OK;
}
static esp_err_t fw_service(muse_ptest_load_t load)
{
    if (current_index() == service_index) { return service_error; }
    if (current_index() == empty_index) { return ESP_OK; }
    if (load == MUSE_PTEST_MIC || load == MUSE_PTEST_SINE) { frames += 16000; }
    if (load == MUSE_PTEST_ADC_SAMPLE) { adc_samples += 8; }
    if (load == MUSE_PTEST_WIFI_SCAN) { scans++; }
    return ESP_OK;
}
static esp_err_t fw_prepare_deep(const muse_ptest_state_t *state, int64_t us)
{
    if (us <= 0) { return ESP_ERR_INVALID_ARG; }
    if (state->knobs & MUSE_PTEST_GPIO_HOLD) { applied_knobs |= MUSE_PTEST_GPIO_HOLD; }
    return timer_error;
}
static void fw_enter_deep(void) { deep_entered = true; deep_entries++; }
static void advance(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 100) {
        if (deep_entered) { return; } /* CPU timer is stopped, not advanced. */
        uint32_t step = ms - t < 100 ? ms - t : 100;
        now_us += (int64_t)step * 1000;
        rtc_us += (int64_t)step * 1000;
        if (active && !active->cpu_mhz && !usb_present && current_index() != no_sleep_index) { slept_us += (int64_t)step * 990; sleep_count++; }
        tick();
    }
}
static void reset_fake(uint32_t boot, int reason)
{
    active = NULL;
    applied_knobs = 0;
    uart_parked = false;
    codec_history = MUSE_PTEST_CODEC_COLD;
    deep_entered = false;
    slept_us = 0;
    sleep_count = 0;
    now_us = 100000;
    rtc_us += 100000; /* measured boot startup, part of RTC elapsed time */
    log_reset(); /* emulate DRAM loss; retained error evidence must stand alone */
    boot_results(boot, reason);
    if (s_release_deep_holds) { holds_released++; }
    if (s_run.codec_history > codec_history) { codec_history = s_run.codec_history; }
    if (s_resume_pending) { fw_apply(resting_state()); resume_run(); }
}
int main(void)
{
    boot_results(111, 1);
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        if (setjmp(reset_env)) { reset_fake(222, 3); continue; }
        line[strcspn(line, "\r\n")] = 0;
        unsigned value, cause;
        int index, error, reason;
        if (sscanf(line, "usb %u", &value) == 1) { usb_present = value != 0; }
        else if (sscanf(line, "advance %u", &value) == 1) { advance(value); }
        else if (sscanf(line, "deep_resume %u %d %u", &value, &reason, &cause) == 3) {
            rtc_us += (int64_t)value * 1000;
            wake_cause = cause;
            reset_fake(s_boot_id + 111, reason);
        }
        else if (sscanf(line, "power_error %d", &error) == 1) { power_error = error; }
        else if (sscanf(line, "init_error %d", &error) == 1) { s_init_error = error; }
        else if (sscanf(line, "timer_error %d", &error) == 1) { timer_error = error; }
        else if (sscanf(line, "apply_error %d %d", &index, &error) == 2) { apply_index = index; apply_error = error; }
        else if (sscanf(line, "readback_error %d %d", &index, &error) == 2) { readback_index = index; readback_error = error; }
        else if (sscanf(line, "service_error %d %d", &index, &error) == 2) { service_index = index; service_error = error; }
        else if (sscanf(line, "no_sleep %d", &index) == 1) { no_sleep_index = index; }
        else if (sscanf(line, "readback_usb %d %d", &index, &error) == 2) { readback_usb_index = index; readback_usb_nth = error; }
        else if (sscanf(line, "empty_work %d", &index) == 1) { empty_index = index; }
        else if (sscanf(line, "restore_error %d", &error) == 1) { restore_error = error; }
        else if (sscanf(line, "knob_apply %u", &value) == 1 && value < SLEEP_MATRIX_COUNT) {
            esp_err_t e = fw_apply(&s_sleep_matrix[value]);
            printf("PTEST {\"type\":\"knob_apply\",\"error\":%d,\"knobs\":%u,\"codec_state\":%u,\"uart_parked\":%s}\n", e, applied_knobs, codec_history, uart_parked ? "true" : "false");
        }
        else if (sscanf(line, "codec_mismatch %u", &value) == 1) { codec_mismatch = value != 0; }
        else if (sscanf(line, "flush_nth %u", &value) == 1) { fail_flush_nth = value; }
        else if (sscanf(line, "flush_error %d", &error) == 1) { flush_error = error; }
        else if (sscanf(line, "noise %u", &value) == 1) { noise_log = value != 0; }
        else if (!strcmp(line, "flush_stats")) { printf("PTEST {\"type\":\"flush_stats\",\"calls\":%u,\"pending\":%s}\n", flush_calls, pending_tx ? "true" : "false"); }
        else if (sscanf(line, "drop_usb %d", &index) == 1) { drop_usb_index = index; }
        else if (sscanf(line, "reset_cleanup %u", &value) == 1) { reset_on_cleanup = value != 0; }
        else if (!strcmp(line, "log_split")) {
            log_reset();
            fake_log("\033[0;31mE (42) fake: ");
            fake_log("transport %d", 263);
            fake_log("\n");
            fake_log("\033[0m");
        }
        else if (!strncmp(line, "log ", 4)) { fake_log("%s", line + 4); }
        else if (!strcmp(line, "journal_hash")) { printf("PTEST {\"type\":\"journal_hash\",\"hash\":%u}\n", checksum(&s_saved, sizeof(s_saved))); }
        else if (!strcmp(line, "audio_open")) { printf("PTEST {\"type\":\"audio_open\",\"error\":%d}\n", fake_audio_open()); }
        else if (!strcmp(line, "reboot")) { wake_cause = 0; reset_fake(222, 3); }
        else if (!strcmp(line, "corrupt")) { s_saved.run.run_id[0] ^= 1; }
        else if (!strcmp(line, "wrong_pending")) { s_saved.run.deep[0].entry_boot_id++; s_saved.checksum = checksum(&s_saved.run, sizeof(run_t)); }
        else if (sscanf(line, "journal_bad %d", &index) == 1) {
            switch (index) {
            case 0: s_saved.run.status = (run_status_t)-1; break;
            case 1: s_saved.run.matrix = (matrix_t)-1; break;
            case 2: s_saved.run.settle_ms = 0; break;
            case 3: s_saved.run.repeats = 2; break;
            case 4: s_saved.run.codec_history = 255; break;
            case 5: s_saved.run.records[0].state_index = 65535; break;
            case 6: s_saved.run.records[0].status = UINT8_MAX; break;
            case 7: s_saved.run.deep[0].record_pos = 65535; break;
            case 8: s_saved.run.deep[0].programmed_us++; break;
            case 9: s_saved.run.deep_count = 3; break;
            case 10: s_saved.run.timeline_uncertainty_us = -1; break;
            case 11: memset(s_saved.run.run_id, 'x', sizeof(s_saved.run.run_id)); break;
            case 12: s_saved.run.records[0].repeat = 1; break;
            case 13: s_saved.run.deep[0].rtc_slept_us = -1; break;
            case 14: s_saved.run.codec_count = MUSE_PTEST_CODEC_SLOTS + 1; break;
            case 15: s_saved.run.records[0].codec_slot = MUSE_PTEST_CODEC_SLOTS; break;
            case 16: s_saved.run.codec_regs[0].adc_variant = 3; break;
            case 17: s_saved.run.codec_regs[0].dac_mask = 0x80; break;
            case 18: s_saved.run.codec_regs[0].adc_mask = 0xff; break;
            case 19: ((uint8_t *)&s_saved.run.codec_regs[0].expected)[0] = 2; break;
            case 20: memset(s_saved.run.last_error_log, 'x', sizeof(s_saved.run.last_error_log)); break;
            case 21: s_saved.run.last_error_log[0] = '"'; break;
            case 22: s_saved.run.error_record = MUSE_PTEST_MAX_RECORDS; break;
            }
            s_saved.checksum = checksum(&s_saved.run, sizeof(run_t));
        }
        else if (!strcmp(line, "stats")) { printf("PTEST {\"type\":\"stats\",\"power_reads\":%u,\"active\":%d,\"rtc_bytes\":%u,\"deep_entries\":%u,\"holds_released\":%u,\"uart_restores\":%u,\"knob_reverts\":%u,\"knobs\":%u}\n", power_reads, current_index(), (unsigned)(sizeof(s_saved) + MUSE_PTEST_RTC_RECOVERY_BYTES), deep_entries, holds_released, uart_restores, knob_reverts, applied_knobs); }
        else { command(line); }
    }
    return 0;
}
