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

from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TUNNEL_SOURCE = ROOT / "main" / "noise_tunnel.cpp"


def _cc_command() -> list[str]:
    command = shlex.split(os.environ.get("CC", "cc"))
    if not command or shutil.which(command[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return command


def _sanitizer_flags(cc: list[str], tmp: Path) -> list[str]:
    binary = tmp / "asan_probe"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    compiled = subprocess.run(
        [*cc, "-x", "c", "-std=c11", *flags, "-o", str(binary), "-"],
        input=b"int main(void) { return 0; }\n",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if compiled.returncode != 0:
        return []
    ran = subprocess.run(
        [str(binary)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1"},
    )
    return flags if ran.returncode == 0 else []


class LinkBugReportHarnessTest(unittest.TestCase):
    def test_tunnel_snapshot_state_uses_atomics(self) -> None:
        source = TUNNEL_SOURCE.read_text()
        self.assertIn("static std::atomic_bool s_connected{false};", source)
        self.assertIn(
            "s_connected.load(std::memory_order_acquire)", source
        )
        self.assertIn(
            "s_connected.store(true, std::memory_order_release)", source
        )
        self.assertIn(
            "s_connected.store(false, std::memory_order_relaxed)", source
        )
        self.assertNotIn("volatile bool s_connected", source)
        # rx_idle_seconds reads a 32-bit copy: 64-bit reads tear on the C5.
        self.assertIn("static std::atomic<uint32_t> s_last_rx_s{UINT32_MAX};", source)
        self.assertIn("s_last_rx_s.load(", source)

    def test_bug_report_harness_and_opaque_json_contract(self) -> None:
        cc = _cc_command()
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            binary = tmp / "link_bug_report_harness"
            sanitizer_flags = _sanitizer_flags(cc, tmp)
            command = [
                *cc,
                "-std=c11",
                "-D_GNU_SOURCE",
                "-DLINK_FAKE_CUSTOM_TASKS=1",
                "-DLINK_FAKE_CUSTOM_HEAP_CAPS=1",
                "-DCONFIG_SPIRAM=1",
                '-DCONFIG_GADGET_PRODUCT_NAME="Muse Gadget"',
                "-Wall",
                "-Wextra",
                "-Werror",
                "-g",
                "-O1",
                *sanitizer_flags,
                "-I",
                str(ROOT / "tests" / "link_fakes"),
                "-I",
                str(ROOT / "main"),
                str(ROOT / "tests" / "link_bug_report_harness.c"),
                str(ROOT / "main" / "bug_report.c"),
                str(ROOT / "tests" / "link_fakes" / "stack_monitor.c"),
                str(ROOT / "tests" / "link_fakes" / "cJSON.c"),
                "-o",
                str(binary),
            ]
            compiled = subprocess.run(
                command,
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                compiled.returncode,
                0,
                msg=compiled.stdout + compiled.stderr,
            )

            env = os.environ.copy()
            if sanitizer_flags:
                env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1"
            ran = subprocess.run(
                [str(binary)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
            )
            self.assertEqual(ran.returncode, 0, msg=ran.stdout + ran.stderr)

            payload_line = next(
                line
                for line in ran.stdout.splitlines()
                if line.startswith("PRIMARY_PAYLOAD_JSON=")
            )
            report_json = payload_line.removeprefix("PRIMARY_PAYLOAD_JSON=")
            report = json.loads(report_json)
            source = (ROOT / "main" / "bug_report.c").read_text()
            header = (ROOT / "main" / "bug_report.h").read_text()

            self.assertNotIn("BUG_REPORT_SIGNATURE", source)
            self.assertNotIn("BUG_REPORT_ENVELOPE", source + header)
            self.assertLessEqual(len(report_json.encode()), 32 * 1024)
            self.assertEqual(
                set(report),
                {
                    "schema_version",
                    "report_type",
                    "source_request_id",
                    "node_id",
                    "redaction_profile",
                    "device",
                    "runtime",
                    "network",
                    "ota",
                    "device_health",
                    "logs",
                },
            )
            self.assertEqual(report["report_type"], "hatch_link_bug_report")
            self.assertEqual(report["source_request_id"], "request-1")
            self.assertEqual(report["redaction_profile"], "hatch-link-support-v1")
            self.assertEqual(report["device"]["model_id"], "hatch-link")
            self.assertEqual(report["network"], {"wifi_connected": True, "rssi_dbm": -47})
            self.assertEqual(
                report["device_health"]["connectivity_events"],
                ["12s ago tunnel_down: keepalive timeout (dma_free=1 dma_largest=1)"],
            )
            self.assertNotIn("network_ssid", report["device_health"])
            self.assertEqual(report["logs"]["format"], "hatch-link-application-log-v1")
            self.assertEqual(report["logs"]["sensitivity"], "restricted")
            self.assertTrue(report["logs"]["contains_pii"])
            self.assertEqual(report["logs"]["overwritten_lines"], 0)
            self.assertEqual(report["logs"]["dropped_lines"], 0)
            self.assertEqual(report["logs"]["truncated_lines"], 0)
            self.assertEqual(
                base64.b64decode(report["logs"]["tail_base64"], validate=True),
                b"I (100) link.app: connected ssid=Office\n"
                b"W (200) link.noise_ctrl: reconnecting VM vm-123\n",
            )


if __name__ == "__main__":
    unittest.main()
