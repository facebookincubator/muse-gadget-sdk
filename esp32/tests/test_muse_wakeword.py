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
        # No ET_ENABLE to select: the registry component links nothing
        # until the model and kernel bundle are declared in CMake.
        self.assertNotIn('ET_ENABLE', kconfig)
        self.assertIn('ww_runner_et.cpp', cmake)
        self.assertIn('models/hey_muse.pte', cmake)
        self.assertIn('executorch_add_model', cmake)
        self.assertIn('make_kernel_bundle.py', cmake)
        # The bundle is generated at build time; no committed bundle dir.
        self.assertNotIn('"${CMAKE_CURRENT_SOURCE_DIR}/et_kernels"', cmake)
        self.assertFalse((WW / 'et_kernels').exists())
        self.assertTrue((WW / 'models/hey_muse.pte').is_file())

    def test_runner_copies_unaligned_embedded_pte(self):
        # et_model_create requires 16-byte-aligned program
        # data, but EMBED_FILES alignment is layout luck: after the
        # Oct 2026 rebase the embedded .pte landed at a 4-aligned
        # address, load failed (0x12), wake init died, and the
        # Settings toggle reverted instantly. The runner must copy a
        # misaligned .pte into owned aligned storage instead.
        src = (WW / 'ww_runner_et.cpp').read_text()
        self.assertIn('heap_caps_aligned_alloc', src)
        self.assertIn('pte_copy', src)
        self.assertIn('free(runner->pte_copy)', src)
        # Zero-copy normal path: the embed itself is 16-aligned in
        # flash (custom asm), so the copy above is only a fallback.
        cmake = (WW / 'CMakeLists.txt').read_text()
        self.assertIn('hey_muse_pte.S.in', cmake)
        self.assertNotIn('EMBED_FILES ${', cmake)
        asm = (WW / 'hey_muse_pte.S.in').read_text()
        self.assertIn('.balign 16', asm)
        self.assertIn('_binary_hey_muse_pte_start', asm)

    def test_runner_gives_kernels_a_temp_arena(self):
        # et_model leaves temp_arena_size at 0 by default, which makes
        # Method fall back to PAL malloc for kernel scratch during
        # execute. The always-on runner opts into a bounded pool in the
        # same default location class as its planned buffers instead.
        runner = (WW / 'ww_runner_et.cpp').read_text()
        self.assertRegex(runner, r'kTempArenaBytes\s*=\s*[1-9]')
        self.assertIn('model_cfg.temp_arena_size = kTempArenaBytes', runner)
        self.assertIn('model_cfg.temp_location = ET_MEM_DEFAULT', runner)

    def test_model_is_declared_and_bundle_is_build_time(self):
        # The embedded .pte only runs on firmware whose build-time kernel
        # bundle registers its ops. The bundle is generated at configure
        # time from the declared model, so the tree carries the model,
        # its recorded ops, and the declaration — and no committed
        # bundle that could go stale when the component is updated.
        sidecar = json.loads((WW / 'models/hey_muse.json').read_text())
        pte = WW / 'models/hey_muse.pte'
        self.assertEqual(sidecar['pte_size'], pte.stat().st_size)
        self.assertEqual(
            sidecar['pte_sha256'], hashlib.sha256(pte.read_bytes()).hexdigest()
        )
        self.assertTrue(sidecar['model_ops'])
        cmake = (WW / 'CMakeLists.txt').read_text()
        self.assertIn(
            'executorch_add_model(PTE "${CMAKE_CURRENT_SOURCE_DIR}/models/hey_muse.pte")',
            cmake,
        )
        self.assertFalse((WW / 'et_kernels').exists())
        helper = (WW / 'tools/make_kernel_bundle.py').read_text()
        self.assertIn('module._check_version = _unchecked_version', helper)

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
        self.assertIn('if (!s_up_owed && post_ptt(MUSE_PTT_DOWN))', down)
        self.assertIn('muse_wakeword_core_abort(&s_core);', down)
        self.assertIn('muse_voice_seed_preroll(NULL, 0);', down)

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
                      'free(pre_storage)', 'free(s_snap)'):
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
        board = (ROOT / 'tools/muse/board.sh').read_text()
        self.assertIn('B=$B-wakeword; defaults="$defaults;devices/sdkconfig.muse-wakeword"', board)

    def test_listening_needs_a_working_runner(self):
        # A runner that failed to load or run hears nothing, so the
        # status and the indicator must not say listening.
        glue = (ROOT / 'components/muse/muse_wakeword_glue.c').read_text()
        listening = between(glue, 'bool muse_wakeword_listening(void)\n{', '\n}')
        self.assertIn('s_runner_ok', listening)
        snapshot = between(glue, 'static void snapshot_status(void)', '\n}')
        self.assertIn('ww_runner_ok(s_runner)', snapshot)

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


