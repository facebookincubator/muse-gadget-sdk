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

from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from test_link_ota import function_source  # noqa: E402
from host_cjson import cjson_dir, cjson_source, require_cjson  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = cjson_dir()


class TunnelRelayTest(unittest.TestCase):
    def test_packet_sorting_napt_input_check_and_forward_parameters(self):
        relay = (ROOT / "main/tunnel_relay.c").read_text()
        guard = (ROOT / "main/lwip_hooks/tunnel_napt_guard.h").read_text()
        app = (ROOT / "main/app.c").read_text()
        with tempfile.TemporaryDirectory() as name:
            out = Path(name)
            (out / "napt_guard.inc").write_text(
                function_source(guard, "static inline bool tunnel_napt_input_short(") + "\n"
                + function_source(guard, "static inline bool tunnel_napt_input_inside(") + "\n"
                + function_source(guard, "static inline bool tunnel_napt_input_foreign("))
            start = relay.index("typedef enum {")
            (out / "relay_classify.inc").write_text(
                relay[start:relay.index("// Sorts one IPv4 packet", start)]
                + function_source(relay, "static relay_kind_t relay_classify(") + "\n"
                + function_source(relay, "static bool relay_sender_ok("))
            (out / "net_forward.inc").write_text(
                function_source(app, "static bool int_param(") + "\n"
                + function_source(app, "static cJSON *net_forward_command("))
            require_cjson(JSON)
            cc = shlex.split(os.environ.get("CC", "cc"))
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE",
                     "-DCJSON_NESTING_LIMIT=16", "-I", str(JSON), "-I", str(out)]
            binary = out / "relay"
            compiled = subprocess.run(
                [*cc, *flags, str(ROOT / "tests/tunnel_relay_harness.c"),
                 *cjson_source(JSON), "-lm", "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_forwards(self):
        # Everything after the includes, so the harness supplies lwIP.
        relay = (ROOT / "main/tunnel_relay.c").read_text()
        with tempfile.TemporaryDirectory() as name:
            out = Path(name)
            (out / "tunnel_relay_forward.inc").write_text(
                relay[relay.index("static const char *TAG"):
                      relay.index("bool tunnel_relay_lan_address(")])
            cc = shlex.split(os.environ.get("CC", "cc"))
            # The build defines HOMEHUB_NAPT_INPUT_GUARD where the check is
            # compiled into lwIP; without it TCP forwards are refused.
            for guard in ("1", "0"):
                binary = out / f"forward{guard}"
                compiled = subprocess.run(
                    [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE",
                     f"-DHOMEHUB_NAPT_INPUT_GUARD={guard}", *(["-Wno-unused-function"] if guard == "0" else []),
                     "-I", str(out), "-I", str(ROOT / "main"),
                     str(ROOT / "tests/tunnel_relay_forward_harness.c"), "-o", str(binary)],
                    capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                ran = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_tunnel_input_passes_the_relay_first(self):
        # Every packet from the tunnel goes through tunnel_relay_from_vm(),
        # which drops any source but the VM, before lwIP sees it.
        netif = (ROOT / "main/tunnel_netif.c").read_text()
        rx = function_source(netif, "static void on_tunnel_packet(")
        self.assertLess(rx.index("tunnel_relay_from_vm(&data[off], plen)"),
                        rx.index("tcpip_input(p, &s_netif)"))
        start = function_source(netif, "bool tunnel_netif_start(")
        self.assertLess(start.index("tunnel_relay_init(&s_netif, gw)"),
                        start.index("noise_tunnel_set_packet_cb(on_tunnel_packet)"))
        # Relay ports and forwards belong to one tunnel stream: a reopen clears
        # them first.
        self.assertIn("noise_tunnel_set_open_cb(tunnel_relay_reset)", start)
        link = function_source(netif, "void tunnel_netif_set_link(")
        self.assertIn("tunnel_relay_reset()", link[link.index("} else {"):])


if __name__ == "__main__":
    unittest.main()
