#!/usr/bin/env python3
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

"""components/muse/boards/p4_csi_camera.c (p4_csi_camera_harness.c, against a
scripted esp_video device and JPEG encoder): every capture, a missing sensor
included, tears esp_video down, and the JPEG quality steps down to fit."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOARDS = ROOT / "components" / "muse" / "boards"
TESTS = ROOT / "tests"

# Every header the driver includes, each standing for the one fakes header.
HEADERS = ("esp_err.h", "esp_check.h", "esp_heap_caps.h", "esp_log.h", "esp_timer.h",
           "esp_video_device.h", "esp_video_init.h", "esp_video_ioctl.h", "fcntl.h", "unistd.h",
           "sys/ioctl.h", "sys/mman.h", "driver/i2c_master.h", "driver/jpeg_encode.h",
           "linux/videodev2.h", "mbedtls/base64.h")


class P4CsiCameraTest(unittest.TestCase):
    def test_captures_tear_down_and_fit(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            for name in HEADERS:
                (out / name).parent.mkdir(parents=True, exist_ok=True)
                (out / name).write_text('#include "p4_csi_camera_fakes.h"\n')
            binary = out / "p4_csi_camera_harness"
            proc = subprocess.run([*cc, "-std=gnu11", "-Wall", "-Wextra", "-Wno-sign-compare", "-Werror",
                                   "-I", str(out), "-I", str(TESTS), "-I", str(BOARDS),
                                   "-I", str(ROOT / "components" / "camera"),
                                   str(TESTS / "p4_csi_camera_harness.c"), str(BOARDS / "p4_csi_camera.c"),
                                   str(ROOT / "components" / "camera" / "camera.c"), "-o", str(binary)],
                                  text=True, capture_output=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            proc = subprocess.run([str(binary)], capture_output=True)
            self.assertEqual((proc.returncode, proc.stdout), (0, b"ok\n"), proc.stderr.decode())


if __name__ == "__main__":
    unittest.main()
