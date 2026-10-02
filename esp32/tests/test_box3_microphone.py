# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

"""Exercise the BOX-3 ADC power lifecycle without physical I2C hardware."""
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
#include <stdlib.h>
#include <assert.h>
typedef int esp_err_t;
typedef int esp_codec_dev_type_t;
typedef void *esp_codec_dev_handle_t;
typedef uint32_t TickType_t;
typedef void *SemaphoreHandle_t;
typedef struct { int sample_rate, channel, bits_per_sample; } esp_codec_dev_sample_info_t;
typedef struct { int unused; } audio_codec_ctrl_if_t;
typedef struct audio_codec_if_t audio_codec_if_t;
struct audio_codec_if_t {
    int (*set_fs)(const audio_codec_if_t *, esp_codec_dev_sample_info_t *);
    int (*enable)(const audio_codec_if_t *, bool);
    int (*set_mic_gain)(const audio_codec_if_t *, float);
    int (*mute_mic)(const audio_codec_if_t *, bool);
};
typedef struct audio_codec_data_if_t audio_codec_data_if_t;
struct audio_codec_data_if_t {
    bool (*is_open)(const audio_codec_data_if_t *);
    int (*enable)(const audio_codec_data_if_t *, esp_codec_dev_type_t, bool);
    int (*set_fmt)(const audio_codec_data_if_t *, esp_codec_dev_type_t, esp_codec_dev_sample_info_t *);
    int (*read)(const audio_codec_data_if_t *, uint8_t *, int);
};
typedef struct { const audio_codec_ctrl_if_t *ctrl_if; } es7210_codec_cfg_t;
typedef struct { int port, addr; void *bus_handle; } audio_codec_i2c_cfg_t;
typedef struct { int dev_type; const audio_codec_data_if_t *data_if; } esp_codec_dev_cfg_t;
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
#define GPIO_MODE_INPUT 1
#define BSP_BUTTON_MUTE_IO 1
#define BSP_I2C_NUM 0
#define ES7210_CODEC_DEFAULT_ADDR 0x80
#define ESP_CODEC_DEV_TYPE_IN 1
#define ESP_CODEC_DEV_OK 0
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGI(tag, ...) ((void)(tag))
int gpio_get_level(int);
int gpio_config(const gpio_config_t *);
TickType_t xTaskGetTickCount(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void xSemaphoreTake(SemaphoreHandle_t, TickType_t);
void xSemaphoreGive(SemaphoreHandle_t);
const audio_codec_data_if_t *bsp_audio_get_codec_itf(void);
void *bsp_i2c_get_handle(void);
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *);
const audio_codec_if_t *es7210_codec_new(es7210_codec_cfg_t *);
int audio_codec_delete_codec_if(const audio_codec_if_t *);
esp_codec_dev_handle_t esp_codec_dev_new(esp_codec_dev_cfg_t *);
'''

HARNESS = r'''
#include "fake.h"
#include <string.h>
#include "box_3_microphone.h"
static bool hw_muted, fail_init;
static int creations, deletions, reads, locked;
static float gain;
static TickType_t ticks;
static const audio_codec_data_if_t *mic;
int gpio_get_level(int pin) { assert(pin == 1); return !hw_muted; }
int gpio_config(const gpio_config_t *cfg) { assert(cfg->mode == GPIO_MODE_INPUT); return 0; }
TickType_t xTaskGetTickCount(void) { return ticks; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &locked; }
void xSemaphoreTake(SemaphoreHandle_t h, TickType_t t) { (void)h; (void)t; assert(!locked); locked = 1; }
void xSemaphoreGive(SemaphoreHandle_t h) { (void)h; assert(locked); locked = 0; }
static int set_fs(const audio_codec_if_t *h, esp_codec_dev_sample_info_t *fs) {
    (void)h; assert(!hw_muted); assert(fs->sample_rate == 16000 && fs->channel == 2); return 0;
}
static int enable(const audio_codec_if_t *h, bool v) { (void)h; (void)v; assert(!hw_muted); return 0; }
static int set_gain(const audio_codec_if_t *h, float db) { (void)h; assert(!hw_muted); gain = db; return 0; }
static const audio_codec_if_t adc = { set_fs, enable, set_gain, enable };
const audio_codec_if_t *es7210_codec_new(es7210_codec_cfg_t *cfg) {
    assert(cfg->ctrl_if && !hw_muted); creations++; return fail_init ? NULL : &adc;
}
int audio_codec_delete_codec_if(const audio_codec_if_t *h) { assert(h && !hw_muted); deletions++; return 0; }
static bool bus_open(const audio_codec_data_if_t *h) { (void)h; return true; }
static int bus_enable(const audio_codec_data_if_t *h, int type, bool v) { (void)h; (void)type; (void)v; return 0; }
static int bus_fmt(const audio_codec_data_if_t *h, int type, esp_codec_dev_sample_info_t *fs) {
    (void)h; (void)type; (void)fs; return 0;
}
static int bus_read(const audio_codec_data_if_t *h, uint8_t *data, int n) {
    (void)h; memset(data, 0x25, n); ticks += 20; reads++; return 0;
}
static const audio_codec_data_if_t bus = { bus_open, bus_enable, bus_fmt, bus_read };
const audio_codec_data_if_t *bsp_audio_get_codec_itf(void) { return &bus; }
void *bsp_i2c_get_handle(void) { return &ticks; }
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *cfg) {
    static audio_codec_ctrl_if_t ctrl; assert(cfg->addr == 0x80); return &ctrl;
}
esp_codec_dev_handle_t esp_codec_dev_new(esp_codec_dev_cfg_t *cfg) { mic = cfg->data_if; return &ticks; }
static bool capture(void) {
    uint8_t data[1280];
    assert(mic->read(mic, data, sizeof(data)) == 0);
    for (unsigned i = 1; i < sizeof(data); i++) assert(data[i] == data[0]);
    return data[0] != 0;
}
static void expect_audio(void) {
    bool audio = false;
    for (int i = 0; i < 50; i++) audio |= capture();
    assert(audio);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    hw_muted = !strcmp(argv[1], "muted_boot");
    assert(box_3_microphone_init() == 0 && box_3_microphone_create());
    esp_codec_dev_sample_info_t fs = {16000, 2, 16};
    assert(mic->is_open(mic));
    assert(mic->set_fmt(mic, 1, &fs) == 0 && mic->enable(mic, 1, true) == 0);
    box_3_microphone_set_gain(30);
    if (hw_muted) {
        for (int i = 0; i < 50; i++) assert(!capture());
        assert(creations == 0 && reads == 50);
        hw_muted = false;
    }
    expect_audio(); assert(gain == 30);
    int before = creations;
    hw_muted = true; box_3_microphone_poll();
    box_3_microphone_set_gain(33); // Saved without I2C access to the unpowered ADC.
    if (strcmp(argv[1], "mute_during_playback")) {
        for (int i = 0; i < 10; i++) assert(!capture());
    }
    hw_muted = false;
    if (!strcmp(argv[1], "retry")) {
        fail_init = true;
        for (int i = 0; i < 50; i++) assert(!capture());
        assert(creations - before <= 2); // Retry throttling, no tight I2C loop.
        fail_init = false;
    }
    expect_audio(); assert(creations > before && gain == 34.5f && deletions >= 1);
    assert(mic->enable(mic, 1, false) == 0);
    assert(mic->enable(mic, 1, true) == 0);
    expect_audio(); // Sleep/wake reopens the input with its saved format and gain.
    return 0;
}
'''


class Box3MicrophoneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        tmp = Path(cls.temp.name)
        (tmp / "fake.h").write_text(FAKE)
        for name in ("esp_codec_dev.h", "esp_err.h", "bsp/esp-bsp.h",
                     "esp_codec_dev_defaults.h", "esp_log.h", "freertos/FreeRTOS.h",
                     "freertos/semphr.h"):
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "fake.h"\n')
        (tmp / "test.c").write_text(HARNESS)
        cls.exe = tmp / "test"
        board = ROOT / "components/muse/boards"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp), "-I", str(board),
            str(board / "box_3_microphone.c"), str(tmp / "test.c"), "-o", str(cls.exe)
        ], check=True, capture_output=True)

    def test_power_lifecycle(self):
        for scenario in ("normal", "muted_boot", "mute_during_playback", "retry"):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.exe), scenario], check=True, capture_output=True, timeout=10)
