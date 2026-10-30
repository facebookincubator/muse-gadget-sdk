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
/* AIPI shares one codec and detects USB via Serial/JTAG. Waveshare 1.75C/2.16
 * have a separate ADC codec and PMIC USB detection; only they support charging. */
#include "sdkconfig.h"
#include <string.h>
#include "boards/muse_power_test_board.h"
#include "muse_board.h"
#include "driver/usb_serial_jtag.h"
#include "esp_pm.h"
#include "driver/i2c_master.h"
#include "esp_system.h"
#include "esp_timer.h"
#ifndef MUSE_PTEST_HOST
#include "esp_attr.h"
#else
#define RTC_NOINIT_ATTR
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef CONFIG_MUSE_BOARD_AIPI
#define CONFIG_MUSE_BOARD_AIPI 0
#endif
#ifndef CONFIG_MUSE_BOARD_WAVESHARE_S3_175C
#define CONFIG_MUSE_BOARD_WAVESHARE_S3_175C 0
#endif
#ifndef CONFIG_MUSE_BOARD_WAVESHARE_S3_216
#define CONFIG_MUSE_BOARD_WAVESHARE_S3_216 0
#endif
#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175C || CONFIG_MUSE_BOARD_WAVESHARE_S3_216
#include "bsp/esp-bsp.h"
#endif

