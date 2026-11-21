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

"""Production proxy handshakes + TLS adapter against test-owned loopback peers.

OpenSSL substitutes for ESP-TLS on the host; the firmware socket, config,
transport and WebSocket upgrade construction are compiled unchanged.
"""
import base64
import contextlib
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from host_cjson import cjson_source

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))
PROXY = ROOT / "components/network_proxy"


def read_exact(conn, n):
    out = b""
    while len(out) < n:
        part = conn.recv(n - len(out))
        if not part:
            raise EOFError("truncated request")
        out += part
    return out


def read_head(conn):
    out = b""
    while not out.endswith(b"\r\n\r\n"):
        out += read_exact(conn, 1)
        if len(out) > 4096:
            raise AssertionError("oversize request")
    return out


class NetworkProxy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.dir = Path(cls.tmp.name)
        if not shutil.which("pkg-config") or not shutil.which("openssl"):
            raise unittest.SkipTest("proxy TLS host tests need pkg-config and OpenSSL")
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "openssl"], capture_output=True, text=True)
        if flags.returncode or not (JSON / "cJSON.c").exists():
            raise unittest.SkipTest("proxy host tests need OpenSSL development files and cJSON (run idf.py build)")
        cls.bin = cls.dir / "proxy-client"
        compiled = subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "tests/proxy_fakes"), "-I", str(PROXY / "include"), "-I", str(ROOT / "main"),
            str(ROOT / "tests/network_proxy_tls_harness.c"),
            str(PROXY / "network_proxy.c"), str(PROXY / "network_proxy_socket.c"), str(PROXY / "network_proxy_tls.c"),
            *cjson_source(JSON), *shlex.split(flags.stdout), "-o", str(cls.bin),
        ], capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)
        cls.cert, cls.key = cls.dir / "cert.pem", cls.dir / "key.pem"
        generated = subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=cloud.invalid", "-addext", "subjectAltName=DNS:cloud.invalid,DNS:localhost,IP:127.0.0.1",
            "-keyout", str(cls.key), "-out", str(cls.cert),
        ], capture_output=True, text=True)
        if generated.returncode:
            raise AssertionError(generated.stderr)
        cls.tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cls.tls.load_cert_chain(cls.cert, cls.key)

    def client(self, proxy, host="cloud.invalid", port=443, mode="tunnel", timeout=1000, ca=None):
        return subprocess.run([str(self.bin), json.dumps(proxy), host, str(port), str(timeout), mode],
            env={**os.environ, "PROXY_TEST_CA": str(ca or self.cert)}, capture_output=True, text=True, timeout=5)

    def assert_exit(self, result, expected):
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    @contextlib.contextmanager
    def peer(self, handler):
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(2)

            def run():
                try:
                    conn, _ = listener.accept()
                    with conn:
                        conn.settimeout(2)
                        handler(conn)
                except BaseException as e:
                    errors.append(e)

            thread = threading.Thread(target=run, daemon=True)
            thread.start()
            try:
                yield listener.getsockname()[1]
            finally:
                failed = sys.exc_info()[0] is not None
                listener.close()
                thread.join(3)
                self.assertFalse(thread.is_alive(), "fake proxy leaked its worker")
                if errors and not failed:
                    raise errors[0]

    def config(self, port, kind="http", **fields):
        return {"type": kind, "host": "127.0.0.1", "port": port, **fields}

    def cloud(self, conn, tls=False):
        if tls:
            conn = self.tls.wrap_socket(conn, server_side=True)
        with conn:
            head = read_head(conn)
            self.assertIn(b"GET /v1/noise?vm_id=test-vm HTTP/1.1\r\n", head)
            self.assertIn(b"Authorization: Bearer fake-device-token\r\n", head)
            self.assertNotIn(b"Proxy-Authorization", head)
            conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\n\r\n")

    def test_http_connect_fragmented_and_auth(self):
        def handler(conn):
            head = read_head(conn)
            self.assertTrue(head.startswith(b"CONNECT cloud.invalid:443 HTTP/1.1\r\n"))
            self.assertIn(b"Host: cloud.invalid:443\r\n", head)
            expected = base64.b64encode(b"user:p%40ss:word")
            self.assertIn(b"Proxy-Authorization: Basic " + expected + b"\r\n", head)
            reply = b"HTTP/1.1 200 Connection established\r\nX-Test: yes\r\n\r\n"
            for i in range(0, len(reply), 3):
                conn.sendall(reply[i:i + 3])
            self.cloud(conn)
        with self.peer(handler) as port:
            self.assert_exit(self.client(self.config(port, username="user", password="p%40ss:word")), 0)

    def test_http_tunnel_preserves_first_payload_byte(self):
        def handler(conn):
            self.assertNotIn(b"Proxy-Authorization", read_head(conn))
            # Coalesced bytes after CRLFCRLF must reach the cloud reader.
            conn.sendall(b"HTTP/1.0 200 OK\r\n\r\nHTTP/1.1 101 Switching Protocols\r\n\r\n")
            self.assertIn(b"GET /v1/noise", read_head(conn))
        with self.peer(handler) as port:
            self.assert_exit(self.client(self.config(port)), 0)

    def test_http_rejection_malformed_and_oversize_fail_closed(self):
        for reply in (b"HTTP/1.1 407 Auth Required\r\n\r\n", b"HTTP/1.1 403 Forbidden\r\n\r\n",
                      b"garbage\r\n\r\n", b"HTTP/1.1 2xx bad\r\n\r\n", b"HTTP/1.1 200x bad\r\n\r\n", b"HTTP/1.1 200 OK\r\n" + b"x" * 4096, b""):
            with self.subTest(reply=reply[:40]):
                def handler(conn):
                    read_head(conn)
                    conn.sendall(reply)
                with self.peer(handler) as port:
                    self.assert_exit(self.client(self.config(port)), 1)

    def test_handshake_deadline(self):
        def handler(conn):
            read_head(conn)
            time.sleep(0.2)
        with self.peer(handler) as port:
            started = time.monotonic()
            self.assert_exit(self.client(self.config(port), timeout=80), 1)
            self.assertLess(time.monotonic() - started, 0.5)

    def socks(self, conn, auth=False, response_type=1, host=b"cloud.invalid"):
        method = 2 if auth else 0
        self.assertEqual(read_exact(conn, 3), bytes([5, 1, method]))
        conn.sendall(bytes([5, method]))
        if auth:
            self.assertEqual(read_exact(conn, 2), b"\x01\x04")
            self.assertEqual(read_exact(conn, 4), b"user")
            self.assertEqual(read_exact(conn, 1), b"\x04")
            self.assertEqual(read_exact(conn, 4), b"pass")
            conn.sendall(b"\x01\x00")
        prefix = read_exact(conn, 4)
        self.assertEqual(prefix[:3], b"\x05\x01\x00")
        if prefix[3] == 3:
            self.assertEqual(read_exact(conn, read_exact(conn, 1)[0]), host)
        else:
            family, size = (socket.AF_INET, 4) if prefix[3] == 1 else (socket.AF_INET6, 16)
            self.assertEqual(socket.inet_ntop(family, read_exact(conn, size)), host.decode())
        self.assertEqual(read_exact(conn, 2), b"\x01\xbb")
        address = {1: b"\x00" * 4, 4: b"\x00" * 16, 3: b"\x04test"}[response_type]
        conn.sendall(bytes([5, 0, 0, response_type]) + address + b"\x00\x50")

    def test_socks5_auth_and_bound_address_forms(self):
        for auth, atyp in ((False, 1), (True, 4), (False, 3)):
            with self.subTest(auth=auth, atyp=atyp):
                def handler(conn):
                    self.socks(conn, auth, atyp)
                    self.cloud(conn)
                with self.peer(handler) as port:
                    cfg = self.config(port, "socks5", **({"username": "user", "password": "pass"} if auth else {}))
                    self.assert_exit(self.client(cfg), 0)

    def test_socks5_remote_dns_and_ip_destination_forms(self):
        for host in ("cloud.invalid", "203.0.113.8", "2001:db8::8"):
            with self.subTest(host=host):
                def handler(conn):
                    self.socks(conn, host=host.encode())
                    self.cloud(conn)
                with self.peer(handler) as port:
                    self.assert_exit(self.client(self.config(port, "socks5"), host=host), 0)

    def test_socks5_method_downgrade_and_auth_refusal(self):
        for response in (b"\x05\x00", b"\x05\xff", b"\x04\x02", b"\x05\x02"):
            with self.subTest(response=response):
                def handler(conn):
                    self.assertEqual(read_exact(conn, 3), b"\x05\x01\x02")
                    conn.sendall(response)
                    if response == b"\x05\x02":
                        read_exact(conn, 11)
                        conn.sendall(b"\x01\x01")
                with self.peer(handler) as port:
                    self.assert_exit(self.client(self.config(port, "socks5", username="user", password="pass")), 1)

    def test_socks5_bad_connect_reply(self):
        for reply in (b"\x05\x05\x00\x01", b"\x05\x00\x01\x01", b"\x05\x00\x00\x09", b"\x05\x00"):
            def handler(conn):
                read_exact(conn, 3)
                conn.sendall(b"\x05\x00")
                read_exact(conn, 4)
                read_exact(conn, read_exact(conn, 1)[0] + 2)
                conn.sendall(reply)
            with self.subTest(reply=reply), self.peer(handler) as port:
                self.assert_exit(self.client(self.config(port, "socks5")), 1)

    def test_cloud_tls_and_http_transport_through_fake_proxy(self):
        for kind, mode in (("http", "tls"), ("http", "http"), ("socks5", "tls")):
            with self.subTest(kind=kind, mode=mode):
                names = []
                self.tls.set_servername_callback(lambda sock, name, ctx: names.append(name))
                def handler(conn):
                    if kind == "http":
                        self.assertTrue(read_head(conn).startswith(b"CONNECT cloud.invalid:443 "))
                        conn.sendall(b"HTTP/1.1 200 OK\r\n\r\n")
                    else:
                        self.socks(conn)
                    self.cloud(conn, tls=True)
                with self.peer(handler) as port:
                    self.assert_exit(self.client(self.config(port, kind), mode=mode), 0)
                self.assertEqual(names, ["cloud.invalid"])
        self.tls.set_servername_callback(None)

    def test_http_client_local_plain_http_stays_direct(self):
        with self.peer(lambda c: self.cloud(c)) as port:
            self.assert_exit(self.client(self.config(1), host="127.0.0.1", port=port, mode="http_plain"), 0)

    def test_plain_http_client_uses_configured_tunnel(self):
        def handler(conn):
            self.assertTrue(read_head(conn).startswith(b"CONNECT cloud.invalid:80 "))
            conn.sendall(b"HTTP/1.1 200 OK\r\n\r\n")
            self.cloud(conn)
        with self.peer(handler) as port:
            self.assert_exit(self.client(self.config(port), port=80, mode="http_plain"), 0)

    def test_proxy_keeps_tls_certificate_verification(self):
        def handler(conn):
            read_head(conn)
            conn.sendall(b"HTTP/1.1 200 OK\r\n\r\n")
            with self.assertRaises(ssl.SSLError):
                self.tls.wrap_socket(conn, server_side=True)
        with self.peer(handler) as port:
            self.assert_exit(self.client(self.config(port), host="wrong.invalid", mode="tls"), 1)

    def test_no_proxy_and_local_traffic_stay_direct(self):
        for cfg, mode in (({"type": "off"}, "tls"), (self.config(1), "tls"), (self.config(1, "socks5"), "tunnel")):
            with self.subTest(cfg=cfg, mode=mode), self.peer(lambda c: self.cloud(c, tls=mode == "tls")) as port:
                self.assert_exit(self.client(cfg, host="127.0.0.1", port=port, mode=mode), 0)

    def test_local_destination_policy(self):
        for local in ("localhost", "localhost.", "printer", "ball.local", "ball.LOCAL.",
                      "127.0.0.1", "10.2.3.4", "172.16.1.1", "172.31.255.255", "192.168.1.4",
                      "169.254.1.8", "::1", "fe80::1", "fd00::1", "fc00::1", "::ffff:192.168.1.4"):
            with self.subTest(local=local):
                self.assert_exit(self.client({"type": "off"}, host=local, mode="local"), 0)
        for remote in ("cloud.invalid", "203.0.113.8", "172.32.0.1", "192.169.0.1", "2001:db8::8", "::ffff:203.0.113.8"):
            with self.subTest(remote=remote):
                self.assert_exit(self.client({"type": "off"}, host=remote, mode="local"), 3)

    def test_invalid_provisioning_proxy_fields_are_rejected(self):
        good = self.config(8080)
        for invalid in (None, [], {"type": "pac"}, {"type": "socks4"},
                        {**good, "port": "8080"}, {**good, "port": 1.5}, {**good, "port": 65536},
                        {**good, "port": 0}, {**good, "host": "bad\r\nInjected: yes"},
                        {**good, "host": "http://proxy"}, {**good, "host": "x" * 128},
                        {**good, "username": "x" * 64}, {**good, "password": "x"},
                        {**good, "username": "user:name"}, {**good, "password": 123}):
            with self.subTest(invalid=invalid):
                self.assert_exit(self.client(invalid), 2)


if __name__ == "__main__":
    unittest.main()
