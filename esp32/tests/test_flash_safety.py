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
"""Flash safety in tools/muse/board.sh's esptool flash.

The root cause (on a gadget): a piped `| tail -1` hid a failed USB write and
the script booted a half-written image. board.sh's flash must not reset the
chip unless esptool exited 0 and every region came back verified, and then
expects a boot within 60 s. Runs the flash tail of board.sh with a fake
`python` (esptool) on PATH.
"""

from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOARD_SH = ROOT / "tools" / "muse" / "board.sh"

FAKE = r"""#!/bin/sh
# A fake python: records each esptool call; write-flash prints what the
# scenario says. The boot check (python3 -) isn't this.
echo "$*" >> "$CALLS"
case "$*" in
  *write-flash*) cat "$WRITE_OUT"; exit "$WRITE_RC" ;;
  *no-reset*read-mac*) exit "${RESET1_RC:-0}" ;;
  *read-mac*) exit "${RESET2_RC:-0}" ;;
esac
exit 0
"""


class FlashSafety(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        d = Path(self.tmp.name)
        self.d = d
        (d / "bin").mkdir()
        fake = d / "bin" / "python"
        fake.write_text(FAKE)
        fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
        # python3 (the boot check): succeed at once.
        py3 = d / "bin" / "python3"
        py3.write_text("#!/bin/sh\ncat > /dev/null; echo booted; exit \"${BOOT_RC:-0}\"\n")
        py3.chmod(py3.stat().st_mode | stat.S_IEXEC)
        (d / "build").mkdir()
        (d / "build" / "flash_args").write_text(
            "--flash-mode dio --flash-size 16MB --flash-freq 80m\n"
            "0x0 bootloader/bootloader.bin\n0x10000 partition_table/partition-table.bin\n"
            "0x1d000 ota_data_initial.bin\n0x20000 muse-gadget.bin\n")
        src = BOARD_SH.read_text()
        tail = src[src.index("# Flash safety: "):]
        self.script = d / "flash_tail.sh"
        self.script.write_text("set -uo pipefail\n"
                               "B=%s; log=%s/muse_build_x.log; board=x; target=esp32s3; port=/dev/null; baud=460800\n"
                               "esptool='-m esptool'\n" % (d / "build", d) + tail)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_flash(self, out: str, rc: int, boot_rc: int = 0, reset1_rc: int = 0,
                  reset2_rc: int = 0) -> tuple[int, list[str], str]:
        (self.d / "out.txt").write_text(out)
        env = dict(os.environ, PATH="%s:%s" % (self.d / "bin", os.environ["PATH"]), CALLS=str(self.d / "calls"),
                   WRITE_OUT=str(self.d / "out.txt"), WRITE_RC=str(rc), BOOT_RC=str(boot_rc),
                   RESET1_RC=str(reset1_rc), RESET2_RC=str(reset2_rc))
        p = subprocess.run(["bash", str(self.script)], env=env, capture_output=True, text=True)
        calls = (self.d / "calls").read_text().splitlines() if (self.d / "calls").exists() else []
        return p.returncode, calls, p.stdout + p.stderr

    def test_a_failed_write_is_not_reset(self) -> None:
        rc, calls, out = self.run_flash("Writing at 0x00020000...\nA fatal error occurred: write failed\n", 2)
        self.assertEqual(rc, 1)
        self.assertEqual(len(calls), 1, calls)          # no read-mac (the reset)
        self.assertIn("FLASH FAILED", out)
        self.assertIn("NOT reset", out)

    def test_a_write_that_exits_0_with_a_region_unverified_is_not_reset(self) -> None:
        rc, calls, out = self.run_flash("Hash of data verified.\n" * 3, 0)   # 4 regions, 3 verified
        self.assertEqual(rc, 1)
        self.assertEqual(len(calls), 1, calls)
        self.assertIn("3 of 4 regions verified", out)

    def test_a_verified_write_resets_and_checks_the_boot(self) -> None:
        rc, calls, out = self.run_flash("Hash of data verified.\n" * 4, 0)
        self.assertEqual(rc, 0, out)
        self.assertEqual(len(calls), 2, calls)
        # The S3 boots the new image by the RTC watchdog: a hard reset over its
        # USB-Serial-JTAG (or the Watcher's CH342) can land back in download mode.
        self.assertIn("--before no-reset --after watchdog-reset read-mac", calls[1])   # esp32s3 (the test's board)

    def test_a_reset_that_finds_no_rom_tries_once_more_through_the_lines(self) -> None:
        # The Watcher's CH342: "No serial data received" after "Staying in bootloader".
        ok = "Hash of data verified.\n" * 4
        rc, calls, out = self.run_flash(ok, 0, reset1_rc=2)
        self.assertEqual(rc, 0, out)
        self.assertEqual(len(calls), 3, calls)
        self.assertIn("--before default-reset --after watchdog-reset read-mac", calls[2])
        rc, calls, out = self.run_flash(ok, 0, reset1_rc=2, reset2_rc=2)
        self.assertEqual(rc, 1)
        self.assertIn("reset it by hand", out)

    def test_no_boot_within_60_s_is_reported(self) -> None:
        rc, calls, out = self.run_flash("Hash of data verified.\n" * 4, 0, boot_rc=1)
        self.assertEqual(rc, 1)
        self.assertIn("NO BOOT within 60 s", out)


if __name__ == "__main__":
    unittest.main()
