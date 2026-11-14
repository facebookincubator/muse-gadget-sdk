# SPDX-License-Identifier: Apache-2.0

"""Host checks for AEC frame packing, reference delay, and fallback."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SoftwareAEC(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
