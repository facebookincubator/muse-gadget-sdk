# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

"""Exercise the reSpeaker audio driver without physical I2C/I2S hardware."""
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
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
typedef struct {
    int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct { bool enable_internal_pullup; } flags;
} i2c_master_bus_config_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
typedef struct { int dma_desc_num, dma_frame_num; bool auto_clear; } i2s_chan_config_t;
typedef struct {
    int clk_cfg, slot_cfg;
    struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
} i2s_std_config_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_SIZE 1
#define ESP_ERR_INVALID_RESPONSE 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_TIMEOUT 5
#define GPIO_MODE_OUTPUT 1
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
#define I2S_GPIO_UNUSED -1
#define I2S_CHANNEL_DEFAULT_CONFIG(num, role) ((i2s_chan_config_t){0})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) 32
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
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(int, int);
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
static int locked, command, muted, read_error, status_error;
static int rx, tx, codec, xmos, bus, lock;
static int8_t gains[2];
static uint8_t dac_mute;
static int write_calls, write_error;
static bool short_write;
static int32_t written_pcm[256];
static size_t written_samples;
const char *esp_err_to_name(esp_err_t err) { (void)err; return "fake"; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &lock; }
void xSemaphoreTake(SemaphoreHandle_t h, uint32_t t) { (void)h; (void)t; assert(!locked); locked = 1; }
void xSemaphoreGive(SemaphoreHandle_t h) { (void)h; assert(locked); locked = 0; }
void vTaskDelay(uint32_t ms) { (void)ms; }
esp_err_t gpio_config(const gpio_config_t *cfg) { assert(cfg->pin_bit_mask == (1ULL << 2)); return 0; }
esp_err_t gpio_set_level(int pin, int level) { assert(pin == 2); (void)level; return 0; }
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, void **h) {
    assert(cfg->sda_io_num == 5 && cfg->scl_io_num == 6); *h = &bus; return 0;
}
esp_err_t i2c_master_bus_add_device(void *h, const i2c_device_config_t *cfg, void **dev) {
    assert(h == &bus); *dev = cfg->device_address == 0x42 ? &xmos : &codec; return 0;
}
esp_err_t i2c_master_transmit(void *h, const uint8_t *data, size_t n, int timeout) {
    (void)timeout; assert(locked);
    if (h == &xmos) {
        assert(n == 3 && !command);
        assert((data[0] == 0xf0 && data[1] == 0xd8 && data[2] == 4) ||
               (data[0] == 0xf1 && data[1] == 0x81 && data[2] == 2));
        command = data[0];
    } else {
        assert(h == &codec && n == 2);
        if (data[0] == 0x41 || data[0] == 0x42) gains[data[0] - 0x41] = (int8_t)data[1];
        if (data[0] == 0x40) dac_mute = data[1];
    }
    return 0;
}
esp_err_t i2c_master_receive(void *h, uint8_t *data, size_t n, int timeout) {
    (void)timeout; assert(h == &xmos && locked && command);
    memset(data, 0, n); data[0] = status_error;
    if (command == 0xf0) { assert(n == 4); data[1] = 1; data[3] = 9; }
    else { assert(n == 2); data[1] = muted; }
    command = 0; return read_error;
}
esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, void **out, void **in) {
    assert(cfg->dma_frame_num == 320); *out = &tx; *in = &rx; return 0;
}
esp_err_t i2s_channel_init_std_mode(void *h, const i2s_std_config_t *cfg) {
    assert(h == &rx || h == &tx); assert(cfg->clk_cfg == 16000);
    assert(cfg->gpio_cfg.bclk == 8 && cfg->gpio_cfg.ws == 7);
    assert(cfg->gpio_cfg.dout == 43 && cfg->gpio_cfg.din == 44); return 0;
}
esp_err_t i2s_channel_enable(void *h) { assert(h == &rx || h == &tx); return 0; }
esp_err_t i2s_channel_disable(void *h) { assert(h == &rx); return 0; }
esp_err_t i2s_channel_read(void *h, void *out, size_t n, size_t *got, int timeout) {
    (void)timeout; assert(h == &rx && n <= 320 * 8);
    int32_t *pcm = out;
    const int32_t left[] = {INT32_C(0x7fff1234), INT32_MIN, INT32_C(0x0001ffff), -1};
    for (size_t i = 0; i < n / 8; i++) { pcm[2*i] = left[i % 4]; pcm[2*i+1] = INT32_MAX; }
    *got = n; return 0;
}
esp_err_t i2s_channel_write(void *h, const void *pcm, size_t n, size_t *got, int timeout) {
    (void)timeout; assert(h == &tx && n <= 32 * 8);
    assert(written_samples + n / 4 <= 256);
    memcpy(written_pcm + written_samples, pcm, n); written_samples += n / 4;
    write_calls++; *got = short_write ? n - 4 : n; return write_error;
}
int main(int argc, char **argv) {
    assert(argc == 2 && voice_board_init() == ESP_OK);
    if (!strcmp(argv[1], "mute")) {
        assert(!voice_board_muted()); muted = 1; assert(voice_board_muted());
        muted = 0; read_error = ESP_ERR_TIMEOUT; assert(voice_board_muted());
        read_error = 0; status_error = 1; assert(voice_board_muted());
        status_error = 0; assert(!voice_board_muted()); assert(!locked && !command);
    } else if (!strcmp(argv[1], "microphone")) {
        int16_t pcm[324]; int peak;
        for (int i = 0; i < 324; i++) pcm[i] = 42;
        assert(voice_board_mic_read(pcm, 4, &peak) == 0 && peak == 0);
        assert(voice_board_mic_start() == ESP_OK);
        assert(voice_board_mic_read(pcm, 324, &peak) == 320 && peak == 32768);
        assert(pcm[0] == 32767 && pcm[1] == -32768 && pcm[2] == 1 && pcm[3] == -1);
        for (int i = 320; i < 324; i++) assert(pcm[i] == 42);
        voice_board_mic_stop(); assert(voice_board_mic_read(pcm, 4, &peak) == 0);
    } else if (!strcmp(argv[1], "speaker")) {
        int32_t pcm[99 * 2];
        for (int i = 0; i < 99; i++) { pcm[2*i] = INT32_MAX; pcm[2*i+1] = INT32_MIN; }
        assert(voice_board_speaker_write(pcm, 99) == ESP_OK);
        assert(write_calls == 2 && written_samples == 66);
        for (int i = 0; i < 33; i++) { assert(written_pcm[2*i] == INT32_MAX); assert(written_pcm[2*i+1] == INT32_MIN); }
        int32_t mixed[] = {0, -3, 3, -6, 6, -9}; written_samples = 0;
        assert(voice_board_speaker_write(mixed, 3) == ESP_OK);
        assert(written_pcm[0] == 3 && written_pcm[1] == -6);
        assert(voice_board_speaker_write(mixed, 2) == ESP_ERR_INVALID_SIZE);
        short_write = true; assert(voice_board_speaker_write(mixed, 3) == ESP_ERR_TIMEOUT);
        short_write = false; write_error = ESP_ERR_INVALID_STATE;
        assert(voice_board_speaker_write(mixed, 3) == ESP_ERR_INVALID_STATE);
    } else if (!strcmp(argv[1], "volume")) {
        voice_board_set_volume(-10); assert(gains[0] == -127 && gains[1] == -127 && dac_mute == 0x0c);
        voice_board_set_volume(60); assert(gains[0] == -51 && gains[1] == -51 && dac_mute == 0);
        voice_board_set_volume(110); assert(gains[0] == 0 && gains[1] == 0 && dac_mute == 0);
    } else { assert(false); }
    return 0;
}
'''


class RespeakerAudioTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        tmp = Path(cls.temp.name)
        (tmp / "fake.h").write_text(FAKE)
        for name in ("esp_err.h", "esp_log.h", "driver/gpio.h",
                     "driver/i2c_master.h", "driver/i2s_std.h",
                     "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"):
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "fake.h"\n')
        (tmp / "test.c").write_text(HARNESS)
        cls.exe = tmp / "test"
        result = subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp),
            "-I", str(ROOT / "main"), str(ROOT / "main/voice_board_respeaker_lite.c"),
            str(tmp / "test.c"), "-o", str(cls.exe)
        ], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_scenario(self, scenario):
        result = subprocess.run([str(self.exe), scenario], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_mute_and_i2c_failures(self):
        self.run_scenario("mute")

    def test_microphone_channel_peak_and_bounds(self):
        self.run_scenario("microphone")

    def test_speaker_conversion_and_write_failures(self):
        self.run_scenario("speaker")

    def test_volume_limits(self):
        self.run_scenario("volume")
