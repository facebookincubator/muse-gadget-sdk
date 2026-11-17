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
/* Pins and PDM operations follow board_reterminal_sticky.c, based on Seeed's
 * factory_fw main/boards/reterminal_e1005. GPIO19/20 belong to the microphone;
 * the diagnostic console uses the CH343 UART, never native USB Serial/JTAG. */
#include <string.h>
#include "muse_board.h"
#include "muse_power_test_board.h"
#include "sticky_power_test_display.h"

#define STATE(id, note, load, mhz) { id, note, load, mhz, -1, false, 0, 1000, MUSE_PTEST_CODEC_KEEP, "other", "" }
#define PANEL(id, note, load, role) { id, note, load, 80, -1, false, 0, 20, MUSE_PTEST_CODEC_KEEP, role, "epaper" }
static const muse_ptest_state_t s_matrix[] = {
    STATE("baseline_pre", "DFS/light sleep; panel deep-sleep requested, PDM off, touch/SD rails on, charging enabled", MUSE_PTEST_IDLE, 0),
    STATE("cpu_fixed_40_idle", "fixed CPU clock; PDM off, panel resting", MUSE_PTEST_IDLE, 40),
    STATE("cpu_fixed_80_idle", "fixed CPU clock; PDM off, panel resting", MUSE_PTEST_IDLE, 80),
    STATE("cpu_fixed_160_idle", "fixed CPU clock; PDM off, panel resting", MUSE_PTEST_IDLE, 160),
    STATE("cpu_fixed_240_idle", "fixed CPU clock; PDM off, panel resting", MUSE_PTEST_IDLE, 240),
    STATE("baseline_cpu_post", "paired DFS baseline after fixed CPU clocks", MUSE_PTEST_IDLE, 0),
    PANEL("epaper_off_pre", "panel deep-sleep requested; fixed 80MHz reference", MUSE_PTEST_IDLE, "ref"),
    PANEL("epaper_full", "FULL black/white alternation; 500ms rest after each BUSY-confirmed refresh", MUSE_PTEST_EPAPER, "variant"),
    PANEL("epaper_off_between", "panel deep-sleep requested; fixed 80MHz reference", MUSE_PTEST_IDLE, "ref"),
    PANEL("epaper_fast", "FAST black/white alternation after FULL white seed; 500ms rest", MUSE_PTEST_EPAPER, "variant"),
    PANEL("epaper_off_post", "panel deep-sleep requested; fixed 80MHz reference", MUSE_PTEST_IDLE, "ref"),
    STATE("lcd_brightness", "unsupported: no LCD or backlight", MUSE_PTEST_UNSUPPORTED, 80),
    STATE("codecs_open", "unsupported: PDM microphone, no external audio codecs", MUSE_PTEST_UNSUPPORTED, 80),
    STATE("sine_1k_minus18dbfs", "unsupported: no speaker", MUSE_PTEST_UNSUPPORTED, 80),
    STATE("mic_off_pre", "PDM rail off; fixed 80MHz reference", MUSE_PTEST_IDLE, 80),
    STATE("mic_capture_16k", "mono 16kHz PDM drained/discarded; microphone GPIO38 on", MUSE_PTEST_MIC, 80),
    STATE("mic_off_post", "PDM stopped, GPIO38 off; fixed 80MHz reference", MUSE_PTEST_IDLE, 80),
    STATE("wifi_scanning", "unassociated active scans; no credentials or NVS", MUSE_PTEST_WIFI_SCAN, 80),
    STATE("baseline_post", "paired DFS/light-sleep baseline; PDM off, touch/SD rails on, charging enabled", MUSE_PTEST_IDLE, 0),
};
#undef STATE
#undef PANEL
_Static_assert(sizeof(s_matrix) / sizeof(s_matrix[0]) * MUSE_PTEST_MAX_REPEATS <= MUSE_PTEST_MAX_RECORDS, "result capacity");

static const muse_board_t *s_board;
static bool s_ready, s_mic_attempted, s_mic_on;
static esp_err_t s_mic_error;
static uint32_t s_frames;
static muse_ptest_load_t s_load;
static muse_power_t s_power;

const char *muse_ptest_board_name(void) { return "Seeed reTerminal Sticky"; }
const muse_ptest_state_t *muse_ptest_board_matrix(const char *name, size_t *count)
{
    bool available = name && !strcmp(name, "peripheral");
    if (count) { *count = available ? sizeof(s_matrix) / sizeof(s_matrix[0]) : 0; }
    return available ? s_matrix : NULL;
}

esp_err_t muse_ptest_board_stop(void)
{
    esp_err_t e = ESP_OK;
    if (s_mic_on) {
        e = s_board->mic_power(false);
        if (!e) { s_mic_on = false; }
    }
    /* A failed I2S disable must not leave the microphone powered. */
    esp_err_t rail = gpio_set_level(GPIO_NUM_38, 0);
    esp_err_t panel = muse_sticky_ptest_display_stop();
    return e ? e : rail ? rail : panel;
}

