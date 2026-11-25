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
import re
import shlex
import shutil
import subprocess
import sys
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
        "build_sha": "a1b2c3d4e5f60718",
        "uptime_seconds": 5321,
        "fault": {"address": "0x42012345", "return_address": "0x4200a1b2",
                  "cause": "Load access fault"},
    },
}


# The section the harness keeps across its simulated resets, as RTC memory
# does on the chip (see link_error_report_harness.c).
RETAINED_SECTION = (
    "__DATA,__err_retained" if sys.platform == "darwin" else "error_report_retained"
)


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
                    f'-DRETAINED_ATTR=__attribute__((section("{RETAINED_SECTION}")))',
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

        # The crashed version and binary survive a rollback to 1.0.8, uptime
        # rounds down, and the first of two nested panics is the one reported.
        self.assertEqual(
            {"method": "link.error_report", "report": reports["panic"][0]},
            DOCUMENTED_EXAMPLE,
        )
        self.assertEqual(
            reports["after_panic"],
            [{"schema_version": 1, "kind": "crash", "reason": "watchdog",
              "consecutive_count": 2, "version": "1.0.8",
              "build_sha": "0fb2c3d4e5f60718"}],
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

        # A panic before error_report_init() in its boot isn't pinned on the
        # previous boot's record: the reset is reported, with nothing about a
        # build or fault the record can't vouch for.
        for label in ("early_abort", "startup_panic"):
            self.assertEqual(
                reports[label],
                [{"schema_version": 1, "kind": "crash", "reason": "panic",
                  "consecutive_count": 1}],
                label,
            )
        # It still counts toward a run of crashes, without the details of the
        # earlier crash it replaces.
        self.assertEqual(
            [(r["consecutive_count"], set(r)) for r in
             reports["crash_then_early"] + reports["crash_then_two_early"]],
            [(n, {"schema_version", "kind", "reason", "consecutive_count"})
             for n in (2, 3)],
        )

        # Restarts before registering (pairing, a deep-sleep wake) keep the
        # report until it is sent, and still end the run of crashes.
        self.assertEqual(
            reports["unsent"],
            [{"schema_version": 1, "kind": "crash", "reason": "panic",
              "consecutive_count": 1, "version": "1.0.9",
              "build_sha": "123456789abcdef0", "uptime_seconds": 42,
              "fault": {"address": "0x42002000", "cause": "Store access fault"}}],
        )
        (replaced,) = reports["replaced"]
        self.assertEqual(replaced["consecutive_count"], 1)
        self.assertEqual(replaced["fault"],
                         {"address": "0x42004000", "cause": "Illegal instruction"})


class LinkErrorReportResetTest(unittest.TestCase):
    def test_factory_reset_drops_a_waiting_report(self) -> None:
        source = (ROOT / "main" / "app.c").read_text()
        start = source.index("static bool reset_setup_with_gate_held(")
        body = source[start : source.index("\n}\n", start)]
        # After the wipe, which stops the control task, and only for a reset
        # that forgets everything: one that keeps something (an unpair keeps
        # Wi-Fi) leaves the device with its owner.
        drop = re.search(
            r"#if CONFIG_HOMEHUB_ERROR_REPORT\n(?:\s*//.*\n)*"
            r"\s*if \(scope == SETUP_RESET_(?!KEEP_)[A-Z_]+\) error_report_discard\(\);\n"
            r"#endif\n",
            body,
        )
        self.assertIsNotNone(drop, body)
        self.assertLess(body.index("setup_wipe_to_clean(scope)"), drop.start())
        self.assertLess(drop.end(), body.index("finish_setup_reset_with_gate_held()"))


if __name__ == "__main__":
    unittest.main()
