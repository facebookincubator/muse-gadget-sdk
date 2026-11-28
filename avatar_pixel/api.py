# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Development/self-hostable, bounded HTTP API; no upload storage or egress."""

from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, HTTPServer
import hmac
import ipaddress
import math
import os
from pathlib import Path
import re
import socket
from socketserver import TCPServer, ThreadingMixIn
import subprocess
import sys
import threading
import time

from .compiler import AvatarError
from .protocol import MAX_BODY_BYTES, MAX_RESPONSE_BYTES, json_bytes


@dataclass(frozen=True)
class ServerConfig:
    host: str = "127.0.0.1"
    port: int = 8080
    bearer_token: str | None = field(default=None, repr=False)
    max_connections: int = 2
    request_timeout: float = 15.0
    io_timeout: float = 3.0
    compile_timeout: float = 5.0

    def __post_init__(self):
        if type(self.host) is not str:
            raise AvatarError("invalid_bind_address")
        # Literal IP/localhost only; no DNS lookup and no hidden network fetch.
        host = "127.0.0.1" if self.host == "localhost" else self.host
        try:
            address = ipaddress.ip_address(host)
        except ValueError:
            raise AvatarError("invalid_bind_address") from None
        if "%" in host:
            raise AvatarError("invalid_bind_address")
        object.__setattr__(self, "host", str(address))
        if type(self.port) is not int or not 0 <= self.port <= 65535:
            raise AvatarError("invalid_port")
        if type(self.max_connections) is not int or not 1 <= self.max_connections <= 8:
            raise AvatarError("invalid_concurrency")
        for value in (self.request_timeout, self.io_timeout, self.compile_timeout):
            if (
                type(value) not in (int, float)
                or not math.isfinite(value)
                or not 0.1 <= value <= 60
            ):
                raise AvatarError("invalid_timeout")
        if self.compile_timeout > 30 or self.compile_timeout >= self.request_timeout:
            raise AvatarError("invalid_timeout")
        if self.io_timeout > self.request_timeout:
            raise AvatarError("invalid_timeout")
        if self.bearer_token is not None and (
            type(self.bearer_token) is not str
            or re.fullmatch(r"[A-Za-z0-9._~-]{32,256}", self.bearer_token) is None
        ):
            raise AvatarError("invalid_bearer_token")
        if not address.is_loopback or getattr(address, "ipv4_mapped", None) is not None:
            if self.bearer_token is None:
                raise AvatarError("nonloopback_requires_auth")