#define DISPLAY_LOCK_TIMEOUT_MS 1000
#define STATE(id, note, load, mhz, bl, amp) { id, note, load, mhz, bl, amp, 0, 1000, MUSE_PTEST_CODEC_KEEP, "other", "" }
static const muse_ptest_state_t s_matrix[] = {
    STATE("baseline_pre", "display paused/dark, panel sleep if available, codecs closed, radio off, DFS + automatic light sleep (expected skipped with USB host)", MUSE_PTEST_IDLE, 0, -1, false),
    STATE("cpu_fixed_40_idle", "fixed CPU clock, idle", MUSE_PTEST_IDLE, 40, -1, false),
    STATE("cpu_fixed_80_idle", "fixed CPU clock, idle", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("cpu_fixed_160_idle", "fixed CPU clock, idle", MUSE_PTEST_IDLE, 160, -1, false),
    STATE("cpu_fixed_240_idle", "fixed CPU clock, idle", MUSE_PTEST_IDLE, 240, -1, false),
    STATE("baseline_cpu_post", "paired DFS baseline after fixed clocks (expected skipped with USB host)", MUSE_PTEST_IDLE, 0, -1, false),
    STATE("lcd_black_bl_0", "panel awake, static black, brightness 0%, CPU 80MHz", MUSE_PTEST_LCD, 80, 0, false),
    STATE("lcd_black_bl_25", "panel awake, static black, brightness 25%, CPU 80MHz", MUSE_PTEST_LCD, 80, 25, false),
    STATE("lcd_black_bl_50", "panel awake, static black, brightness 50%, CPU 80MHz", MUSE_PTEST_LCD, 80, 50, false),
    STATE("lcd_black_bl_100", "panel awake, static black, brightness 100%, CPU 80MHz", MUSE_PTEST_LCD, 80, 100, false),
    STATE("lcd_white_bl_0", "panel awake, static full-screen white, brightness 0%, CPU 80MHz; AMOLED lit-pixel brightness cost", MUSE_PTEST_LCD, 80, 0, false),
    STATE("lcd_white_bl_25", "panel awake, static full-screen white, brightness 25%, CPU 80MHz; AMOLED lit-pixel brightness cost", MUSE_PTEST_LCD, 80, 25, false),
    STATE("lcd_white_bl_50", "panel awake, static full-screen white, brightness 50%, CPU 80MHz; AMOLED lit-pixel brightness cost", MUSE_PTEST_LCD, 80, 50, false),
    STATE("lcd_white_bl_100", "panel awake, static full-screen white, brightness 100%, CPU 80MHz; AMOLED lit-pixel brightness cost", MUSE_PTEST_LCD, 80, 100, false),
    STATE("lcd_anim_bl_100", "full-screen LVGL color animation at board frame_ms, brightness 100%, CPU 240MHz", MUSE_PTEST_LCD, 240, 100, false),
    STATE("codecs_closed_pre", "codecs uninitialized/closed, display dark", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("codecs_open", "16kHz stereo-slot codecs open, no drain; amplifier managed by codec driver", MUSE_PTEST_CODECS_IDLE, 80, -1, false),
    STATE("codecs_open_amp_on", "skipped: board has no independent amplifier control", MUSE_PTEST_UNSUPPORTED, 80, -1, true),
    STATE("mic_capture_16k", "16kHz stereo slots drained/discarded, board mic slot, 0dB gain; codec driver keeps speaker amplifier on", MUSE_PTEST_MIC, 80, -1, false),
    STATE("sine_1k_minus18dbfs", "1kHz sine -18dBFS peak, volume 25/100, 16kHz stereo slots", MUSE_PTEST_SINE, 80, -1, true),
    STATE("codecs_closed_post", "paired baseline after codec close", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("wifi_scanning", "unassociated active scans, no credentials/NVS; completed scans counted", MUSE_PTEST_WIFI_SCAN, 80, -1, false),
    STATE("baseline_post", "display paused/dark, panel sleep if available, codecs closed, radio off, DFS + automatic light sleep (expected skipped with USB host)", MUSE_PTEST_IDLE, 0, -1, false),
};
#undef STATE
#define MUSE_GENERIC_PTEST_STATES (sizeof(s_matrix) / sizeof(s_matrix[0]))

static const muse_board_t *s_board;
static lv_display_t *s_display;
static lv_obj_t *s_screen;
static lv_timer_t *s_animation;
static unsigned s_hue;
static bool s_paused, s_panel_asleep;
static esp_codec_dev_handle_t s_spk, s_mic;
static bool s_audio_attempted, s_spk_open, s_mic_open;
static bool s_spk_needs_close, s_mic_needs_close;
static esp_err_t s_audio_error;
static muse_ptest_codec_regs_t s_codec_regs;
static i2c_master_dev_handle_t s_codec_read_devices[2];
/* Independent of the run journal: a reset during audio_init must not restore
 * a cold label. RTC RAM and the codec rail both lose history on PMIC power-off. */
#define CODEC_HISTORY_MAGIC 0x47434831u
static RTC_NOINIT_ATTR volatile struct {
    uint32_t magic, history, history_inverse;
} s_codec_history;
_Static_assert(sizeof(s_codec_history) <= MUSE_PTEST_RTC_RECOVERY_BYTES, "codec history RTC budget");

unsigned muse_ptest_board_codec_history(void)
{
    if (s_codec_history.magic == CODEC_HISTORY_MAGIC) {
        unsigned h = s_codec_history.history;
        if (h != MUSE_PTEST_CODEC_COLD && h <= MUSE_PTEST_CODEC_SUSPENDED
            && s_codec_history.history_inverse == ~h) { return h; }
    }
    /* Flashing or opening a USB console can reset the MCU without resetting
     * the codec rail. Missing or torn RTC history proves cold only on POR. */
    return esp_reset_reason() == ESP_RST_POWERON ? MUSE_PTEST_CODEC_COLD : 0;
}

/* Journal recovery hints cannot replace this backend's independent RTC evidence. */
void muse_ptest_board_restore_codec_history(unsigned history) { (void)history; }
static void codec_history_set(unsigned history)
{
    if (history == MUSE_PTEST_CODEC_COLD || history > MUSE_PTEST_CODEC_SUSPENDED) { return; }
    s_codec_history.history_inverse = ~history;
    s_codec_history.history = history;
    s_codec_history.magic = CODEC_HISTORY_MAGIC;
    __sync_synchronize(); /* before any codec constructor/partial write */
}

bool muse_ptest_board_warm_seen(void)
{
    unsigned h = muse_ptest_board_codec_history();
    return s_codec_history.magic == CODEC_HISTORY_MAGIC
        && h != MUSE_PTEST_CODEC_COLD && s_codec_history.history_inverse == ~h;
}

static esp_err_t read_device(uint8_t address, i2c_master_dev_handle_t *device)
{
    if (*device) { return ESP_OK; }
    for (int port = 0; port < 2; port++) {
        i2c_master_bus_handle_t bus = NULL;
        if (i2c_master_get_bus_handle(port, &bus) || i2c_master_probe(bus, address, 50)) { continue; }
        const i2c_device_config_t cfg = { .device_address = address, .scl_speed_hz = 400000 };
        return i2c_master_bus_add_device(bus, &cfg, device);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static void codec_read_devices_init(void)
{
    /* Read-only devices survive for the image lifetime; no codec constructor. */
    (void)read_device(0x18, &s_codec_read_devices[0]);
    (void)read_device(0x40, &s_codec_read_devices[1]);
}

static uint8_t codec_read(bool adc, const uint8_t *regs, size_t n, uint8_t *values)
{
    i2c_master_dev_handle_t device = s_codec_read_devices[adc];
    uint8_t mask = 0;
    for (size_t i = 0; device && i < n; i++) {
        if (!i2c_master_transmit_receive(device, &regs[i], 1, &values[i], 1, 50)) { mask |= 1u << i; }
    }
    return mask;
}

static bool codec_shutdown_matches(void)
{
    bool has_adc = !CONFIG_MUSE_BOARD_AIPI;
    if (s_spk_needs_close || s_mic_needs_close || s_codec_regs.dac_mask != 0x7f) { return false; }
    /* Shutdown readbacks, not written values: reserved bits read as zero. */
    const uint8_t *d = s_codec_regs.dac;
    static const uint8_t adc_off[] = {0x7f,0x07,0xc0,0x3f};
    return d[0] == 0x1f && d[1] == 0 && d[2] == 0 && d[3] == 0xfc
        && d[4] == 0x7f && d[5] == 2 && d[6] == 0
        && (!has_adc || (s_codec_regs.adc_mask == 0x0f
                        && !memcmp(s_codec_regs.adc, adc_off, sizeof(adc_off))));
}

static void codec_snapshot(void)
{
    static const uint8_t dac[] = {0x00,0x01,0x02,0x0d,0x0e,0x12,0x14};
    /* ES7210 shutdown registers from es7210_stop(). */
    static const uint8_t adc[] = {0x01,0x06,0x40,0x47};
    memset(&s_codec_regs, 0, sizeof(s_codec_regs));
    s_codec_regs.dac_mask = (uint8_t)codec_read(false, dac, sizeof(dac), s_codec_regs.dac);
    bool has_adc = !CONFIG_MUSE_BOARD_AIPI;
    if (has_adc) {
        s_codec_regs.adc_variant = 3;
        s_codec_regs.adc_mask = codec_read(true, adc, sizeof(adc), s_codec_regs.adc);
    }
    s_codec_regs.expected = muse_ptest_board_codec_history() == MUSE_PTEST_CODEC_SUSPENDED
        && codec_shutdown_matches();
}

void muse_ptest_board_codec_regs(muse_ptest_codec_regs_t *out)
{
    /* Status/results may be requested during capture. Never perform I2C here. */
    if (out) { *out = s_codec_regs; }
}
static uint32_t s_frames;
static int s_volume;
static muse_ptest_load_t s_load;
static bool s_charge_restore_pending, s_charge_run_active;
/* Only PMIC transitions/guard state hold this mutex, never display/audio. */
static SemaphoreHandle_t s_charge_mutex;
static TaskHandle_t s_charge_guard;
static TickType_t s_charge_wait = portMAX_DELAY;
#define CHARGE_EXPIRED_MAGIC 0x43484731u
static RTC_NOINIT_ATTR uint32_t s_charge_expired;
static int64_t s_boot_retry_us;
static i2c_master_dev_handle_t s_pmu_device;
_Static_assert(sizeof(s_codec_history) + sizeof(s_charge_expired) <= MUSE_PTEST_RTC_RECOVERY_BYTES, "recovery RTC budget");

bool muse_ptest_board_charging_supported(void)
{
    return CONFIG_MUSE_BOARD_WAVESHARE_S3_175C || CONFIG_MUSE_BOARD_WAVESHARE_S3_216;
}

static esp_err_t set_charging(bool on)
{
    esp_err_t e = ESP_OK;
#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175C || CONFIG_MUSE_BOARD_WAVESHARE_S3_216
    /* Board init may never run, or fail before creating the bus. Retry only
     * the BSP's idempotent I2C init, not its GPIO/PMIC/display initialization. */
    if (!s_pmu_device) { e = bsp_i2c_init(); }
#endif
    if (!e) { e = read_device(0x34, &s_pmu_device); }
    uint8_t reg = 0x18, value, actual;
    if (!e) { e = i2c_master_transmit_receive(s_pmu_device, &reg, 1, &value, 1, 50); }
    if (!e) {
        uint8_t write[] = {reg, on ? value | 2 : value & ~2};
        e = i2c_master_transmit(s_pmu_device, write, sizeof(write), 50);
        if (!e) { e = i2c_master_transmit_receive(s_pmu_device, &reg, 1, &actual, 1, 50); }
        if (!e && actual != write[1]) { e = ESP_FAIL; }
    }
    return e;
}

/* Caller holds the mutex, or is booting/polling before a guard exists. */
static esp_err_t restore_charging_locked(unsigned attempts)
{
    esp_err_t e = ESP_OK;
    for (unsigned i = 0; s_charge_restore_pending && i < attempts; i++) {
        e = set_charging(true);
        if (!e) { s_charge_restore_pending = false; s_charge_wait = portMAX_DELAY; }
    }
    /* Keep the timeout visible until the core ends this run, even if the
     * guard has already restored charging and verified its readback. */
    if (!s_charge_restore_pending && !s_charge_run_active) { s_charge_expired = 0; }
    return e;
}

static void charge_guard(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_charge_mutex, portMAX_DELAY);
        TickType_t wait = s_charge_wait;
        xSemaphoreGive(s_charge_mutex);
        if (ulTaskNotifyTake(pdTRUE, wait)) { continue; }
        xSemaphoreTake(s_charge_mutex, portMAX_DELAY);
        /* An arm/disarm racing the timeout wins; start its new wait. */
        if (!ulTaskNotifyTake(pdTRUE, 0) && s_charge_restore_pending) {
            s_charge_expired = CHARGE_EXPIRED_MAGIC;
            if (restore_charging_locked(1)) { s_charge_wait = pdMS_TO_TICKS(5000); }
        }
        xSemaphoreGive(s_charge_mutex);
    }
}

/* No allocation is needed to keep a failed boot restore alive. The core polls
 * power even after initialization errors, including console/PM-lock failures. */
static void boot_charge_retry(void)
{
    if (!s_charge_guard && s_charge_restore_pending && esp_timer_get_time() >= s_boot_retry_us) {
        (void)restore_charging_locked(1);
        s_boot_retry_us = esp_timer_get_time() + 5000000;
    }
}

esp_err_t muse_ptest_board_charging(bool off, uint32_t guard_ms)
{
    if (!muse_ptest_board_charging_supported()) { return ESP_ERR_NOT_SUPPORTED; }
    if (off && !guard_ms) { return ESP_ERR_INVALID_ARG; }
    if (off && !s_charge_mutex) { s_charge_mutex = xSemaphoreCreateMutex(); }
    if (off && !s_charge_mutex) { return ESP_ERR_NO_MEM; }
    if (s_charge_mutex) { xSemaphoreTake(s_charge_mutex, portMAX_DELAY); }
    bool expired = !off && s_charge_expired == CHARGE_EXPIRED_MAGIC;
    if (!off) { s_charge_run_active = false; }
    esp_err_t e = restore_charging_locked(3);
    if (off && !e && !s_charge_guard) {
        if (xTaskCreate(charge_guard, "charge_guard", 4096, NULL, 1, &s_charge_guard) != pdPASS) { e = ESP_ERR_NO_MEM; }
    }
    if (off && !e) {
        s_charge_run_active = true;
        s_charge_expired = 0;
        s_charge_wait = pdMS_TO_TICKS(guard_ms);
        s_charge_restore_pending = true;
        /* Arm before the write: a failed write/readback may still disable it. */
        xTaskNotifyGive(s_charge_guard);
        e = set_charging(false);
    } else if (s_charge_guard) {
        if (s_charge_restore_pending) { s_charge_wait = pdMS_TO_TICKS(5000); }
        xTaskNotifyGive(s_charge_guard);
    }
    if (!e && expired) { e = ESP_ERR_TIMEOUT; }
    if (s_charge_mutex) { xSemaphoreGive(s_charge_mutex); }
    return e;
}

static bool charge_expired(void)
{
    if (s_charge_mutex) { xSemaphoreTake(s_charge_mutex, portMAX_DELAY); }
    bool expired = s_charge_expired == CHARGE_EXPIRED_MAGIC;
    if (s_charge_mutex) { xSemaphoreGive(s_charge_mutex); }
    return expired;
}

static void animate(lv_timer_t *timer)
{
    (void)timer;
    s_hue = (s_hue + 7) % 360;
    lv_obj_set_style_bg_color(s_screen, lv_color_hsv_to_rgb(s_hue, 100, 100), 0);
}

static esp_err_t generic_init(void)
{
    s_board = muse_board_get();
    if (!s_board || !s_board->init) { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t e = s_board->init();
    esp_err_t charging_error = restore_charging_locked(3);
    s_boot_retry_us = esp_timer_get_time() + 5000000;
    if (e) { return e; }
    if (charging_error) { return charging_error; }
    codec_read_devices_init();
    if (!s_board->display_start || !s_board->display_lock
        || !s_board->display_unlock || !s_board->set_brightness) { return ESP_ERR_NOT_SUPPORTED; }
    lv_indev_t *touch = NULL;
    s_display = s_board->display_start(&touch);
    if (!s_display) { return ESP_FAIL; }
    if (!s_board->display_lock(DISPLAY_LOCK_TIMEOUT_MS)) { return ESP_ERR_TIMEOUT; }
    s_screen = lv_display_get_screen_active(s_display);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    s_animation = lv_timer_create(animate, s_board->frame_ms > 0 ? s_board->frame_ms : 40, NULL);
    if (s_animation) { lv_timer_pause(s_animation); }
    s_board->display_unlock();
    if (!s_animation) { return ESP_ERR_NO_MEM; }

    return ESP_OK;
}

static muse_power_t s_power;
static esp_err_t console_lock(void);
esp_err_t muse_ptest_board_power(muse_ptest_readback_t *out)
{
    boot_charge_retry();
    if (!s_board) { return ESP_ERR_INVALID_STATE; }
    esp_err_t e = console_lock();
    if (e) { return e; }
    if (!CONFIG_MUSE_BOARD_AIPI && s_board->read_power) {
        e = s_board->read_power(&s_power);
        if (e) { return e; }
    }
    out->usb = CONFIG_MUSE_BOARD_AIPI ? usb_serial_jtag_is_connected() : s_power.usb;
    out->charging = s_power.charging;
    out->battery_mv = s_power.battery_mv;
    return ESP_OK;
}

static esp_err_t audio_close(void)
{
    if (s_board->audio_power) { s_board->audio_power(false); }
    esp_err_t first = s_audio_error;
    if (first == ESP_ERR_NOT_SUPPORTED && !s_spk && !s_mic) { first = ESP_OK; }
    if (s_spk_needs_close) {
        if (esp_codec_dev_close(s_spk) != ESP_CODEC_DEV_OK) { if (!first) { first = ESP_FAIL; } }
        else { s_spk_open = s_spk_needs_close = false; }
    }
    if (s_mic_needs_close) {
        if (esp_codec_dev_close(s_mic) != ESP_CODEC_DEV_OK) { if (!first) { first = ESP_FAIL; } }
        else { s_mic_open = s_mic_needs_close = false; }
    }
    if (!s_spk_open && !s_mic_open) { s_volume = 0; }
    if (s_audio_attempted) {
        /* close() ignores lower-level disable errors. Only hardware evidence
         * may promote history to suspended; unknown also survives MCU resets. */
        codec_history_set(0);
        codec_snapshot();
        if (!first && codec_shutdown_matches()) {
            codec_history_set(MUSE_PTEST_CODEC_SUSPENDED);
            s_codec_regs.expected = true;
        }
    }
    return first;
}

static esp_err_t audio_initialize(void)
{
    if (!s_board->audio_init) { return ESP_ERR_NOT_SUPPORTED; }
    if (!s_audio_attempted) {
        s_audio_attempted = true;
        codec_history_set(MUSE_PTEST_CODEC_INITIALIZED);
        s_audio_error = s_board->audio_init(&s_spk, &s_mic);
        if (!s_audio_error && (!s_spk || !s_mic)) { s_audio_error = ESP_ERR_NOT_SUPPORTED; }
    }
    return s_audio_error;
}

static esp_err_t audio_open(void)
{
    esp_err_t e = audio_initialize();
    if (e) { return e; }
    esp_codec_dev_sample_info_t fs = { .sample_rate = 16000, .channel = 2, .bits_per_sample = 16 };
    /* Failed opens can start shared I2S/amp resources; close both handles. */
    s_spk_needs_close = s_mic_needs_close = true;
    if (!s_spk_open) {
        if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) { return ESP_FAIL; }
        s_spk_open = true;
    }
    if (!s_mic_open) {
        if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) { return ESP_FAIL; }
        s_mic_open = true;
    }
    if (esp_codec_dev_set_out_vol(s_spk, 25) != ESP_CODEC_DEV_OK) { return ESP_FAIL; }
    s_volume = 25;
    if (s_board->set_mic_gain) { s_board->set_mic_gain(s_mic, 0); }
    else if (esp_codec_dev_set_in_gain(s_mic, 0) != ESP_CODEC_DEV_OK) { return ESP_FAIL; }
    return ESP_OK;
}

static esp_err_t display_apply(const muse_ptest_state_t *state)
{
    bool lit = state->backlight_pct >= 0;
    bool animated = !strcmp(state->id, "lcd_anim_bl_100");
    s_board->set_brightness(0);
    /* AIPI panel commands share the flush bus: send while LVGL is paused. */
    if (lit && s_board->panel_sleep && s_panel_asleep) {
        s_board->panel_sleep(false);
        s_panel_asleep = false;
    }
    /* Refresh through a running adapter; pause only after the frame is sent. */
    if (lit && s_paused) {
        s_board->display_pause(false);
        s_paused = false;
    }
    if (!s_board->display_lock(DISPLAY_LOCK_TIMEOUT_MS)) { return ESP_ERR_TIMEOUT; }
    lv_timer_pause(s_animation);
    lv_obj_set_style_bg_color(s_screen, !strncmp(state->id, "lcd_white_", 10) ? lv_color_white() : lv_color_black(), 0);
    if (lit) { lv_refr_now(s_display); }
    if (animated) {
        s_hue = 0;
        lv_timer_reset(s_animation);
        lv_timer_resume(s_animation);
    }
    s_board->display_unlock();
    if (lit) {
        s_board->set_brightness(state->backlight_pct);
    }
    /* Pause outside the LVGL lock: the adapter waits for its task. */
    if (!animated && s_board->display_pause && !s_paused) {
        s_board->display_pause(true);
        s_paused = true;
    }
    if (!lit && s_board->panel_sleep && !s_panel_asleep && s_paused) {
        s_board->panel_sleep(true);
        s_panel_asleep = true;
    }
    return ESP_OK;
}

esp_err_t muse_ptest_board_apply(const muse_ptest_state_t *state)
{
    if (CONFIG_MUSE_BOARD_AIPI && s_board && s_board->read_power) {
        esp_err_t e = s_board->read_power(&s_power);
        if (e) { return e; }
    }
    if (!s_display || !s_animation) { return ESP_ERR_INVALID_STATE; }
    if (state->load == MUSE_PTEST_UNSUPPORTED) { return ESP_ERR_NOT_SUPPORTED; }
    s_load = state->load;
    s_frames = 0;
    if (s_board->audio_power) { s_board->audio_power(false); }
    bool audio = state->load == MUSE_PTEST_CODECS_IDLE || state->load == MUSE_PTEST_MIC || state->load == MUSE_PTEST_SINE;
    esp_err_t e = audio ? audio_open() : audio_close();
    if (audio && e) { (void)audio_close(); }
    esp_err_t display_error = display_apply(state);
    codec_snapshot(); /* apply/cleanup boundary, including partial failures */
    if (e) { return e; }
    if (display_error) { return display_error; }
    if (audio && s_board->audio_power) { s_board->audio_power(state->amplifier); }
    return ESP_OK;
}

esp_err_t muse_ptest_board_readback(muse_ptest_readback_t *out)
{
    out->codec_state = muse_ptest_board_codec_history();
    esp_err_t e = muse_ptest_board_power(out);
    if (e) { return e; }
    out->backlight_duty = -1; /* Board API exposes no PWM/register readback. */
    out->codecs_open = s_spk_open && s_mic_open;
    out->configured_volume = s_volume;
    out->io_frames = s_frames;
    return ESP_OK;
}

esp_err_t muse_ptest_board_service(muse_ptest_load_t load)
{
    if (charge_expired()) { return ESP_ERR_TIMEOUT; }
    if (load != s_load) { return ESP_ERR_INVALID_STATE; }
    if (load != MUSE_PTEST_MIC && load != MUSE_PTEST_SINE) { return ESP_OK; }
    int16_t pcm[160 * 2];
    int e;
    if (load == MUSE_PTEST_MIC) {
        e = esp_codec_dev_read(s_mic, pcm, sizeof(pcm));
    } else {
        static const int16_t tone[16] = { 0,1579,2917,3811,4125,3811,2917,1579,0,-1579,-2917,-3811,-4125,-3811,-2917,-1579 };
        for (size_t i = 0; i < 160; i++) { pcm[2*i] = pcm[2*i+1] = tone[i % 16]; }
        e = esp_codec_dev_write(s_spk, pcm, sizeof(pcm));
    }
    if (e != ESP_CODEC_DEV_OK) { return ESP_FAIL; }
    s_frames += 160;
    return ESP_OK;
}

const char *muse_ptest_board_name(void) { return muse_board_get()->name; }
const muse_ptest_state_t *muse_ptest_board_matrix(const char *name, size_t *count)
{
    bool peripheral = name && !strcmp(name, "peripheral");
    if (count) { *count = peripheral ? MUSE_GENERIC_PTEST_STATES : 0; }
    return peripheral ? s_matrix : NULL;
}
esp_err_t muse_ptest_board_uart_restore(void) { return ESP_OK; }
esp_err_t muse_ptest_board_prepare_deep(bool held) { (void)held; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t muse_ptest_board_release_deep_holds(void) { return ESP_OK; }
bool muse_ptest_board_deep_holds_owned(void) { return false; }
void muse_ptest_board_usb_dryrun(bool on) { (void)on; }
_Static_assert(MUSE_GENERIC_PTEST_STATES * MUSE_PTEST_MAX_REPEATS <= MUSE_PTEST_MAX_RECORDS, "result capacity");

/* USB Serial/JTAG drops out in light sleep. Keep its own lock through a
 * two-second disconnect grace; physical VBUS evidence remains independent. */
static esp_pm_lock_handle_t s_console_lock;
static bool s_console_held, s_console_ready, s_tx_failed;
static int64_t s_usb_seen_us;
static esp_err_t console_lock(void)
{
    int64_t now = esp_timer_get_time();
    if (usb_serial_jtag_is_connected()) { s_usb_seen_us = now; }
    bool awake = now - s_usb_seen_us < 2000000;
    if (s_console_lock && awake != s_console_held) {
        esp_err_t e = awake ? esp_pm_lock_acquire(s_console_lock) : esp_pm_lock_release(s_console_lock);
        if (e) { return e; }
        s_console_held = awake;
    }
    return ESP_OK;
}
esp_err_t muse_ptest_board_console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    esp_err_t e = usb_serial_jtag_driver_install(&cfg);
    s_console_ready = !e;
    if (!e) { e = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ptest_console", &s_console_lock); }
    s_usb_seen_us = esp_timer_get_time();
    return e ? e : console_lock();
}
void muse_ptest_board_console_write(const char *buf, size_t n)
{
    if (!s_console_ready) { return; }
    while (n) {
        size_t chunk = n > 256 ? 256 : n;
        if (usb_serial_jtag_write_bytes(buf, chunk, pdMS_TO_TICKS(10)) != (int)chunk) {
            s_tx_failed = true; return;
        }
        buf += chunk; n -= chunk;
    }
}
bool muse_ptest_board_console_getc(uint8_t *c)
{
    return s_console_ready && usb_serial_jtag_read_bytes(c, 1, 0) == 1;
}
esp_err_t muse_ptest_board_console_flush(void)
{
    if (!s_console_ready) { return ESP_ERR_INVALID_STATE; }
    esp_err_t e = usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
    if (!e && s_tx_failed) { e = ESP_ERR_TIMEOUT; }
    s_tx_failed = false;
    return e;
}

#ifndef MUSE_PTEST_HOST
#include "muse_mem.h"
#endif
static esp_err_t s_init_result;
static void init_on_ui_core(void *arg)
{
    s_init_result = generic_init();
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}
esp_err_t muse_ptest_board_init(void)
{
    /* The PMIC bit survives resets, even without a valid saved run. Arm and
     * attempt recovery before task allocation or any board initialization. */
    s_charge_restore_pending = muse_ptest_board_charging_supported();
    (void)restore_charging_locked(3);
    s_boot_retry_us = esp_timer_get_time() + 5000000;
    if (xTaskCreatePinnedToCore(init_on_ui_core, "ptest_init", 8192,
            xTaskGetCurrentTaskHandle(), MUSE_UI_PRIORITY, NULL, MUSE_UI_CORE) != pdPASS) { return ESP_ERR_NO_MEM; }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return s_init_result;
}
