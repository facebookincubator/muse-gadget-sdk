# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Exercise the actual asynchronous response owner under ASan/UBSan."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
class AvatarRequests(unittest.TestCase):
    def test_late_callbacks(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe=str(Path(tmp)/'requests')
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT/'main'),
                            str(ROOT/'main/avatar_request.c'),str(ROOT/'tests/avatar_request_harness.c'),'-o',exe],check=True)
            subprocess.run([exe],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1:detect_stack_use_after_return=1'})
