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

"""The data-path bench's host side (tools/coexist/bench/bench_server.py),
its contract with main/pipeline_bench.cpp, and the config rules that keep
bench-only settings out of real builds."""

from __future__ import annotations

import http.client
import http.server
import importlib.util
import io
import contextlib
import re
import socket
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / "tools" / "coexist" / "bench"


def _load_server():
    spec = importlib.util.spec_from_file_location("bench_server", BENCH / "bench_server.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BenchServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.server = _load_server()

    def _http(self):
        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), self.server.TunnelHTTP)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        self.addCleanup(httpd.shutdown)
        return http.client.HTTPConnection("127.0.0.1", httpd.server_address[1], timeout=10)

    def test_blob_streams_the_requested_bytes(self) -> None:
        conn = self._http()
        with contextlib.redirect_stdout(io.StringIO()) as out:
            conn.request("GET", "/blob?bytes=200000")
            resp = conn.getresponse()
            body = resp.read()
        self.assertEqual(resp.status, 200)
        self.assertEqual(len(body), 200000)
        self.assertEqual(int(resp.getheader("Content-Length")), 200000)
        self.assertRegex(out.getvalue(), r"http GET /blob 200000 B in [0-9.]+ s")

    def test_sink_counts_the_body(self) -> None:
        conn = self._http()
        with contextlib.redirect_stdout(io.StringIO()) as out:
            conn.request("POST", "/sink", body=b"x" * 150000)
            resp = conn.getresponse()
            body = resp.read().decode()
        self.assertEqual(resp.status, 200)
        self.assertIn('"bytes":150000', body)
        self.assertRegex(out.getvalue(), r"http POST /sink 150000 B in")

    def test_other_paths_are_not_found(self) -> None:
        conn = self._http()
        with contextlib.redirect_stdout(io.StringIO()):
            conn.request("GET", "/nope")
            self.assertEqual(conn.getresponse().status, 404)

    def test_tcp_directions(self) -> None:
        # "TX": the device sends and the server counts; "RX": the server sends.
        with contextlib.redirect_stdout(io.StringIO()) as out:
            a, b = socket.socketpair()
            t = threading.Thread(target=self.server.serve_conn, args=(b, ("dev", 0), "tcp", 1))
            t.start()
            a.sendall(b"TX\n" + b"y" * 70000)
            a.shutdown(socket.SHUT_WR)
            t.join(10)
            a.close()
            a, b = socket.socketpair()
            t = threading.Thread(target=self.server.serve_conn, args=(b, ("dev", 0), "tcp", 0))
            t.start()
            a.sendall(b"RX\n")
            got = 0
            while True:
                chunk = a.recv(65536)
                if not chunk:
                    break
                got += len(chunk)
            t.join(10)
            a.close()
        self.assertIn("tcp TX from dev: 70000 B", out.getvalue())
        self.assertGreater(got, 0)

    def test_beacon_matches_the_device_parser(self) -> None:
        source = (BENCH / "bench_server.py").read_text()
        self.assertIn('f"C5BENCH {a.tcp} {a.tls}"', source)
        device = (ROOT / "main" / "pipeline_bench.cpp").read_text()
        self.assertIn('sscanf(buf, "C5BENCH %d %d", &s_tcp_port, &s_tls_port) == 2', device)
        # The device opens each connection with the direction line the server reads.
        self.assertIn('"TX\\n"', device)
        self.assertIn('"RX\\n"', device)


class BenchConfigTest(unittest.TestCase):
    def _guard(self, marker: str) -> str:
        source = (ROOT / "cmake" / "validate_config.cmake").read_text()
        start = source.index(marker)
        return source[start:source.index("endif()", start) + len("endif()")]

    def test_insecure_tls_only_in_the_bench(self) -> None:
        guard = self._guard("if((CONFIG_ESP_TLS_INSECURE OR CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY)")
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "guard.cmake"
            for insecure, skip, bench, accepted in (
                ("OFF", "OFF", "OFF", True),
                ("ON", "OFF", "OFF", False),
                ("OFF", "ON", "OFF", False),
                ("ON", "ON", "OFF", False),
                ("ON", "ON", "ON", True),
            ):
                with self.subTest(insecure=insecure, skip=skip, bench=bench):
                    script.write_text(
                        f"set(CONFIG_ESP_TLS_INSECURE {insecure})\n"
                        f"set(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY {skip})\n"
                        f"set(CONFIG_HOMEHUB_PIPELINE_BENCH {bench})\n"
                        "set(GADGET_CONFIG_REGEN_HINT x)\n" + guard)
                    ran = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
                    self.assertEqual(ran.returncode == 0, accepted, ran.stderr)

    def test_bench_only_settings_stay_in_bench_overlays(self) -> None:
        coexist = ROOT / "tools" / "coexist"
        bench = (coexist / "sdkconfig.bench").read_text()
        self.assertIn("CONFIG_HOMEHUB_PIPELINE_BENCH=y", bench)
        bench_only = ("CONFIG_ESP_TLS_INSECURE=y", "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y",
                      "CONFIG_HOMEHUB_BLE_THROUGHPUT_TEST=y", "CONFIG_HOMEHUB_PIPELINE_BENCH=y")
        allowed = {"sdkconfig.bench", "sdkconfig.bench-quick", "sdkconfig.ble-bench"}
        shipped = [ROOT / "sdkconfig.defaults", *sorted(ROOT.glob("sdkconfig.defaults.*")),
                   *sorted((ROOT / "devices").glob("sdkconfig.*")),
                   *[p for p in sorted(coexist.glob("sdkconfig.*")) if p.name not in allowed]]
        for path in shipped:
            text = path.read_text()
            for setting in bench_only:
                with self.subTest(path=path.name, setting=setting):
                    self.assertNotIn(setting, text)


if __name__ == "__main__":
    unittest.main()
