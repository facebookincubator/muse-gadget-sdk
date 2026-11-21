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

"""Text protocol and native LVGL/compact-keypad integration."""
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('muse_text_viewer', ROOT / 'tools/muse/mirror.py')
try:
    mirror = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mirror)
except ImportError as error:
    raise unittest.SkipTest(str(error)) from error

class TextInputTests(unittest.TestCase):
    @unittest.skipUnless(os.name == 'posix' and shutil.which('cmake'), 'native LVGL test requires CMake and POSIX')
    def test_compact_keypad_and_lvgl_editing(self):
        lvgl = ROOT / 'managed_components/lvgl__lvgl'
        if not (lvgl / 'CMakeLists.txt').exists():
            self.skipTest('configure a Muse LVGL build to fetch the pinned LVGL source')
        source = (ROOT / 'components/muse_mirror/muse_mirror.c').read_text()
        ui = source[source.index('/* The focused, visible textarea'):source.index('static void capture_tick')]
        with tempfile.TemporaryDirectory(prefix='muse-mirror-lvgl-') as tmp:
            p = Path(tmp)
            (p / 'text_ui.inc').write_text(ui)
            (p / 'esp_err.h').write_text('typedef int esp_err_t;\n')
            (p / 'sdkconfig.h').write_text('/* Optional firmware features are disabled in this harness. */\n')
            (p / 'lv_conf.h').write_text('''#pragma once
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_LOG 0
#define LV_FONT_MONTSERRAT_20 1
''')
            (p / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(mirror_text C CXX)
set(CMAKE_C_FLAGS "${{CMAKE_C_FLAGS}} -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS "${{CMAKE_CXX_FLAGS}} -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("{lvgl}" lvgl)
add_executable(mirror_text "{ROOT / 'tests/muse_mirror_text_lvgl_harness.c'}")
target_include_directories(mirror_text PRIVATE "{ROOT / 'components/muse'}" "{ROOT / 'components/muse_mirror'}" "{p}")
target_link_libraries(mirror_text PRIVATE lvgl m)
''')
            for command in (['cmake', '-S', str(p), '-B', str(p / 'build')],
                            ['cmake', '--build', str(p / 'build'), '--parallel', '4'],
                            [str(p / 'build/mirror_text')]):
                result = subprocess.run(command, capture_output=True, text=True, timeout=180,
                                        env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
                                             'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_field_routing_digits_edits_paste(self):
        link = mirror.Link('/dev/test')
        self.assertFalse(mirror.route_text_key(link, None, '1'))
        self.assertTrue(link.out.empty())
        link.text_open = True
        self.assertTrue(mirror.route_text_key(link, None, '1234'))
        self.assertEqual(link.out.get_nowait(), b'I1234')
        for key, code in mirror.TEXT_EDITS.items():
            self.assertTrue(mirror.route_text_key(link, key))
            self.assertEqual(link.out.get_nowait(), bytes((ord('K'), code)))
        text = 'token-\u00e9\U0001f642' * 50
        self.assertTrue(mirror.route_text_key(link, None, paste=text, modified=True))
        chunks = []
        while not link.out.empty():
            msg = link.out.get_nowait()
            self.assertLessEqual(len(msg), 64)
            chunks.append(msg[1:].decode('utf-8'))
        self.assertEqual(''.join(chunks), text)
        self.assertTrue(mirror.route_text_key(link, 'Escape'))
        self.assertTrue(link.out.empty())
        self.assertFalse(mirror.route_text_key(link, None, 's', modified=True))
        self.assertFalse(mirror.route_text_key(link, None, '\x07'))
        link.disconnect()
        self.assertFalse(mirror.route_text_key(link, None, paste=text, modified=True))

    @unittest.skipUnless(os.name == 'posix', 'C harness requires a POSIX sanitizer toolchain')
    def test_firmware_text_queue(self):
        source = (ROOT / 'components/muse_mirror/muse_mirror_session.c').read_text()
        session = source[source.index('void muse_mirror_text_open('):source.index('static void *alloc(')]
        receive = source[source.index('static void input('):source.index("    if (len == 1 && buf[0] == 'R')")]
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            (p / 'esp_err.h').write_text('typedef int esp_err_t;\n')
            (p / 'text_session.inc').write_text(session + receive + '}\n')
            exe = p / 'text-queue'
            result = subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I' + str(p), '-I' + str(ROOT / 'components/muse'), '-I' + str(ROOT / 'components/muse_mirror'),
                str(ROOT / 'components/muse_mirror/muse_mirror_core.c'),
                str(ROOT / 'tests/muse_mirror_text_queue_harness.c'), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([exe], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())

    @unittest.skipUnless(os.name == 'posix', 'C harness requires a POSIX sanitizer toolchain')
    def test_firmware_text_parser(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / 'text-harness'
            result = subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I' + str(ROOT / 'components/muse'), '-I' + str(ROOT / 'components/muse_mirror'), str(ROOT / 'components/muse_mirror/muse_mirror_core.c'),
                str(ROOT / 'tests/muse_mirror_text_harness.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([exe], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
