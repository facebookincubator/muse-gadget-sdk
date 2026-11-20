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

"""Photo with voice notes (CONFIG_MUSE_PHOTO_NOTES): muse_photo_note.c against
a faked camera, chat and FreeRTOS, and its wiring on the SenseCAP Watcher.
The note's request itself is in test_muse_voice_note.py."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
SANITIZE = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
ENV = {**os.environ,
       'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
       'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}

STUBS = {
    'sdkconfig.h': '',
    'esp_err.h': 'typedef int esp_err_t;\n#define ESP_OK 0\n',
    'esp_heap_caps.h': '#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n',
    'esp_log.h': '#define ESP_LOGW(tag, ...) ((void)(tag))\n',
    'freertos/FreeRTOS.h': '''#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xTaskCreateWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                               UBaseType_t prio, TaskHandle_t *handle, uint32_t caps);
void vTaskDeleteWithCaps(TaskHandle_t task);
''',
    'freertos/idf_additions.h': '#include "freertos/FreeRTOS.h"\n',
    'freertos/semphr.h': '#include "freertos/FreeRTOS.h"\n',
    'freertos/task.h': '#include "freertos/FreeRTOS.h"\n',
}


class PhotoNote(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        for name, text in STUBS.items():
            (out / name).parent.mkdir(parents=True, exist_ok=True)
            (out / name).write_text(text)
        cls.binary = out / 'photo_note'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        result = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-DCONFIG_MUSE_PHOTO_NOTES=1', '-DCONFIG_MUSE_WATCHER_CAMERA=1',
             '-I', str(out), '-I', str(MUSE),
             str(ROOT / 'tests/muse_photo_note_harness.c'), str(MUSE / 'muse_photo_note.c'),
             '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def scenario(self, name):
        ran = subprocess.run([str(self.binary), name], capture_output=True, text=True, env=ENV)
        self.assertEqual((ran.returncode, ran.stdout, ran.stderr), (0, 'ok\n', ''), name)

    def test_off_or_not_a_press_takes_no_photo(self):
        self.scenario('off')
        self.scenario('not_press')

    def test_photo_goes_to_the_note_it_was_taken_for(self):
        self.scenario('taken')

    def test_note_end_waits_for_the_photo(self):
        self.scenario('during_wait')

    def test_failed_or_refused_photo_says_so(self):
        self.scenario('failed')
        self.scenario('refused')
        self.scenario('no_task')

    def test_late_photo_is_dropped(self):
        self.scenario('too_slow')

    def test_one_capture_at_a_time(self):
        self.scenario('busy')


def preprocessor_blocks(text, symbol):
    """The line ranges inside `#if CONFIG_<symbol>` blocks (one level deep is
    enough here: none of these nest another #if inside)."""
    lines, inside, depth = text.splitlines(), set(), 0
    for number, line in enumerate(lines, 1):
        stripped = line.strip()
        if depth:
            if stripped.startswith('#if'):
                depth += 1
            elif stripped.startswith('#endif'):
                depth -= 1
            if depth:
                inside.add(number)
        elif re.match(rf'#if CONFIG_{symbol}\b', stripped):
            depth = 1
    return lines, inside


class PhotoNoteWiring(unittest.TestCase):
    """Only the Watcher builds it, only behind its option, and only a press of
    the person's own takes a photo."""

    def test_option_is_the_watcher_cameras_and_off_elsewhere(self):
        kconfig = (MUSE / 'Kconfig').read_text()
        block = kconfig[kconfig.index('config MUSE_PHOTO_NOTES'):]
        block = block[:block.index('\n    config ', 1)]
        self.assertIn('depends on MUSE_WATCHER_CAMERA && MUSE_HATCH', block)
        cmake = (MUSE / 'CMakeLists.txt').read_text()
        self.assertRegex(cmake, r'if\(CONFIG_MUSE_PHOTO_NOTES\)\n\s+list\(APPEND srcs "muse_photo_note.c" "muse_chat_image.c"\)')
        self.assertEqual(cmake.count('muse_photo_note.c'), 1)
        self.assertEqual(cmake.count('muse_chat_image.c'), 1)
        for overlay in (ROOT / 'devices').glob('sdkconfig*'):
            self.assertNotIn('CONFIG_MUSE_PHOTO_NOTES', overlay.read_text(), overlay.name)

    def test_setting_starts_off(self):
        settings = (MUSE / 'muse_settings.c').read_text()
        body = settings[settings.index('bool muse_settings_photo_notes(void)'):]
        self.assertIn('nvs_get_u8(s_nvs, "photo_notes", &b) == ESP_OK && b', body[:400])
        ui = (MUSE / 'muse_settings_ui.c').read_text()
        lines, inside = preprocessor_blocks(ui, 'MUSE_PHOTO_NOTES')
        rows = [n for n, line in enumerate(lines, 1) if 'photo_notes' in line]
        self.assertTrue(rows)
        self.assertTrue(all(n in inside for n in rows), rows)

    def test_voice_task_takes_it_at_a_press_and_waits_before_the_end(self):
        voice = (MUSE / 'muse_voice.c').read_text()
        lines, inside = preprocessor_blocks(voice, 'MUSE_PHOTO_NOTES')
        calls = [n for n, line in enumerate(lines, 1) if 'muse_photo_note' in line]
        self.assertTrue(calls)
        self.assertTrue(all(n in inside for n in calls), calls)
        # Begun right after the note goes live, for a press only (not a wake
        # word note, nor one recorded to send later).
        record = voice[voice.index('static bool record(bool seeded'):]
        self.assertRegex(record, r'go_live\(\);\n    \}\n#if CONFIG_MUSE_PHOTO_NOTES\n.*\n    muse_photo_note_begin\(s_live && !seeded\);')
        # Waited for before anything ends the note.
        finish = voice[voice.index('static bool finish_note(void)'):]
        finish = finish[:finish.index('\n}\n')]
        self.assertLess(finish.index('muse_photo_note_wait();'), finish.index('muse_hatch_turn_end();'))
        self.assertLess(finish.index('muse_photo_note_wait();'), finish.index('feed_rest('))

    def test_nothing_else_takes_or_sends_a_photo(self):
        users = {}
        for path in [*(ROOT / 'main').rglob('*.c*'), *MUSE.rglob('*.c*')]:
            text = path.read_text(errors='replace')
            for name in ('muse_photo_note_begin', 'muse_chat_attach_jpeg'):
                if name + '(' in text:
                    users.setdefault(name, set()).add(path.name)
        self.assertEqual(users, {
            'muse_photo_note_begin': {'muse_photo_note.c', 'muse_voice.c'},
            'muse_chat_attach_jpeg': {'muse_photo_note.c', 'muse_chat_session.cpp'},
        })
        note = (MUSE / 'muse_photo_note.c').read_text()
        self.assertIn('#if CONFIG_MUSE_WATCHER_CAMERA\n#include "boards/watcher_camera.h"\n'
                      '#define capture_base64 watcher_camera_capture\n'
                      '#define capture_done watcher_camera_preview_close\n#endif', note)
        # The live view gives its frame and closes at once, before the next
        # frame could cover the note's screen.
        camera = (MUSE / 'boards/watcher_camera.c').read_text()
        close = camera[camera.index('void watcher_camera_preview_close(void)'):]
        close = close[:close.index('\n}\n')]
        self.assertLess(close.index('s_preview_active = false;'), close.index('camera_stream_stop();'))
        self.assertLess(close.index('camera_stream_stop();'), close.index('muse_ui_image_hide();'))


if __name__ == '__main__':
    unittest.main()
