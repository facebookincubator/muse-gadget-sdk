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

"""The ESP-Mosaico's speaker: its volume map (volume 100 = DAC -5 dB, the
NS4150B's clip) and the speech loudness chain on every reply and sound it
plays (components/muse/muse_spk_dsp.c).

tests/muse_spk_dsp_harness.c runs the chain (its ceilings, a settings change
mid-signal), its settings (the NVS blob, the range check, the console word)
and the volume map's ceiling at the DAC;
tests/muse_spk_barge_harness.c runs it through the barge-in
pause on the real muse_audio_tx core and the TX driver model.
"""

from __future__ import annotations

import math
import os
import re
import shlex
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
MG = ROOT / 'components/muse_gadget_ble'
RATE = 16000
CEIL = round(32768 * 10 ** (-1 / 20))   # the limiter's -1 dBFS as a sample (29205)


def function(text: str, head: str) -> str:
    body = text[text.index(head):]
    return body[:body.index('\n}\n')]


class SpkDsp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cc = shlex.split(os.environ.get('CC', 'cc'))
        cls.exe = Path(cls.tmp.name) / 'dsp'
        subprocess.run([*cc, '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror', '-O2', '-I', str(MUSE),
                        str(ROOT / 'tests/muse_spk_dsp_harness.c'), str(MUSE / 'muse_spk_dsp.c'), '-lm',
                        '-o', str(cls.exe)], check=True)

    def run_h(self, *args):
        return subprocess.run([str(self.exe), *map(str, args)], capture_output=True, text=True, check=True).stdout

    # ---- the limiter and the final clamp ----

    def test_limiter_ceiling_never_exceeded(self):
        out = self.run_h('ceiling').split('\n')
        worst = int([l for l in out if l.startswith('worst')][0].split()[1])
        self.assertLessEqual(worst, CEIL)   # full-scale noise, sweeps, clicks, squares, the torture mix
        for seed in (1, 2, 3):
            peak = int(self.run_h('torture', seed, 700, -30, 4, 20, -1).split('\n')[0].split()[1])
            self.assertLessEqual(peak, CEIL, seed)
        # with nothing in front (no HPF, no compression, +20 dB makeup): the limiter alone holds it
        out = self.run_h('torture', 9, 0, 0, 1, 20, -1).split('\n')
        self.assertLessEqual(int(out[0].split()[1]), CEIL)
        st = [float(v) for v in out[1].split()[1:]]
        self.assertGreater(st[2], 15)   # it did reduce, hard
        self.assertLessEqual(st[4], -1.0 + 1e-3)

    def test_final_clamp_at_the_dac_never_over_minus_6_at_volume_100(self):
        # The chain's largest sample, through the Mosaico's volume 100 (ES8311 -5.0 dB): DAC -6 dBFS at most.
        out = self.run_h('ceiling').split('\n')
        worst = int([l for l in out if l.startswith('worst')][0].split()[1])
        vol = self.volmap()
        dac_db = 20 * math.log10(worst / 32768) + vol[100][2]
        self.assertLessEqual(dac_db, -6.0 + 0.001)

    # ---- a retune ----

    def test_a_settings_change_mid_reply_doesnt_click(self):
        m = re.search(r'retune largest step (\d+)', self.run_h('retune'))
        # a 1 kHz sine's own largest step at the ceiling: 2 sin(pi/16) x 29205 = 11395
        self.assertLessEqual(int(m.group(1)), 11400)

    # ---- the settings ----

    def test_defaults_shared_config_and_range_check(self):
        out = self.run_h('config').split('\n')
        self.assertEqual(out[0], 'default 700.0 -30.0 4.0 20.0 3.0 120.0 -1.0 50.0 on=1')
        self.assertEqual(out[1], 'set 500.0 gen_changed=1 off=1 gen_changed2=1 even=1')
        # 700/-30/4/20 and HPF 0 in; HPF 40, THR +1, RATIO 0.5, MAKEUP 31 out; the corners in; NaN never
        self.assertEqual(out[2], 'ok 1 1 0 0 0 0 1')
        self.assertEqual(out[3], 'ok_nan 0')

    def blob(self, ver, on, hpf, thr, ratio, makeup, pad=b'\0\0', extra=b''):
        return (bytes([ver, on]) + pad + struct.pack('<4f', hpf, thr, ratio, makeup) + extra).hex()

    def test_nvs_blob_versioning(self):
        good = self.blob(1, 0, 500, -24, 3, 12)
        cases = [good,
                 self.blob(2, 1, 500, -24, 3, 12),                 # another version: the defaults
                 self.blob(0, 1, 500, -24, 3, 12),
                 self.blob(1, 2, 500, -24, 3, 12),                 # on out of range
                 self.blob(1, 1, 40, -24, 3, 12),                  # a value out of range
                 self.blob(1, 1, 500, -24, 3, 31),
                 self.blob(1, 1, float('nan'), -24, 3, 12),
                 self.blob(1, 1, 500, -24, 3, 12, extra=b'\0'),    # too long
                 good[:-2],                                        # too short
                 self.blob(1, 1, 0, -60, 20, 0)]                   # the corners
        out = [l.split() for l in self.run_h('blob', 'pack', *cases).split('\n') if l]
        # the stored blob's layout, byte for byte
        self.assertEqual(out[0], ['pack', self.blob(1, 0, 500, -30, 4, 12.5)])
        rows = out[1:]
        self.assertEqual(rows[0], ['unpack', '1', '500.00', '-24.00', '3.00', '12.00', 'on=0'])
        for r in rows[1:9]:
            self.assertEqual(r, ['unpack', '0', '700.00', '-30.00', '4.00', '20.00', 'on=1'], r)
        self.assertEqual(rows[9], ['unpack', '1', '0.00', '-60.00', '20.00', '0.00', 'on=1'])
        src = (MUSE / 'muse_spk_dsp.c').read_text()
        self.assertIn('nvs_open("muse_spk", NVS_READONLY', src)
        self.assertIn('nvs_get_blob(h, "dsp", blob, &len) == ESP_OK && muse_spk_dsp_blob_unpack(blob, len', src)
        self.assertIn('nvs_set_blob(h, "dsp", blob, sizeof(blob))', src)

    def test_console_word_shares_the_range_check(self):
        words = ['EMPTY', '?', '=on', '=off', '=700,-30,4,20', '=0,-60,1,0', '=4000,0,20,30', '=40,-30,4,20',
                 '=700,1,4,20', '=700,-30,0.5,20', '=700,-30,4,31', '=700,-30,4', '=700,-30,4,20,1', '=700;-30;4;20',
                 '=nan,-30,4,20', '=on1', 'x', '=']
        out = [l for l in self.run_h('word', *words).split('\n') if l.startswith('word')]
        kinds = [l.split()[2] for l in out]
        self.assertEqual(kinds, ['show', 'show', 'on', 'off', 'set', 'set', 'set', 'bad', 'bad', 'bad', 'bad', 'bad',
                                 'bad', 'bad', 'bad', 'bad', 'bad', 'bad'])
        self.assertTrue(out[4].endswith('set 700.00 -30.00 4.00 20.00'))
        desc = [l for l in self.run_h('word').split('\n') if l.startswith('describe')][0]
        self.assertEqual(desc, 'describe @spk.dsp on: high-pass 700 Hz (4th-order Butterworth), compressor -30.0 dBFS '
                               '4.0:1 attack 3 ms release 120 ms, makeup +20.0 dB, limiter -1.0 dBFS (2 ms look-ahead, '
                               'release 50 ms)')
        inp = (MUSE / 'muse_input.c').read_text()
        word = inp[inp.index('    if (!strncmp(line, "spk.dsp", 7)) {'):]
        word = word[:word.index('\n    }\n')]
        self.assertIn('muse_spk_dsp_word(line + 7, v)', word)   # the one parser, its range check
        self.assertIn('muse_spk_dsp_set(&c);', word)
        # set words change state (a release build takes them over the Nordic UART only proven), the show
        # words are reads
        glue = (ROOT / 'main/mg_glue.c').read_text()
        allow = function(glue, 'static int ble_bench_input(char *line) {')
        self.assertIn('"spk.dsp", "spk.dsp?"', allow)
        self.assertIn('"spk.dsp="', allow)
        unlock = function((MG / 'mg_unlock.c').read_text(), 'bool mg_unlock_automation_word(const char *line)\n{')
        self.assertIn('strcmp(w, "spk.dsp")', unlock)
        self.assertIn('strcmp(w, "spk.dsp?")', unlock)

    # ---- the volume map ----

    def volmap(self):
        board = (MUSE / 'boards/board_esp_mosaico.c').read_text()
        top = float(re.search(r'#define SPK_VOL_TOP_DB \((-?[\d.]+)f\)', board).group(1))
        pa = float(re.search(r'#define SPK_HW_PA_V ([\d.]+)f', board).group(1))
        dac = float(re.search(r'#define SPK_HW_DAC_V ([\d.]+)f', board).group(1))
        out = self.run_h('volmap', top, pa, dac, 0).split('\n')
        self.assertEqual(out[0], 'hw 0.0000')
        rows = {}
        for l in out[1:]:
            if l.startswith('vol'):
                _, v, db, reg, regdb = l.split()
                rows[int(v)] = (float(db), int(reg), float(regdb))
        return rows


class SpkChainWiring(unittest.TestCase):
    """Every reply path to the speaker of a board with the chain, and the barge-in pause through it."""

    def test_barge_pause_through_the_chain(self):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'barge'
            build = subprocess.run(
                [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-fsanitize=address,undefined',
                 '-I', str(MUSE), '-I', str(MG), str(ROOT / 'tests/muse_spk_barge_harness.c'),
                 str(MUSE / 'muse_audio_tx.c'), str(MUSE / 'muse_spk_dsp.c'), str(MG / 'mg_chain.c'), '-lm',
                 '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        for line in ('pause and tap: 300', 'chain output exact (no state jump)', 'rewind post-chain and written once',
                     'speaker exact after the ramp', 'hold\'s claim and flush in the pause: nothing of the reply again',
                     'stop keep: the chain\'s last 2 ms play', 'flush without a pause: the chain\'s tail, then the fade',
                     'control: a chain after the rewind replays'):
            self.assertIn(line, run.stdout)


if __name__ == '__main__':
    unittest.main()