class WakeWordFrontendDetector(unittest.TestCase):
    """The SDK-owned LFBE frontend and posterior detector used by the
    ExecuTorch runner. Inference itself is covered on device; these pin
    framing, the frozen 512/480/320 geometry, silence values, and the
    detector warm-up/smoothing/cooldown state machine on the host."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'ww_frontend_harness'
        harness = Path(cls.tmp.name) / 'harness.c'
        harness.write_text(r'''
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ww_detector.h"
#include "ww_frontend.h"

static float g_frames[16][WW_FRONTEND_N_MELS];
static size_t g_count;
static void on_frame(void *ctx, const float frame[WW_FRONTEND_N_MELS]) {
    (void)ctx;
    if (g_count < 16) memcpy(g_frames[g_count], frame, sizeof(g_frames[0]));
    g_count++;
}

int main(int argc, char **argv) {
    ww_frontend_t *fe = ww_frontend_create();
    if (!fe) return 2;
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "silence")) {
        int16_t zeros[512] = {0};
        size_t n = ww_frontend_push(fe, zeros, 512, on_frame, NULL);
        float worst = 0;
        for (int m = 0; m < WW_FRONTEND_N_MELS; m++) {
            float want = (logf(WW_FRONTEND_LOG_EPS) - kWwFrontendNormMean[m]) /
                kWwFrontendNormStd[m];
            float d = fabsf(g_frames[0][m] - want);
            if (d > worst) worst = d;
        }
        printf("frames %zu worst %.9g\n", n, worst);
        return n == 1 && g_count == 1 && worst < 1e-5f ? 0 : 1;
    }
    if (!strcmp(mode, "sine") || !strcmp(mode, "split")) {
        enum { N = 3219 };
        static int16_t pcm[N];
        for (int i = 0; i < N; i++)
            pcm[i] = (int16_t)(12000.0f * sinf(2.0f * 3.1415926535f * 1000.0f * (float)i / 16000.0f));
        g_count = 0;
        ww_frontend_push(fe, pcm, N, on_frame, NULL);
        if (!strcmp(mode, "sine")) {
            int best = 0;
            for (int m = 1; m < WW_FRONTEND_N_MELS; m++)
                if (g_frames[0][m] > g_frames[0][best]) best = m;
            printf("frames %zu argmax %d peak %.9g\n", g_count, best, g_frames[0][best]);
            return g_count == 9 ? 0 : 1;
        }
        static float whole[16][WW_FRONTEND_N_MELS];
        size_t whole_count = g_count;
        memcpy(whole, g_frames, sizeof(whole));
        ww_frontend_reset(fe);
        g_count = 0;
        const size_t chunks[] = {1, 319, 17, 511, 320, 999, 1052};
        size_t off = 0;
        for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
            ww_frontend_push(fe, pcm + off, chunks[i], on_frame, NULL);
            off += chunks[i];
        }
        printf("frames %zu equal %d\n", g_count,
               g_count == whole_count && !memcmp(whole, g_frames, sizeof(whole)));
        return off == N && g_count == whole_count &&
            !memcmp(whole, g_frames, sizeof(whole)) ? 0 : 1;
    }
    if (!strcmp(mode, "detector")) {
        ww_detector_t det;
        ww_detector_init(&det, WW_SENSITIVITY_DEFAULT);
        for (int i = 0; i < WW_DETECTOR_WARMUP_FRAMES; i++)
            if (ww_detector_push(&det, 1.0f)) return 1;
        if (!ww_detector_push(&det, 1.0f)) return 1; /* valid frame 30 */
        for (int i = 0; i < WW_DETECTOR_COOLDOWN_FRAMES; i++)
            if (ww_detector_push(&det, 1.0f)) return 1;
        if (!ww_detector_push(&det, 1.0f)) return 1;
        ww_detector_reset(&det);
        for (int i = 0; i < WW_DETECTOR_WARMUP_FRAMES; i++)
            if (ww_detector_push(&det, 1.0f)) return 1;
        printf("detector ok\n");
        return 0;
    }
    return 2;
}
''')
        cc = shlex.split(os.environ.get('CC', 'cc'))
        result = subprocess.run(
            [*cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-I', str(WW), '-I', str(WW / 'include'), str(harness),
             str(WW / 'ww_frontend.c'), str(WW / 'ww_detector.c'), '-lm',
             '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_mode(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        return ran.stdout.decode().strip()

    def test_silence_frame_matches_log_eps_normalization(self):
        out = self.run_mode('silence').split()
        self.assertEqual(out[:2], ['frames', '1'])
        self.assertLess(float(out[-1]), 1e-5)

    def test_sine_peaks_at_the_1khz_mel(self):
        out = self.run_mode('sine').split()
        self.assertEqual(out[1], '9')
        self.assertIn(int(out[3]), (12, 13))
        self.assertGreater(float(out[5]), 2.0)

    def test_arbitrary_chunking_is_bit_identical(self):
        self.assertEqual(self.run_mode('split'), 'frames 9 equal 1')

    def test_detector_warmup_fire_and_cooldown(self):
        self.assertEqual(self.run_mode('detector'), 'detector ok')


ET_FAKES = {
    'esp_heap_caps.h': r"""
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MALLOC_CAP_DEFAULT (1u << 0)
#define MALLOC_CAP_INTERNAL (1u << 1)
#define MALLOC_CAP_SPIRAM (1u << 2)
#define MALLOC_CAP_8BIT (1u << 3)
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
#ifdef __cplusplus
}
#endif
""",
    'esp_log.h': r"""
