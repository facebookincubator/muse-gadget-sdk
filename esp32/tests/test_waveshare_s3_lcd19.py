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

"""The Waveshare ESP32-S3-Touch-LCD-1.9's wiring into the board list, and the
shared Kconfig and component rules it must not disturb."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
BOARD = MUSE / 'boards/board_waveshare_s3_lcd19.c'


def dependencies():
    """Each managed dependency's name and `if` rules, in file order."""
    deps, current = [], None
    for line in (MUSE / 'idf_component.yml').read_text().splitlines():
        top = re.match(r'^  ([\w./-]+):\s*$', line)
        if top:
            current = (top.group(1), [])
            deps.append(current)
            continue
        rule = re.match(r'^\s+- if:\s*(.+)$', line)
        if rule and current:
            current[1].append(rule.group(1))
    return deps


def function(source, name):
    match = re.search(r"static (?:esp_err_t|void|int) " + name + r"\([^)]*\)\n\{", source)
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def board_choice():
    """(config name, its depends-on lines) for each MUSE_BOARD choice entry."""
    text = (MUSE / 'Kconfig').read_text()
    choice = text[text.index('choice MUSE_BOARD'):text.index('endchoice')]
    entries = re.split(r'^\s*config (MUSE_BOARD_\w+)\s*$', choice, flags=re.M)[1:]
    return [(name, re.findall(r'^\s*depends on (.+)$', body, flags=re.M))
            for name, body in zip(entries[::2], entries[1::2])]


class WaveshareS3Lcd19(unittest.TestCase):
    def test_no_dependency_is_declared_twice(self):
        names = [name for name, _ in dependencies()]
        self.assertEqual(len(names), len(set(names)), names)

    def test_ili9341_stays_on_the_fnk0104b(self):
        # Rules are ANDed: a second rule that excludes fnk0104b drops the
        # FNK0104B's panel driver.
        rules = dict(dependencies())['espressif/esp_lcd_ili9341']
        self.assertEqual(rules, ['"$CONFIG{MUSE_BOARD_ID} == \\"fnk0104b\\""'])

    def test_lcd19_gets_its_panel_touch_and_lvgl_components(self):
        deps = dict(dependencies())
        for name in ('espressif/esp_lcd_sh8601', 'espressif/esp_lcd_touch_cst816s',
                     'espressif/esp_lvgl_adapter'):
            self.assertEqual(len(deps[name]), 1, name)
            self.assertIn('\\"waveshare_s3_lcd19\\"', deps[name][0], name)

    def test_every_board_is_guarded_by_its_target(self):
        for name, depends in board_choice():
            if name == 'MUSE_BOARD_NONE':
                continue
            with self.subTest(board=name):
                self.assertEqual(len(depends), 1, depends)
                self.assertRegex(depends[0], r'^IDF_TARGET_ESP32\w*$')
        boards = dict(board_choice())
        self.assertEqual(boards['MUSE_BOARD_GUITION_JC3248W535'], ['IDF_TARGET_ESP32S3'])
        self.assertEqual(boards['MUSE_BOARD_WAVESHARE_S3_LCD19'], ['IDF_TARGET_ESP32S3'])

    def test_no_power_off_panel_sleep_or_aux_icon_it_cannot_back(self):
        source = BOARD.read_text()
        # No PMU or latch: leaving power_off out hides Settings' Power off
        # (muse_board.h) instead of offering one that always fails.
        self.assertNotRegex(source, r'\.power_off\s*=')
        self.assertNotIn('ESP_ERR_NOT_SUPPORTED', source)
        # esp_lcd_sh8601 has no disp_sleep, so that call could only fail.
        self.assertNotIn('esp_lcd_panel_disp_sleep', source)
        # One button: an aux_hint would draw a power icon beside nothing.
        self.assertNotRegex(source, r'\.aux_hint\s*=')

    def test_inmp441_keeps_32_bit_slots_through_codec_open(self):
        source = function(BOARD.read_text(), 'audio_init')
        mic = source[source.index('mic_chan'):]
        # 64 clocks a frame, as the INMP441 needs (and as on the JC3248W535).
        self.assertIn('mic_std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;', mic)
        self.assertIn('mic_std.slot_cfg.ws_width = 32;', mic)
        # esp_codec_dev's I2S data interface would reset them to 16-bit slots
        # on open, so the mic has its own, with no set_fmt.
        self.assertNotIn('rx_handle', source)
        self.assertRegex(source, r'mic_data = \{ \.enable = mic_enable, \.read = mic_read \}')
        self.assertRegex(BOARD.read_text(), r'\.set_mic_gain = set_mic_gain')

    def test_backlight_turns_fully_off(self):
        # Active low: an inverted channel at duty 0 holds the pin high. A
        # hand-inverted duty of 1023/1024 would leave it faintly lit.
        source = BOARD.read_text()
        self.assertIn('.flags.output_invert = 1,', function(source, 'init'))
        self.assertIn('(uint32_t)pct * BL_DUTY_MAX / 100', function(source, 'set_brightness'))

    def test_mic_read_removes_dc_and_applies_gain(self):
        board = BOARD.read_text()
        code = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
