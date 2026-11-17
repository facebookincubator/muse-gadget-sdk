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

import hashlib
import json
import math
import os
import shlex
import struct
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
            if state == CAPTURING:   # a hands-free note is dropped, not sent
                act = POST_UP | CANCELLED
                stats[1] += 1
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
            if state == ARMED:   # a reply (or button turn) ends a waiting wake
                state, act = IDLE, CANCELLED
                stats[1] += 1
            trace.append((state, act))
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
             '-include', str(ROOT / 'tests/host_compat.h'),
             '-I', str(WW / 'include'), '-I', str(ROOT / 'tests'),
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

    def test_playback_ends_a_waiting_wake_for_good(self):
        # Regression: ARMED used to survive playback with its start timer
        # paused, so a wake just before a late reply opened a note on the
        # first loud chunk after the reply, however long it was.
        ticks = [tick('W'), tick()] + [tick(playback=1)] * 200 + [tick(voiced=1)] * 3
        trace = self.run_core(ticks)
        self.assertEqual(trace[2], (IDLE, CANCELLED))
        self.assertFalse(any(a & POST_DOWN for _, a in trace))

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
        # Turning listening off cancels the hands-free note: never submitted.
        self.assertEqual(trace[3][1], POST_UP | CANCELLED)
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


def between(source, start, end):
    at = source.index(start)
    return source[at:source.index(end, at)]


