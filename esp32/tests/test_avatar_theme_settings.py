# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Run the production avatar and appearance stores against the same NVS handle."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest
from test_muse_ptt_ui import FAKE_HEADERS, LINK_GC

ROOT = Path(__file__).resolve().parents[1]

class AvatarThemeSettings(unittest.TestCase):
    @unittest.skipUnless((ROOT / 'components/muse/muse_theme.c').exists(), 'Requires the optional UI appearance implementation')
    def test_independent_persistence_and_failed_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            for source in (ROOT / 'components/muse').glob('*.h'):
                shutil.copyfile(source, stage / source.name)
            for name in ('muse_settings.c', 'muse_theme.c'):
                shutil.copyfile(ROOT / 'components/muse' / name, stage / name)
            shutil.copyfile(ROOT / 'tests/muse_ptt_ui_fakes.h', stage / 'muse_ptt_ui_fakes.h')
            for name in FAKE_HEADERS:
                path = stage / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "muse_ptt_ui_fakes.h"\n')
            (stage / 'sdkconfig.h').write_text('#define CONFIG_MUSE_UI_THEME 1\n#define CONFIG_MUSE_AVATAR_SYNC 1\n#define CONFIG_MUSE_DEFAULT_VOLUME 20\n')
            exe = str(stage / 'settings')
            subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
                '-DESP_ERR_INVALID_ARG=0x102', '-ffunction-sections', '-fdata-sections', *LINK_GC,
                '-fsanitize=address,undefined', '-include', str(ROOT / 'tests/host_compat.h'), '-I', tmp,
                str(ROOT / 'tests/avatar_theme_settings_harness.c'), '-o', exe], check=True)
            subprocess.run([exe], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1'})