def _run_compile(body, timeout):
    # Only a fresh worker sees the private input. No shell, inherited credentials,
    # temp images, worker logs, persistent cache, or external service is involved.
    root = str(Path(__file__).resolve().parent.parent)
    env = {"PYTHONPATH": root, "PYTHONDONTWRITEBYTECODE": "1"}
    # Windows needs these for process initialization; neither is a credential.
    for name in ("SYSTEMROOT", "WINDIR"):
        if name in os.environ:
            env[name] = os.environ[name]
    try:
        completed = subprocess.run(
            [sys.executable, "-B", "-m", "avatar_pixel._worker"],
            input=body,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=timeout,
            env=env,
            cwd=root,
            check=False,
        )
    except subprocess.TimeoutExpired:
        # subprocess.run kills and reaps the worker before raising.
        return 504, b'{"error":"compile_timeout"}'
    except OSError:
        return 503, b'{"error":"worker_unavailable"}'
    if not completed.stdout or len(completed.stdout) > MAX_RESPONSE_BYTES:
        return 500, b'{"error":"compile_failed"}'
    if completed.returncode == 0:
        return 200, completed.stdout
    if completed.returncode == 2:
        return 400, completed.stdout
    return 500, b'{"error":"compile_failed"}'


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.deadline = time.monotonic() + self.server.config.request_timeout

    def log_message(self, _format, *args):
        # BaseHTTPRequestHandler otherwise logs attacker paths and errors.
        pass

    def send_error(self, code, message=None, explain=None):
        # Also covers malformed HTTP and unsupported methods without echoing data.
        self._reply(code, b'{"error":"invalid_http_request"}')

    def _reply(self, status, body):
        self.close_connection = True
        try:
            self.send_response_only(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Connection", "close")
            if status == 401:
                self.send_header("WWW-Authenticate", "Bearer")
            self.end_headers()
            if getattr(self, "command", None) != "HEAD":
                self.wfile.write(body)
        except (OSError, ValueError):
            pass

    def _error(self, status, code):
        self._reply(status, json_bytes({"error": code}))

    def _single(self, header, required=False):
        values = self.headers.get_all(header, [])
        if len(values) > 1 or (required and len(values) != 1):
            raise AvatarError("invalid_headers")
        return values[0] if values else None

    def _host_ok(self, host):
        if len(host) > 128:
            return False
        match = re.fullmatch(
            r"(?:\[([0-9a-fA-F:]+)\]|([0-9a-zA-Z.]+))(?::([0-9]{1,5}))?", host
        )
        if not match:
            return False
        name = match[1] or match[2]
        port = int(match[3]) if match[3] else 80
        if port != self.server.server_port:
            return False
        local = ipaddress.ip_address(self.connection.getsockname()[0])
        if name.lower() == "localhost":
            return local.is_loopback
        try:
            return ipaddress.ip_address(name) == local
        except ValueError:
            return False

    def _authorized(self):
        try:
            host = self._single("Host", required=True)
            authorization = self._single("Authorization")
            if not self._host_ok(host):
                self._error(403, "invalid_host")
                return False
            # No browser cross-origin uploads/CSRF or URL credentials.
            if self.headers.get_all("Origin"):
                self._error(403, "origin_prohibited")
                return False
            token = self.server.config.bearer_token
            if token is not None and (
                authorization is None
                or not hmac.compare_digest(
                    authorization.encode("utf-8"), ("Bearer " + token).encode("ascii")
                )
            ):
                self._error(401, "unauthorized")
                return False
            return True
        except AvatarError:
            self._error(400, "invalid_headers")
            return False

    def do_GET(self):
        if not self._authorized():
            return
        if self.path != "/health":
            self._error(404, "not_found")
            return
        if self.headers.get_all("Transfer-Encoding") or self.headers.get_all(
            "Content-Length"
        ):
            self._error(400, "invalid_headers")
            return
        self._reply(200, b'{"status":"ok"}')

    def handle_expect_100(self):
        # Do not invite uploads before authentication or framing validation.
        self._error(417, "expect_prohibited")
        return False

    def do_POST(self):
        if not self._authorized():
            return
        if self.path != "/v1/compile":
            self._error(404, "not_found")
            return
        try:
            if self.headers.get_all("Transfer-Encoding") or self.headers.get_all(
                "Expect"
            ):
                raise AvatarError("invalid_headers")
            content_type = self._single("Content-Type", required=True)
            if content_type.lower().replace(" ", "") not in (
                "application/json",
                "application/json;charset=utf-8",
                'application/json;charset="utf-8"',
            ):
                self._error(415, "json_required")
                return
            if self.headers.get_all("Content-Encoding"):
                self._error(415, "content_encoding_prohibited")
                return
            length_text = self._single("Content-Length", required=True)
            if re.fullmatch(r"[0-9]{1,10}", length_text) is None:
                raise AvatarError("invalid_headers")
            length = int(length_text)
            if length > MAX_BODY_BYTES:
                self._error(413, "body_bytes_exceeded")
                return
            if length == 0:
                self._error(400, "invalid_json")
                return
            remaining = length
            chunks = []
            while remaining:
                seconds = self.deadline - time.monotonic()
                if seconds <= 0:
                    self._error(408, "request_timeout")
                    return
                self.connection.settimeout(min(seconds, self.server.config.io_timeout))
                chunk = self.rfile.read1(min(remaining, 65536))
                if not chunk:
                    self._error(400, "incomplete_body")
                    return
                chunks.append(chunk)
                remaining -= len(chunk)
            body = b"".join(chunks)
            del chunks
            seconds = self.deadline - time.monotonic()
            if seconds <= 0.1:
                self._error(408, "request_timeout")
                return
            status, result = _run_compile(
                body, min(self.server.config.compile_timeout, seconds)
            )
            del body
            self.connection.settimeout(
                min(
                    self.server.config.io_timeout,
                    max(0.1, self.deadline - time.monotonic()),
                )
            )
            self._reply(status, result)
        except AvatarError:
            self._error(400, "invalid_headers")
        except TimeoutError:
            self._error(408, "request_timeout")
        except OSError:
            self.close_connection = True


class AvatarHTTPServer(ThreadingMixIn, HTTPServer):
    # Limit before spawning any thread, including slow headers and health clients.
    # Join admitted handlers on server_close; hard deadlines make this bounded.
    daemon_threads = False
    block_on_close = True
    request_queue_size = 8
    allow_reuse_address = False

    def __init__(self, config: ServerConfig):
        # Parent handlers also hold private bodies/outputs: disable Unix core
        # dumps before accepting any connection, not just inside the decoder.
        try:
            import resource
        except ImportError:
            resource = None
        if resource is not None:
            try:
                resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            except (OSError, ValueError):
                raise AvatarError("privacy_controls_unavailable") from None
        self.config = config
        self._slots = threading.BoundedSemaphore(config.max_connections)
        self.address_family = socket.AF_INET6 if ":" in config.host else socket.AF_INET
        super().__init__((config.host, config.port), _Handler)

    def server_bind(self):
        # HTTPServer.server_bind normally does a reverse DNS getfqdn lookup.
        # Binding numeric addresses must not trigger hidden public egress.
        TCPServer.server_bind(self)
        self.server_name = self.config.host
        self.server_port = self.socket.getsockname()[1]

    def get_request(self):
        request, address = super().get_request()
        request.settimeout(self.config.io_timeout)
        return request, address

    def process_request(self, request, client_address):
        if not self._slots.acquire(blocking=False):
            try:
                body = b'{"error":"busy"}'
                request.settimeout(0.1)
                request.sendall(
                    b"HTTP/1.1 503 Service Unavailable\r\n"
                    b"Content-Type: application/json\r\n"
                    b"Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                    + str(len(body)).encode("ascii")
                    + b"\r\n\r\n"
                    + body
                )
            except OSError:
                pass
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._slots.release()
            self.shutdown_request(request)
            raise

    def process_request_thread(self, request, client_address):
        def expire():
            try:
                request.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

        timer = threading.Timer(self.config.request_timeout, expire)
        timer.daemon = True
        timer.start()
        try:
            super().process_request_thread(request, client_address)
        finally:
            timer.cancel()
            timer.join()
            self._slots.release()

    def handle_error(self, request, client_address):
        # Never emit exceptions/request lines/addresses/credentials/body/output.
        pass


def make_server(config=None):
    return AvatarHTTPServer(config if config is not None else ServerConfig())
