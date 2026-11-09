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

"""components/muse/boards/mosaico_camera.c (mosaico_camera_harness.c, against
a scripted slot EEPROM, esp_video device, PPA and JPEG encoder): the slot's
pins move only for a Camera Module, every capture tears esp_video down and
puts the sensor to sleep, the flash fires only for a dark frame, and the JPEG
is the frame turned upright."""

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
           "sys/ioctl.h", "sys/mman.h", "driver/gpio.h", "driver/i2c_master.h", "driver/jpeg_encode.h",
           "driver/ppa.h", "freertos/FreeRTOS.h", "freertos/task.h", "linux/videodev2.h",
           "mbedtls/base64.h", "esp_cam_sensor_types.h")


class MosaicoCameraTest(unittest.TestCase):
    def test_slot_flash_teardown_and_upright(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            for name in HEADERS:
                (out / name).parent.mkdir(parents=True, exist_ok=True)
                (out / name).write_text('#include "mosaico_camera_fakes.h"\n')
            binary = out / "mosaico_camera_harness"
            proc = subprocess.run([*cc, "-std=gnu11", "-Wall", "-Wextra", "-Wno-sign-compare", "-Werror",
                                   "-I", str(out), "-I", str(TESTS), "-I", str(BOARDS),
                                   "-I", str(ROOT / "components" / "camera"),
                                   str(TESTS / "mosaico_camera_harness.c"), str(BOARDS / "mosaico_camera.c"),
                                   str(ROOT / "components" / "camera" / "camera.c"), "-o", str(binary)],
                                  text=True, capture_output=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            proc = subprocess.run([str(binary)], capture_output=True)
            self.assertEqual((proc.returncode, proc.stdout), (0, b"ok\n"), proc.stderr.decode())


if __name__ == "__main__":
    unittest.main()
