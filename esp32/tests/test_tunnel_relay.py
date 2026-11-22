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
import tempfile
import unittest

from test_link_ota import function_source

ROOT = Path(__file__).resolve().parents[1]


class TunnelRelayTest(unittest.TestCase):
    def test_packet_sorting_and_napt_input_check(self):
        relay = (ROOT / "main/tunnel_relay.c").read_text()
        guard = (ROOT / "main/lwip_hooks/tunnel_napt_guard.h").read_text()
        with tempfile.TemporaryDirectory() as name:
            out = Path(name)
            (out / "napt_guard.inc").write_text(
                function_source(guard, "static inline bool tunnel_napt_input_short(") + "\n"
                + function_source(guard, "static inline bool tunnel_napt_input_inside("))
            start = relay.index("typedef enum {")
            (out / "relay_classify.inc").write_text(
                relay[start:relay.index("// Sorts one IPv4 packet", start)]
                + function_source(relay, "static relay_kind_t relay_classify(") + "\n"
                + function_source(relay, "static bool relay_sender_ok("))
            cc = shlex.split(os.environ.get("CC", "cc"))
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE",
                     "-I", str(out)]
            binary = out / "relay"
            compiled = subprocess.run(
                [*cc, *flags, str(ROOT / "tests/tunnel_relay_harness.c"), "-o", str(binary)],
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
        # Relay ports belong to one tunnel stream: a reopen clears them first.
        self.assertIn("noise_tunnel_set_open_cb(tunnel_relay_reset)", start)
        link = function_source(netif, "void tunnel_netif_set_link(")
        self.assertIn("tunnel_relay_reset()", link[link.index("} else {"):])


if __name__ == "__main__":
    unittest.main()
