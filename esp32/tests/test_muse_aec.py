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

"""Host checks for AEC frame packing, reference delay, and fallback."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SoftwareAEC(unittest.TestCase):
    def test_live_calls_leave_internal_memory_for_audio_dma(self):
        kc = (ROOT / 'components/muse/Kconfig').read_text()
        voice = kc[kc.index('config MUSE_REALTIME_VOICE'):]
        voice = voice[:voice.index('\n    config ', 1)]
        self.assertIn(
            'select MUSE_STATIC_BUFFERS_IN_PSRAM if MUSE_ENABLED && SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY',
            voice,
        )

    def test_frame_alignment_and_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'muse_aec_harness'
            command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-O1',
                       '-Wall', '-Wextra', '-Werror',
                       '-I', str(ROOT / 'tests/muse_aec_fakes'),
                       '-I', str(ROOT / 'components/muse'),
                       str(ROOT / 'tests/muse_aec_harness.c'),
                       str(ROOT / 'components/muse/muse_aec.c'),
                       '-o', str(binary)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([binary], check=True, capture_output=True, text=True)

    def test_esp_sr_rule_uses_a_symbol_every_build_defines(self):
        # A rule on a symbol a build lacks (the AEC option without live calls,
        # or on a Link-only build) stops the component manager.
        muse = ROOT / 'components/muse'
        manifest = (muse / 'idf_component.yml').read_text()
        self.assertIn('if: "$CONFIG{MUSE_VOICE_AEC_SR} == \\"y\\""', manifest)
        self.assertNotIn('$CONFIG{MUSE_VOICE_SOFTWARE_AEC}', manifest)
        kc = (muse / 'Kconfig').read_text()
        sym = kc[kc.index('config MUSE_VOICE_AEC_SR'):]
        sym = sym[:sym.index('\n\n')]
        self.assertNotIn('depends on', sym)
        self.assertIn('default "n"', sym)
        self.assertGreater(kc.index('config MUSE_VOICE_AEC_SR'),
                           kc.index('endif', kc.index('config MUSE_VOICE_SOFTWARE_AEC')))

    def test_call_loop_keeps_reference_and_hold_fence(self):
        voice = (ROOT / 'components/muse/muse_voice.c').read_text()
        loop = voice[voice.index('static void run_call('):]
        loop = loop[:loop.index('\n}\n')]
        # A short playback read is padded and played whole, so the speaker
        # gets exactly the chunk AEC takes as its reference.
        pad = loop.index('if (aec && n && n < MUSE_AUDIO_CHUNK)')
        self.assertLess(pad, loop.index('muse_audio_write(out, n);'))
        # Audio the call may not take reaches AEC as silence and is never sent.
        self.assertIn('muse_aec_feed(aec, aec_mic ? s_chunk : silence,', loop)
        self.assertIn('if (clean_n && aec_mic)', loop)
        self.assertIn('!c.server_hold', loop[loop.index('bool aec_mic'):loop.index('if (aec_mic)')])


if __name__ == '__main__':
    unittest.main()