esp_err_t muse_ptest_board_init(void)
{
    s_board = muse_board_get();
    if (!s_board || !s_board->init || !s_board->mic_init || !s_board->mic_power || !s_board->mic_read || !s_board->read_power) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t e = s_board->init();
    if (e) { return e; }
    e = gpio_input_enable(GPIO_NUM_38);
    if (e) { return e; }
    e = muse_sticky_ptest_display_init();
    if (e) { return e; }
    e = s_board->read_power(&s_power);
    if (e) { return e; }
    e = muse_ptest_board_stop();
    if (e) { return e; }
    s_ready = true;
    return ESP_OK;
}

esp_err_t muse_ptest_board_power(muse_ptest_readback_t *out)
{
    /* VBUS is cheap to poll; gauge I2C belongs only at state boundaries. */
    out->usb = gpio_get_level(GPIO_NUM_9) != 0;
    out->charging = out->usb && s_power.charging;
    out->battery_mv = s_power.battery_mv;
    return s_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t muse_ptest_board_apply(const muse_ptest_state_t *state)
{
    if (!s_ready) { return ESP_ERR_INVALID_STATE; }
    esp_err_t e = muse_ptest_board_stop();
    if (e) { return e; }
    s_frames = 0;
    s_load = MUSE_PTEST_IDLE;
    if (state->load == MUSE_PTEST_UNSUPPORTED) { return ESP_ERR_NOT_SUPPORTED; }
    if (state->load != MUSE_PTEST_IDLE && state->load != MUSE_PTEST_MIC && state->load != MUSE_PTEST_WIFI_SCAN && state->load != MUSE_PTEST_EPAPER) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (state->load == MUSE_PTEST_EPAPER) {
        e = muse_sticky_ptest_display_start(!strcmp(state->id, "epaper_fast"));
        if (e) { (void)muse_ptest_board_stop(); return e; }
    }
    if (state->load == MUSE_PTEST_MIC) {
        if (!s_mic_attempted) {
            s_mic_attempted = true;
            s_mic_error = s_board->mic_init();
        }
        if (s_mic_error) { return s_mic_error; }
        /* Include a partially successful enable in cleanup ownership. */
        s_mic_on = true;
        e = s_board->mic_power(true);
        if (e) { (void)muse_ptest_board_stop(); return e; }
    }
    s_load = state->load;
    return ESP_OK;
}

esp_err_t muse_ptest_board_readback(muse_ptest_readback_t *out)
{
    if (!s_ready) { return ESP_ERR_INVALID_STATE; }
    if ((gpio_get_level(GPIO_NUM_38) != 0) != s_mic_on) { return ESP_FAIL; }
    esp_err_t e = s_board->read_power(&s_power);
    if (e) { return e; }
    /* Sample VBUS after gauge I2C and gate cached charging on that level. */
    e = muse_ptest_board_power(out);
    if (e) { return e; }
    out->backlight_duty = -1;
    out->codecs_open = false;
    out->codec_state = 0;
    out->configured_volume = 0;
    out->io_frames = s_load == MUSE_PTEST_EPAPER ? muse_sticky_ptest_display_frames() : s_frames;
    return ESP_OK;
}

esp_err_t muse_ptest_board_service(muse_ptest_load_t load)
{
    if (!s_ready || load != s_load) { return ESP_ERR_INVALID_STATE; }
    if (load == MUSE_PTEST_EPAPER) { return muse_sticky_ptest_display_service(); }
    if (load != MUSE_PTEST_MIC) { return ESP_OK; }
    if (!s_mic_on) { return ESP_ERR_INVALID_STATE; }
    int16_t pcm[160];
    esp_err_t e = s_board->mic_read(pcm, sizeof(pcm) / sizeof(pcm[0]));
    if (!e) { s_frames += sizeof(pcm) / sizeof(pcm[0]); }
    return e;
}

esp_err_t muse_ptest_board_uart_restore(void) { return ESP_OK; }
esp_err_t muse_ptest_board_prepare_deep(bool held) { (void)held; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t muse_ptest_board_release_deep_holds(void) { return ESP_OK; }
bool muse_ptest_board_deep_holds_owned(void) { return false; }
bool muse_ptest_board_warm_seen(void) { return false; }
unsigned muse_ptest_board_codec_history(void) { return 0; }
void muse_ptest_board_restore_codec_history(unsigned history) { (void)history; }
void muse_ptest_board_codec_regs(muse_ptest_codec_regs_t *out) { memset(out, 0, sizeof(*out)); }
void muse_ptest_board_usb_dryrun(bool on) { (void)on; }
