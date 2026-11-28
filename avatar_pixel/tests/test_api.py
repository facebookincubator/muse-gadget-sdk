# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

import base64
from contextlib import contextmanager, redirect_stderr, redirect_stdout
import http.client
import io
import json
from pathlib import Path
import socket
import subprocess
import threading
import time
import unittest
from unittest import mock

from avatar_pixel import AvatarError, compile_image, validate_pack
from avatar_pixel.api import ServerConfig, _run_compile, make_server
from avatar_pixel.protocol import MAX_BODY_BYTES
from avatar_pixel.tests.fixtures import avatar, encode, gradient


@contextmanager
def running(config=None):
    server = make_server(config or ServerConfig(port=0))
    thread = threading.Thread(
        target=server.serve_forever, kwargs={"poll_interval": 0.01}
    )
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()
        if thread.is_alive():
            raise AssertionError("test server did not stop")


def request(server, method="GET", path="/health", body=None, headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=10)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read(1024 * 1024)
    finally:
        connection.close()


def raw(server, wire):
    with socket.create_connection(
        ("127.0.0.1", server.server_port), timeout=5
    ) as client:
        client.sendall(wire)
        client.shutdown(socket.SHUT_WR)
        data = bytearray()
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > 1024 * 1024:
                raise AssertionError("unbounded HTTP output")
        return bytes(data)


def receive_until_closed(client, limit=4096):
    """EOF and TCP reset are both terminal; a read timeout is NOT success."""
    data = bytearray()
    while True:
        try:
            chunk = client.recv(min(2048, limit + 1 - len(data)))
        except ConnectionResetError:
            return bytes(data)
        if not chunk:
            return bytes(data)
        data.extend(chunk)
        if len(data) > limit:
            raise AssertionError("unbounded response before timeout closure")


class APITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with avatar("animal") as image:
            cls.encoded = encode(image)
        cls.body = json.dumps(
            {"image_base64": base64.b64encode(cls.encoded).decode(), "profile": 96}
        ).encode()

    def test_real_http_compile_matches_library_and_is_private(self):
        with running() as server:
            status, headers, body = request(
                server,
                "POST",
                "/v1/compile",
                self.body,
                {"Content-Type": "application/json"},
            )
            self.assertEqual(status, 200, body)
            self.assertEqual(headers["Cache-Control"], "no-store")
            self.assertEqual(headers["Connection"], "close")
            self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
            response = json.loads(body)
            pack = base64.b64decode(response["pack_base64"], validate=True)
            preview = base64.b64decode(response["preview_png_base64"], validate=True)
            expected = compile_image(self.encoded, profile=96)
            self.assertEqual(pack, expected.pack)
            self.assertEqual(preview, expected.preview_png)
            validate_pack(pack)
            self.assertEqual(request(server)[0], 200)

    def test_nonloopback_auth_configuration_and_literal_binding(self):
        for host in ("0.0.0.0", "192.0.2.1", "::", "::ffff:127.0.0.1"):
            with self.assertRaisesRegex(AvatarError, "nonloopback_requires_auth"):
                ServerConfig(host=host)
            config = ServerConfig(host=host, bearer_token="a" * 32)
            self.assertEqual(config.bearer_token, "a" * 32)
        for host in (
            "example.invalid",
            "https://example.invalid",
            "127.0.0.1%bad",
            "::1%lo",
        ):
            with self.assertRaisesRegex(AvatarError, "invalid_bind_address"):
                ServerConfig(host=host)
        self.assertEqual(ServerConfig(host="localhost").host, "127.0.0.1")
        self.assertIsNone(ServerConfig(host="::1").bearer_token)
        with mock.patch(
            "socket.getfqdn", side_effect=AssertionError("DNS is forbidden")
        ):
            with running():
                pass

    def test_auth_both_routes_and_no_credential_echo(self):
        token = "test-private-bearer-" + "x" * 32
        with running(ServerConfig(port=0, bearer_token=token)) as server:
            for method, path, body in (
                ("GET", "/health", None),
                ("POST", "/v1/compile", self.body),
            ):
                for authorization in (None, "Bearer wrong", "Basic " + token):
                    headers = {"Content-Type": "application/json"}
                    if authorization:
                        headers["Authorization"] = authorization
                    status, _, output = request(server, method, path, body, headers)
                    self.assertEqual(status, 401)
                    self.assertEqual(output, b'{"error":"unauthorized"}')
                    self.assertNotIn(token.encode(), output)
            self.assertEqual(
                request(server, headers={"Authorization": "Bearer " + token})[0], 200
            )
            self.assertEqual(
                request(
                    server,
                    "POST",
                    "/v1/compile",
                    self.body,
                    {
                        "Content-Type": "application/json",
                        "Authorization": "Bearer " + token,
                    },
                )[0],
                200,
            )
        self.assertNotIn(token, repr(ServerConfig(bearer_token=token)))

    def test_methods_paths_content_types_and_origins(self):
        with running() as server:
            for path in ("/", "/health?secret=private"):
                self.assertEqual(request(server, path=path)[0], 404)
            # http.client sets the absolute URL's Host to its external authority;
            # Host validation rejects this even before routing.
            self.assertEqual(
                request(server, path="http://example.invalid/health")[0], 403
            )
            self.assertEqual(request(server, "PUT", "/v1/compile", b"{}")[0], 501)
            self.assertEqual(
                request(server, headers={"Origin": "https://example.invalid"})[0], 403
            )
            self.assertEqual(
                request(server, headers={"Host": "attacker.invalid"})[0], 403
            )
            self.assertEqual(request(server, headers={"Host": "127.0.0.1:1"})[0], 403)
            for content_type in (
                "text/plain",
                "image/png",
                "multipart/form-data",
                "application/json; charset=latin1",
            ):
                status, _, _ = request(
                    server,
                    "POST",
                    "/v1/compile",
                    self.body,
                    {"Content-Type": content_type},
                )
                self.assertEqual(status, 415)
            status, _, _ = request(
                server,
                "POST",
                "/v1/compile",
                self.body,
                {"Content-Type": "application/json", "Content-Encoding": "gzip"},
            )
            self.assertEqual(status, 415)

    def test_framing_smuggling_duplicates_and_oversize(self):
        with running() as server:
            prefix = (
                "POST /v1/compile HTTP/1.1\r\n"
                f"Host: 127.0.0.1:{server.server_port}\r\n"
                "Content-Type: application/json\r\n"
            ).encode()
            for headers in (
                b"Content-Length: 2\r\nContent-Length: 2\r\n",
                b"Content-Length: 2\r\nTransfer-Encoding: chunked\r\n",
                b"Content-Length: -1\r\n",
                b"Content-Length: 2, 2\r\n",
                b"Content-Length: 00000000000002\r\n",
                b"Content-Length: 2\r\nContent-Type: application/json\r\n",
                b"Content-Length: 2\r\nAuthorization: a\r\nAuthorization: b\r\n",
                b"Content-Length: 2\r\nHost: attacker.invalid\r\n",
                b"",  # length is required
            ):
                with self.subTest(headers=headers):
                    output = raw(server, prefix + headers + b"\r\n{}")
                    self.assertTrue(output.startswith(b"HTTP/1.1 400"), output)
            output = raw(
                server,
                prefix + f"Content-Length: {MAX_BODY_BYTES + 1}\r\n\r\n".encode(),
            )
            self.assertTrue(output.startswith(b"HTTP/1.1 413"), output)
            output = raw(
                server, prefix + b"Content-Length: 2\r\nExpect: 100-continue\r\n\r\n"
            )
            self.assertTrue(output.startswith(b"HTTP/1.1 417"), output)
            output = raw(server, prefix + b"Content-Length: 10\r\n\r\n{}")
            self.assertIn(b"incomplete_body", output)

    def test_http_rejects_bad_data_without_echo(self):
        with running() as server:
            bodies = (
                b'{"image_base64":"secret private data"}',
                b'{"url":"https://example.invalid/private"}',
                b'{"image_base64":"abc","colors":NaN}',
                b'{"image_base64":"abc","colors":2,"colors":24}',
                b'{"image_base64":"YWJj","trusted_source":true}',
                b"[[[[",
            )
            for body in bodies:
                status, _, output = request(
                    server,
                    "POST",
                    "/v1/compile",
                    body,
                    {"Content-Type": "application/json"},
                )
                self.assertEqual(status, 400, output)
                self.assertEqual(set(json.loads(output)), {"error"})
                self.assertNotIn(b"secret", output)
                self.assertNotIn(b"example.invalid", output)

    def test_timeout_closure_accepts_eof_reset_but_not_read_timeout(self):
        eof = mock.Mock()
        eof.recv.side_effect = [b""]
        self.assertEqual(receive_until_closed(eof), b"")
        reset = mock.Mock()
        reset.recv.side_effect = ConnectionResetError("terminal reset")
        self.assertEqual(receive_until_closed(reset), b"")
        response_then_reset = mock.Mock()
        response_then_reset.recv.side_effect = [b"HTTP/1.1 408", ConnectionResetError()]
        self.assertEqual(receive_until_closed(response_then_reset), b"HTTP/1.1 408")
        still_open = mock.Mock()
        still_open.recv.side_effect = socket.timeout("not terminal")
        with self.assertRaises(socket.timeout):
            receive_until_closed(still_open)

    def test_headers_and_body_have_absolute_deadline_and_bounded_slots(self):
        config = ServerConfig(
            port=0,
            max_connections=1,
            request_timeout=0.6,
            io_timeout=0.3,
            compile_timeout=0.1,
        )
        with running(config) as server:
            with socket.create_connection(
                ("127.0.0.1", server.server_port), timeout=3
            ) as slow:
                slow.sendall(b"POST /v1/compile HTTP/1.1\r\nHost: ")
                # Keep the header active beyond inactivity timeout, but not the
                # absolute deadline. Wait is test stimulus, not service polling.
                time.sleep(0.1)
                status, _, body = request(server)
                self.assertEqual(status, 503, body)
                start = time.monotonic()
                for _ in range(12):
                    try:
                        slow.sendall(b"a")
                    except OSError:
                        break
                    time.sleep(0.06)
                slow.settimeout(1)
                self.assertEqual(receive_until_closed(slow), b"")
                self.assertLess(time.monotonic() - start, 1.5)
            self.assertEqual(request(server)[0], 200)
            prefix = (
                "POST /v1/compile HTTP/1.1\r\n"
                f"Host: 127.0.0.1:{server.server_port}\r\n"
                "Content-Type: application/json\r\nContent-Length: 100\r\n\r\n"
            ).encode()
            with socket.create_connection(
                ("127.0.0.1", server.server_port), timeout=3
            ) as slow:
                start = time.monotonic()
                slow.sendall(prefix + b"{")
                output = receive_until_closed(slow)
                # An error response is optional when the deadline closes TCP.
                # Timeout, still-open TCP, or unexpected response is a failure.
                if output:
                    self.assertIn(b"408", output)
                self.assertLess(time.monotonic() - start, 1.5)

    def test_real_worker_wall_timeout_is_killed_and_reaped(self):
        with gradient(512) as image:
            body = json.dumps(
                {
                    "image_base64": base64.b64encode(encode(image)).decode(),
                    "profile": 96,
                    "colors": 64,
                    "dither": True,
                }
            ).encode()
        start = time.monotonic()
        status, output = _run_compile(body, 0.001)
        self.assertEqual(status, 504, output)
        self.assertEqual(output, b'{"error":"compile_timeout"}')
        self.assertLess(time.monotonic() - start, 2)
        # Genuine HTTP deadline too, with an expensive synthetic decode/quantize.
        with running(ServerConfig(port=0, compile_timeout=0.1)) as server:
            status, _, output = request(
                server,
                "POST",
                "/v1/compile",
                body,
                {"Content-Type": "application/json"},
            )
            self.assertEqual(status, 504, output)
            self.assertEqual(request(server)[0], 200)

    def test_no_private_logs_or_persistence(self):
        stdout, stderr = io.StringIO(), io.StringIO()
        root = Path(__file__).resolve().parent.parent
        before = sorted(
            str(path.relative_to(root)) for path in root.glob("**/*") if path.is_file()
        )
        with redirect_stdout(stdout), redirect_stderr(stderr):
            with running() as server:
                self.assertEqual(
                    request(
                        server,
                        "POST",
                        "/v1/compile",
                        self.body,
                        {"Content-Type": "application/json"},
                    )[0],
                    200,
                )
                request(server, path="/not-found-private-secret?credential=test")
                raw(server, b"PRIVATE-CREDENTIAL invalid protocol\r\n\r\n")
        after = sorted(
            str(path.relative_to(root)) for path in root.glob("**/*") if path.is_file()
        )
        self.assertEqual(before, after)
        self.assertEqual(stdout.getvalue(), "")
        self.assertEqual(stderr.getvalue(), "")

    def test_finite_server_limits(self):
        for field, values in (
            ("max_connections", (0, 9, True, 1.0)),
            ("request_timeout", (float("inf"), float("nan"), True, 0, 61)),
            ("io_timeout", (float("inf"), 0, 16)),
            ("compile_timeout", (float("nan"), 15, 31)),
            ("port", (-1, 65536, True, "8080")),
            ("bearer_token", ("", "short", "x" * 257, "x" * 32 + "\n", True)),
        ):
            for value in values:
                with self.subTest(field=field, value=value):
                    with self.assertRaises(AvatarError):
                        ServerConfig(**{field: value})

    def test_bounded_worker_failure(self):
        with mock.patch(
            "avatar_pixel.api.subprocess.run", side_effect=OSError("private error")
        ):
            self.assertEqual(
                _run_compile(self.body, 1), (503, b'{"error":"worker_unavailable"}')
            )
        for completed in (
            subprocess.CompletedProcess([], 1, stdout=b"private decoder trace"),
            subprocess.CompletedProcess([], 0, stdout=b""),
            subprocess.CompletedProcess([], 0, stdout=b"x" * (1024 * 1024)),
        ):
            with mock.patch("avatar_pixel.api.subprocess.run", return_value=completed):
                self.assertEqual(
                    _run_compile(self.body, 1), (500, b'{"error":"compile_failed"}')
                )


if __name__ == "__main__":
    unittest.main()