#pragma once
static inline void fake_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI(tag, ...) fake_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) fake_log(tag, __VA_ARGS__)
""",
    'esp_timer.h': r"""
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
""",
    'freertos/FreeRTOS.h': '#pragma once\n',
    'freertos/task.h': r"""
#pragma once
static inline unsigned uxTaskGetStackHighWaterMark(void *task) { (void)task; return 0; }
""",
    'et_model.h': r"""
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ET_MEM_DEFAULT = 0, ET_MEM_INTERNAL, ET_MEM_PSRAM } et_mem_location_t;
typedef struct {
    const uint8_t *pte; size_t pte_size;
    size_t arena_size; et_mem_location_t arena_location; void *arena_buffer;
    size_t temp_arena_size; et_mem_location_t temp_location; void *temp_buffer;
    et_mem_location_t planned_location; void *planned_buffer; size_t planned_buffer_size;
    bool verify;
} et_model_config_t;
#define ET_MODEL_CONFIG_DEFAULT(p, n) {(p), (n), 0, ET_MEM_DEFAULT, NULL, 0, ET_MEM_DEFAULT, NULL, ET_MEM_DEFAULT, NULL, 0, false}
typedef struct { int32_t scalar_type; size_t ndim; int32_t dims[8]; size_t nbytes; } et_tensor_info_t;
typedef struct et_model et_model_t;
et_model_t *et_model_create(const et_model_config_t *cfg);
void et_model_destroy(et_model_t *model);
bool et_model_load_method(et_model_t *model, const char *name);
bool et_model_planned_info(et_model_t *model, const char *name, size_t *n, size_t *bytes);
size_t et_model_num_inputs(et_model_t *model);
size_t et_model_num_outputs(et_model_t *model);
bool et_model_input_info(et_model_t *model, size_t index, et_tensor_info_t *out);
bool et_model_output_info(et_model_t *model, size_t index, et_tensor_info_t *out);
bool et_model_set_input(et_model_t *model, size_t index, const void *data, size_t nbytes);
bool et_model_execute(et_model_t *model);
const void *et_model_output_data(et_model_t *model, size_t index, size_t *out_nbytes);
const char *et_model_last_error(et_model_t *model);
#ifdef __cplusplus
}
#endif
""",
}

ET_HARNESS = r"""
/* Drives ww_runner_et.cpp against a fake et_model that behaves like the
 * registry component's: a method load without caller buffers allocates
 * its arenas and planned memory, and every load zeroes planned memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "et_model.h"
#include "ww_runner.h"

/* The embedded-model symbols must link; the harness passes its own model. */
const uint8_t _binary_hey_muse_pte_start[16];
const uint8_t _binary_hey_muse_pte_end[16];
__attribute__((aligned(16))) static const uint8_t g_pte[64] = {1};

