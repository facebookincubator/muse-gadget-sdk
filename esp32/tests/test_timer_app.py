# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Run the timer app's actual callbacks against a clock and UI adapter."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TimerApp(unittest.TestCase):
    def test_picker_deadlines_wrap_and_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'timer'
            subprocess.run([
                *shlex.split(os.environ.get('CC', 'cc')),
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                str(ROOT / 'tests/timer_app_harness.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0',
                                'UBSAN_OPTIONS': 'halt_on_error=1'})