typedef void *i2s_chan_handle_t;
typedef void *esp_codec_dev_handle_t;
typedef int esp_codec_dev_type_t;
typedef struct audio_codec_data_if_t audio_codec_data_if_t;
#define ESP_OK 0
#define ESP_CODEC_DEV_OK 0
#define ESP_CODEC_DEV_READ_FAIL -5
#define pdMS_TO_TICKS(ms) (ms)
#define MIC_GAIN_OFFSET_DB 0
static i2s_chan_handle_t s_mic_rx;
static int s_mic_gain_q8 = 256;
static int32_t s_mic_dc[2];
static int16_t feed[2];
static size_t short_by;
static esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *dst, size_t size, size_t *got, int ticks)
{
    (void)h; (void)ticks;
    int16_t *s = dst;
    for (size_t i = 0; i < size / 2; i++) s[i] = feed[i & 1];
    *got = size - short_by;
    return ESP_OK;
}
''' + function(board, 'mic_read') + function(board, 'set_mic_gain') + r'''
int main(void)
{
    int16_t buf[512];
    /* A constant offset on each slot is tracked and taken off. */
    feed[0] = 1000; feed[1] = -800;
    for (int n = 0; n < 40; n++) assert(mic_read(NULL, (uint8_t *)buf, sizeof buf) == 0);
    assert(abs(buf[510]) < 8 && abs(buf[511]) < 8);
    /* 30 dB of gain multiplies a step by about 31.6, and clips at full scale. */
    set_mic_gain(NULL, 30);
    assert(s_mic_gain_q8 > 31 * 256 && s_mic_gain_q8 < 32 * 256);
    feed[0] = 1000 + 100; feed[1] = -800 + 2000;
    assert(mic_read(NULL, (uint8_t *)buf, 4) == 0);
    assert(buf[0] > 3000 && buf[0] < 3300);
    assert(buf[1] == INT16_MAX);
    /* Negative steps clip at the bottom too, at the most gain Muse allows. */
    set_mic_gain(NULL, 36);
    feed[0] = 1000 - 2000; feed[1] = -800 - 2000;
    assert(mic_read(NULL, (uint8_t *)buf, 4) == 0);
    assert(buf[0] == INT16_MIN && buf[1] == INT16_MIN);
    /* A short read is a failure, not a buffer of stale samples. */
    short_by = 2;
    assert(mic_read(NULL, (uint8_t *)buf, sizeof buf) == ESP_CODEC_DEV_READ_FAIL);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'prog.c'
            path.write_text(code)
            cc = shlex.split(os.environ.get('CC', 'cc'))
            subprocess.run(cc + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-include', 'stdlib.h', str(path),
                                 '-o', str(path.with_suffix('')), '-lm'],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(path.with_suffix(''))], check=True)

if __name__ == '__main__':
    unittest.main()