static int g_allocs, g_frees, g_loads, g_fail_load, g_fail_exec;
#define PLANNED 36864

void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
    (void)caps;
    void *p = NULL;
    if (posix_memalign(&p, alignment, size)) return NULL;
    g_allocs++;
    return p;
}
void heap_caps_free(void *p) { if (p) { g_frees++; free(p); } }
size_t heap_caps_get_minimum_free_size(uint32_t c) { (void)c; return 0; }
size_t heap_caps_get_free_size(uint32_t c) { (void)c; return 0; }
size_t heap_caps_get_total_size(uint32_t c) { (void)c; return 0; }
int64_t esp_timer_get_time(void) { return 0; }

struct et_model { et_model_config_t cfg; bool loaded; void *owned[3]; };
static void drop(et_model_t *m) {
    for (int i = 0; i < 3; i++) { heap_caps_free(m->owned[i]); m->owned[i] = NULL; }
    m->loaded = false;
}
et_model_t *et_model_create(const et_model_config_t *cfg) {
    et_model_t *m = calloc(1, sizeof(*m));
    if (m) m->cfg = *cfg;
    return m;
}
void et_model_destroy(et_model_t *m) { if (m) { drop(m); free(m); } }
bool et_model_load_method(et_model_t *m, const char *name) {
    drop(m);
    g_loads++;
    if (strcmp(name, "forward") || g_fail_load) return false;
    if (!m->cfg.arena_buffer) m->owned[0] = heap_caps_aligned_alloc(16, m->cfg.arena_size, 0);
    if (!m->cfg.temp_buffer) m->owned[1] = heap_caps_aligned_alloc(16, m->cfg.temp_arena_size, 0);
    void *planned = m->cfg.planned_buffer;
    if (!planned) planned = m->owned[2] = heap_caps_aligned_alloc(16, PLANNED, 0);
    else if (m->cfg.planned_buffer_size < PLANNED) return false;
    memset(planned, 0, PLANNED);
    m->loaded = true;
    return true;
}
bool et_model_planned_info(et_model_t *m, const char *name, size_t *n, size_t *bytes) {
    if (!name && !m->loaded) return false;
    if (n) *n = 1;
    if (bytes) *bytes = PLANNED;
    return true;
}
size_t et_model_num_inputs(et_model_t *m) { (void)m; return 1; }
size_t et_model_num_outputs(et_model_t *m) { (void)m; return 1; }
bool et_model_input_info(et_model_t *m, size_t i, et_tensor_info_t *o) {
    (void)m; (void)i; memset(o, 0, sizeof(*o)); o->nbytes = 40 * sizeof(float); return true;
}
bool et_model_output_info(et_model_t *m, size_t i, et_tensor_info_t *o) {
    (void)m; (void)i; memset(o, 0, sizeof(*o)); o->nbytes = 3 * sizeof(float); return true;
}
bool et_model_set_input(et_model_t *m, size_t i, const void *d, size_t n) {
    (void)i; (void)d; return m->loaded && n == 40 * sizeof(float);
}
bool et_model_execute(et_model_t *m) { return m->loaded && !g_fail_exec; }
const void *et_model_output_data(et_model_t *m, size_t i, size_t *n) {
    static const float out[3] = {0.0f, 1.0f, 0.0f};
    (void)m; (void)i; *n = sizeof(out); return out;
}
const char *et_model_last_error(et_model_t *m) { (void)m; return "fake"; }

