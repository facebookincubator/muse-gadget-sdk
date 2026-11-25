# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Internal DMA reserve check in validate_config.cmake: the build refuses
ESP-IDF's reserve and the firmware's on together, and names both fixes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DmaReserveConfigTest(unittest.TestCase):
    def check(self):
        source = (ROOT / "cmake" / "validate_config.cmake").read_text()
        start = source.index("# One internal DMA reserve")
        return source[start:source.index("endif()", start) + len("endif()")]

    def configure(self, cmake, directory, idf_reserve, firmware_kb):
        script = Path(directory) / "reserve.cmake"
        lines = ["set(GADGET_CONFIG_REGEN_HINT regenerate)"]
        if idf_reserve is not None:
            lines.append(f"set(CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL {idf_reserve})")
        if firmware_kb is not None:
            lines.append(f"set(CONFIG_HOMEHUB_DMA_RESERVE_KB {firmware_kb})")
        script.write_text("\n".join(lines) + "\n" + self.check() + "\n")
        return subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)

    def test_refuses_both_reserves_on(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("cmake is required")
        # (ESP-IDF's reserve in bytes, the firmware's in KB, accepted); None = option not offered
        cases = [
            (65536, 64, False),
            (0, 64, True),
            (65536, 0, True),
            (65536, None, True),
            (None, 64, True),
            (None, None, True),
        ]
        with tempfile.TemporaryDirectory() as directory:
            for idf_reserve, firmware_kb, accepted in cases:
                with self.subTest(idf_reserve=idf_reserve, firmware_kb=firmware_kb):
                    result = self.configure(cmake, directory, idf_reserve, firmware_kb)
                    self.assertEqual(result.returncode == 0, accepted, result.stderr)
                    if not accepted:
                        message = " ".join(result.stderr.split())
                        self.assertIn("CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=0", message)
                        self.assertIn("CONFIG_HOMEHUB_DMA_RESERVE_KB=0", message)


if __name__ == "__main__":
    unittest.main()
