# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

"""Compile and exercise the reSpeaker XVF3800 audio board."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

FAKE = r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
typedef int esp_err_t;
typedef void *SemaphoreHandle_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef void *i2s_chan_handle_t;
typedef struct {
    int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct { bool enable_internal_pullup; } flags;
} i2c_master_bus_config_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
typedef struct { int port, role, dma_desc_num, dma_frame_num; bool auto_clear; } i2s_chan_config_t;
typedef struct { int sample_rate; } i2s_std_clk_config_t;
typedef struct { int data_bit_width, slot_mode; bool philips; } i2s_std_slot_config_t;
typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
} i2s_std_config_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_SIZE 1
#define ESP_ERR_INVALID_RESPONSE 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_TIMEOUT 5
#define ESP_FAIL 6
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 1
#define I2S_GPIO_UNUSED -1
#define I2S_DATA_BIT_WIDTH_32BIT 32
#define I2S_SLOT_MODE_STEREO 2
#define I2S_CHANNEL_DEFAULT_CONFIG(num, role_value) \
    ((i2s_chan_config_t){.port = (num), .role = (role_value)})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) ((i2s_std_clk_config_t){.sample_rate = (rate)})
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) \
    ((i2s_std_slot_config_t){.data_bit_width = (bits), .slot_mode = (mode), .philips = true})
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
const char *esp_err_to_name(esp_err_t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void xSemaphoreTake(SemaphoreHandle_t, uint32_t);
void xSemaphoreGive(SemaphoreHandle_t);
void vTaskDelay(uint32_t);
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *, i2c_master_bus_handle_t *);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *, i2c_master_dev_handle_t *);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t *, size_t, int);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t, uint8_t *, size_t, int);
esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, i2s_chan_handle_t *);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, int);
esp_err_t i2s_channel_write(i2s_chan_handle_t, const void *, size_t, size_t *, int);
'''

HARNESS = r'''
#include "fake.h"
#include <string.h>
#include "voice_board.h"

typedef struct {
    int attempts, receives;
    int tx_errors[128], rx_errors[128], status[128];
    uint8_t version[3];
} script_t;

static int locked, bus, xmos, codec, rx, tx, lock;
static bool mutex_null;
static int mutex_create_calls, bus_new_calls;
static int bus_error, add_error_at, bus_add_calls;
static int device_addresses[2];
static script_t version_script, gpo_script;
static script_t *pending_script;
static int pending_resource, pending_attempt;
static bool pending_receive;
static uint8_t gpo_values[5];
static int amp_attempts, amp_errors[16], amp_values[16];
static int codec_calls, codec_error_at;
static uint8_t codec_regs[64], codec_vals[64];
static int i2s_new_calls, i2s_new_error, i2s_mode_calls, i2s_mode_error_on;
static int tx_enable_calls, tx_enable_error, rx_enable_calls, rx_enable_error, rx_disable_calls;
static int i2s_read_calls, read_error, expect_post_capture;
static int write_calls, write_error;
static bool short_write;
static int32_t written_pcm[256];
static size_t written_samples;

const char *esp_err_to_name(esp_err_t err) { (void)err; return "fake"; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { mutex_create_calls++; return mutex_null ? NULL : &lock; }
void xSemaphoreTake(SemaphoreHandle_t h, uint32_t timeout) {
    (void)timeout;
    assert(h == &lock && !locked);
    locked = 1;
}
void xSemaphoreGive(SemaphoreHandle_t h) { assert(h == &lock && locked); locked = 0; }
static int boot_waits;
void vTaskDelay(uint32_t ms) {
    assert(ms == 10 || ms == 250);
    if (ms == 250) boot_waits++;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, void **handle) {
    bus_new_calls++;
    assert(cfg->i2c_port == 0 && cfg->sda_io_num == 5 && cfg->scl_io_num == 6);
    assert(cfg->glitch_ignore_cnt == 7 && cfg->flags.enable_internal_pullup);
    *handle = &bus;
    return bus_error;
}
esp_err_t i2c_master_bus_add_device(void *handle, const i2c_device_config_t *cfg, void **device) {
    assert(handle == &bus && cfg->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    assert(bus_add_calls < 2);
    device_addresses[bus_add_calls] = cfg->device_address;
    bus_add_calls++;
    *device = cfg->device_address == 0x2c ? &xmos : &codec;
    return add_error_at == bus_add_calls ? ESP_FAIL : ESP_OK;
}

esp_err_t i2c_master_transmit(void *handle, const uint8_t *data, size_t len, int timeout) {
    assert(locked && timeout == 100);
    if (handle == &codec) {
        assert(len == 2 && codec_calls < 64);
        assert(data[0] == 0 || data[0] == 0x2b || data[0] == 0x2c);
        if (data[0] == 0) assert(data[1] == 0);
        codec_regs[codec_calls] = data[0];
        codec_vals[codec_calls] = data[1];
        codec_calls++;
        return codec_error_at == codec_calls ? ESP_FAIL : ESP_OK;
    }
    assert(handle == &xmos);
    if (len == 3 && data[1] == 0x80) {
        assert(data[0] == 48 || data[0] == 20);
        assert(data[2] == (data[0] == 48 ? 4 : 6));
        pending_resource = data[0];
        pending_script = data[0] == 48 ? &version_script : &gpo_script;
        pending_attempt = pending_script->attempts++;
        pending_receive = true;
        if (pending_resource == 20 && expect_post_capture) assert(i2s_read_calls > 0);
        return pending_attempt < 128 ? pending_script->tx_errors[pending_attempt] : ESP_OK;
    }
    assert(len == 5 && data[0] == 20 && data[1] == 1 && data[2] == 2 && data[3] == 31);
    assert(amp_attempts < 16);
    amp_values[amp_attempts] = data[4];
    int attempt = amp_attempts++;
    return attempt < 16 ? amp_errors[attempt] : ESP_OK;
}

esp_err_t i2c_master_receive(void *handle, uint8_t *data, size_t len, int timeout) {
    assert(handle == &xmos && locked && timeout == 100 && pending_receive);
    pending_receive = false;
    pending_script->receives++;
    assert(len == (pending_resource == 48 ? 4u : 6u));
    memset(data, 0, len);
    data[0] = pending_attempt < 128 ? (uint8_t)pending_script->status[pending_attempt] : 0;
    if (pending_resource == 48) {
        for (int i = 0; i < 3; i++) data[i + 1] = version_script.version[i];
    } else {
        for (int i = 0; i < 5; i++) data[i + 1] = gpo_values[i];
    }
    return pending_attempt < 128 ? pending_script->rx_errors[pending_attempt] : ESP_OK;
}

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, void **out, void **in) {
    i2s_new_calls++;
    assert(cfg->port == I2S_NUM_0 && cfg->role == I2S_ROLE_MASTER);
    assert(cfg->dma_desc_num == 6 && cfg->dma_frame_num == 320 && cfg->auto_clear);
    *out = &tx;
    *in = &rx;
    return i2s_new_error;
}
esp_err_t i2s_channel_init_std_mode(void *handle, const i2s_std_config_t *cfg) {
    assert(handle == &tx || handle == &rx);
    assert(cfg->clk_cfg.sample_rate == 16000);
    assert(cfg->slot_cfg.data_bit_width == 32 && cfg->slot_cfg.slot_mode == I2S_SLOT_MODE_STEREO);
    assert(cfg->slot_cfg.philips);
    assert(cfg->gpio_cfg.mclk == I2S_GPIO_UNUSED && cfg->gpio_cfg.bclk == 8 && cfg->gpio_cfg.ws == 7);
    assert(cfg->gpio_cfg.dout == 44 && cfg->gpio_cfg.din == 43);
    i2s_mode_calls++;
    return i2s_mode_error_on == i2s_mode_calls ? ESP_FAIL : ESP_OK;
}
esp_err_t i2s_channel_enable(void *handle) {
    if (handle == &tx) {
        tx_enable_calls++;
        return tx_enable_error;
    }
    assert(handle == &rx);
    rx_enable_calls++;
    return rx_enable_error;
}
esp_err_t i2s_channel_disable(void *handle) { assert(handle == &rx); rx_disable_calls++; return ESP_OK; }
esp_err_t i2s_channel_read(void *handle, void *out, size_t len, size_t *got, int timeout) {
    assert(handle == &rx && timeout == 500 && len <= 320 * 8 && len % 8 == 0);
    i2s_read_calls++;
    if (read_error) { *got = 0; return read_error; }
    const int32_t left[] = {INT32_C(0x11110000), INT32_MIN, INT32_C(0x0001ffff), -1};
    const int32_t right[] = {INT32_C(0x7fff1234), INT32_MIN, INT32_C(0x00020000), 0};
    int32_t *samples = out;
    for (size_t i = 0; i < len / 8; i++) {
        samples[2 * i] = left[i % 4];
        samples[2 * i + 1] = right[i % 4];
    }
    *got = len;
    return ESP_OK;
}
esp_err_t i2s_channel_write(void *handle, const void *pcm, size_t len, size_t *got, int timeout) {
    assert(handle == &tx && timeout == 1000 && len <= 32 * 8);
    assert(written_samples + len / 4 <= 256);
    memcpy(written_pcm + written_samples, pcm, len);
    written_samples += len / 4;
    write_calls++;
    *got = short_write ? len - 4 : len;
    return write_error;
}

static void expect_init(void) {
    assert(voice_board_init() == ESP_OK);
    assert(mutex_create_calls == 1 && bus_new_calls == 1);
    assert(bus_add_calls == 2 && device_addresses[0] == 0x2c && device_addresses[1] == 0x18);
    assert(version_script.attempts >= 1 && !pending_receive && !locked);
    assert(amp_attempts == 1 && amp_values[0] == 1);
    assert(i2s_new_calls == 1 && i2s_mode_calls == 2);
    assert(tx_enable_calls == 1 && rx_enable_calls == 0 && rx_disable_calls == 0);
    assert(!voice_board_echo_cancelled() && voice_board_dial_steps() == 0);
}

static void expect_failed_safe(void) {
    int xmos_calls = version_script.attempts + gpo_script.attempts + amp_attempts;
    int codec_before = codec_calls;
    int writes_before = write_calls;
    int16_t pcm[4] = {42, 42, 42, 42};
    int32_t playback[6] = {0};
    int peak = -1;
    assert(voice_board_mic_start() == ESP_ERR_INVALID_STATE);
    assert(voice_board_mic_read(pcm, 4, &peak) == 0 && peak == 0);
    voice_board_mic_stop();
    assert(voice_board_speaker_write(playback, 3) == ESP_ERR_INVALID_STATE);
    voice_board_amp(true);
    voice_board_amp(false);
    voice_board_set_volume(50);
    assert(voice_board_muted());
    assert(!voice_board_echo_cancelled() && voice_board_dial_steps() == 0);
    assert(version_script.attempts + gpo_script.attempts + amp_attempts == xmos_calls);
    assert(codec_calls == codec_before && write_calls == writes_before && !locked);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    version_script.version[0] = 1;
    version_script.version[1] = 0;
    version_script.version[2] = 9;
    const char *scenario = argv[1];
    if (!strcmp(scenario, "init")) {
        expect_init();
    } else if (!strcmp(scenario, "newer")) {
        version_script.version[1] = 1;
        version_script.version[2] = 0;
        expect_init();
    } else if (!strcmp(scenario, "old")) {
        version_script.version[2]--;
        assert(voice_board_init() == ESP_ERR_INVALID_RESPONSE && i2s_new_calls == 0);
        expect_failed_safe();
    } else if (!strcmp(scenario, "major")) {
        version_script.version[0] = 2;
        assert(voice_board_init() == ESP_ERR_INVALID_RESPONSE && i2s_new_calls == 0);
        expect_failed_safe();
    } else if (!strcmp(scenario, "retry")) {
        version_script.tx_errors[0] = ESP_FAIL;
        version_script.status[1] = 64;
        expect_init();
        assert(version_script.attempts == 3 && version_script.receives == 2 && boot_waits == 0);
    } else if (!strcmp(scenario, "cold_boot")) {
        for (int i = 0; i < 7; i++) version_script.tx_errors[i] = ESP_FAIL;
        expect_init();
        assert(version_script.attempts == 8 && boot_waits == 1);
    } else if (!strcmp(scenario, "retry_exhausted")) {
        for (int i = 0; i < 128; i++) version_script.status[i] = 64;
        assert(voice_board_init() == ESP_ERR_TIMEOUT);
        assert(version_script.attempts == 65 && version_script.receives == 65 && boot_waits == 12);
        expect_failed_safe();
    } else if (!strcmp(scenario, "transient_exhausted")) {
        for (int i = 0; i < 128; i++) version_script.tx_errors[i] = ESP_FAIL;
        assert(voice_board_init() == ESP_FAIL);
        assert(version_script.attempts == 65 && version_script.receives == 0 && boot_waits == 12);
        expect_failed_safe();
    } else if (!strcmp(scenario, "status_error")) {
        for (int i = 0; i < 128; i++) version_script.status[i] = 1;
        assert(voice_board_init() == ESP_ERR_INVALID_RESPONSE);
        assert(version_script.attempts == 13 && boot_waits == 12);
        expect_failed_safe();
    } else if (!strcmp(scenario, "cold_boot_nack")) {
        for (int i = 0; i < 7; i++) version_script.tx_errors[i] = ESP_ERR_INVALID_RESPONSE;
        expect_init();
        assert(version_script.attempts == 8 && boot_waits == 1);
    } else if (!strcmp(scenario, "bus_failure")) {
        bus_error = ESP_FAIL;
        assert(voice_board_init() == ESP_FAIL && bus_add_calls == 0);
        expect_failed_safe();
    } else if (!strcmp(scenario, "add_failure")) {
        add_error_at = 2;
        assert(voice_board_init() == ESP_FAIL && version_script.attempts == 0);
        expect_failed_safe();
    } else if (!strcmp(scenario, "mutex_failure")) {
        mutex_null = true;
        assert(voice_board_init() == ESP_ERR_NO_MEM && bus_add_calls == 0);
        expect_failed_safe();
    } else if (!strcmp(scenario, "amp_failure")) {
        for (int i = 0; i < 16; i++) amp_errors[i] = ESP_FAIL;
        assert(voice_board_init() == ESP_FAIL && amp_attempts == 5 && i2s_new_calls == 0);
        expect_failed_safe();
    } else if (!strcmp(scenario, "i2s_new_failure")) {
        i2s_new_error = ESP_FAIL;
        assert(voice_board_init() == ESP_FAIL && i2s_mode_calls == 0);
        expect_failed_safe();
    } else if (!strcmp(scenario, "i2s_mode_failure")) {
        i2s_mode_error_on = 2;
        assert(voice_board_init() == ESP_FAIL && tx_enable_calls == 0);
        expect_failed_safe();
    } else if (!strcmp(scenario, "i2s_enable_failure")) {
        tx_enable_error = ESP_FAIL;
        assert(voice_board_init() == ESP_FAIL && rx_enable_calls == 0);
        expect_failed_safe();
    } else if (!strcmp(scenario, "mute")) {
        expect_init();
        gpo_values[0] = gpo_values[2] = gpo_values[3] = gpo_values[4] = 1;
        assert(!voice_board_muted());
        gpo_values[1] = 1;
        assert(voice_board_muted());
        gpo_values[1] = 0;
        int before = gpo_script.attempts;
        gpo_script.status[before] = 64;
        assert(!voice_board_muted() && gpo_script.attempts == before + 2);
        for (int i = 0; i < 16; i++) gpo_script.rx_errors[i] = ESP_FAIL;
        before = gpo_script.attempts;
        assert(voice_board_muted() && gpo_script.attempts == before + 5);
        for (int i = 0; i < 16; i++) { gpo_script.rx_errors[i] = 0; gpo_script.status[i] = 1; }
        before = gpo_script.attempts;
        assert(voice_board_muted() && gpo_script.attempts == before + 1 && !locked);
    } else if (!strcmp(scenario, "amp")) {
        expect_init();
        voice_board_amp(true);
        voice_board_amp(false);
        assert(amp_attempts == 3 && amp_values[1] == 0 && amp_values[2] == 1 && !locked);
    } else if (!strcmp(scenario, "capture")) {
        expect_init();
        int16_t pcm[324];
        for (int i = 0; i < 324; i++) pcm[i] = 42;
        int peak;
        assert(voice_board_mic_read(pcm, 4, &peak) == 0 && peak == 0 && i2s_read_calls == 0);
        assert(voice_board_mic_start() == ESP_OK);
        assert(voice_board_mic_start() == ESP_OK && rx_enable_calls == 1);
        int gpo_before = gpo_script.attempts;
        assert(voice_board_mic_read(pcm, 324, &peak) == 320 && peak == 32768);
        assert(pcm[0] == 32767 && pcm[1] == INT16_MIN && pcm[2] == 2 && pcm[3] == 0);
        assert(gpo_script.attempts == gpo_before + 1);
        for (int i = 320; i < 324; i++) assert(pcm[i] == 42);
        voice_board_mic_stop();
        voice_board_mic_stop();
        assert(rx_disable_calls == 1);
        assert(voice_board_mic_read(pcm, 4, &peak) == 0 && peak == 0);
    } else if (!strcmp(scenario, "capture_error")) {
        expect_init();
        assert(voice_board_mic_start() == ESP_OK);
        int16_t pcm[4];
        int peak = -1;
        int gpo_before = gpo_script.attempts;
        read_error = ESP_FAIL;
        assert(voice_board_mic_read(pcm, 4, &peak) == 0 && peak == 0);
        assert(gpo_script.attempts == gpo_before);
    } else if (!strcmp(scenario, "capture_muted")) {
        expect_init();
        assert(voice_board_mic_start() == ESP_OK);
        expect_post_capture = 1;
        int16_t pcm[4] = {42, 42, 42, 42};
        int peak = -1;
        gpo_values[1] = 1;
        assert(voice_board_mic_read(pcm, 4, &peak) == 0 && peak == 0 && pcm[0] == 42);
        gpo_values[1] = 0;
        for (int i = 0; i < 16; i++) gpo_script.rx_errors[i] = ESP_FAIL;
        assert(voice_board_mic_read(pcm, 4, &peak) == 0 && peak == 0 && pcm[0] == 42);
    } else if (!strcmp(scenario, "speaker")) {
        expect_init();
        int32_t pcm[99 * 2];
        for (int i = 0; i < 99; i++) { pcm[2 * i] = INT32_MAX; pcm[2 * i + 1] = INT32_MIN; }
        assert(voice_board_speaker_write(pcm, 99) == ESP_OK);
        assert(write_calls == 2 && written_samples == 66);
        for (int i = 0; i < 33; i++) {
            assert(written_pcm[2 * i] == INT32_MAX && written_pcm[2 * i + 1] == INT32_MIN);
        }
        int32_t mixed[] = {0, -3, 3, -6, 6, -9};
        written_samples = 0;
        assert(voice_board_speaker_write(mixed, 3) == ESP_OK);
        assert(written_pcm[0] == 3 && written_pcm[1] == -6);
        int writes_before = write_calls;
        assert(voice_board_speaker_write(mixed, 2) == ESP_ERR_INVALID_SIZE);
        assert(write_calls == writes_before);
        short_write = true;
        assert(voice_board_speaker_write(mixed, 3) == ESP_ERR_TIMEOUT);
        short_write = false;
        write_error = ESP_ERR_INVALID_STATE;
        assert(voice_board_speaker_write(mixed, 3) == ESP_ERR_INVALID_STATE);
        write_error = 0;
        assert(voice_board_speaker_write(mixed, 0) == ESP_OK);
    } else if (!strcmp(scenario, "volume")) {
        expect_init();
        voice_board_set_volume(-10);
        assert(codec_calls == 3 && codec_regs[0] == 0 && codec_regs[1] == 0x2b && codec_regs[2] == 0x2c);
        assert(codec_vals[1] == 0x80 && codec_vals[2] == 0x80);
        voice_board_set_volume(60);
        assert(codec_vals[4] == 51 && codec_vals[5] == 51 && !(codec_vals[4] & 0x80));
        voice_board_set_volume(1);
        assert(codec_vals[7] == 126 && codec_vals[8] == 126);
        voice_board_set_volume(110);
        assert(codec_vals[10] == 0 && codec_vals[11] == 0);
        codec_error_at = codec_calls + 2;
        int before = codec_calls;
        voice_board_set_volume(50);
        assert(codec_calls == before + 2 && !locked);
        codec_error_at = 0;
        before = codec_calls;
        voice_board_set_volume(100);
        assert(codec_calls == before + 3 && codec_vals[before + 1] == 0 && codec_vals[before + 2] == 0);
    } else {
        assert(false);
    }
    assert(!locked);
    return 0;
}
'''

COMMON_SCENARIOS = (
    "init",
    "newer",
    "old",
    "major",
    "retry",
    "cold_boot",
    "cold_boot_nack",
    "retry_exhausted",
    "transient_exhausted",
    "status_error",
    "bus_failure",
    "add_failure",
    "mutex_failure",
    "i2s_new_failure",
    "i2s_mode_failure",
    "i2s_enable_failure",
    "capture",
    "capture_error",
    "speaker",
    "volume",
)
SCENARIOS = COMMON_SCENARIOS + ("amp_failure", "amp", "mute", "capture_muted")


class RespeakerXvf3800AudioTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        tmp = Path(cls.temp.name)
        (tmp / "fake.h").write_text(FAKE)
        for name in (
            "esp_err.h",
            "esp_log.h",
            "driver/i2c_master.h",
            "driver/i2s_std.h",
            "freertos/FreeRTOS.h",
            "freertos/semphr.h",
            "freertos/task.h",
        ):
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "fake.h"\n')
        (tmp / "test.c").write_text(HARNESS)
        cls.tmp = tmp
        cls.executable = tmp / "test-xvf3800"
        result = subprocess.run(
            shlex.split(os.environ.get("CC", "cc"))
            + [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-DCONFIG_HOMEHUB_VOICE_BOARD_RESPEAKER_XVF3800=1",
                "-I",
                str(tmp),
                "-I",
                str(ROOT / "main"),
                str(ROOT / "main" / "voice_board_xvf3800.c"),
                str(tmp / "test.c"),
                "-o",
                str(cls.executable),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_scenario(self, scenario):
        result = subprocess.run(
            [str(self.executable), scenario],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, f"XVF3800/{scenario}\n{result.stdout}{result.stderr}")

    def test_initialization_version_retry_and_pcm_behavior(self):
        for scenario in SCENARIOS:
            with self.subTest(scenario=scenario):
                self.run_scenario(scenario)

    def test_compile_time_profile_guard(self):
        result = subprocess.run(
            shlex.split(os.environ.get("CC", "cc"))
            + ["-std=c11", "-Wall", "-Wextra", "-Werror"]
            + [
                "-I",
                str(self.tmp),
                "-I",
                str(ROOT / "main"),
                "-c",
                str(ROOT / "main" / "voice_board_xvf3800.c"),
                "-o",
                str(self.tmp / "guard.o"),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("requires CONFIG_HOMEHUB_VOICE_BOARD_RESPEAKER_XVF3800", result.stderr)


if __name__ == "__main__":
    unittest.main()
