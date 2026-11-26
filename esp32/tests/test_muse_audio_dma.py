# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The codecs open only when their I2S DMA buffers fit (muse_audio_dma.h).

Opening reconfigures each I2S channel, and a new buffer size makes ESP-IDF
reallocate its DMA buffers; if that fails, esp_codec_dev enables the channel
anyway and it faults (LoadProhibited in i2s_rx_channel_start, the Waveshare
S3 1.75C, twice, then SAFE). The real muse_audio.c runs here
against fakes: short of DMA memory it never calls esp_codec_dev_open(), says
why once, leaves voice off with everything else up, waits out reads and
writes at the audio's pace, and opens on a later read, write or wake once the
memory is there; a codec at rest is never reopened by a read.
"""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"

FAKES = r"""
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_RETURN_ON_FALSE(c, err, tag, msg) do { if (!(c)) { ESP_LOGE(tag, "%s", msg); return err; } } while (0)
typedef enum { ESP_LOG_NONE, ESP_LOG_INFO } esp_log_level_t;
static inline esp_log_level_t esp_log_level_get(const char *t) { (void)t; return ESP_LOG_INFO; }
static inline void esp_log_level_set(const char *t, esp_log_level_t l) { (void)t; (void)l; }
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_DMA (1u << 3)
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
void *heap_caps_malloc(size_t n, unsigned caps);
int64_t esp_timer_get_time(void);
typedef uint32_t TickType_t;
typedef void *SemaphoreHandle_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);
int xSemaphoreGive(SemaphoreHandle_t s);
void vTaskDelay(TickType_t t);
typedef void *esp_codec_dev_handle_t;
typedef struct { uint32_t sample_rate; uint8_t channel; uint8_t bits_per_sample; } esp_codec_dev_sample_info_t;
#define ESP_CODEC_DEV_OK 0
int esp_codec_dev_open(esp_codec_dev_handle_t h, esp_codec_dev_sample_info_t *fs);
int esp_codec_dev_close(esp_codec_dev_handle_t h);
int esp_codec_dev_read(esp_codec_dev_handle_t h, void *data, int len);
int esp_codec_dev_write(esp_codec_dev_handle_t h, void *data, int len);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t h, int vol);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t h, float db);
typedef int gpio_num_t;
typedef int lv_align_t;
typedef struct lv_display lv_display_t;
typedef struct lv_indev lv_indev_t;
"""

HARNESS = r"""
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_settings.h"

static size_t s_free = 100000, s_largest = 50000;
static int64_t s_now;
static int s_opens, s_closes, s_power_on, s_power_off, s_delays;
static TickType_t s_last_delay;
static int s_spk_open, s_mic_open;
static int s_open_fail;   /* the next speaker open fails (not for memory) */
static int s_dev_spk = 1, s_dev_mic = 2;

size_t heap_caps_get_free_size(unsigned caps) { assert(caps & MALLOC_CAP_DMA); return s_free; }
size_t heap_caps_get_largest_free_block(unsigned caps) { assert(caps & MALLOC_CAP_DMA); return s_largest; }
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
int64_t esp_timer_get_time(void) { return s_now; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
/* s_lock_held: another task holds the audio lock and never gives it back (the
 * W175's voice task). A read or write must give up, never wait forever. */
static int s_lock_held;
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void)s;
    if (s_lock_held) {
        assert(t != portMAX_DELAY && "an unbounded take of a held lock: the voice task hangs here");
        s_now += (int64_t)t * 1000;
        return 0;
    }
    return 1;
}
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
void vTaskDelay(TickType_t t) { s_delays++; s_last_delay = t; s_now += (int64_t)t * 1000; }
int esp_codec_dev_open(esp_codec_dev_handle_t h, esp_codec_dev_sample_info_t *fs)
{
    assert(fs->sample_rate == MUSE_AUDIO_RATE && fs->channel == 2 && fs->bits_per_sample == 16);
    if (s_open_fail && h == &s_dev_spk) { s_open_fail = 0; return -1; }
    s_opens++;
    if (h == &s_dev_spk) s_spk_open = 1; else s_mic_open = 1;
    return 0;
}
int esp_codec_dev_close(esp_codec_dev_handle_t h) { s_closes++; if (h == &s_dev_spk) s_spk_open = 0; else s_mic_open = 0; return 0; }
int esp_codec_dev_read(esp_codec_dev_handle_t h, void *d, int len) { assert(h == &s_dev_mic && s_mic_open); memset(d, 0, len); return 0; }
int esp_codec_dev_write(esp_codec_dev_handle_t h, void *d, int len) { (void)d; (void)len; assert(h == &s_dev_spk && s_spk_open); return 0; }
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t h, int v) { (void)h; (void)v; return 0; }
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t h, float db) { (void)h; (void)db; return 0; }
int muse_settings_volume(void) { return 50; }
bool muse_settings_speaker_on(void) { return true; }
/* muse_audio_tx_esp.c's (a board attaches its TX channel there; none here): pieces as asked, nothing noted. */
size_t muse_audio_tx_note_begin(size_t frames) { return frames; }
void muse_audio_tx_note_write(const int16_t *mono, size_t frames, bool mute) { (void)mono; (void)frames; (void)mute; }
int muse_settings_mic_gain(void) { return 24; }

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    *spk = &s_dev_spk;
    *mic = &s_dev_mic;
    return ESP_OK;
}
static void codec_power(bool on) { if (on) s_power_on++; else s_power_off++; }
static const muse_board_t s_board = { .audio_init = audio_init, .codec_power = codec_power, .mic_slot = 0 };
const muse_board_t *muse_board = &s_board;

