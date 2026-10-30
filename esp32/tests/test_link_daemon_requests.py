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

"""Daemon requests (noise_ctrl_req_*) in Link-only builds: the option is on by
default and independent of the Muse UI, Muse boards still get it, and the
production request code runs with CONFIG_MUSE_ENABLED off over a real Noise
session."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTROL = ROOT / "main/noise_control.cpp"


def kconfig_entry(text, name):
    start = text.index(f"config {name}\n")
    end = re.search(r"\n\s*(config|menuconfig|choice|endchoice|endmenu|if|endif)\b", text[start + 1:])
    return text[start:start + 1 + end.start()] if end else text[start:]


class DaemonRequests(unittest.TestCase):
    def test_option_defaults_on_without_the_muse_ui(self):
        entry = kconfig_entry((ROOT / "main/Kconfig.projbuild").read_text(), "HOMEHUB_DAEMON_REQUESTS")
        self.assertIn("default y", entry)
        self.assertNotIn("depends on", entry)
        muse = kconfig_entry((ROOT / "components/muse/Kconfig").read_text(), "MUSE_ENABLED")
        self.assertIn("select HOMEHUB_DAEMON_REQUESTS", muse)

    def test_request_code_is_gated_on_the_option_not_the_ui(self):
        source = CONTROL.read_text()
        section = source.index("// ---- Extra daemon requests (noise_ctrl_req_*)")
        self.assertEqual(source[source.index("#if ", section):].split("\n", 1)[0],
                         "#if CONFIG_HOMEHUB_DAEMON_REQUESTS")
        tunnel = (ROOT / "main/noise_tunnel.cpp").read_text()
        for text, marker in ((source, "            if (!req_pump_tx("),
                             (source, 'extern "C" int64_t noise_ctrl_req_open('),
                             (tunnel, 'extern "C" bool noise_tx_has_dma_headroom_reclaiming(')):
            at = text.index(marker)
            guard = text.rindex("#if ", 0, at)
            line = text[guard:text.index("\n", guard)]
            self.assertEqual(line, "#if CONFIG_HOMEHUB_DAEMON_REQUESTS", marker)

    def test_requests_run_with_muse_disabled(self):
        source = CONTROL.read_text()
        start = source.index("#if CONFIG_HOMEHUB_DAEMON_REQUESTS\n", source.index("// ---- Extra daemon requests"))
        end = source.index("#endif  // CONFIG_HOMEHUB_DAEMON_REQUESTS", start) + len("#endif  // CONFIG_HOMEHUB_DAEMON_REQUESTS")
        api_start = source.index('#if CONFIG_HOMEHUB_DAEMON_REQUESTS\nextern "C" int64_t noise_ctrl_req_open(')
        api_end = source.index("#endif  // CONFIG_HOMEHUB_DAEMON_REQUESTS", api_start) + len("#endif  // CONFIG_HOMEHUB_DAEMON_REQUESTS")
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / "daemon_requests.inc").write_text(source[start:end] + "\n")
            (out / "daemon_requests_api.inc").write_text(source[api_start:api_end] + "\n")
            core = ROOT / "components/noise_core"
            sources = [core / "src" / (name + ".cpp") for name in (
                "ClientSession", "InitiatorHandshake", "ServiceCodec", "Status", "Transport",
                "TransportFrameCodec")]
            binary = out / "requests"
            env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
                   "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
            built = subprocess.run(
                [*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DCONFIG_SPIRAM=1",
                 "-I", str(ROOT / "main"), "-I", str(ROOT / "tests"), "-I", tmp, "-I", str(core / "include"),
                 str(ROOT / "tests/link_daemon_requests_harness.cpp"), *map(str, sources), "-o", str(binary)],
                capture_output=True, text=True, env=env)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), "ok")


if __name__ == "__main__":
    unittest.main()
