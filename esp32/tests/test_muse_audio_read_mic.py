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

"""A codec-less mic beside a speaker codec (the CrowPanel's PDM mic).

It is read through the board's mic_read, as on mic-only boards, and muse_audio
applies the gain last set: the loopback test steps the gain through
muse_audio_set_mic_gain() without saving it, so the samples must follow it.
A PDM mic's DC offset must not saturate the samples before the high-pass.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


MUSE = Path(__file__).resolve().parents[1] / "components" / "muse"


def _function(source: str, name: str) -> str:
    match = re.search(r"^[A-Za-z_][^\n;]*\b" + re.escape(name) + r"\([^;{]*\)\s*\{", source, re.M)
    if not match:
        raise AssertionError(f"{name} not found")
    depth = 0
    for i in range(match.end() - 1, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : i + 1]
    raise AssertionError(f"could not find end of {name}")


HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_CODEC_DEV_OK 0
#define CHANNELS 2
#define MUSE_AUDIO_CHUNK 16
typedef int esp_err_t;
typedef int *esp_codec_dev_handle_t;
static int codec_gain_calls, reads;
static int16_t next_sample;
static int biased;   /* 2000 + 100*sin: a DC offset with a small tone */
static unsigned phase;
static int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t h, float db) { (void)h; (void)db; codec_gain_calls++; return 0; }
static int esp_codec_dev_read(esp_codec_dev_handle_t h, void *b, int n) { (void)h; (void)b; (void)n; return 0; }
static bool codecs_reachable(void) { return true; }
static esp_err_t board_mic_read(int16_t *mono, size_t frames) {
    reads++;
    for (size_t i = 0; i < frames; i++, phase++) {
        mono[i] = biased ? (int16_t)(2000 + 100 * sin(phase * 2 * M_PI / 16))
                         : (int16_t)((phase & 1) ? next_sample : -next_sample);
    }
    return ESP_OK;
}
static struct {
    int (*mic_init)(void);
    esp_err_t (*mic_read)(int16_t *, size_t);
    void (*set_mic_gain)(esp_codec_dev_handle_t, int);
    int mic_slot;
} board = { .mic_read = board_mic_read }, *muse_board = &board;
static esp_codec_dev_handle_t s_mic;
static float s_mic_scale = 1.0f;
static int16_t s_in_stereo[MUSE_AUDIO_CHUNK * CHANNELS];
static int16_t s_in_mono[MUSE_AUDIO_CHUNK];
static float s_hpf_a, s_hpf_x1, s_hpf_y1;
'''

MAIN = r'''
int main(void) {
    next_sample = 1000;
    apply_mic_gain(0);
    assert(read_input(MUSE_AUDIO_CHUNK) == ESP_OK && reads == 1);
    assert(s_in_stereo[0] == -1000 && s_in_stereo[1] == -1000 && s_in_stereo[2] == 1000);
    apply_mic_gain(20);              /* x10, as the loopback test steps it */
    assert(read_input(MUSE_AUDIO_CHUNK) == ESP_OK);
    assert(s_in_stereo[0] == -10000 && s_in_stereo[2 * MUSE_AUDIO_CHUNK - 1] == 10000);
    next_sample = 10000;             /* saturates instead of wrapping */
    assert(read_input(MUSE_AUDIO_CHUNK) == ESP_OK);
    assert(s_in_stereo[0] == -32768 && s_in_stereo[3] == 32767);
    assert(codec_gain_calls == 0);

    /* A biased stream at the default 30 dB: the tone survives the high-pass. */
    float rc = 1.0f / (2.0f * (float)M_PI * 80.0f), dt = 1.0f / 16000.0f;
    s_hpf_a = rc / (rc + dt);
    biased = 1;
    apply_mic_gain(30);
    assert(read_input(MUSE_AUDIO_CHUNK) == ESP_OK);   /* diagnostics: DC dropped first */
    int lo = 0, hi = 0;
    for (int i = 0; i < MUSE_AUDIO_CHUNK; i++) {
        lo = s_in_stereo[2 * i] < lo ? s_in_stereo[2 * i] : lo;
        hi = s_in_stereo[2 * i] > hi ? s_in_stereo[2 * i] : hi;
    }
    assert(hi > 2000 && hi < 32767 && lo < -2000 && lo > -32768);
    int16_t out[MUSE_AUDIO_CHUNK];
    for (int k = 0; k < 400; k++) assert(muse_audio_read(out, MUSE_AUDIO_CHUNK) == ESP_OK);
    lo = hi = 0;
    for (int i = 0; i < MUSE_AUDIO_CHUNK; i++) {
        lo = out[i] < lo ? out[i] : lo;
        hi = out[i] > hi ? out[i] : hi;
    }
    assert(hi > 2000 && lo < -2000);  /* ~3160 peak, not silence */

    board.mic_read = NULL;           /* neither a codec nor mic_read: fail */
    assert(read_input(MUSE_AUDIO_CHUNK) == ESP_FAIL);
    puts("ok");
    return 0;
}
'''


class MuseAudioCodeclessMicTest(unittest.TestCase):
    def test_mic_read_beside_a_speaker_codec_follows_the_applied_gain(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C compiler unavailable")
        source = (MUSE / "muse_audio.c").read_text()
        code = HARNESS + "\n".join(_function(source, n) for n in ("apply_mic_gain", "read_input", "muse_audio_read")) + MAIN
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "mic.c"
            path.write_text(code)
            subprocess.run(cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", str(path),
                                 "-o", str(path.with_suffix("")), "-lm"], check=True)
            result = subprocess.run([str(path.with_suffix(""))], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_board_contract_has_one_codecless_mic_hook(self) -> None:
        header = (MUSE / "muse_board.h").read_text()
        self.assertIn("esp_err_t (*mic_read)(int16_t *mono, size_t frames);", header)
        source = (MUSE / "muse_audio.c").read_text()
        init = _function(source, "muse_audio_init")
        self.assertIn("(s_mic || muse_board->mic_read)", init)


if __name__ == "__main__":
    unittest.main()
