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
#define MUSE_PTEST_HOST 1
#include "../components/muse/muse_watcher_power_test.c"

static int64_t now_us = 1000000;
static bool usb_present = true;
static int power_error, apply_error, readback_error, service_error, restore_error;
static int apply_index = -1, readback_index = -1, service_index = -1, empty_index = -1;
static int no_sleep_index = -1, readback_usb_index = -1, readback_usb_nth;
static unsigned state_readbacks;
static uint32_t power_reads, frames, adc_samples, scans;
static int64_t slept_us;
static uint32_t sleep_count;
static const muse_ptest_state_t *active;

static int64_t fw_now(void) { return now_us; }
static void fw_write(const char *buf, size_t n) { fwrite(buf, 1, n, stdout); }
static esp_err_t fw_power(muse_ptest_readback_t *out)
{
    power_reads++;
    if (power_error) { return power_error; }
    out->usb = usb_present;
    out->charging = usb_present;
    out->input = usb_present ? 0 : 4;
    return ESP_OK;
}
static int current_index(void) { return active ? (int)(active - s_matrix) : -1; }
static esp_err_t fw_apply(const muse_ptest_state_t *state)
{
    active = state;
    state_readbacks = 0;
    frames = adc_samples = scans = 0;
    now_us += 25000; /* Fake 25ms device setup, not host nominal schedule. */
    if (restore_error && state == &s_matrix[MATRIX_COUNT - 1] && s_run.status != RUN_RUNNING) { return restore_error; }
    return current_index() == apply_index ? apply_error : ESP_OK;
}
static esp_err_t fw_readback(muse_ptest_readback_t *out)
{
    if (current_index() == readback_index) { return readback_error; }
    esp_err_t e = fw_power(out);
    if (e) { return e; }
    state_readbacks++;
    if (current_index() == readback_usb_index && state_readbacks == (unsigned)readback_usb_nth) { out->usb = true; }
    out->output = 1024;
    out->direction = 0x20ff;
    out->backlight_duty = active && active->backlight_pct >= 0 ? active->backlight_pct * 1023 / 100 : 0;
    out->codecs_open = active && (active->load == MUSE_PTEST_CODECS_IDLE || active->load == MUSE_PTEST_MIC || active->load == MUSE_PTEST_SINE);
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
static void advance(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 100) {
        uint32_t step = ms - t < 100 ? ms - t : 100;
        now_us += (int64_t)step * 1000;
        if (active && !active->cpu_mhz && !usb_present && current_index() != no_sleep_index) { slept_us += (int64_t)step * 990; sleep_count++; }
        tick();
    }
}
int main(void)
{
    boot_results(111, 1);
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        unsigned value;
        int index, error;
        if (sscanf(line, "usb %u", &value) == 1) { usb_present = value != 0; }
        else if (sscanf(line, "advance %u", &value) == 1) { advance(value); }
        else if (sscanf(line, "power_error %d", &error) == 1) { power_error = error; }
        else if (sscanf(line, "init_error %d", &error) == 1) { s_init_error = error; }
        else if (sscanf(line, "apply_error %d %d", &index, &error) == 2) { apply_index = index; apply_error = error; }
        else if (sscanf(line, "readback_error %d %d", &index, &error) == 2) { readback_index = index; readback_error = error; }
        else if (sscanf(line, "service_error %d %d", &index, &error) == 2) { service_index = index; service_error = error; }
        else if (sscanf(line, "no_sleep %d", &index) == 1) { no_sleep_index = index; }
        else if (sscanf(line, "readback_usb %d %d", &index, &error) == 2) { readback_usb_index = index; readback_usb_nth = error; }
        else if (sscanf(line, "empty_work %d", &index) == 1) { empty_index = index; }
        else if (sscanf(line, "restore_error %d", &error) == 1) { restore_error = error; }
        else if (!strcmp(line, "reboot")) { now_us = 100000; boot_results(222, 3); }
        else if (!strcmp(line, "corrupt")) { s_saved.run.run_id[0] ^= 1; }
        else if (!strcmp(line, "stats")) { printf("PTEST {\"type\":\"stats\",\"power_reads\":%u,\"active\":%d,\"rtc_bytes\":%u}\n", power_reads, current_index(), (unsigned)sizeof(s_saved)); }
        else { command(line); }
    }
    return 0;
}
