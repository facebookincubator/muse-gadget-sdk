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

"""Whole production voice/input/settings C against deterministic, hardware-free fakes."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
FAKE_HEADERS = [
    'esp_attr.h', 'esp_err.h', 'esp_heap_caps.h', 'esp_log.h', 'esp_timer.h',
    'esp_codec_dev.h', 'lvgl.h', 'driver/gpio.h', 'nvs.h', 'nvs_flash.h',
    'freertos/FreeRTOS.h', 'freertos/task.h', 'freertos/queue.h',
    'freertos/idf_additions.h', 'freertos/semphr.h',
]


class StopWatchPTT(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binaries = {}
        for board, realtime in ((1, 1), (0, 1), (1, 0)):
            stage = Path(cls.tmp.name) / f'{board}-{realtime}'
            stage.mkdir()
            # Byte-identical production units/headers. No function extraction or
            # source-string assertions: the same control flow as firmware runs.
            for source in MUSE.glob('*.h'):
                shutil.copyfile(source, stage / source.name)
            for name in ('muse_voice.c', 'muse_input.c', 'muse_settings.c'):
                shutil.copyfile(MUSE / name, stage / name)
            # Independent UI/core workers can validate against the agreed header
            # before integration. Normal runs always use this checkout's header.
            api_header = os.environ.get('MUSE_PTT_API_HEADER')
            if api_header:
                shutil.copyfile(api_header, stage / 'muse_voice_call.h')
            shutil.copyfile(ROOT / 'tests/muse_ptt_ui_fakes.h', stage / 'muse_ptt_ui_fakes.h')
            for name in FAKE_HEADERS:
                path = stage / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "muse_ptt_ui_fakes.h"\n')
            (stage / 'sdkconfig.h').write_text(
                '#pragma once\n'
                f'#define CONFIG_MUSE_BOARD_M5STACK_STOPWATCH {board}\n'
                f'#define CONFIG_MUSE_REALTIME_VOICE {realtime}\n'
                '#define CONFIG_MUSE_HATCH 1\n#define CONFIG_SPIRAM 1\n'
                '#define CONFIG_MUSE_DEFAULT_VOLUME 20\n')
            ui = (MUSE / 'muse_ui.c').read_text()
            start = ui.index('static void frame_tick(')
            end = ui.index('{', start) + 1
            depth = 1
            while depth:
                depth += (ui[end] == '{') - (ui[end] == '}')
                end += 1
            (stage / 'muse_ptt_frame.inc').write_text(ui[start:end])
            for kind in ('voice', 'input', 'settings', 'frame'):
                source = ROOT / f'tests/muse_ptt_{kind}_harness.c'
                out = stage / kind
                command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                           '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                           '-Wno-unused-variable', '-Wno-unused-function',
                           '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                           '-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                           '-no-pie', '-include', str(ROOT / 'tests/host_compat.h'),
                           '-I', str(stage), str(source)]
                if kind == 'voice' and realtime:
                    command.append(str(MUSE / 'muse_voice_call_core.c'))
                result = subprocess.run([*command, '-lm', '-o', str(out)], capture_output=True, text=True)
                if result.returncode:
                    raise AssertionError(result.stdout + result.stderr)
                cls.binaries[kind, board, realtime] = out

    def run_case(self, kind, scenario, board=1, realtime=1):
        result = subprocess.run([str(self.binaries[kind, board, realtime]), str(scenario)],
                                capture_output=True, text=True, timeout=10,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
                                     'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_release_tail_is_sample_bounded_even_with_fast_reads(self):
        self.run_case('voice', 22)

    def test_failed_tail_reads_cannot_extend_release_deadline(self):
        self.run_case('voice', 23)

    def test_blue_shutdown_hold_and_remote_end_cancel_capture_tail(self):
        for scenario in (24, 25, 26, 27):
            self.run_case('voice', scenario)

    def test_new_down_closes_old_tail_before_opening_new_press(self):
        self.run_case('voice', 28)

    def test_full_outer_queue_fails_call_closed_and_requires_fresh_press(self):
        for scenario in (30, 31, 32, 33, 34):
            self.run_case('voice', scenario)

    def test_speaking_avatar_level_cannot_decay_into_listening_meter(self):
        self.run_case('frame', 1)

    def test_live_mic_not_speaker_and_interrupt_release_hold_errors(self):
        self.run_case('voice', 1)

    def test_down_up_down_up_in_one_poll(self):
        self.run_case('voice', 2)

    def test_remote_end_clears_ui(self):
        self.run_case('voice', 3)

    def test_shutdown_requests_end(self):
        self.run_case('voice', 4)

    def test_legacy_press_hangs_up(self):
        self.run_case('voice', 5, board=0)

    def test_release_during_startup(self):
        self.run_case('voice', 6)

    def test_end_wait_is_bounded(self):
        self.run_case('voice', 7)

    def test_waking_tap_preserves_next_press(self):
        self.run_case('voice', 8)

    def test_waking_hold_does_not_upload_preroll(self):
        self.run_case('voice', 9)

    def test_ui_labels_hints_and_modes(self):
        self.run_case('voice', 10)

    def test_poweroff_only_signals_until_voice_task_closes_tail_once(self):
        self.run_case('voice', 11)

    def test_default_ptt_bypasses_entire_note_path(self):
        self.run_case('voice', 20)

    def test_interrupting_saved_note_preserves_release_into_call(self):
        self.run_case('voice', 0)

    def test_waking_hold_prefix_excludes_stale_idle_audio(self):
        self.run_case('voice', 17)

    def test_release_during_wake_threshold_discards_capture(self):
        self.run_case('voice', 18)

    def test_waking_capture_failure_never_reuses_stale_chunk(self):
        self.run_case('voice', 19)

    def test_shutdown_rejects_new_admission(self):
        self.run_case('voice', 16)

    def test_held_call_press_does_not_leave_a_pending_call(self):
        self.run_case('input', 10)

    def test_consumer_plays_three_seconds_before_delayed_stop_ended_event(self):
        self.run_case('voice', 41)

    def test_goodbye_tail_drains_bounded_and_yellow_interrupts(self):
        for scenario in (12, 13, 14):
            self.run_case('voice', scenario)

    def test_new_call_after_remote_end_has_fresh_ui(self):
        self.run_case('voice', 15)

    def test_note_mode_persists_without_changing_other_board_defaults(self):
        for board, realtime in ((1, 1), (0, 1), (1, 0)):
            self.run_case('settings', 1, board=board, realtime=realtime)

    def test_legacy_aux_sleep_wake_double_and_long_press(self):
        for scenario in (1, 2, 3):
            self.run_case('input', scenario)

    def test_stopwatch_blue_hangs_up_in_call_and_pending_call(self):
        # The StopWatch's dedicated speaker button (blue with calls on), through
        # the production dedicated_buttons() path.
        for scenario in (4, 5, 6):
            self.run_case('input', scenario)
        for scenario in (5, 6):
            self.run_case('input', scenario, board=0)
            self.run_case('input', scenario, realtime=0)

    def test_pending_call_mark_cleared_by_waking_tap_and_rest_only(self):
        for scenario in (35, 36, 37, 38, 39, 40):
            self.run_case('voice', scenario)
            self.run_case('voice', scenario, board=0)

    def test_stopwatch_hangup_fails_closed_and_survives_sleep_and_power(self):
        for scenario in (11, 12, 13, 14):
            self.run_case('input', scenario)
        for scenario in (11, 12):
            self.run_case('input', scenario, board=0)

    def test_talk_quick_tap_wake_menu_and_pairing(self):
        self.run_case('input', 7)

    def test_blue_call_routing_is_stopwatch_only(self):
        self.run_case('input', 8)
        self.run_case('input', 8, board=0)
        self.run_case('input', 8, realtime=0)

    def test_active_call_prevents_auto_sleep(self):
        self.run_case('input', 9)

    def test_explicit_notes_and_other_boards_keep_notes(self):
        self.run_case('voice', 21)
        self.run_case('voice', 20, board=0)
        self.run_case('voice', 20, realtime=0)


class StopWatchControlMapping(unittest.TestCase):
    """The production StopWatch board's control blocks, preprocessed with live
    calls off (public mapping) and on (yellow talks, blue mutes/hangs up)."""

    def mapping(self, realtime):
        board = (MUSE / 'boards/board_m5stack_stopwatch.c').read_text()
        blocks = re.findall(r'^#if !CONFIG_MUSE_REALTIME_VOICE\n.*?^#endif\n', board, re.M | re.S)
        self.assertEqual(len(blocks), 2)   # the GPIOs, and the labels and icon positions
        source = ('#define GPIO_NUM_1 1\n#define GPIO_NUM_2 2\n#define LV_ALIGN_CENTER 9\n'
                  f'#define CONFIG_MUSE_REALTIME_VOICE {realtime}\n' + blocks[0] +
                  'int talk_gpio = TALK_GPIO, speaker_gpio = SPEAKER_GPIO;\n'
                  'struct { const char *talk_button, *speaker_button; struct { int a, x, y; } talk_hint, speaker_hint; } b = {\n'
                  + blocks[1] + '};\n')
        result = subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-E', '-P', '-x', 'c', '-'],
                                input=source, capture_output=True, text=True, check=True)
        out = result.stdout
        talk_gpio, speaker_gpio = map(int, re.search(r'int talk_gpio = (\d+), speaker_gpio = (\d+)', out).groups())
        talk = re.search(r'\.talk_button = "([^"]+)"', out).group(1)
        speaker = re.search(r'\.speaker_button = "([^"]+)"', out).group(1)
        talk_x = int(re.search(r'\.talk_hint = \{ 9, (-?\d+), -110 \}', out).group(1))
        speaker_x = int(re.search(r'\.speaker_hint = \{ 9, (-?\d+), -110 \}', out).group(1))
        return talk_gpio, speaker_gpio, talk, speaker, talk_x, speaker_x

    def test_public_mapping_unchanged_without_calls(self):
        # GPIO1 is blue (top-right), GPIO2 yellow (top-left).
        self.assertEqual(self.mapping(0), (1, 2, 'top-right', 'top-left', 144, -144))

    def test_calls_put_talk_on_yellow_and_speaker_on_blue(self):
        self.assertEqual(self.mapping(1), (2, 1, 'top-left', 'top-right', -144, 144))


if __name__ == '__main__':
    unittest.main()
