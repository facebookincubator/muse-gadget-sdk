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

"""The wake word session core: state machine,
timers, PTT/playback interleavings, audio rings, and the energy fallback,
driven through scripted runner events and checked against a Python
model written from the design rules, not from the C."""

import os
import shlex
import subprocess
import tempfile
import unittest
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WW = ROOT / 'components/muse_wakeword'
SANITIZE = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
ENV = {**os.environ,
       'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
       'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}

IDLE, ARMED, CAPTURING, CAPTURING_PTT = 0, 1, 2, 3
POST_DOWN, POST_UP, CANCELLED, SUBMITTED, CONVERTED = 1, 2, 4, 8, 16
NONE, WAKE, SPEECH_START, SPEECH_END = 0, 1, 2, 3
EV = {'.': NONE, 'W': WAKE, 's': SPEECH_START, 'e': SPEECH_END}


def model(ticks, start, end, cap, energy):
    """State machine restated: wake arms only; speech opens the turn (POST_DOWN);
    T_start cancels with nothing posted; trailing silence or the hard cap
    posts up; playback discards wakes; a real press cancels arming and
    converts a capture, whose own release then ends it."""
    start, end, cap = max(1, start), max(1, end), max(1, cap)   # init clamp
    state, armed, capture, silence = IDLE, 0, 0, 0
    stats = [0, 0, 0, 0, 0]   # wakes, cancels, submits, conversions, suppressed
    trace = []
    for ev_ch, voiced, down, up, playback, enabled in ticks:
        if ev_ch == 'A':
            # The glue could not post the press: back to idle, a cancel.
            if state != IDLE:
                stats[1] += 1
            state, armed, capture, silence = IDLE, 0, 0, 0
            trace.append((state, 0))
            continue
        ev = EV[ev_ch] if enabled else NONE   # harness: listening == enabled
        act = 0
        if not enabled:
            if state == CAPTURING:
                act = POST_UP | SUBMITTED
                stats[2] += 1
            elif state == ARMED:
                act = CANCELLED
                stats[1] += 1
            elif state == CAPTURING_PTT:
                act = SUBMITTED
                stats[2] += 1
            state, armed, capture, silence = IDLE, 0, 0, 0
            trace.append((state, act))
            continue
        if down:
            # A real press wins over playback suppression: it cancels
            # arming or converts a capture even on a suppressed tick. A
            # release noted in the same tick ends the button's capture.
            if state == ARMED:
                state, act = IDLE, CANCELLED
                stats[1] += 1
            elif state == CAPTURING:
                stats[3] += 1
                if up:
                    state, act = IDLE, CONVERTED | SUBMITTED
                    stats[2] += 1
                else:
                    state, act = CAPTURING_PTT, CONVERTED
            elif state == CAPTURING_PTT and up:
                state, act = IDLE, SUBMITTED
                stats[2] += 1
            trace.append((state, act))
            continue
        if state == CAPTURING_PTT:
            # The release also wins over suppression, or a converted
            # capture could outlive the button turn that took it over.
            capture += 1
            if up or capture >= cap:
                state, act = IDLE, SUBMITTED
                stats[2] += 1
            trace.append((state, act))
            continue
        if playback:
            if ev == WAKE:
                stats[4] += 1
            trace.append((state, 0))
            continue
        if state == IDLE:
            if ev == WAKE:
                state, armed = ARMED, 0
                stats[0] += 1
        elif state == ARMED:
            if ev == SPEECH_START or (energy and voiced):
                state, capture, silence, act = CAPTURING, 0, 0, POST_DOWN
            else:
                armed += 1
                if armed >= start:
                    state, act = IDLE, CANCELLED
                    stats[1] += 1
        elif state == CAPTURING:
            capture += 1
            silence = 0 if ev == SPEECH_START or (energy and voiced) else silence + 1
            if ev == SPEECH_END or (energy and silence >= end) or capture >= cap:
                state, act = IDLE, POST_UP | SUBMITTED
                stats[2] += 1
        trace.append((state, act))
    return trace, stats


def tick(ev='.', voiced=0, down=0, up=0, playback=0, enabled=1):
    return (ev, voiced, down, up, playback, enabled)


class WakeWordCore(unittest.TestCase):
    START, END, CAP = 5, 3, 10

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'muse_wakeword_harness'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        result = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-include', str(ROOT / 'tests/host_compat.h'), '-I', str(WW / 'include'),
             str(ROOT / 'tests/muse_wakeword_harness.c'),
             str(WW / 'muse_wakeword_core.c'),
             '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_core(self, ticks, energy=1, start=None, end=None, cap=None):
        start = self.START if start is None else start
        end = self.END if end is None else end
        cap = self.CAP if cap is None else cap
        data = ''.join(f'{e}{v}{d}{u}{p}{n}\n' for e, v, d, u, p, n in ticks)
        ran = subprocess.run([str(self.binary), 'core', str(start), str(end),
                              str(cap), str(energy)],
                             input=data.encode(), capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        lines = ran.stdout.decode().splitlines()
        trace = [tuple(map(int, line.split())) for line in lines[:-1]]
        stats = list(map(int, lines[-1].split()[1:]))
        want_trace, want_stats = model(ticks, start, end, cap, energy)
        self.assertEqual(trace, want_trace)
        self.assertEqual(stats, want_stats)
        return trace

    def test_false_trigger_cancels_without_opening_a_turn(self):
        trace = self.run_core([tick('W')] + [tick()] * 8)
        # No POST_DOWN on wake, and a cancel posts nothing at all.
        self.assertFalse(any(a & POST_DOWN for _, a in trace))
        self.assertFalse(any(a & POST_UP for _, a in trace))
        self.assertIn(CANCELLED, [a for _, a in trace])

    def test_wake_then_speech_events_open_and_close(self):
        trace = self.run_core([tick('W'), tick('s'), tick(), tick('e'), tick()], energy=0)
        self.assertEqual([a for _, a in trace if a], [POST_DOWN, POST_UP | SUBMITTED])

    def test_energy_endpointing_without_runner_events(self):
        ticks = [tick('W'), tick(voiced=1)] + [tick()] * (self.END + 1)
        trace = self.run_core(ticks, energy=1)
        self.assertEqual([a for _, a in trace if a], [POST_DOWN, POST_UP | SUBMITTED])

    def test_zero_silence_config_clamps_to_one_chunk(self):
        # end=0 must not submit a voiced capture on its first tick:
        # core_init clamps every chunk count to at least 1.
        ticks = [tick('W'), tick(voiced=1), tick(voiced=1), tick()]
        trace = self.run_core(ticks, energy=1, end=0)
        self.assertEqual(trace[2], (CAPTURING, 0))
        self.assertEqual(trace[3][1], POST_UP | SUBMITTED)

    def test_hard_cap_ends_capture_without_endpointing(self):
        ticks = [tick('W'), tick('s')] + [tick('s')] * (self.CAP + 2)
        trace = self.run_core(ticks, energy=0)
        self.assertEqual(trace[1 + self.CAP][1], POST_UP | SUBMITTED)

    def test_playback_suppresses_wake(self):
        trace = self.run_core([tick('W', playback=1), tick('s'), tick()])
        self.assertEqual(trace[0], (IDLE, 0))

    def test_real_press_cancels_arming_and_converts_capture(self):
        armed = self.run_core([tick('W'), tick(down=1), tick('s')])
        self.assertEqual(armed[1][1], CANCELLED)
        ticks = [tick('W'), tick('s'), tick(down=1), tick(), tick(up=1)]
        trace = self.run_core(ticks, energy=0)
        # The physical release ends it: SUBMITTED alone, no synthetic POST_UP.
        self.assertEqual(trace[-1][1], SUBMITTED)
        self.assertFalse(any(a & POST_UP for _, a in trace))

    def test_press_and_release_in_one_tick_end_the_capture(self):
        # Regression: the release used to be lost with the press, leaving
        # CAPTURING_PTT (and every wake ignored) until the hard cap.
        ticks = [tick('W'), tick('s'), tick(down=1, up=1), tick('W'), tick('s')]
        trace = self.run_core(ticks, energy=0)
        self.assertEqual(trace[2], (IDLE, CONVERTED | SUBMITTED))
        self.assertEqual(trace[4], (CAPTURING, POST_DOWN))   # wakes work again
        converted = [tick('W'), tick('s'), tick(down=1), tick(down=1, up=1)]
        trace = self.run_core(converted, energy=0)
        self.assertEqual(trace[3], (IDLE, SUBMITTED))

    def test_abort_drops_a_capture_whose_press_never_went(self):
        ticks = [tick('W'), tick('s'), tick('A'), tick(), tick(), tick(), tick()]
        trace = self.run_core(ticks, energy=0)
        self.assertEqual(trace[2], (IDLE, 0))
        # Nothing posts afterwards: no stray release for a turn that never opened.
        self.assertFalse(any(a & POST_UP for _, a in trace))

    def test_press_during_playback_cancels_arming(self):
        # Regression: the glue reports a physical PTT turn through the
        # same suppression gate as playback, so the cancel press can
        # arrive on a suppressed tick. It must still cancel; otherwise
        # the stale ARMED opens a synthetic turn on the next voiced
        # chunk after the button turn ends.
        ticks = [tick('W'), tick(down=1, playback=1), tick(voiced=1)]
        trace = self.run_core(ticks)
        self.assertEqual(trace[1], (IDLE, CANCELLED))
        self.assertFalse(any(a & POST_DOWN for _, a in trace))

    def test_release_during_playback_ends_converted_capture(self):
        ticks = [tick('W'), tick('s'), tick(down=1), tick(up=1, playback=1)]
        trace = self.run_core(ticks, energy=0)
        self.assertEqual(trace[-1], (IDLE, SUBMITTED))

    def test_disable_closes_an_open_capture_and_cancels_arming(self):
        ticks = [tick('W'), tick('s'), tick(), tick(enabled=0), tick('W', enabled=0)]
        trace = self.run_core(ticks, energy=0)
        self.assertEqual(trace[3][1], POST_UP | SUBMITTED)
        self.assertEqual(trace[4], (IDLE, 0))
        armed = self.run_core([tick('W'), tick(enabled=0)])
        self.assertEqual(armed[1][1], CANCELLED)

    def run_ring(self, cap, chunk, pushes, pops):
        ran = subprocess.run([str(self.binary), 'ring', str(cap), str(chunk),
                              str(pushes), str(pops)], capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        popped_line, snap_line, drops_line = ran.stdout.decode().splitlines()
        # deque model of the ring: overflow drops the oldest frames, counted.
        held, drops, nxt = deque(), 0, 0
        for _ in range(pushes):
            for _ in range(chunk):
                if len(held) == cap:
                    held.popleft()
                    drops += 1
                held.append(nxt)
                nxt += 1
        popped = []
        for _ in range(pops):
            for _ in range(chunk):
                if held:
                    popped.append(held.popleft())
        got_popped = [int(x) for x in popped_line.split()[1:] if x != '-']
        got_snap = [int(x) for x in snap_line.split()[1:]]
        self.assertEqual(got_popped, popped)
        self.assertEqual(got_snap, list(held))
        self.assertEqual(drops_line.split(), ['drops', str(drops), 'fill', str(len(held))])

    def test_ring_drops_oldest_and_snapshots_in_order(self):
        self.run_ring(cap=8, chunk=4, pushes=1, pops=0)      # partial fill
        self.run_ring(cap=8, chunk=4, pushes=3, pops=1)      # overflow by a chunk
        self.run_ring(cap=10, chunk=4, pushes=4, pops=2)     # unaligned wrap
        self.run_ring(cap=320 * 25, chunk=320, pushes=30, pops=5)   # handoff ring size
        self.run_ring(cap=4, chunk=4, pushes=1, pops=3)      # pop past empty

    def run_energy(self, samples, threshold):
        ran = subprocess.run([str(self.binary), 'energy', str(threshold)],
                             input=(' '.join(map(str, samples))).encode(),
                             capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        want = bool(samples) and sum(s * s for s in samples) >= threshold * threshold * len(samples)
        self.assertEqual(int(ran.stdout.decode().strip()), int(want))

    def test_energy_voiced_is_a_mean_square_check(self):
        self.run_energy([], 512)
        self.run_energy([0] * 320, 512)
        self.run_energy([511] * 320, 512)
        self.run_energy([512] * 320, 512)     # exactly at the threshold: voiced
        self.run_energy([-4000] * 320, 512)
        self.run_energy([100] * 319 + [9000], 512)   # one loud sample is not speech energy


class WakeWordBenchRunner(unittest.TestCase):
    """The bench stand-in for the ET runner: a serial trigger and a
    clap trigger. A burst is a short transient (or sustained loud
    sound) spanning at most 8 chunks, then 2 quiet chunks to fire and
    50 feeds of cooldown; constants pinned from ww_runner_bench.h."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'muse_wakeword_bench_harness'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        result = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-include', str(ROOT / 'tests/host_compat.h'), '-I', str(WW / 'include'),
             str(ROOT / 'tests/muse_wakeword_bench_harness.c'),
             str(WW / 'muse_wakeword_core.c'), str(WW / 'ww_runner_bench.c'),
             '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_bench(self, pattern):
        ran = subprocess.run([str(self.binary), pattern], capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        return [int(x) for x in ran.stdout.decode().split()]

    def test_quiet_room_never_wakes(self):
        self.assertEqual(self.run_bench('Q' * 20), [NONE] * 20)

    def test_serial_trigger_fires_once_on_the_next_feed(self):
        self.assertEqual(self.run_bench('TQ'), [WAKE, NONE])
        self.assertEqual(self.run_bench('WQ'), [WAKE, SPEECH_START])

    def test_energy_burst_needs_loud_then_quiet(self):
        self.assertEqual(self.run_bench('LLLQQ'), [NONE] * 4 + [WAKE])
        self.assertEqual(self.run_bench('LQQ'), [NONE, NONE, WAKE])
        self.assertEqual(self.run_bench('LLLLLQ'), [NONE] * 6)     # still loud

    def test_short_clap_transient_wakes_after_quiet(self):
        # Whole-chunk RMS is only 3000: sustained-chunk thresholds missed
        # real claps on the BOX-3, whose energy is in the first few ms.
        self.assertEqual(self.run_bench('CQQ'), [NONE, NONE, WAKE])
        self.assertEqual(self.run_bench('CCCQQ')[-1], WAKE)        # decay across chunks
        self.assertEqual(self.run_bench('cQQ'), [NONE] * 3)        # peak too soft

    def test_sustained_loud_sound_is_not_a_clap(self):
        self.assertEqual(self.run_bench('L' * 8 + 'QQ')[-1], WAKE)
        self.assertEqual(self.run_bench('L' * 9 + 'QQ')[-1], NONE)

    def test_cooldown_blocks_an_immediate_second_burst(self):
        self.assertEqual(self.run_bench('LLLQQ' + 'LLLQQ')[-1], NONE)
        again = self.run_bench('LLLQQ' + 'Q' * 50 + 'LLLQQ')
        self.assertEqual(again[-1], WAKE)

    def test_gates_discard_bursts_and_triggers(self):
        self.assertEqual(self.run_bench('0LLLQQ1Q'), [NONE] * 6)   # not listening
        self.assertEqual(self.run_bench('sLLLQQuQ'), [NONE] * 6)   # suppressed
        self.assertEqual(self.run_bench('sTuQ'), [NONE, NONE])     # trigger dropped

    def test_reset_forgets_a_half_finished_burst(self):
        self.assertEqual(self.run_bench('LLLRQQ'), [NONE] * 5)
        self.assertEqual(self.run_bench('CRQQ'), [NONE] * 3)


def between(source, start, end):
    at = source.index(start)
    return source[at:source.index(end, at)]


class WakeWordFirmwareContract(unittest.TestCase):
    """The firmware wiring that only exists as source relationships:
    commands advertised exactly where they are dispatched under the
    same option, the tap inside both read loops, and the opt-in
    defaulting off. Behaviour is the other classes' job."""

    def test_commands_advertised_and_dispatched_with_the_feature(self):
        noise = (ROOT / 'main/noise_control.cpp').read_text()
        block = between(noise, '#if CONFIG_MUSE_WAKEWORD_ENABLE', '#endif')
        self.assertIn('add_command(commands, "enable_wakeword"', block)
        self.assertIn('add_command(commands, "disable_wakeword"', block)
        app = (ROOT / 'main/app.c').read_text()
        dispatch = between(app, 'static cJSON *on_ws_command(', 'unsupported command')
        block = between(dispatch, '#if CONFIG_MUSE_WAKEWORD_ENABLE', '#endif')
        self.assertIn('"enable_wakeword"', block)
        self.assertIn('"disable_wakeword"', block)

    def test_tap_is_inside_both_audio_read_loops(self):
        voice = (ROOT / 'components/muse/muse_voice.c').read_text()
        idle = between(voice, 'static void idle_capture(void)', 'static bool got_event')
        record = between(voice, 'static bool record(', 'static void go_idle')
        self.assertIn('muse_wakeword_on_audio(s_chunk', idle)
        self.assertIn('muse_wakeword_on_audio(s_chunk', record)

    def test_synthetic_turns_are_tagged_seeded_and_exempt(self):
        self.assertIn('bool synthetic;', (ROOT / 'components/muse/muse_input.h').read_text())
        voice = (ROOT / 'components/muse/muse_voice.c').read_text()
        self.assertIn('void muse_voice_seed_preroll(', voice)
        # Short-press handling moved into note_held's qualification;
        # synthetic turns stay exempt by carrying the flag into record(),
        # which consumes the seed and pauses the runner only for real turns.
        self.assertIn('record(synthetic, why', voice)
        self.assertIn('if (seeded && s_seed_frames)', voice)
        self.assertIn('muse_wakeword_set_ptt_turn(!synthetic)', voice)

    def test_feature_off_builds_allocate_no_seed(self):
        # The one-second seed is only used by wake word turns: a build
        # without the feature (no-PSRAM boards included) must not pay
        # 32 KB of internal RAM for it.
        voice = (ROOT / 'components/muse/muse_voice.c').read_text()
        start = between(voice, 'esp_err_t muse_voice_start(', 's_pre = heap_caps_malloc')
        gated = between(start, '#if CONFIG_MUSE_WAKEWORD_ENABLE', '#endif')
        self.assertIn('s_seed = heap_caps_malloc(', gated)
        self.assertEqual(voice.count('s_seed = heap_caps_malloc('), 1)
        kconfig = (WW / 'Kconfig').read_text()
        enable = between(kconfig, 'config MUSE_WAKEWORD_ENABLE', 'choice MUSE_WAKEWORD_RUNNER')
        self.assertIn('depends on SPIRAM', enable)

    def test_synthetic_edges_carry_the_input_epoch_and_time(self):
        # note_held cancels a press whose epoch is not the current one, and
        # the epoch moves whenever a real edge is dropped: an untagged
        # (zero) synthetic press would then never record again.
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        post = between(glue, 'static bool post_ptt(', 'xQueueSend(')
        self.assertIn('.epoch = muse_input_call_epoch()', post)
        self.assertIn('.at_us = esp_timer_get_time()', post)

    def test_glue_retries_a_refused_release_and_drops_a_refused_press(self):
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        tick_fn = between(glue, 'static void tick(', 'static void apply_requests(')
        # The owed release goes before the core ticks again ...
        self.assertLess(tick_fn.index('if (s_up_owed && post_ptt(MUSE_PTT_UP))'),
                        tick_fn.index('muse_wakeword_core_tick('))
        self.assertIn('!post_ptt(MUSE_PTT_UP)) {\n        s_up_owed = true;', tick_fn)
        # ... and a refused press ends the session and clears its seed.
        down = between(tick_fn, 'MUSE_WW_ACTION_POST_PTT_DOWN', 'MUSE_WW_ACTION_POST_PTT_UP')
        self.assertIn('if (!s_up_owed && post_ptt(MUSE_PTT_DOWN))', down)
        self.assertIn('muse_wakeword_core_abort(&s_core);', down)
        self.assertIn('muse_voice_seed_preroll(NULL, 0);', down)

    def test_glue_starts_with_the_voice_pipeline(self):
        app = (ROOT / 'components/muse/muse_app.c').read_text()
        started = app.index('muse_voice_start(q)')
        self.assertIn('muse_wakeword_init(q)', app[started:])

    def test_speech_start_shows_listening_before_posting(self):
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        post = between(glue, 'MUSE_WW_ACTION_POST_PTT_DOWN', 'MUSE_WW_ACTION_POST_PTT_UP')
        listening = post.index('muse_state_set_mode(MUSE_MODE_LISTENING)')
        caption = post.index('muse_state_set_caption("LISTENING...")')
        posted = post.index('post_ptt(MUSE_PTT_DOWN)')
        self.assertLess(listening, posted)
        self.assertLess(caption, posted)

    def test_wake_heard_wakes_the_screen(self):
        # A talk press wakes the screen; a wake must too, or its prompt and
        # the whole hands-free turn happen on a dark display.
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        heard = between(glue, 'before == MUSE_WW_IDLE && now == MUSE_WW_ARMED', 'MUSE_WW_ACTION_POST_PTT_DOWN')
        self.assertIn('muse_state_set_asleep(false)', heard)

    def test_sound_page_has_wake_word_toggle(self):
        # Same row name as the production UI; the small print is honest
        # that only the clap bench trigger sits behind it for now.
        ui = (ROOT / 'components/muse/muse_settings_ui.c').read_text()
        sound = between(ui, 'static void build_sound_page(', 'static void tick_sound(void)')
        self.assertIn('switch_row(list, "Wake word", muse_wakeword_enabled(), on_wake_sw)', sound)
        self.assertIn('Model not implemented yet, clap to wake', sound)
        self.assertNotIn('Clap test', sound)
        callback = between(ui, 'static void on_wake_sw(', 'static void build_sound_page(')
        self.assertIn('muse_wakeword_set_enabled(', callback)
        tick = between(ui, 'static void tick_sound(void)', '/* ---------- Sleep')
        self.assertIn('muse_wakeword_enabled()', tick)   # switch tracks serial/Muse toggles

    def test_bench_keys_and_off_by_default(self):
        input_c = (ROOT / 'components/muse/muse_input.c').read_text()
        for key in ('"wake.on"', '"wake=1"', '"wake.test=1"'):
            self.assertIn(key, input_c)
        # The toggle must compare the whole key: an index check here once
        # made "wake.on" turn listening off (caught on the BOX-3).
        self.assertIn('muse_wakeword_set_enabled(!strcmp(line, "wake.on"))', input_c)
        kconfig = (WW / 'Kconfig').read_text()
        enable = between(kconfig, 'config MUSE_WAKEWORD_ENABLE', 'choice MUSE_WAKEWORD_RUNNER')
        self.assertIn('default n', enable)
        settings = (ROOT / 'components/muse/muse_settings.c').read_text()
        self.assertIn('"ww_on"', settings)   # the persisted opt-in


if __name__ == '__main__':
    unittest.main()