class WakeWordFirmwareContract(unittest.TestCase):
    """The firmware wiring that only exists as source relationships:
    commands advertised exactly where they are dispatched under the
    same option, the tap inside both read loops, and the opt-in
    defaulting off. Behaviour is the other classes' job."""

    def test_executorch_runner_is_the_default_production_runner(self):
        kconfig = (WW / 'Kconfig').read_text()
        cmake = (WW / 'CMakeLists.txt').read_text()
        # One runner, no choice: nothing but the model can start a turn.
        self.assertNotIn('MUSE_WAKEWORD_RUNNER', kconfig)
        self.assertNotIn('clap', kconfig.lower())
        self.assertNotIn('ET_ENABLE', kconfig)
        self.assertIn('ww_runner_espnn.cpp', cmake)
        self.assertIn('models/hey_muse.pte', cmake)
        # The program is one ESP-NN delegate call: only the backend is
        # declared, so no kernel bundle and no executorch Python at build time.
        self.assertIn('executorch_add_backend(EspNNBackend)', cmake)
        for gone in ('executorch_add_model', 'executorch_add_kernels', 'gen_kernels.py', 'EXECUTORCH_PYTHON'):
            self.assertNotIn(gone, cmake)
        self.assertFalse((WW / 'et_kernels').exists())
        self.assertFalse((WW / 'tools/export_hey_muse.py').exists())
        self.assertTrue((WW / 'models/hey_muse.pte').is_file())

    def test_runner_copies_unaligned_embedded_pte(self):
        # Program::load needs aligned program data, but EMBED_FILES
        # alignment is layout luck: an embed that once landed 4-aligned made
        # the load fail and the Settings toggle revert. The embed is 16-aligned
        # in flash (custom asm), and the runner copies a misaligned
        # caller-supplied model into aligned storage it owns.
        src = (WW / 'ww_runner_espnn.cpp').read_text()
        self.assertIn('% kAlign) != 0', src)
        self.assertIn('pte_copy', src)
        self.assertIn('ww_free(r->pte_copy)', src)
        cmake = (WW / 'CMakeLists.txt').read_text()
        self.assertIn('hey_muse_pte.S.in', cmake)
        self.assertNotIn('EMBED_FILES ${', cmake)
        asm = (WW / 'hey_muse_pte.S.in').read_text()
        self.assertIn('.balign 16', asm)
        self.assertIn('_binary_hey_muse_pte_start', asm)

    def test_runner_binds_its_buffers_once_and_sizes_the_arena(self):
        # Zero-copy: every input and output is bound once at load, each
        # state's input and output share one buffer, a step is one execute();
        # the method arena is exactly what a counting load used.
        runner = (WW / 'ww_runner_espnn.cpp').read_text()
        bind = between(runner, 'bool bind_io(ww_runner_t *r)', '\n}\n')
        self.assertIn('m.set_input(EValue(Tensor(h)), 1 + k)', bind)
        self.assertIn('m.set_output_data_ptr(buf, out->nbytes(), 1 + k)', bind)
        step = between(runner, 'void on_features(', '\n}\n')
        self.assertIn('r->method->execute()', step)
        self.assertNotIn('set_input', step)
        self.assertIn('class CountingAllocator', runner)
        self.assertIn('r->arena_bytes = round16(used)', runner)
        # No temp allocator: the delegate keeps its own arena.
        self.assertIn('r->memory.emplace(allocator, &*r->planned, nullptr)', runner)

    def test_model_sidecar_matches_the_binary_and_the_code(self):
        sidecar = json.loads((WW / 'models/hey_muse.json').read_text())
        pte = WW / 'models/hey_muse.pte'
        self.assertEqual(sidecar['pte_size'], pte.stat().st_size)
        self.assertEqual(sidecar['pte_sha256'], hashlib.sha256(pte.read_bytes()).hexdigest())
        model_h = (WW / 'ww_model.h').read_text()

        def define(name):
            for line in model_h.splitlines():
                if line.startswith('#define ' + name + ' '):
                    return line.split(None, 2)[2].split('/*')[0].strip().strip('()f')
            raise AssertionError(name)

        self.assertEqual(sidecar['input']['shape'], [1, 40, 3, 1])
        self.assertEqual(float(define('WW_MODEL_INPUT_SCALE')), sidecar['input']['scale'])
        self.assertEqual(int(define('WW_MODEL_INPUT_ZERO_POINT')), sidecar['input']['zero_point'])
        self.assertEqual(int(define('WW_MODEL_OUTPUT_ZERO_POINT')), sidecar['output']['zero_point'])
        self.assertEqual(sidecar['output']['scale'], 1 / 256)
        self.assertEqual(int(define('WW_MODEL_STATES')), len(sidecar['state']))
        self.assertEqual(int(define('WW_MODEL_STATE_BYTES')), sidecar['state_bytes'])
        self.assertEqual(sum(math.prod(t['shape']) for t in sidecar['state']), sidecar['state_bytes'])
        # The runner checks each state's exact shape against these.
        listed = lambda name: [int(v) for v in define(name).strip('{} ').split(',')]
        self.assertEqual([[1, c, n, 1] for c, n in zip(listed('WW_MODEL_STATE_CHANNELS'),
                                                        listed('WW_MODEL_STATE_LENGTHS'))],
                         [t['shape'] for t in sidecar['state']])
        self.assertEqual({t['zero_point'] for t in sidecar['state']},
                         {int(define('WW_MODEL_STATE_ZERO_POINT'))})
        frontend = (WW / 'ww_frontend.c').read_text()
        fe = sidecar['frontend']
        for text in ('window.size_ms = %d' % fe['window_ms'], 'window.step_size_ms = %d' % fe['step_ms'],
                     'lower_band_limit = 125.0f', 'upper_band_limit = 7500.0f',
                     'smoothing_bits = 10', 'strength = 0.95f', 'offset = 80.0f', 'gain_bits = 21',
                     'scale_shift = 6', '0.0390625f'):
            self.assertIn(text, frontend)
        # Training data attribution ships with the binary.
        attribution = (WW / 'models/ATTRIBUTION.md').read_text()
        for source in ('LibriTTS', 'LibriSpeech', 'AMI', 'VoxPopuli', 'MUSAN', 'Kevin MacLeod'):
            self.assertIn(source, attribution)

    def test_muse_can_only_turn_listening_off(self):
        # Only audio after a detection on the device may leave it, so no
        # remote command turns listening on; Muse gets wakeword.disable.
        noise = (ROOT / 'main/noise_control.cpp').read_text()
        block = between(noise, '#if CONFIG_MUSE_WAKEWORD_ENABLE', '#endif')
        self.assertIn('add_command(commands, "wakeword.disable"', block)
        self.assertEqual(block.count('add_command('), 1)
        app = (ROOT / 'main/app.c').read_text()
        dispatch = between(app, 'static cJSON *on_ws_command(', 'unsupported command')
        block = between(dispatch, '#if CONFIG_MUSE_WAKEWORD_ENABLE', '#endif')
        self.assertIn('"wakeword.disable"', block)
        self.assertEqual(block.count('strcmp(command,'), 1)
        for name in ('enable_wakeword', 'disable_wakeword', 'wakeword.enable'):
            self.assertNotIn(name, noise)
            self.assertNotIn(name, app)
        # The only call from the Link side sets it off.
        main_calls = [line.strip() for path in (ROOT / 'main').glob('*.c*')
                      for line in path.read_text().splitlines()
                      if 'muse_wakeword_set_enabled(' in line]
        self.assertEqual(main_calls, ['muse_wakeword_set_enabled(false);'])

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
        enable = between(kconfig, 'config MUSE_WAKEWORD_ENABLE', 'choice MUSE_WAKEWORD_SENSITIVITY')
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
        self.assertIn('if (!s_up_owed && s_enabled && post_ptt(MUSE_PTT_DOWN))', down)
        self.assertIn('muse_wakeword_core_abort(&s_core);', down)
        self.assertIn('muse_voice_seed_preroll(NULL, 0);', down)
        # The LISTENING state shown before posting goes back to idle, so a
        # refused press leaves no capture on screen and auto-sleep resumes.
        refused = down[down.index('muse_voice_seed_preroll(NULL, 0);'):]
        self.assertIn('if (muse_state_mode(NULL) == MUSE_MODE_LISTENING) {\n                muse_state_set_mode(MUSE_MODE_IDLE);', refused)

    def test_enable_wakes_a_resting_voice_task_and_needs_a_running_glue(self):
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        setter = between(glue, 'void muse_wakeword_set_enabled(bool on)', 'static bool post_ptt(')
        not_started = between(setter, 'if (!s_started) {', 'return;')
        # After a failed init only "off" is saved.
        self.assertIn('if (!on) {', not_started)
        self.assertLess(setter.index('if (!s_started) {'),
                        setter.index('muse_settings_set_wakeword_on(on);'))
        self.assertIn('muse_state_nudge();', setter)

    def test_failed_wake_task_start_frees_everything(self):
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        failed = between(glue, '"no memory for the wake task"', 's_started = true;')
        for freed in ('ww_runner_destroy(s_runner)', 'vStreamBufferDeleteWithCaps(s_tap)',
                      'free(s_hist_slots)', 'free(pre_storage)', 'free(s_snap)'):
            self.assertIn(freed, failed)

    def test_turn_ended_closes_the_capture_but_not_a_new_arming(self):
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        apply = between(glue, 'static void apply_requests(void)', 'static void snapshot_status(void)')
        block = between(apply, 'if (s_turn_end_req) {', 'if (s_playback != s_applied_playback)')
        self.assertIn('s_up_owed = false;', block)
        self.assertIn('st == MUSE_WW_CAPTURING || st == MUSE_WW_CAPTURING_PTT', block)
        self.assertIn('muse_wakeword_core_abort(&s_core);', block)
        self.assertNotIn('MUSE_WW_ARMED', block)

    def test_glue_starts_with_the_voice_pipeline(self):
        voice = (ROOT / 'components/muse/muse_voice.c').read_text()
        start = between(voice, 'esp_err_t muse_voice_start(QueueHandle_t queue)', '\n}\n')
        task = start.index('xTaskCreatePinnedToCoreWithCaps(voice_task')
        self.assertIn('muse_wakeword_init(queue)', start[task:])

    def test_feature_off_builds_compile_no_wake_word_code(self):
        # Builds without the feature must not change: the glue isn't
        # compiled, its hooks are inline no-ops, and every wake word line in
        # shared files is behind the option.
        cmake = (ROOT / 'components/muse/CMakeLists.txt').read_text()
        self.assertIn('if(CONFIG_MUSE_WAKEWORD_ENABLE)\n        # Wake word tap and task', cmake)
        header = (ROOT / 'components/muse/muse_wakeword_glue.h').read_text()
        stubs = header[header.index('#else   /* !CONFIG_MUSE_WAKEWORD_ENABLE */'):]
        self.assertIn('static inline void muse_wakeword_on_audio(', stubs)
        self.assertNotIn('!CONFIG_MUSE_WAKEWORD_ENABLE', (ROOT / 'components/muse/muse_wakeword_glue.c').read_text())
        self.assertNotIn('muse_wakeword', (ROOT / 'components/muse/muse_app.c').read_text())
        main_yml = (ROOT / 'main/idf_component.yml').read_text()
        self.assertNotIn('executorch', main_yml)
        manifest = (WW / 'idf_component.yml').read_text()
        self.assertIn('if: "$CONFIG{MUSE_WAKEWORD_EXECUTORCH} == \\"y\\""', manifest)
        kconfig = (WW / 'Kconfig').read_text()
        helper = between(kconfig, 'config MUSE_WAKEWORD_EXECUTORCH', 'menu "Muse wake word"')
        self.assertIn('default "y" if MUSE_WAKEWORD_ENABLE', helper)
        self.assertIn('default "n"', helper)

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

    def test_wakeword_overlay_is_committed_and_wired(self):
        # Wake word builds come from a committed overlay, not local
        # sdkconfig fragments, and board.sh builds it in its own dir.
        overlay = (ROOT / 'devices/sdkconfig.muse-wakeword').read_text()
        self.assertIn('CONFIG_MUSE_WAKEWORD_ENABLE=y', overlay)
        self.assertIn('# CONFIG_ET_OPTIMIZE_SIZE is not set', overlay)
        self.assertIn('# CONFIG_ET_ENABLE_LOGGING is not set', overlay)
        self.assertIn('# CONFIG_ESPNN_DELEGATE_CONSTANTS_TO_PSRAM is not set', overlay)
        board = (ROOT / 'tools/muse/board.sh').read_text()
        self.assertIn('B=$B-wakeword; defaults="$defaults;devices/sdkconfig.muse-wakeword"', board)
        self.assertNotIn('EXECUTORCH_PYTHON', board)

    def test_listening_needs_a_working_runner(self):
        # A runner that failed to load or run hears nothing, so the
        # status and the indicator must not say listening.
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        listening = between(glue, 'bool muse_wakeword_listening(void)\n{', '\n}')
        self.assertIn('s_runner_ok', listening)
        # A requested disable reads as not listening at once, so
        # wakeword.disable's reply is never enabled:false, listening:true.
        self.assertIn('s_enabled && s_applied_enabled', listening)
        app = (ROOT / 'main/app.c').read_text()
        disable = between(app, 'static cJSON *wakeword_disable_command(void) {', '\n}\n')
        self.assertLess(disable.index('muse_wakeword_set_enabled(false);'), disable.index('muse_wakeword_status(&st);'))
        snapshot = between(glue, 'static void snapshot_status(void)', '\n}')
        self.assertIn('ww_runner_ok(s_runner)', snapshot)

    def test_face_shows_when_the_wake_word_is_on(self):
        # A visible indicator whenever listening is on: lit while the runner
        # hears, dim while it's paused, hidden when off.
        ui = (ROOT / 'components/muse/muse_ui.c').read_text()
        status = between(ui, '/* Status line: connectivity icons + power. */', 's_power_lbl =')
        self.assertIn('s_listen_icon = make_mic(status,', status)
        chrome = between(ui, 'static void update_chrome(float now)', '\n}\n')
        block = between(chrome, '#if CONFIG_MUSE_WAKEWORD_ENABLE', '#endif')
        self.assertIn('!muse_wakeword_enabled() ? 0 : muse_wakeword_listening() ? 2 : 1', block)
        self.assertIn('lv_obj_set_flag(s_listen_icon, LV_OBJ_FLAG_HIDDEN, listen == 0);', block)

    def test_sound_page_has_wake_word_toggle(self):
        ui = (ROOT / 'components/muse/muse_settings_ui.c').read_text()
        sound = between(ui, 'static void build_sound_page(', 'static void tick_sound(void)')
        self.assertIn('switch_row(list, "Wake word", muse_wakeword_enabled(), on_wake_sw)', sound)
        self.assertIn('Listens for \\"Hey Muse\\"', sound)
        self.assertNotIn('not implemented', sound)   # user copy, not a placeholder
        self.assertNotIn('clap', sound.lower())
        callback = between(ui, 'static void on_wake_sw(', 'static void build_sound_page(')
        self.assertIn('muse_wakeword_set_enabled(', callback)
        tick = between(ui, 'static void tick_sound(void)', '/* ---------- Sleep')
        self.assertIn('muse_wakeword_enabled()', tick)   # switch tracks serial/Muse toggles

    def test_option_needs_psram_and_the_lvgl_ui(self):
        # The audio buffers (~133 KB) only fit in PSRAM, and only the LVGL UI
        # has the controls and the listening indicator.
        kconfig = (WW / 'Kconfig').read_text()
        enable = between(kconfig, 'config MUSE_WAKEWORD_ENABLE', 'choice MUSE_WAKEWORD_SENSITIVITY')
        self.assertIn('depends on SPIRAM && !MUSE_UI_FREEINK', enable)
        sensitivity = between(kconfig, 'choice MUSE_WAKEWORD_SENSITIVITY', 'endchoice')
        sidecar = json.loads((WW / 'models/hey_muse.json').read_text())
        for name, label in (('low', 'Low'), ('default', 'Default'), ('high', 'High')):
            cutoff = sidecar['detector']['cutoffs'][name]
            measured = sidecar['evaluation']['by_sensitivity'][name]
            self.assertIn('%s %.2f' % (label, cutoff), sensitivity)
            self.assertIn('%.2ff' % cutoff, (WW / 'ww_detector.c').read_text())
            row = '| %s | %.2f | %.1f%% | %.2f |' % (label, cutoff, measured['recall_percent'],
                                                    measured['false_wakes_per_hour'])
            self.assertIn(row, (WW / 'models/README.md').read_text())
        self.assertIn('synthetic', sensitivity)   # how it was measured
        self.assertIn("Real voices haven't", sensitivity)

    def test_serial_keys_and_off_by_default(self):
        input_c = (ROOT / 'components/muse/muse_input.c').read_text()
        for key in ('"wake"', '"wake.on"', '"wake.off"'):
            self.assertIn(key, input_c)
        # No bench triggers: only the model can start a turn.
        for key in ('"wake=1"', '"wake.test=1"', 'bench_trigger'):
            self.assertNotIn(key, input_c)
        # The toggle must compare the whole key: an index check here once
        # made "wake.on" turn listening off (caught on the BOX-3).
        self.assertIn('muse_wakeword_set_enabled(!strcmp(line, "wake.on"))', input_c)
        kconfig = (WW / 'Kconfig').read_text()
        enable = between(kconfig, 'config MUSE_WAKEWORD_ENABLE', 'choice MUSE_WAKEWORD_SENSITIVITY')
        self.assertIn('default n', enable)
        settings = (ROOT / 'components/muse/muse_settings.c').read_text()
        self.assertIn('"ww_on"', settings)   # the persisted opt-in


