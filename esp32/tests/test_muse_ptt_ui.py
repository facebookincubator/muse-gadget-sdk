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
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
# Builds with the wake word compiled in (boards with PSRAM, as the option needs).
WAKEWORD_BUILDS = {(1, 1), (0, 1), (1, 0), (3, 0)}
MG = ROOT / 'components/muse_gadget_ble'
FAKE_HEADERS = [
    'esp_attr.h', 'esp_err.h', 'esp_heap_caps.h', 'esp_log.h', 'esp_system.h', 'esp_timer.h',
    'esp_codec_dev.h', 'lvgl.h', 'driver/gpio.h', 'nvs.h', 'nvs_flash.h',
    'freertos/FreeRTOS.h', 'freertos/task.h', 'freertos/queue.h',
    'freertos/idf_additions.h', 'freertos/semphr.h', 'muse_restart.h', 'muse_bootguard.h', 'muse_wdt.h', 'muse_stack.h', 'muse_local_press.h',
]


# GNU ld options; Apple's linker has its own spellings (and no -no-pie).
DARWIN = sys.platform == 'darwin'
LINK_GC = ['-Wl,-dead_strip'] if DARWIN else ['-Wl,--gc-sections']
NO_PIE = [] if DARWIN else ['-no-pie']


class StopWatchPTT(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binaries = {}
        # Board 1 is the StopWatch, 2 the AI Passport (low memory), 3 a
        # press-toggle voice-note build, 4 one that resends failed notes. The
        # ble=1 builds: with musegadgets BLE audio, where a press may go to the
        # phone instead.
        for board, realtime, ble in ((1, 1, 0), (0, 1, 0), (1, 0, 0), (0, 0, 0), (2, 0, 0), (3, 0, 0),
                                     (4, 0, 0), (1, 1, 1), (1, 0, 1)):
            stage = Path(cls.tmp.name) / f'{board}-{realtime}-{ble}'
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
                f'#define CONFIG_MUSE_BOARD_M5STACK_STOPWATCH {int(board == 1)}\n'
                f'#define CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY {int(board == 1)}\n'
                f'#define CONFIG_MUSE_BOARD_AI_PASSPORT {int(board == 2)}\n'
                f'#define CONFIG_MUSE_REALTIME_VOICE {realtime}\n'
                f'#define CONFIG_MUSE_VOICE_NOTE_TOGGLE {int(board == 3)}\n'
                f'#define CONFIG_MUSE_HATCH {int(board in (1, 3, 4) or realtime)}\n'
                f'#define CONFIG_SPIRAM {int(board in (1, 3, 4) or realtime)}\n'
                f'#define CONFIG_MUSE_RESEND_NOTES {int(board == 4)}\n'
                '#define CONFIG_MUSE_RESEND_NOTES_MAX 3\n'
                '#define CONFIG_MUSE_RESEND_NOTES_KB 1024\n'
                '#define CONFIG_MUSE_DEFAULT_VOLUME 20\n'
                f'#define CONFIG_MUSE_GADGET_BLE_AUDIO {ble}\n')
            ui = (MUSE / 'muse_ui.c').read_text()

            def function(name):
                start = ui.index(f'static void {name}(')
                end = ui.index('{', start) + 1
                depth = 1
                while depth:
                    depth += (ui[end] == '{') - (ui[end] == '}')
                    end += 1
                return ui[start:end] + '\n'

            # The tick's body may sit in its own function (timed under CONFIG_MUSE_PERF).
            body = function('frame_tick_body') if 'static void frame_tick_body(' in ui else ''
            (stage / 'muse_ptt_frame.inc').write_text(body + function('frame_tick'))
            for kind in ('voice',) if ble else ('voice', 'input', 'settings', 'frame'):
                source = ROOT / f'tests/muse_ptt_{kind}_harness.c'
                out = stage / kind
                command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                           '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                           '-Wno-unused-variable', '-Wno-unused-function',
                           '-ffunction-sections', '-fdata-sections', *LINK_GC,
                           '-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                           *NO_PIE, '-include', str(ROOT / 'tests/host_compat.h'),
                           '-I', str(stage),
                           '-I', str(MG / 'include'),   # header-only helpers muse_voice.c takes (mg_loud1s.h)
                           str(source)]
                if kind in ('voice', 'input'):
                    command.append(str(MUSE / 'muse_mic_ring.c'))   # the real ring state (input: the bench's >ring=)
                if kind == 'input':
                    command.append(str(MUSE / 'muse_charged.c'))   # what counts as charged
                if kind == 'voice':
                    command.append(str(MUSE / 'muse_turn_dot.c'))   # the turn dot follows the face's mode
                if kind == 'voice' and (board, realtime) in WAKEWORD_BUILDS:
                    # The wake word paths, with the harness's no-op glue hooks.
                    # The other builds compile them out, as firmware without
                    # the feature does (muse_wakeword_glue.h's inline stubs).
                    command.append('-DCONFIG_MUSE_WAKEWORD_ENABLE=1')
                if kind == 'voice' and realtime:
                    command.append(str(MUSE / 'muse_voice_call_core.c'))
                if ble:
                    # mg_turn() keeps a count only its log line reads; the fakes' ESP_LOGI drops it.
                    command += ['-Wno-unused-but-set-variable', '-I', str(MG / 'include'),
                                '-I', str(ROOT.parent / 'protocols'), str(MG / 'mg_route.c')]
                if kind == 'voice':
                    command.append(str(MUSE / 'muse_adpcm.c'))
                    command.append(str(MUSE / 'muse_resend.c'))
                result = subprocess.run([*command, '-lm', '-o', str(out)], capture_output=True, text=True)
                if result.returncode:
                    raise AssertionError(result.stdout + result.stderr)
                cls.binaries[(kind, board, realtime) + ((1,) if ble else ())] = out

    def run_case(self, kind, scenario, board=1, realtime=1, ble=0):
        key = (kind, board, realtime) + ((1,) if ble else ())
        result = subprocess.run([str(self.binaries[key]), str(scenario)],
                                capture_output=True, text=True, timeout=10,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
                                     'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_toggle_notes_are_not_offered_where_the_gesture_cannot_work(self):
        # The Watcher's short wheel click sleeps, and the FreeInk UI has no
        # navigation guard for a toggled note.
        kconfig = (ROOT / "components" / "muse" / "Kconfig").read_text()
        block = kconfig.split("config MUSE_VOICE_NOTE_TOGGLE", 1)[1].split("\n\n", 1)[0]
        self.assertIn("!MUSE_BOARD_SENSECAP_WATCHER", block)
        self.assertIn("!MUSE_UI_FREEINK", block)
    # main's voice scenarios carried into muse_ptt_voice_harness.c are 100 +
    # main's numbers (141-149, 164-169, 185-215): the call scenarios are 41-51 and 60-78.
    def test_failed_notes_make_room_by_dropping_the_oldest(self):
        self.run_case('voice', 210, board=4, realtime=0)

    def test_failed_notes_retry_quietly_until_they_land(self):
        self.run_case('voice', 211, board=4, realtime=0)

    def test_refused_or_unacknowledged_notes_are_not_sent_again(self):
        for scenario in (212, 213):
            with self.subTest(scenario=scenario):
                self.run_case('voice', scenario, board=4, realtime=0)

    def test_a_note_is_kept_after_the_release_only_if_muse_cannot_have_it(self):
        self.run_case('voice', 214, board=4, realtime=0)

    def test_a_full_resend_queue_does_not_refuse_a_press(self):
        self.run_case('voice', 215, board=4, realtime=0)

    def test_reply_ring_advances_without_rewinding_across_messages(self):
        for board, realtime in ((1, 1), (0, 1), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 143, board=board, realtime=realtime)

    def test_reply_ring_clears_on_incomplete_error_and_interruption(self):
        for scenario in (144, 145, 146):
            self.run_case('voice', scenario)

    def test_wake_word_press_that_interrupts_a_reply_stays_synthetic(self):
        # Otherwise the next turn treats it as a finger: on live-call PTT
        # boards a held call with the mic open and no release coming.
        for board, realtime in ((1, 1), (0, 1)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 190, board=board, realtime=realtime)
        voice = (MUSE / 'muse_voice.c').read_text()
        self.assertIn('bool synthetic = pending_down && s_press_synthetic;', voice)

    def test_stray_wake_word_edges_never_shape_a_button_note(self):
        for board, realtime in ((1, 1), (0, 1), (1, 0), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 169, board=board, realtime=realtime)
        voice = (MUSE / 'muse_voice.c').read_text()
        # Nor drive a call.
        self.assertIn('/* A call is never driven by the wake word: drop its edges. */', voice)

    def test_recorder_cap_tells_the_wake_word_glue(self):
        # The recorder's cap counts the seed and admission audio, so it can
        # end a hands-free turn before the core's own cap: the glue must hear.
        for board, realtime in ((1, 1), (1, 0), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 168, board=board, realtime=realtime)
        voice = (MUSE / 'muse_voice.c').read_text()
        task = voice[voice.index('static void voice_task('):]
        # Every voice-side end of a turn that never reached record(): shutdown
        # and can't record (a hands-free turn passes no gate, so none fails one).
        self.assertGreaterEqual(task.count('muse_wakeword_turn_ended()'), 2)
        self.assertIn('bool gated = wake || toggle || synthetic;', task)

    def test_short_wake_word_turn_never_starts_a_call(self):
        # Disabling listening right after a hands-free start posts a
        # synthetic release: that's a short note, never tap-to-call. A turn
        # that can't qualify (e.g. a capture error) just ends.
        for board, realtime in ((0, 1), (1, 1), (1, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 187, board=board, realtime=realtime)
        voice = (MUSE / 'muse_voice.c').read_text()
        task = voice[voice.index('static void voice_task('):]
        # It skips the hold gate (no tap to judge) and never takes the call branch.
        self.assertIn('bool gated = wake || toggle || synthetic;', task)
        self.assertIn('if (ptt && !synthetic)', task)

    def test_wake_word_turn_is_admitted_at_once_and_a_press_takes_it_over(self):
        for board, realtime in ((1, 1), (0, 1), (1, 0), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 192, board=board, realtime=realtime)
                self.run_case('voice', 193, board=board, realtime=realtime)

    def test_wake_word_note_ends_at_its_release_in_toggle_builds(self):
        self.run_case('voice', 167, board=3, realtime=0)

    def test_wake_word_seed_is_at_most_one_second(self):
        # The pre-roll seed is the only audio from before a detection that
        # can leave the device.
        for board, realtime in ((1, 1), (1, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 194, board=board, realtime=realtime)

    def test_wake_word_note_continues_past_the_seed_without_a_gap(self):
        # The wake task trails the voice task: the chunks already tapped past
        # the seed's end follow the seed, then live reads, each once.
        for board, realtime in ((1, 1), (1, 0), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 195, board=board, realtime=realtime)

    def test_finger_owns_a_hands_free_note_against_a_racing_release(self):
        # Endpointing read its input before the press and posts a release
        # after it: the note must run on until the finger lets go.
        for board, realtime in ((1, 1), (1, 0), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 196, board=board, realtime=realtime)

    def test_turning_listening_off_sends_nothing_more(self):
        # Off after admission: no turn opens. Off while recording: the note
        # is dropped, even with listening already back on. Off while a note
        # is still uploading: no more goes, no commit, nothing kept.
        for board, realtime in ((1, 1), (1, 0), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 201, board=board, realtime=realtime)
                self.run_case('voice', 200, board=board, realtime=realtime)
                self.run_case('voice', 204, board=board, realtime=realtime)

    def test_turning_listening_off_leaves_a_note_the_button_took_over(self):
        for board, realtime in ((1, 1), (1, 0), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 203, board=board, realtime=realtime)

    def test_wake_word_note_with_lost_audio_ends_unsent(self):
        # Sustained overload overwrote part of the audio after the seed: the
        # turn ends before anything goes, rather than sending a note with a hole.
        for board, realtime in ((1, 1), (1, 0), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 197, board=board, realtime=realtime)

    def test_stale_press_takes_nothing_over_from_a_hands_free_note(self):
        # A press the input queue's overflow invalidated must not take the
        # note over (its release would be ignored too, leaving a 15 s note).
        for board, realtime in ((1, 1), (1, 0), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 198, board=board, realtime=realtime)

    def test_rejected_hands_free_turns_free_their_buffer(self):
        # Each overload rejection frees the recording buffer, across
        # consecutive attempts through the voice loop.
        for board, realtime in ((1, 1), (1, 0), (3, 0)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 188, board=board, realtime=realtime)

    def test_wake_word_note_does_not_repeat_the_idle_pre_roll(self):
        # The seed already ends at the speech start; the idle ring covers
        # the same audio and would play it twice.
        for board, realtime in ((1, 1), (0, 1)):
            with self.subTest(board=board, realtime=realtime):
                self.run_case('voice', 191, board=board, realtime=realtime)

    def test_reply_completion_is_visible_but_ptt_still_interrupts(self):
        self.run_case('voice', 143)
        self.run_case('voice', 147)

    def test_release_tail_is_sample_bounded_even_with_fast_reads(self):
        self.run_case('voice', 22)

    def test_failed_tail_reads_cannot_extend_release_deadline(self):
        self.run_case('voice', 23)

    def test_note_preserves_buffered_order_and_streams_fresh_pcm_exactly(self):
        # PSRAM PCM, low-memory 320 ms pre-roll, low-memory 120 ms pre-roll.
        for board in (1, 0, 2):
            for scenario in (160, 161, 162, 163):
                with self.subTest(board=board, scenario=scenario):
                    self.run_case('voice', scenario, board=board, realtime=0)

    def test_toggle_note_ignores_release_and_sends_on_second_press(self):
        self.run_case('voice', 164, board=3, realtime=0)

    def test_toggle_note_shorter_than_the_minimum_is_not_sent(self):
        for scenario in (165, 186):
            with self.subTest(scenario=scenario):
                self.run_case('voice', scenario, board=3, realtime=0)

    def test_toggle_note_still_sends_at_the_recording_limit(self):
        self.run_case('voice', 166, board=3, realtime=0)

    def test_toggle_waking_press_only_wakes(self):
        self.run_case('voice', 185, board=3, realtime=0)

    def test_toggled_note_blocks_menu_and_sleep(self):
        self.run_case('input', 23, board=3, realtime=0)

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
        self.run_case('voice', 141)

    def test_hands_free_transcript_starts_speaking_before_audio(self):
        self.run_case('voice', 49)

    def test_hands_free_working_keeps_the_mic_open(self):
        self.run_case('voice', 149)

    def test_goodbye_tail_drains_bounded_and_yellow_interrupts(self):
        for scenario in (12, 13, 14):
            self.run_case('voice', scenario)

    def test_new_call_after_remote_end_has_fresh_ui(self):
        self.run_case('voice', 15)

    def test_note_mode_persists_without_changing_other_board_defaults(self):
        for board, realtime in ((1, 1), (0, 1), (1, 0)):
            self.run_case('settings', 1, board=board, realtime=realtime)

    def test_mic_gain_kept_and_console_words(self):
        # A stored Settings gain boots as stored, a fresh NVS at the default; mic.gain? /
        # mic.gain=N parse and clamp.
        for board, realtime in ((1, 1), (0, 1)):
            self.run_case('settings', 2, board=board, realtime=realtime)

    def test_bench_volume_is_ram_only(self):
        # Bench and test mutes are RAM-only. vol=N on
        # muse_settings.c never writes NVS, a reboot comes back at the saved volume, vol=saved restores
        # it, a user's or the app's change ends it and is saved over the saved values; the boot's
        # restore from NVS notifies nothing; the word's grammar and answers (muse_vol_word.h).
        for board, realtime in ((1, 1), (0, 1)):
            self.run_case('settings', 3, board=board, realtime=realtime)

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

    def test_talk_quick_tap_wake_menu_pairing_and_lock(self):
        # Locked by the passcode, a talk press opens the keypad and is dropped.
        self.run_case('input', 7)

    def test_a_talk_press_applies_the_lock_after_5_min_dark(self):
        # "After 5 min" lapsed with the screen dark, before the UI's tick: the press itself locks and opens the keypad.
        self.run_case('input', 24)
        self.run_case('input', 24, board=0)

    def test_display_only_talk_press_never_posts(self):
        self.run_case('input', 22)
        self.run_case('input', 22, board=0)

    def test_blue_call_routing_is_stopwatch_only(self):
        self.run_case('input', 8)
        self.run_case('input', 8, board=0)
        self.run_case('input', 8, realtime=0)

    def test_active_call_prevents_auto_sleep(self):
        # And on USB the screen times out as on battery (no keep-alive).
        self.run_case('input', 9)

    def test_sleep_when_charged(self):
        # USB/battery x charged/not x Always/Never, a held lit screen,
        # the usual timeout after, dark stays dark, the charger's done bit
        # over the percentage, the latch tracked whatever the setting (a
        # switch to Never holds at once mid top-up; only unplug clears it;
        # a failed read keeps it but holds nothing until a good reading
        # on USB, with no plug-in wake), the % fallback's 90/85 hysteresis, a
        # hidden setting, and the USB plug-in wake.
        self.run_case('input', 20)

    def test_dark_panel_hold_talks_from_the_press(self):
        # A held talk key talks from a dark panel, with one press: 400 ms
        # held talks, 150 ms only wakes, and the resting codecs' slow first read
        # never eats the 250 ms threshold (it counts from the press). Every
        # route: the StopWatch's push-to-talk call, a voice note, the BLE client.
        for scenario in (41, 42, 43):
            with self.subTest(scenario=scenario):
                self.run_case('voice', scenario)
                self.run_case('voice', scenario, board=0)
                self.run_case('voice', scenario, realtime=0)
                self.run_case('voice', scenario, ble=1)
                self.run_case('voice', scenario, realtime=0, ble=1)

    def test_face_and_panel_follow_audio_not_the_open_stream(self):
        # One reply stream per Live call (mgcommands.h): open and silent between
        # replies, no speaking face and no panel hold; each reply's audio brings them, its end ends them.
        self.run_case('voice', 51, ble=1)
        self.run_case('voice', 51, realtime=0, ble=1)

    def test_client_capture_waits_for_the_codecs_to_open(self):
        # The StopWatch on battery: a start_mic right after a wake, while the codecs are still opening,
        # must not end at the first closed read with nothing sent.
        for scenario in (44, 45):
            with self.subTest(scenario=scenario):
                self.run_case('voice', scenario, ble=1)
                self.run_case('voice', scenario, realtime=0, ble=1)


    def test_auto_sleep_only_when_truly_idle(self):
        for board, realtime in ((1, 1), (0, 1), (1, 0)):
            self.run_case('input', 21, board=board, realtime=realtime)

    def test_active_turn_pokes_idle_clock(self):
        # main's voice 42 (#281), carried as 142 in muse_ptt_voice_harness.c.
        for board, realtime in ((1, 1), (0, 1), (1, 0)):
            self.run_case('voice', 142, board=board, realtime=realtime)

    def test_explicit_notes_and_other_boards_keep_notes(self):
        self.run_case('voice', 21)
        self.run_case('voice', 20, board=0)
        self.run_case('voice', 20, realtime=0)


    def test_ble_or_realtime_voice_by_route(self):
        # With musegadgets BLE audio, each press goes the way the route policy
        # says (mg_route.c, the real decision): to the phone over BLE, into the
        # queue for it, nowhere, or Muse's realtime voice over Wi-Fi, which then
        # runs exactly as on a build without BLE (a call on the StopWatch,
        # otherwise a voice note). No route leaves a call mark behind.
        for scenario in range(60, 69):
            with self.subTest(scenario=scenario):
                self.run_case('voice', scenario, ble=1)
                self.run_case('voice', scenario, realtime=0, ble=1)

    def test_release_post_roll_to_the_phone(self):
        # A release keeps capturing ~200 ms, then
        # the last frame, then stop_mic; the ring is up through the post-roll
        # (72). A turn the device ends, at the 15 s cap (67) or the client
        # stopping it (68), stops at once with nothing after button_up. Every
        # capture still ends with stop_mic.
        # A new press 100 ms into a post-roll (73) cuts it: turn 1 ends with
        # its stop_mic, turn 2 starts at once, no refused press, no gap in the
        # mic (its pre-roll holds every read since turn 1's end).
        for scenario in (72, 73, 67, 68):
            with self.subTest(scenario=scenario):
                self.run_case('voice', scenario, ble=1)
                self.run_case('voice', scenario, realtime=0, ble=1)

    def test_barge_in_mutes_the_reply_at_touch_down(self):
        # The barge-in echo rule: a talk
        # press during a reply mutes it at touch-down (the input task's post,
        # before the voice task sees the press), so the pre-roll records none
        # of it. Held (74): no reply sample plays from touch-down on and the
        # turn is claimed at the hold threshold (stop_streaming drop, as
        # before), never resumed. A tap (75): the reply resumes at the tap
        # from the very sample it paused at, nothing claimed. The pause itself
        # (credits, the ring, a call's flush, a client's stop) is
        # test_mg_ble's barge pause case.
        for scenario in (74, 75):
            with self.subTest(scenario=scenario):
                self.run_case('voice', scenario, ble=1)
                self.run_case('voice', scenario, realtime=0, ble=1)


if __name__ == '__main__':
    unittest.main()