int main(void)
{
    int16_t pcm[MUSE_AUDIO_CHUNK];
    /* Short of DMA memory at start: not opened, voice off, the rest goes on. */
    s_free = 9000;
    assert(muse_audio_init(50, 24) == ESP_OK);
    assert(s_opens == 0 && s_power_off == 1);
    /* Reads and writes wait out their audio (20 ms) and fail; no retry within a second. */
    assert(muse_audio_read(pcm, MUSE_AUDIO_CHUNK) == ESP_ERR_INVALID_STATE);
    assert(s_last_delay == 20 && s_opens == 0);
    s_free = 100000;
    assert(muse_audio_write(pcm, MUSE_AUDIO_CHUNK) == ESP_ERR_INVALID_STATE);
    assert(s_opens == 0);
    /* A second later, with the memory there: a read opens them and reads. */
    s_now += 1000 * 1000;
    assert(muse_audio_read(pcm, MUSE_AUDIO_CHUNK) == ESP_OK);
    assert(s_opens == 2 && s_spk_open && s_mic_open);
    assert(muse_audio_write(pcm, MUSE_AUDIO_CHUNK) == ESP_OK);
    printf("deferred, then opened by a read\n");

    /* Rest, then wake short of memory: the channels kept their buffers from
     * the open (the same format: ESP-IDF reallocates only for a new size), so
     * the codecs open with no new memory. The StopWatch on battery: its
     * I2S as the board sets it up (ESP-IDF's defaults, 16-bit stereo, the
     * format they open in), rest at every dark screen, and a BLE video build
     * with less than the 12.5 KB the guard asked each wake. */
    muse_audio_power(false);
    assert(!s_spk_open && !s_mic_open);
    s_largest = 500;
    s_free = 3000;
    muse_audio_power(true);
    assert(s_spk_open && s_mic_open && s_opens == 4);
    assert(muse_audio_read(pcm, MUSE_AUDIO_CHUNK) == ESP_OK);
    printf("rest and wake: the kept buffers\n");
    /* Resting again: nothing reopens them, memory or not; the next wake does. */
    muse_audio_power(false);
    s_now += 5 * 1000 * 1000;
    assert(muse_audio_read(pcm, MUSE_AUDIO_CHUNK) == ESP_ERR_INVALID_STATE);
    assert(s_opens == 4);
    muse_audio_power(true);
    assert(s_spk_open && s_mic_open && s_opens == 6);
    printf("rest and wake\n");

    /* Awake but closed (an open that failed), and another task holds the
     * lock for good: a read gives up after 5 s and fails; nothing hangs. */
    muse_audio_power(false);
    s_open_fail = 1;
    muse_audio_power(true);
    assert(!s_spk_open);
    s_now += 2 * 1000 * 1000;
    s_lock_held = 1;
    int64_t t0 = s_now;
    assert(muse_audio_read(pcm, MUSE_AUDIO_CHUNK) == ESP_ERR_INVALID_STATE);
    assert(s_now - t0 >= 5000 * 1000 && s_now - t0 < 6000 * 1000);
    assert(!s_spk_open);
    s_lock_held = 0;
    s_now += 2 * 1000 * 1000;
    assert(muse_audio_read(pcm, MUSE_AUDIO_CHUNK) == ESP_OK);   /* free again: opens and reads */
    printf("held lock: the read gave up\n");
    return 0;
}
"""

# The StopWatch's exact path (on battery): audio_init sets its I2S channels up in the format muse_audio
# opens (i2s_ready_at_init), with less internal DMA memory free than the guard's full budget, at boot and at every
# wake; rest comes before any open succeeded (a first open that failed for any reason); then a client's start_mic
# wakes it. Frames flow, and playback runs at real time (no closed_wait).
STOPWATCH = HARNESS[:HARNESS.index("static const muse_board_t s_board")] + r"""
static const muse_board_t s_board = { .audio_init = audio_init, .codec_power = codec_power, .mic_slot = 0,
                                      .i2s_ready_at_init = true };