DETECTOR_HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ww_detector.h"

/* stdin: one probability per line; stdout: the 0-based indices that fire. */
int main(int argc, char **argv) {
    ww_detector_t det;
    ww_sensitivity_t s = argc > 1 && !strcmp(argv[1], "low") ? WW_SENSITIVITY_LOW
                       : argc > 1 && !strcmp(argv[1], "high") ? WW_SENSITIVITY_HIGH
                       : WW_SENSITIVITY_DEFAULT;
    ww_detector_init(&det, s);
    printf("cutoff %.2f\n", (double)det.cutoff);
    char line[64];
    int i = 0;
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "reset", 5)) {
            ww_detector_reset(&det);
            continue;
        }
        if (ww_detector_push(&det, strtof(line, NULL))) printf("%d\n", i);
        i++;
    }
    return 0;
}
"""


def detector_model(probs, cutoff):
    """microWakeWord's rule restated: the mean of the last five must be
    above the cutoff with five real values; a fire clears the window and
    ignores the next 2 s (200 feature frames, 3 per inference)."""
    window, fired, refractory = [], [], 0
    for i, p in enumerate(probs):
        window = (window + [p])[-5:]
        if refractory > 0:
            refractory -= 3
            continue
        if len(window) == 5 and sum(window) / 5 > cutoff:
            fired.append(i)
            refractory, window = 200, []
    return fired


class WakeWordDetector(unittest.TestCase):
    """ww_detector.c against detector_model() on scripted probabilities."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        harness = Path(cls.tmp.name) / 'detector.c'
        harness.write_text(DETECTOR_HARNESS)
        cls.binary = Path(cls.tmp.name) / 'detector'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        result = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-I', str(WW), '-I', str(WW / 'include'), str(harness),
             str(WW / 'ww_detector.c'), '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_detector(self, lines, sensitivity='default'):
        ran = subprocess.run([str(self.binary), sensitivity], input='\n'.join(lines) + '\n',
                             capture_output=True, text=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        out = ran.stdout.split()
        return float(out[1]), [int(x) for x in out[2:]]

    def test_cutoffs_follow_the_sensitivity_and_the_sidecar(self):
        cutoffs = json.loads((WW / 'models/hey_muse.json').read_text())['detector']['cutoffs']
        for name in ('low', 'default', 'high'):
            cutoff, _ = self.run_detector([], name)
            self.assertAlmostEqual(cutoff, cutoffs[name], places=6)
        # Low sensitivity is the strictest cutoff, High the most permissive.
        self.assertGreater(cutoffs['low'], cutoffs['default'])
        self.assertGreater(cutoffs['default'], cutoffs['high'])

    def test_needs_five_real_values_and_a_mean_strictly_above(self):
        _, fired = self.run_detector(['1.0'] * 4)
        self.assertEqual(fired, [])
        _, fired = self.run_detector(['1.0'] * 5)
        self.assertEqual(fired, [4])
        _, fired = self.run_detector(['0.22'] * 10)   # equal to the cutoff
        self.assertEqual(fired, [])

    def test_refractory_is_two_seconds_and_clears_the_window(self):
        _, fired = self.run_detector(['1.0'] * 200)
        self.assertEqual(fired, detector_model([1.0] * 200, 0.22))
        self.assertEqual(fired[:2], [4, 72])   # 67 inferences ignored after a fire

    def test_reset_clears_the_window_and_the_refractory(self):
        _, fired = self.run_detector(['1.0'] * 5 + ['reset'] + ['1.0'] * 5)
        self.assertEqual(fired, [4, 9])

    def test_matches_the_restated_rule_on_a_long_random_stream(self):
        state, probs = 7, []
        for _ in range(5000):
            state = (1103515245 * state + 12345) & 0x7fffffff
            probs.append(((state >> 8) % 257) / 256.0)   # output steps are 1/256
        lines = ['%.8f' % p for p in probs]
        for name, cutoff in (('low', 0.26), ('default', 0.22), ('high', 0.13)):
            _, fired = self.run_detector(lines, name)
            self.assertEqual(fired, detector_model([float(x) for x in lines], cutoff))


def wake_word_deps():
    """The registry components the runner builds against: pytorch/executorch,
    espressif/esp-nn and esphome/esp-micro-speech-features, from
    MUSE_WAKEWORD_DEPS or from managed_components/ after a wake-word
    configure (CI runs one)."""
    names = {'executorch': 'pytorch__executorch', 'esp-nn': 'espressif__esp-nn',
             'esp-micro-speech-features': 'esphome__esp-micro-speech-features'}
    for base in filter(None, (os.environ.get('MUSE_WAKEWORD_DEPS'), str(ROOT / 'managed_components'))):
        found = {}
        for short, managed in names.items():
            for candidate in (Path(base) / short, Path(base) / managed):
                if (candidate / 'idf_component.yml').is_file():
                    found[short] = candidate
        if len(found) == 3:
            return found
    return None


def synthetic_speech(rate=16000):
    """Self-authored test audio: six crude formant-synthesized "hey muse"-like
    vowel sequences (an impulse train through three moving resonators) over
    a fixed-LCG noise floor. No recordings or text-to-speech."""
    import math
    import struct

    def interp(points, n):
        out = []
        for i in range(n):
            t = i / n
            for (t0, v0), (t1, v1) in zip(points, points[1:]):
                if t0 <= t <= t1:
                    out.append(v0 + (v1 - v0) * (t - t0) / (t1 - t0))
                    break
            else:
                out.append(points[-1][1])
        return out

    def resonate(x, freqs, bw):
        y1 = y2 = 0.0
        out = []
        for s, f in zip(x, freqs):
            r = math.exp(-math.pi * bw / rate)
            y = (1 - r) * s + 2 * r * math.cos(2 * math.pi * f / rate) * y1 - r * r * y2
            y2, y1 = y1, y
            out.append(y)
        return out

    def phrase(pitch, speed, seed):
        n = int(0.75 / speed * rate)
        f0 = interp([(0, pitch * 1.1), (0.4, pitch), (1, pitch * 0.85)], n)
        phase, src, st = 0.0, [], seed
        for i in range(n):
            phase += f0[i] / rate
            pulse = 1.0 if phase >= 1.0 else 0.0
            phase -= int(phase)
            st = (1103515245 * st + 12345) & 0x7fffffff
            src.append(pulse * 40 + (st / 0x7fffffff - 0.5) * 0.4)
        f1 = interp([(0, 600), (0.25, 450), (0.4, 300), (0.55, 280), (0.75, 320), (1, 300)], n)
        f2 = interp([(0, 1900), (0.25, 2100), (0.4, 1100), (0.55, 2100), (0.75, 1000), (1, 1400)], n)
        f3 = interp([(0, 2600), (0.4, 2400), (1, 2500)], n)
        amp = interp([(0, 0), (0.05, 1), (0.35, 1), (0.42, 0.3), (0.5, 1), (0.85, 0.9), (1, 0)], n)
        y = [p + q + s for p, q, s in zip(resonate(src, f1, 90), resonate(src, f2, 120), resonate(src, f3, 160))]
        return [v * g for v, g in zip(y, amp)]

    out = []
    for k, (pitch, speed) in enumerate([(120, 1.0), (180, 1.1), (100, 0.9), (220, 1.2), (150, 1.0), (130, 0.85)]):
        out += [0.0] * int(0.6 * rate)
        p = phrase(pitch, speed, 1000 + k)
        peak = max(abs(v) for v in p) or 1
        out += [v / peak * 9000 for v in p]
    out += [0.0] * int(0.6 * rate)
    pcm, st = [], 777
    for v in out:
        st = (1103515245 * st + 12345) & 0x7fffffff
        pcm.append(int(max(-32768, min(32767, round(v + (st / 0x7fffffff - 0.5) * 300)))))
    return struct.pack('<%dh' % len(pcm), *pcm)


# SHA-256 of the wake probabilities ((p * 256) - 128 as bytes, one per 30 ms)
# for synthetic_speech(): the same bytes TensorFlow Lite Micro's reference
# kernels give for the same weights (models/README.md), so a change here is
# a change in the front end, the model or the runtime.
SYNTHETIC_PROBABILITIES_SHA256 = 'badf5323b84b666a2a4e277f3ce8db5504673bd81ef3a988f13e8b964a48c093'

RUNNER_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ww_runner.h"

static unsigned g_count;
static void on_score(void *ctx, float p) {
    (void)ctx;
    printf("%02x", (unsigned)((int)(p * 256.0f + 0.5f) - 128) & 0xffu);
    g_count++;
}

static int16_t *load(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    int16_t *pcm = malloc((size_t)bytes);
    *n = fread(pcm, 2, (size_t)bytes / 2, f);
    fclose(f);
    return pcm;
}

#ifdef COUNT_ALLOCS
int g_counting, g_allocs;
void *__real_malloc(size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);
void *__real_aligned_alloc(size_t a, size_t n);
void *__wrap_malloc(size_t n) { g_allocs += g_counting; return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { g_allocs += g_counting; return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { g_allocs += g_counting; return __real_realloc(p, n); }
void *__wrap_aligned_alloc(size_t a, size_t n) { g_allocs += g_counting; return __real_aligned_alloc(a, n); }
#endif

static size_t feed_all(ww_runner_t *r, const int16_t *pcm, size_t n, size_t chunk, int *wakes) {
    size_t calls = 0;
    for (size_t off = 0; off < n; off += chunk, calls++) {
        size_t k = n - off < chunk ? n - off : chunk;
        if (ww_runner_feed(r, pcm + off, k) == WW_EVENT_WAKE) (*wakes)++;
    }
    return calls;
}

int main(int argc, char **argv) {
    const char *mode = argv[1];
    size_t n = 0;
    int16_t *pcm = argc > 2 && strcmp(mode, "model") ? load(argv[2], &n) : NULL;
    ww_config_t cfg = { .sample_rate_hz = 16000, .sensitivity = WW_SENSITIVITY_DEFAULT, .vad = WW_VAD_ENERGY };
    if (!strcmp(mode, "bad")) {
        static uint8_t junk[4096];
        for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 37 + 11);
        cfg.model = junk;
        cfg.model_len = sizeof junk;
        ww_runner_t *r = ww_runner_create(&cfg);
        ww_config_t bad_rate = { .sample_rate_hz = 8000 };
        printf("junk %d rate %d\n", r != NULL, ww_runner_create(&bad_rate) != NULL);
        return 0;
    }
    if (!strcmp(mode, "model")) {
        /* argv[2]: a model file, loaded as an app-supplied model */
        FILE *f = fopen(argv[2], "rb");
        static uint8_t buf[1 << 16];
        size_t len = f ? fread(buf, 1, sizeof buf, f) : 0;
        if (f) fclose(f);
        cfg.model = buf;
        cfg.model_len = len;
        ww_runner_t *m = ww_runner_create(&cfg);
        printf("model %d\n", m != NULL && ww_runner_ok(m));
        ww_runner_destroy(m);
        return 0;
    }
    ww_runner_t *r = ww_runner_create(&cfg);
    if (!r || !ww_runner_ok(r)) return 3;
    ww_runner_set_score_hook(r, on_score, NULL);
    int wakes = 0;
    if (!strcmp(mode, "digest")) {
        feed_all(r, pcm, n, (size_t)atoi(argv[3]), &wakes);
        printf("\ncount %u wakes %d ok %d\n", g_count, wakes, ww_runner_ok(r));
    } else if (!strcmp(mode, "reset")) {
        feed_all(r, pcm, n / 2, 320, &wakes);
        ww_runner_reset(r);
        printf("\n");
        feed_all(r, pcm, n, 320, &wakes);
        printf("\n");
    } else if (!strcmp(mode, "gates")) {
        ww_runner_set_listening(r, false);
        feed_all(r, pcm, n, 320, &wakes);
        ww_runner_set_listening(r, true);
        ww_runner_set_suppressed(r, true);
        feed_all(r, pcm, n, 320, &wakes);
        printf("muted %u", g_count);
        ww_runner_set_suppressed(r, false);
        printf("\n");
        feed_all(r, pcm, n, 320, &wakes);
        printf("\nnull %d\n", ww_runner_feed(r, NULL, 320) == WW_EVENT_NONE && ww_runner_feed(NULL, pcm, 320) == WW_EVENT_NONE);
    }
#ifdef COUNT_ALLOCS
    else if (!strcmp(mode, "allocs")) {
        g_counting = 1;
        feed_all(r, pcm, n, 320, &wakes);
        ww_runner_reset(r);
        ww_runner_set_suppressed(r, true);
        ww_runner_set_suppressed(r, false);
        ww_runner_set_listening(r, false);
        ww_runner_set_listening(r, true);
        feed_all(r, pcm, n, 999, &wakes);
        g_counting = 0;
        printf("\nallocs %d\n", g_allocs);
    }
#endif
    ww_runner_destroy(r);
    free(pcm);
    return 0;
}
"""


class WakeWordRunner(unittest.TestCase):
    """ww_runner_espnn.cpp with the real ExecuTorch runtime, ESP-NN delegate
    (esp-nn's portable C kernels) and microfrontend, built for the host from
    the same pinned registry components as the firmware, with the stand-ins
    in components/muse_wakeword/host/."""

    @classmethod
    def setUpClass(cls):
        deps = wake_word_deps()
        if deps is None:
            raise unittest.SkipTest('needs the wake word registry components: configure a '
                                    'wake-word build first or set MUSE_WAKEWORD_DEPS')
        import concurrent.futures
        import re
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        et, nn, mf = deps['executorch'], deps['esp-nn'], deps['esp-micro-speech-features']
        generated = (et / 'et_generated.cmake').read_text()

        def listed(name):
            block = re.search(r'set\(%s\n(.*?)\n\)' % name, generated, re.S).group(1)
            return [et / p for p in re.findall(r'"(src/[^"]+)"', block)]

        cc = shlex.split(os.environ.get('CC', 'cc'))
        cxx = shlex.split(os.environ.get('CXX', 'c++'))
        et_defs = ['-DC10_USING_CUSTOM_GENERATED_MACROS', '-DET_USE_BUILTIN_FUNCTION_NAME=0',
                   '-DFLATBUFFERS_MAX_ALIGNMENT=1024', '-DET_ENABLE_PROGRAM_VERIFICATION=0',
                   '-DET_LOG_ENABLED=0', '-DET_MIN_LOG_LEVEL=Error']
        et_inc = ['-I', str(et / 'include'), '-I', str(et / 'priv_include')]
        nn_inc = ['-I', str(nn / 'include'), '-I', str(nn / 'src/common')]
        host = ['-I', str(WW / 'host')]
        ww_inc = ['-I', str(WW), '-I', str(WW / 'include'), '-I', str(mf / 'include')]
        cxx_flags = ['-std=gnu++17', '-fno-exceptions', '-fno-rtti']
        jobs = []
        for src in listed('ET_CORE_SRCS') + listed('ET_ESPNN_SRCS'):
            jobs.append((cxx, cxx_flags + ['-w', *host, *et_inc, *nn_inc, *et_defs], src))
        jobs.append((cxx, cxx_flags + ['-w', *et_inc, *et_defs], WW / 'host/et_pal.cpp'))
        for src in sorted(nn.glob('src/*/*_ansi.c')) + [nn / 'src/common/esp_nn_multicore.c']:
            jobs.append((cc, ['-std=gnu11', '-w', *host, *nn_inc], src))
        for src in sorted(mf.glob('src/*.c')):
            jobs.append((cc, ['-std=gnu11', '-w', '-I', str(mf / 'include'), '-I', str(mf / 'src')], src))
        strict = ['-Wall', '-Wextra', '-Werror']
        jobs.append((cc, ['-std=gnu11', *strict, '-ffp-contract=off', *ww_inc], WW / 'ww_frontend.c'))
        jobs.append((cc, ['-std=gnu11', *strict, *ww_inc], WW / 'ww_detector.c'))
        et_system = ['-isystem', str(et / 'include')]
        jobs.append((cxx, cxx_flags + [*strict, *ww_inc, *et_system, *et_defs], WW / 'ww_runner_espnn.cpp'))
        pte = WW / 'models/hey_muse.pte'
        asm = tmp / 'hey_muse_pte.S'
        asm.write_text((WW / 'hey_muse_pte.S.in').read_text()
                       .replace('@HEY_MUSE_PTE_PATH@', str(pte))
                       .replace('@HEY_MUSE_PTE_LEN@', str(pte.stat().st_size)))
        jobs.append((cc, [], asm))
        harness = tmp / 'runner.c'
        harness.write_text(RUNNER_HARNESS)

        def build(variant, extra, link_extra, harness_defs):
            out = tmp / variant
            out.mkdir()

            def one(job):
                tool, flags, src = job
                obj = out / (hashlib.sha256(str(src).encode()).hexdigest()[:12] + '.o')
                ran = subprocess.run([*tool, *extra, *flags, '-c', str(src), '-o', str(obj)],
                                     capture_output=True, text=True)
                if ran.returncode:
                    raise AssertionError(f'{src}: {ran.stderr}')
                return str(obj)

            with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
                objs = list(pool.map(one, jobs + [(cc, ['-std=gnu11', *strict, *harness_defs,
                                                         '-I', str(WW / 'include')], harness)]))
            binary = out / 'runner'
            ran = subprocess.run([*cxx, *extra, *objs, *link_extra, '-lm', '-o', str(binary)],
                                 capture_output=True, text=True)
            if ran.returncode:
                raise AssertionError(ran.stderr)
            return binary

        cls.binary = build('asan', SANITIZE, [], [])
        cls.counting = build('plain', ['-O1'], ['-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,'
                                                '--wrap=aligned_alloc'], ['-DCOUNT_ALLOCS'])
        cls.speech = tmp / 'speech.raw'
        cls.speech.write_bytes(synthetic_speech())

    def run_mode(self, *args, binary=None):
        ran = subprocess.run([str(binary or self.binary), *map(str, args)],
                             capture_output=True, text=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        return ran.stdout.split('\n')

    def test_probabilities_are_the_reference_bytes_at_any_chunking(self):
        for chunk in (320, 1, 7, 999, 16000):
            out = self.run_mode('digest', self.speech, chunk)
            self.assertEqual(hashlib.sha256(bytes.fromhex(out[0])).hexdigest(),
                             SYNTHETIC_PROBABILITIES_SHA256, chunk)
            self.assertEqual(out[1], 'count 290 wakes 0 ok 1')

    def test_reset_starts_the_stream_over(self):
        first, second = self.run_mode('reset', self.speech)[:2]
        whole = self.run_mode('digest', self.speech, 320)[0]
        self.assertEqual(second, whole)
        self.assertTrue(whole.startswith(first[:len(first) - 8]))

    def test_paused_and_suppressed_runners_infer_nothing(self):
        out = self.run_mode('gates', self.speech)
        self.assertEqual(out[0], 'muted 0')
        self.assertEqual(hashlib.sha256(bytes.fromhex(out[1])).hexdigest(),
                         SYNTHETIC_PROBABILITIES_SHA256)
        self.assertEqual(out[2], 'null 1')

    def test_feed_and_reset_never_allocate(self):
        out = self.run_mode('allocs', self.speech, binary=self.counting)
        self.assertEqual(out[1], 'allocs 0')

    def test_a_model_that_breaks_the_contract_is_refused(self):
        self.assertEqual(self.run_mode('bad')[0], 'junk 0 rate 0')

    def model_file(self, data):
        path = Path(self.tmp.name) / f'model-{len(data)}-{hashlib.sha256(data).hexdigest()[:8]}.pte'
        path.write_bytes(data)
        return path

    def test_the_model_also_loads_from_a_file(self):
        model = (WW / 'models/hey_muse.pte').read_bytes()
        self.assertEqual(self.run_mode('model', self.model_file(model))[0], 'model 1')

    def test_a_model_planning_huge_buffers_is_refused_before_allocating(self):
        # The shipped model with forward's two planned buffers declared at
        # 2 GiB and 2 GiB + 32: their rounded sum wraps a 32-bit size_t.
        data = bytearray((WW / 'models/hey_muse.pte').read_bytes())
        for offset, fmt, value in ((0x10, '<Q', 0x9d8), (0xc0, '<I', 0x8fc), (0x9bc, '<I', 3),
                                   (0x9c0, '<q', 0), (0x9c8, '<q', 0x80000000), (0x9d0, '<q', 0x80000020)):
            struct.pack_into(fmt, data, offset, value)
        self.assertEqual(self.run_mode('model', self.model_file(bytes(data)))[0], 'model 0')


HANDOFF_HARNESS = r"""
/* The wake task -> recorder hand-off with the wake task trailing: the
 * voice task numbers and keeps every chunk it taps (muse_ww_history) and
 * queues it for the wake task; the wake task pre-rolls what it consumes
 * and, at the speech start, seeds the turn and notes the next number; the
 * recorder sends the seed, the history from that number, then live chunks.
 * Each chunk's samples are its absolute index, so the note must count up
 * by one with nothing missing or repeated. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_wakeword.h"

#define CHUNK MUSE_WW_CHUNK_FRAMES
#define TAP 25
#define HIST 32
#define PRE (16000)

static void fill(int16_t *pcm, long k) { for (int i = 0; i < CHUNK; i++) pcm[i] = (int16_t)(k % 30000); }

static int run(long start_seq, int lag)
{
    static muse_ww_chunk_t slots[HIST];
    static int16_t pre_store[PRE], seed[PRE], note[PRE * 4];
    muse_ww_history_t hist;
    muse_ww_history_init(&hist, slots, HIST);
    hist.next_seq = (uint32_t)start_seq;   /* numbers may wrap */
    muse_ww_ring_t pre;
    muse_ww_ring_init(&pre, pre_store, PRE);
    muse_ww_chunk_t tap[TAP];   /* the stream buffer, as a FIFO */
    int tap_n = 0;
    long produced = 0, speech_at = 120;   /* the wake task seeds at chunk 120 */
    uint32_t seed_end = 0;
    size_t seed_frames = 0;
    int seeded = 0;
    /* Voice task runs ahead; the wake task consumes only once `lag` chunks are queued. */
    while (!seeded) {
        int16_t pcm[CHUNK];
        fill(pcm, produced++);
        const muse_ww_chunk_t *item = muse_ww_history_push(&hist, pcm);
        if (tap_n < TAP) tap[tap_n++] = *item; else return 10;   /* never drops here */
        while (tap_n > lag) {
            muse_ww_chunk_t got = tap[0];
            memmove(tap, tap + 1, (size_t)--tap_n * sizeof(tap[0]));
            muse_ww_ring_push(&pre, got.pcm, CHUNK);
            if ((long)(got.seq - (uint32_t)start_seq) == speech_at) {
                seed_frames = muse_ww_ring_snapshot(&pre, seed, PRE);
                seed_end = got.seq + 1;
                seeded = 1;
                break;
            }
        }
    }
    /* Two more chunks are read before the press reaches the recorder. */
    for (int j = 0; j < 2; j++) {
        int16_t pcm[CHUNK];
        fill(pcm, produced++);
        muse_ww_history_push(&hist, pcm);
    }
    size_t n = 0;
    memcpy(note, seed, seed_frames * sizeof(int16_t)); n += seed_frames;
    const int16_t *later[HIST];
    bool complete;
    size_t k = muse_ww_history_since(&hist, seed_end, later, HIST, &complete);
    if (!complete) return 14;
    for (size_t i = 0; i < k; i++) { memcpy(note + n, later[i], CHUNK * sizeof(int16_t)); n += CHUNK; }
    for (int j = 0; j < 5; j++) { fill(note + n, produced++); n += CHUNK; }   /* live */
    if (seed_frames > PRE) return 11;   /* at most 1 s before the speech start */
    long first = note[0];
    for (size_t c = 0; c < n / CHUNK; c++)
        for (int i = 0; i < CHUNK; i++)
            if (note[c * CHUNK + i] != (int16_t)((first + (long)c) % 30000)) return 12;
    if ((long)(n / CHUNK) != produced - first) return 13;
    printf("lag %d backlog %zu seed %zu\n", lag, k, seed_frames / CHUNK);
    return 0;
}

/* Sustained overload: the wake task stalls while the voice task keeps
 * reading. The tap holds the first TAP chunks and drops the rest, but the
 * history numbers and keeps every chunk, so it moves past what the tap
 * still holds. A seed from those old items can't be followed without a
 * hole: since() must say so. After the gap, the pre-roll restarts, and a
 * seed there has no audio from before the gap and a complete backlog. */
static int overload(void)
{
    static muse_ww_chunk_t slots[HIST];
    static int16_t pre_store[PRE], seed[PRE];
    muse_ww_history_t hist;
    muse_ww_history_init(&hist, slots, HIST);
    muse_ww_ring_t pre;
    muse_ww_ring_init(&pre, pre_store, PRE);
    muse_wakeword_config_t cfg = { 0 };
    muse_wakeword_core_t core;
    muse_wakeword_core_init(&core, &cfg);
    muse_ww_chunk_t tap[TAP];
    int tap_n = 0;
    long produced = 0;
    for (; produced < 65; produced++) {   /* the stall: 65 chunks read, 25 queued */
        int16_t pcm[CHUNK];
        fill(pcm, produced);
        const muse_ww_chunk_t *item = muse_ww_history_push(&hist, pcm);
        if (tap_n < TAP) tap[tap_n++] = *item;
    }
    /* The wake task wakes and works through the queue; a seed at the last
     * queued item (chunk 24) can't be followed: 25-32 are gone. */
    uint32_t last = 0;
    int have = 0;
    for (int i = 0; i < tap_n; i++) {
        if (have && tap[i].seq != last + 1) return 30;   /* the queued items run on */
        muse_ww_ring_push(&pre, tap[i].pcm, CHUNK);
        last = tap[i].seq; have = 1;
    }
    const int16_t *later[HIST];
    bool complete = true;
    muse_ww_history_since(&hist, last + 1, later, HIST, &complete);
    if (complete) return 31;
    /* The next chunk the tap takes is 65: a gap. The pre-roll restarts. */
    int16_t pcm[CHUNK];
    fill(pcm, produced);
    muse_ww_chunk_t next = *muse_ww_history_push(&hist, pcm);
    produced++;
    if (next.seq == last + 1) return 32;
    muse_wakeword_core_gap(&core, &pre);
    if (muse_ww_ring_fill(&pre) != 0) return 33;
    muse_ww_ring_push(&pre, next.pcm, CHUNK);
    for (int j = 0; j < 9; j++) {   /* the wake task keeps up again */
        fill(pcm, produced++);
        const muse_ww_chunk_t *it = muse_ww_history_push(&hist, pcm);
        muse_ww_ring_push(&pre, it->pcm, CHUNK);
        last = it->seq;
    }
    size_t seed_frames = muse_ww_ring_snapshot(&pre, seed, PRE);
    if (seed[0] != 65 || seed_frames != 10 * CHUNK) return 34;   /* nothing from before the gap */
    size_t k = muse_ww_history_since(&hist, last + 1, later, HIST, &complete);
    if (!complete || k != 0) return 35;
    printf("overload ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "overload")) {
        return overload();
    }
    if (argc > 1 && !strcmp(argv[1], "gap")) {
        /* A gap ends a wake still waiting for speech and empties the
         * pre-roll; a running capture is left alone. */
        static int16_t store[PRE];
        muse_ww_ring_t pre;
        muse_ww_ring_init(&pre, store, PRE);
        muse_wakeword_config_t cfg = { .start_timeout_chunks = 100, .end_silence_chunks = 100,
                                       .max_capture_chunks = 1000, .energy_endpointing = true };
        muse_wakeword_core_t core;
        muse_wakeword_core_init(&core, &cfg);
        int16_t pcm[CHUNK] = {0};
        muse_wakeword_input_t wake = { .enabled = true, .event = WW_EVENT_WAKE };
        muse_wakeword_core_tick(&core, &wake);
        if (muse_wakeword_core_state(&core) != MUSE_WW_ARMED) return 40;
        muse_ww_ring_push(&pre, pcm, CHUNK);
        if (!muse_wakeword_core_gap(&core, &pre)) return 41;
        if (muse_wakeword_core_state(&core) != MUSE_WW_IDLE || muse_ww_ring_fill(&pre)) return 42;
        muse_wakeword_core_tick(&core, &wake);
        muse_wakeword_input_t talk = { .enabled = true, .voiced = true };
        muse_wakeword_core_tick(&core, &talk);
        if (muse_wakeword_core_state(&core) != MUSE_WW_CAPTURING) return 43;
        muse_ww_ring_push(&pre, pcm, CHUNK);
        if (muse_wakeword_core_gap(&core, &pre)) return 44;
        if (muse_wakeword_core_state(&core) != MUSE_WW_CAPTURING || muse_ww_ring_fill(&pre)) return 45;
        printf("gap ok\n");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "pause")) {
        /* A wake, then the voice task stops tapping for pause_ms (sending a
         * saved note, say), then speech. The tap's clock turns a pause past
         * the limit into a numbering gap; the wake task's gap rule (as in the
         * glue) then ends the waiting wake and empties the pre-roll, so the
         * resumed speech opens nothing. A short pause changes nothing. */
        uint32_t pause_ms = (uint32_t)strtoul(argv[2], NULL, 10);
        static muse_ww_chunk_t slots[HIST];
        static int16_t store[PRE];
        muse_ww_history_t hist;
        muse_ww_history_init(&hist, slots, HIST);
        muse_ww_ring_t pre;
        muse_ww_ring_init(&pre, store, PRE);
        muse_wakeword_config_t cfg = { .start_timeout_chunks = 125, .end_silence_chunks = 35,
                                       .max_capture_chunks = 750, .energy_endpointing = true };
        muse_wakeword_core_t core;
        muse_wakeword_core_init(&core, &cfg);
        uint32_t now = 4294967000u, last = 0;   /* the clock wraps too */
        int have = 0;
        uint32_t posted = 0;
        for (int k = 0; k < 20; k++) {
            int16_t pcm[CHUNK];
            int voiced = k >= 10;
            if (k == 10) now += pause_ms;   /* the pause, then speech */
            for (int i = 0; i < CHUNK; i++) pcm[i] = voiced ? (int16_t)(i % 2 ? 8000 : -8000) : 0;
            muse_ww_history_note_time(&hist, now, 200);
            const muse_ww_chunk_t *item = muse_ww_history_push(&hist, pcm);
            now += 20;
            /* the wake task, keeping up */
            if (have && item->seq != last + 1) muse_wakeword_core_gap(&core, &pre);
            have = 1;
            last = item->seq;
            muse_ww_ring_push(&pre, item->pcm, CHUNK);
            muse_wakeword_input_t in = { .enabled = true, .event = k == 3 ? WW_EVENT_WAKE : WW_EVENT_NONE,
                                         .voiced = muse_wakeword_energy_voiced(item->pcm, CHUNK, 512) };
            posted |= muse_wakeword_core_tick(&core, &in);
        }
        const int16_t *back[HIST];
        bool complete;
        /* a backlog reaching back before the pause is never complete */
        muse_ww_history_since(&hist, last - 12, back, HIST, &complete);
        printf("pause %u opened %d state %d preroll_chunks %u backlog_complete %d\n", (unsigned)pause_ms,
               (posted & MUSE_WW_ACTION_POST_PTT_DOWN) != 0, (int)muse_wakeword_core_state(&core),
               (unsigned)(muse_ww_ring_fill(&pre) / CHUNK), complete);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "since")) {
        static muse_ww_chunk_t slots[4];
        muse_ww_history_t h;
        const int16_t *out[8];
        muse_ww_history_init(&h, slots, 4);
        bool c;
        if (muse_ww_history_since(&h, 0, out, 8, &c) != 0 || !c) return 20;     /* empty, nothing owed */
        int16_t pcm[CHUNK] = {0};
        for (int i = 0; i < 6; i++) muse_ww_history_push(&h, pcm);         /* holds 2..5 */
        if (muse_ww_history_since(&h, 6, out, 8, &c) != 0 || !c) return 21;     /* nothing newer */
        if (muse_ww_history_since(&h, 100, out, 8, &c) != 0 || c) return 22;    /* the future */
        if (muse_ww_history_since(&h, 0, out, 8, &c) != 4 || c) return 23;      /* 0 and 1 are gone */
        if (muse_ww_history_since(&h, 2, out, 8, &c) != 4 || !c) return 26;     /* exactly what's held */
        if (muse_ww_history_since(&h, 4, out, 8, NULL) != 2) return 24;
        if (muse_ww_history_since(&h, 2, out, 3, &c) != 3 || c) return 25;      /* more than max */
        printf("since ok\n");
        return 0;
    }
    /* The queue holds at most TAP: the chunk being seeded plus TAP - 1 behind it. */
    const int lags[] = { 0, 1, 5, 12, 20, TAP - 1 };
    for (size_t li = 0; li < sizeof(lags) / sizeof(lags[0]); li++) {
        int lag = lags[li];
        int r = run(0, lag);
        if (r) return r;
        r = run(4294967200L, lag);   /* numbers wrap mid-turn */
        if (!r) r = run(4294967174L, lag);   /* the wrap lands inside the backlog */
        if (r) return 100 + r;
    }
    return 0;
}
"""


class WakeWordHandoff(unittest.TestCase):
    """The seed-plus-backlog hand-off keeps a hands-free note continuous
    however far the wake task trails the voice task (up to a full tap),
    and the pre-roll before the speech start stays within 1 s."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        harness = Path(cls.tmp.name) / 'handoff.c'
        harness.write_text(HANDOFF_HARNESS)
        cls.binary = Path(cls.tmp.name) / 'handoff'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        result = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-I', str(WW / 'include'), str(harness), str(WW / 'muse_wakeword_core.c'),
             '-o', str(cls.binary)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_mode(self, *args):
        ran = subprocess.run([str(self.binary), *args], capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stdout.decode() + ran.stderr.decode())
        return ran.stdout.decode()

    def test_history_since(self):
        self.assertEqual(self.run_mode('since').strip(), 'since ok')

    def test_overload_never_yields_a_note_with_a_hole(self):
        # The tap drops what doesn't fit while the history keeps numbering:
        # a seed from the old queued items is reported incomplete, and after
        # the gap the pre-roll restarts with nothing from before it.
        self.assertEqual(self.run_mode('overload').strip(), 'overload ok')

    def test_a_pause_in_the_taps_ends_a_waiting_wake(self):
        # Regression: while the voice task sent a saved note (up to 30 s)
        # the tap was empty and the wake task never expired the wake; the
        # first loud chunk afterwards opened a hands-free note.
        stalled = self.run_mode('pause', '30000').split()
        self.assertEqual(stalled[2:], ['opened', '0', 'state', '0', 'preroll_chunks', '10',
                                       'backlog_complete', '0'])
        # A pause shorter than the limit is just scheduling: the wake stands.
        short = self.run_mode('pause', '100').split()
        self.assertEqual(short[2:6], ['opened', '1', 'state', '2'])
        self.assertEqual(short[-1], '1')

    def test_a_disable_is_seen_before_any_press_goes(self):
        # A disable published while the wake task waited for a chunk or ran
        # the model ends the session on that tick (the core then cancels),
        # the press itself is re-checked, and every press carries the
        # disable count muse_voice checks at admission and while recording.
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        nxt = between(glue, 'static muse_wakeword_input_t next_input(', '\n}\n')
        self.assertLess(nxt.index('ww_runner_feed('), nxt.index('in.enabled = s_applied_enabled && s_enabled;'))
        setter = between(glue, 'void muse_wakeword_set_enabled(bool on)', 'static bool post_ptt(')
        self.assertLess(setter.index('atomic_fetch_add(&s_disables, 1)'), setter.index('s_enabled = on;'))
        self.assertIn('.disables = atomic_load(&s_disables)', between(glue, 'static bool post_ptt(', '\n}\n'))
        voice = (ROOT / 'components/muse/muse_voice.c').read_text()
        self.assertIn('if (!muse_wakeword_turn_wanted(s_press_disables)) return NOTE_CANCELLED;', voice)
        self.assertIn('if (seeded && !muse_wakeword_turn_wanted(s_press_disables)) {', voice)

    def test_glue_expires_a_waiting_wake_by_the_clock(self):
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        on_audio = between(glue, 'void muse_wakeword_on_audio(', '\n}\n')
        self.assertLess(on_audio.index('muse_ww_history_note_time(&s_hist'),
                        on_audio.index('muse_ww_history_push(&s_hist'))
        task = between(glue, 'static void wake_task(void *arg)', '\n}\n')
        self.assertIn('esp_timer_get_time() - s_armed_us >= (int64_t)CONFIG_MUSE_WAKEWORD_START_TIMEOUT_MS * 1000', task)
        playback = between(glue, 'if (s_playback != s_applied_playback) {', 'if (s_ptt_turn != s_applied_ptt_turn)')
        self.assertIn('muse_wakeword_core_gap(&s_core, &s_pre)', playback)

    def test_gap_ends_a_waiting_wake_and_empties_the_pre_roll(self):
        self.assertEqual(self.run_mode('gap').strip(), 'gap ok')

    def test_glue_handles_gaps_and_lost_backlogs(self):
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        task = between(glue, 'static void wake_task(void *arg)', '\n}\n')
        self.assertIn('if (s_have_seq && item.seq != s_last_seq + 1) {', task)
        # Re-enabling empties the tap, which skips numbers: not a lost chunk.
        enable = between(glue, 'static void apply_requests(void)', '} else {')
        self.assertLess(enable.index('xStreamBufferReset(s_tap);'), enable.index('s_have_seq = false;'))
        self.assertLess(task.index('muse_wakeword_core_gap(&s_core, &s_pre)'), task.index('muse_ww_ring_push(&s_pre, item.pcm'))
        take = between(glue, 'bool muse_wakeword_take_backlog(', '\n}\n')
        self.assertIn('&complete);', take)
        self.assertIn('return complete;', take)
        voice = (ROOT / 'components/muse/muse_voice.c').read_text()
        record = between(voice, 'static bool record(', 'static void go_idle')
        # Checked before go_live() opens the turn: nothing goes.
        self.assertLess(record.index('!muse_wakeword_take_backlog('), record.index('go_live();'))

    def test_note_is_continuous_at_every_lag(self):
        out = self.run_mode()
        self.assertIn('lag 24 backlog 26 seed 50', out)   # a full tap behind: nothing lost
        self.assertIn('lag 0 backlog 2 seed 50', out)

    def test_glue_posts_no_release_for_a_capture_the_button_took(self):
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        tick = between(glue, 'static void tick(const muse_wakeword_input_t *in)', '\n}\n')
        self.assertIn('bool taken_over = atomic_load(&s_pending_down);', tick)
        self.assertIn('(act & MUSE_WW_ACTION_POST_PTT_UP) && !taken_over && !post_ptt(MUSE_PTT_UP)', tick)

    def test_glue_numbers_the_tap_and_seeds_from_the_next_chunk(self):
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        tap = between(glue, 'void muse_wakeword_on_audio(', '\n}\n')
        self.assertLess(tap.index('muse_ww_history_push(&s_hist, pcm)'), tap.index('xStreamBufferSend(s_tap, item, ITEM_BYTES, 0)'))
        seed = between(glue, 'if (act & MUSE_WW_ACTION_POST_PTT_DOWN) {', 'post_ptt(MUSE_PTT_DOWN)')
        self.assertIn('atomic_store(&s_seed_end, s_last_seq + 1);', seed)
        task = between(glue, 'static void wake_task(void *arg)', '\n}\n')
        self.assertIn('s_last_seq = item.seq;', task)
        voice = (ROOT / 'components/muse/muse_voice.c').read_text()
        record = between(voice, 'static bool record(', 'static void go_idle')
        # Seed first, then the backlog (taken up front), then live reads.
        self.assertLess(record.index('take(&st, s_seed + off)'), record.index('take(&st, later[i])'))


if __name__ == '__main__':
    unittest.main()
