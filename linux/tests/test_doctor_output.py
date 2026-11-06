# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import subprocess
import sys

import pytest

from musegadget import doctor


@pytest.mark.parametrize("check,output", [
    (doctor.check_service, b"active\n"),
    (doctor.check_bluetooth, b"Controller name: \xff\n\tPowered: yes\n"),
])
def test_diagnostic_tolerates_undecodable_tool_output(monkeypatch, check, output):
    original = subprocess.run

    def local_output(args, **options):
        # Exercise real subprocess text decoding without systemd or Bluetooth.
        script = "import sys; sys.stdout.buffer.write(%r); sys.stderr.buffer.write(b'\\xff')" % output
        return original([sys.executable, "-c", script], **options)

    monkeypatch.setattr(doctor.shutil, "which", lambda name: sys.executable)
    monkeypatch.setattr(doctor.subprocess, "run", local_output)
    assert check()["status"] == "OK"
