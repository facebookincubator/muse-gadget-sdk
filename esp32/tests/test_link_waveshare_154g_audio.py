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

"""Compile the 1.54G's start cue against a fake I2S microphone: when
voice_board_mic_start() returns, nothing the mic buffered during the cue is
left for voice.c to count, so a tap released during the cue stays a tap."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
typedef void *SemaphoreHandle_t;
typedef void *i2s_chan_handle_t;
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
#define VOICE_MIC_RATE 16000
#define AUDIO_CHUNK 320
#define ESP_LOGW(tag, ...) ((void)(tag))
static const char *TAG = "test";
__attribute__((unused)) static const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }

static SemaphoreHandle_t s_lock;
static i2s_chan_handle_t s_rx;
static bool s_recording, s_cue;
static int16_t s_stereo[AUDIO_CHUNK * 2];

/* Fake time and a 16 kHz microphone filling 12 DMA buffers of 240 frames. */
static int64_t now_us;
static int64_t mic_frames;          /* buffered, not yet read */
#define DMA_FRAMES (12 * 240)
static void advance(int64_t us) {
    now_us += us;
    mic_frames += us * VOICE_MIC_RATE / 1000000;
    if (mic_frames > DMA_FRAMES) mic_frames = DMA_FRAMES;
}
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(int ms) { advance((int64_t)ms * 1000); }
static int xSemaphoreTake(SemaphoreHandle_t l, int t) { (void)l, (void)t; return 1; }
static void xSemaphoreGive(SemaphoreHandle_t l) { (void)l; }
static bool speaker_on(void) { return true; }
static void release(void) {}
static esp_err_t write_stereo(const int16_t *s, size_t frames) {
    (void)s;
    advance((int64_t)frames * 1000000 / VOICE_MIC_RATE);   /* the tone plays out */
    return ESP_OK;
}
/* Whole DMA buffers are returned; without enough buffered, wait up to timeout_ms. */
static esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *dst, size_t size, size_t *got, int timeout_ms) {
    (void)h, (void)dst;
    int64_t want = (int64_t)(size / 4);
    if (mic_frames < want && timeout_ms > 0) {
        int64_t need_us = (want - mic_frames) * 1000000 / VOICE_MIC_RATE + 1;
        advance(need_us < timeout_ms * 1000LL ? need_us : timeout_ms * 1000LL);
    }
    int64_t take = mic_frames < want ? (mic_frames / 240) * 240 : want;
    mic_frames -= take;
    *got = (size_t)(take * 4);
    return take < want ? ESP_ERR_TIMEOUT : ESP_OK;
}

%(cue)s

int main(void) {
    s_recording = true;
    mic_frames = DMA_FRAMES;     /* the codec just powered up and has been capturing */
    cue(false);
    printf("left %lld frames (%lld ms)\n", (long long)mic_frames, (long long)(mic_frames * 1000 / VOICE_MIC_RATE));
    /* Less than one 20 ms read may remain, never the cue tail's samples. */
    assert(mic_frames < VOICE_MIC_RATE / 50);
    assert(!s_cue);
    return 0;
}
'''


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class LinkWaveshare154gAudioTest(unittest.TestCase):
    def test_start_cue_leaves_no_buffered_microphone_audio(self):
        source = (ROOT / "main/voice_board_waveshare_154g.c").read_text()
        cue = function(source, "static void cue(bool end)")
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / "cue.c").write_text(HARNESS.replace("%(cue)s", cue))
            cc = shlex.split(os.environ.get("CC", "cc"))
            compiled = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(out / "cue.c"), "-o", str(out / "cue")],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(out / "cue")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
