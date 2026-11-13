# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Real bounded option model and wire adapter under ASan/UBSan."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MuseOptions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'options'
        json = Path(os.environ.get('CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'))
        muse = ROOT / 'components/muse'
        flags = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-g', '-O1',
                   '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                   '-fno-omit-frame-pointer', '-pthread', '-include', str(ROOT / 'tests/host_compat.h'), '-I', str(json),
                   '-I', str(muse), '-I', str(ROOT / 'simulator/compat')]
        json_object = Path(cls.tmp.name) / 'cjson.o'
        result = subprocess.run([*flags, '-Wno-deprecated-declarations', '-c', str(json / 'cJSON.c'),
                                 '-o', str(json_object)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        command = [*flags,
                   str(ROOT / 'tests/muse_options_harness.c'),
                   str(muse / 'muse_options.c'), str(muse / 'muse_options_json.c'),
                   str(json_object), '-lm', '-o', str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_wire_metadata_and_selection_are_not_duplicated(self):
        self.run_case(0)

    def test_invalid_and_oversized_widgets_are_not_actionable(self):
        for mode in range(1, 9):
            with self.subTest(mode=mode):
                self.run_case(mode)

    def test_old_commands_and_callbacks_cannot_change_a_new_question(self):
        self.run_case(9)

    def test_disconnect_never_automatically_retries_a_choice(self):
        self.run_case(10)

    def test_phone_selection_wins_and_state_conflict_does_not_resend(self):
        self.run_case(11)

    def test_json_encoded_data_and_state(self):
        self.run_case(12)

    def test_phone_state_event_and_old_versions(self):
        self.run_case(13)

    def test_late_nested_question_and_empty_title_preserve_the_choice(self):
        self.run_case(14)

    def test_question_sources_skip_empty_placeholders_and_clip_whole_utf8(self):
        self.run_case(15)

    def test_canonical_transcript_question_excludes_user_and_nontext_blocks(self):
        self.run_case(16)

    def run_case(self, mode):
        result = subprocess.run([str(self.binary), str(mode)], capture_output=True, text=True,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1',
                                     'UBSAN_OPTIONS': 'halt_on_error=1'}, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
