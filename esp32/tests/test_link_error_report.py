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

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]

# The link.error_report example every implementation and the VM test against.
DOCUMENTED_EXAMPLE = {
    "method": "link.error_report",
    "report": {
        "schema_version": 1,
        "kind": "crash",
        "reason": "panic",
        "consecutive_count": 1,
        "version": "1.0.9",
        "uptime_seconds": 5321,
        "fault": {"address": "0x42012345", "cause": "Load access fault"},
    },
}


class LinkErrorReportHarnessTest(unittest.TestCase):
    def test_crash_summary_across_boots(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        with tempfile.TemporaryDirectory() as tmpdir:
            binary = Path(tmpdir) / "link_error_report_harness"
            compiled = subprocess.run(
                [
                    *cc, "-std=c11", "-D_GNU_SOURCE", "-DLINK_FAKE_CUSTOM_TIMER=1",
                    "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(ROOT / "tests" / "link_fakes"),
                    "-I", str(ROOT / "main"),
                    str(ROOT / "tests" / "link_error_report_harness.c"),
                    str(ROOT / "main" / "error_report.c"),
                    str(ROOT / "tests" / "link_fakes" / "cJSON.c"),
                    "-o", str(binary),
                ],
                text=True, capture_output=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
                   "UBSAN_OPTIONS": "halt_on_error=1"}
            ran = subprocess.run([str(binary)], text=True, capture_output=True, env=env)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

        reports: dict[str, list[dict]] = {}
        for line in ran.stdout.splitlines():
            label, _, payload = line.partition("=")
            self.assertLess(len(payload), 512)
            message = json.loads(payload)
            self.assertEqual(set(message), {"method", "report"})
            self.assertEqual(message["method"], "link.error_report")
            reports.setdefault(label, []).append(message["report"])

        # The crashed version survives a rollback to 1.0.8, uptime rounds
        # down, and the first of two nested panics is the one reported.
        self.assertEqual(
            {"method": "link.error_report", "report": reports["panic"][0]},
            DOCUMENTED_EXAMPLE,
        )
        self.assertEqual(
            reports["after_panic"],
            [{"schema_version": 1, "kind": "crash", "reason": "watchdog",
              "consecutive_count": 2, "version": "1.0.8"}],
        )
        self.assertEqual(
            reports["no_record"],
            [{"schema_version": 1, "kind": "crash", "reason": "brownout",
              "consecutive_count": 1}],
        )
        self.assertEqual(
            [(r["reason"], r["consecutive_count"]) for r in reports["in_a_row"]],
            [("panic", 1), ("interrupt_watchdog", 2), ("task_watchdog", 3),
             ("watchdog", 4), ("brownout", 5), ("efuse", 6),
             ("power_glitch", 7), ("cpu_lockup", 8)],
        )
        self.assertEqual(reports["after_normal_reset"][0]["consecutive_count"], 1)

        (abort,) = reports["abort"]
        self.assertEqual(abort["uptime_seconds"], 7)
        self.assertEqual(set(abort["fault"]), {"cause", "detail"})
        self.assertEqual(abort["fault"]["cause"], "abort")
        self.assertEqual(abort["fault"]["detail"], 'assert "a"    ' + "x" * 81)


if __name__ == "__main__":
    unittest.main()