static int16_t g_pcm[640];

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    ww_config_t cfg = { .model = g_pte, .model_len = sizeof(g_pte), .sample_rate_hz = 16000,
                        .sensitivity = WW_SENSITIVITY_DEFAULT, .vad = WW_VAD_ENERGY };
    ww_runner_t *r = ww_runner_create(&cfg);
    if (!r || !ww_runner_ok(r)) return 2;
    if (!strcmp(mode, "reset")) {
        int allocs = g_allocs, loads = g_loads;
        for (int i = 0; i < 5; i++) ww_runner_reset(r);
        printf("allocs %d reloads %d ok %d\n", g_allocs - allocs, g_loads - loads, ww_runner_ok(r));
    } else if (!strcmp(mode, "recover")) {
        g_fail_exec = 1;
        ww_runner_feed(r, g_pcm, 640);   /* first frame: inference fails */
        int after_fail = ww_runner_ok(r);
        g_fail_exec = 0;
        g_fail_load = 1;
        ww_runner_reset(r);              /* reload fails too */
        int after_bad_reload = ww_runner_ok(r);
        g_fail_load = 0;
        ww_runner_reset(r);              /* and recovers */
        printf("failed %d bad_reload %d recovered %d\n", after_fail, after_bad_reload, ww_runner_ok(r));
    } else {
        return 2;
    }
    ww_runner_destroy(r);
    printf("balanced %d\n", g_allocs == g_frees);
    return 0;
}
"""


class WakeWordExecuTorchRunner(unittest.TestCase):
    """ww_runner_et.cpp's memory and failure handling against a fake
    et_model with the component's allocation behaviour: reset reloads
    the model into memory allocated once at create, and a runner that
    cannot detect says so."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        for name, text in ET_FAKES.items():
            (tmp / 'inc' / name).parent.mkdir(parents=True, exist_ok=True)
            (tmp / 'inc' / name).write_text(text)
        harness = tmp / 'harness.c'
        harness.write_text(ET_HARNESS)
        cls.binary = tmp / 'ww_et_harness'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        cxx = shlex.split(os.environ.get('CXX', 'c++'))
        inc = ['-I', str(tmp / 'inc'), '-I', str(WW), '-I', str(WW / 'include')]
        objs = []
        for src, tool, std in ((harness, cc, '-std=gnu11'),
                               (WW / 'ww_frontend.c', cc, '-std=gnu11'),
                               (WW / 'ww_detector.c', cc, '-std=gnu11'),
                               (WW / 'ww_runner_et.cpp', cxx, '-std=gnu++17')):
            obj = tmp / (src.name + '.o')
            result = subprocess.run(
                [*tool, std, '-Wall', '-Wextra', '-Werror', *SANITIZE, *inc,
                 '-c', str(src), '-o', str(obj)], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            objs.append(str(obj))
        result = subprocess.run([*cxx, *SANITIZE, *objs, '-lm', '-o', str(cls.binary)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_mode(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        return ran.stdout.decode().split('\n')

    def test_reset_reloads_without_allocating(self):
        # Every reset reloads (zeroing the streaming state) into the
        # memory allocated at create: nothing is freed and reallocated,
        # so a reset after a reply cannot fail on a fragmented heap.
        out = self.run_mode('reset')
        self.assertEqual(out[0], 'allocs 0 reloads 5 ok 1')
        self.assertEqual(out[1], 'balanced 1')

    def test_failed_runner_reports_not_ok_until_a_reload_works(self):
        out = self.run_mode('recover')
        self.assertEqual(out[0], 'failed 0 bad_reload 0 recovered 1')
        self.assertEqual(out[1], 'balanced 1')


if __name__ == '__main__':
    unittest.main()