const muse_board_t *muse_board = &s_board;

int main(void)
{
    int16_t pcm[MUSE_AUDIO_CHUNK];
    /* Boot short of the full budget: the board's buffers are there, so it opens. */
    s_free = 3000;
    s_largest = 500;
    assert(muse_audio_init(50, 24) == ESP_OK);
    assert(s_opens == 2 && s_spk_open && s_mic_open);
    /* Rest, then wake (a client's start_mic): opens, reads, plays at real time. */
    muse_audio_power(false);
    muse_audio_power(true);
    assert(s_spk_open && s_mic_open && s_opens == 4);
    int delays = s_delays;
    for (int i = 0; i < 100; i++) {
        assert(muse_audio_read(pcm, MUSE_AUDIO_CHUNK) == ESP_OK);
        assert(muse_audio_write(pcm, MUSE_AUDIO_CHUNK) == ESP_OK);
    }
    assert(s_delays == delays);   /* no closed_wait: 2 s of audio takes the codec's 2 s, not twice that */
    printf("stopwatch: boot, rest, wake: open\n");
    /* A first open that failed (not for memory), rest before any open, then
     * the wake: the board's buffers still count. */
    muse_audio_power(false);
    s_open_fail = 1;
    muse_audio_power(true);
    assert(!s_spk_open);
    muse_audio_power(false);
    muse_audio_power(true);
    assert(s_spk_open && s_mic_open);
    assert(muse_audio_read(pcm, MUSE_AUDIO_CHUNK) == ESP_OK);
    printf("stopwatch: rest before any open, then the wake: open\n");
    return 0;
}
"""


def compile_and_run(test: unittest.TestCase, files: dict[str, str], sources: list[Path], expect: list[str]) -> str:
    """Builds files (written to a stage; the .c ones compiled) with sources and runs it."""
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp)
        for name, text in files.items():
            (stage / name).parent.mkdir(parents=True, exist_ok=True)
            (stage / name).write_text(text)
        exe = stage / "run"
        build = subprocess.run(
            cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined",
                  "-I", str(stage), "-I", str(MUSE), "-I", str(MUSE.parent / "muse_mirror")]
            + [str(stage / n) for n in files if n.endswith(".c")] + [str(s) for s in sources] + ["-lm", "-o", str(exe)],
            capture_output=True, text=True)
        test.assertEqual(build.returncode, 0, build.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        test.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        for line in expect:
            test.assertIn(line, run.stdout)
        return run.stdout


class AudioDmaGuardTest(unittest.TestCase):

    def test_codecs_open_only_when_their_dma_fits(self) -> None:
        files = {"fakes.h": FAKES, "harness.c": HARNESS, "sdkconfig.h": "#define CONFIG_SPIRAM 1\n"}
        for name in ("esp_check.h", "esp_codec_dev.h", "esp_heap_caps.h", "esp_log.h", "esp_timer.h", "esp_err.h",
                     "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h", "driver/gpio.h", "lvgl.h"):
            files[name] = '#include "fakes.h"\n'
        out = compile_and_run(self, files, [MUSE / "muse_audio.c", MUSE / "muse_audio_dma.c", MUSE / "muse_spk_dsp.c"],
                              ["deferred, then opened by a read", "rest and wake: the kept buffers", "rest and wake",
                               "held lock: the read gave up",
                               "ensure_open: the audio lock is still held by",
                               "audio unavailable: 12512 B DMA needed (960 B buffers), 9000 B free, largest 50000 B"])
        # Said once per spell, and its end too: only at start, before the channels held buffers of this size.
        self.assertEqual(out.count("audio unavailable"), 1)
        self.assertEqual(out.count("audio available again"), 1)

    def test_the_stopwatch_path_on_battery(self) -> None:
        files = {"fakes.h": FAKES, "harness.c": STOPWATCH, "sdkconfig.h": "#define CONFIG_SPIRAM 1\n"}
        for name in ("esp_check.h", "esp_codec_dev.h", "esp_heap_caps.h", "esp_log.h", "esp_timer.h", "esp_err.h",
                     "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h", "driver/gpio.h", "lvgl.h"):
            files[name] = '#include "fakes.h"\n'
        out = compile_and_run(self, files, [MUSE / "muse_audio.c", MUSE / "muse_audio_dma.c", MUSE / "muse_spk_dsp.c"],
                              ["stopwatch: boot, rest, wake: open", "stopwatch: rest before any open, then the wake: open"])
        self.assertNotIn("audio unavailable", out)


if __name__ == "__main__":
    unittest.main()
