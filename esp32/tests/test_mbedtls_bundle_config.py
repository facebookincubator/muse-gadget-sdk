# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Certificate bundle checks in validate_config.cmake: the build refuses
cross-signed verification on ESP-IDF that leaks, and warns about a stale full
bundle."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CertificateBundleConfigTest(unittest.TestCase):
    def guard(self):
        source = (ROOT / "cmake" / "validate_config.cmake").read_text()
        start = source.index("# Cross-signed bundle verification")
        return source[start:source.index("endif()", start) + len("endif()")]

    def accepts(self, cmake, directory, enabled, version):
        major, minor, patch = version.split(".")
        script = Path(directory) / "guard.cmake"
        script.write_text(
            f"set(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY {'y' if enabled else ''})\n"
            f"set(IDF_VERSION_MAJOR {major})\nset(IDF_VERSION_MINOR {minor})\n"
            f"set(IDF_VERSION_PATCH {patch})\n" + self.guard() + "\n")
        result = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
        if result.returncode:
            self.assertIn("needs ESP-IDF v6.0.3 or later", " ".join(result.stderr.split()))
        return result.returncode == 0

    def test_guard_tracks_the_fixed_release(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("cmake is required")
        cases = [
            (True, "6.0.1", False),
            (True, "6.0.2", False),
            (True, "5.5.9", False),
            (True, "6.0.3", True),
            (True, "6.1.0", True),
            (True, "6.2.0", True),
            (False, "6.0.1", True),
        ]
        with tempfile.TemporaryDirectory() as directory:
            for enabled, version, expected in cases:
                with self.subTest(enabled=enabled, version=version):
                    self.assertEqual(self.accepts(cmake, directory, enabled, version), expected)

    def test_stale_full_bundle_warns(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("cmake is required")
        source = (ROOT / "cmake" / "validate_config.cmake").read_text()
        start = source.index("# Stale bundle choice")
        block = source[start:source.index("endif()", start) + len("endif()")]
        defaults = (ROOT / "sdkconfig.defaults").read_text().splitlines()
        self.assertIn("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y", defaults)
        self.assertIn("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y", defaults)
        with tempfile.TemporaryDirectory() as directory:
            for full in (True, False):
                with self.subTest(full=full):
                    script = Path(directory) / "stale.cmake"
                    script.write_text(
                        f"set(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL {'y' if full else ''})\n"
                        "set(GADGET_CONFIG_REGEN_HINT regenerate)\n" + block + "\n")
                    result = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual("full root CA bundle" in " ".join(result.stderr.split()), full)


if __name__ == "__main__":
    unittest.main()
