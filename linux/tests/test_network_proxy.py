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

"""The setup probe behind an HTTP proxy, against an in-process fake proxy."""

from __future__ import annotations

import base64
import socket
import threading

import pytest

from musegadget import network

API_HOST = "api.muse.ai"


class FakeProxy:
    """Accepts one connection, records the request head, and sends ``reply``."""

    def __init__(self, reply: bytes) -> None:
        self.reply = reply
        self.requests: list[str] = []
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(1)
        self._listener.settimeout(5)
        self.port: int = self._listener.getsockname()[1]
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        try:
            conn, _ = self._listener.accept()
        except OSError:
            return
        with conn:
            conn.settimeout(5)
            head = b""
            while b"\r\n\r\n" not in head:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                head += chunk
            self.requests.append(head.decode())
            conn.sendall(self.reply)

    def close(self) -> None:
        self._listener.close()
        self._thread.join(5)


@pytest.fixture
def connections(monkeypatch):
    """Every address the probe connects to; connections other than to 127.0.0.1 fail.

    conftest.py clears the proxy variables, so each test sets its own.
    """
    seen: list[tuple[str, int]] = []
    real = socket.create_connection

    def create_connection(address, *args, **kwargs):
        seen.append(address)
        if address[0] != "127.0.0.1":
            raise OSError("no direct internet access in tests")
        return real(address, *args, **kwargs)

    monkeypatch.setattr(network.socket, "create_connection", create_connection)
    return seen


@pytest.fixture
def proxy():
    proxies: list[FakeProxy] = []

    def start(reply: bytes = b"HTTP/1.1 200 Connection established\r\n\r\n") -> FakeProxy:
        proxies.append(FakeProxy(reply))
        return proxies[-1]

    yield start
    for p in proxies:
        p.close()


def test_without_a_proxy_the_probe_connects_directly(connections):
    assert network.api_proxy() is None
    assert network.is_online(timeout=1) is False
    assert connections == [(API_HOST, 443)]


def test_the_probe_opens_a_tunnel_through_the_proxy(monkeypatch, connections, proxy):
    fake = proxy()
    monkeypatch.setenv("https_proxy", f"http://127.0.0.1:{fake.port}")
    assert network.is_online(timeout=5) is True
    assert connections == [("127.0.0.1", fake.port)]     # never a direct connection
    request_line, *headers = fake.requests[0].split("\r\n")
    assert request_line == f"CONNECT {API_HOST}:443 HTTP/1.1"
    assert f"Host: {API_HOST}:443" in headers
    assert not any(h.startswith("Proxy-Authorization") for h in headers)


@pytest.mark.parametrize("reply", [
    b"HTTP/1.1 407 Proxy Authentication Required\r\n\r\n",
    b"HTTP/1.1 403 Forbidden\r\n\r\n",
    b"not http at all\r\n\r\n",
    b"",
])
def test_a_refused_tunnel_is_offline(monkeypatch, connections, proxy, reply):
    fake = proxy(reply)
    monkeypatch.setenv("https_proxy", f"http://127.0.0.1:{fake.port}")
    assert network.is_online(timeout=5) is False
    assert connections == [("127.0.0.1", fake.port)]


def test_an_unreachable_proxy_is_offline(monkeypatch, connections):
    with socket.socket() as unused:
        unused.bind(("127.0.0.1", 0))
        port = unused.getsockname()[1]
    monkeypatch.setenv("https_proxy", f"http://127.0.0.1:{port}")
    assert network.is_online(timeout=1) is False
    assert connections == [("127.0.0.1", port)]


def test_credentials_in_the_proxy_url_are_sent_as_basic_auth(monkeypatch, connections, proxy):
    fake = proxy()
    monkeypatch.setenv("https_proxy", f"http://us%40er:p%3Ass@127.0.0.1:{fake.port}")
    assert network.is_online(timeout=5) is True
    expected = base64.b64encode(b"us@er:p:ss").decode()
    assert f"Proxy-Authorization: Basic {expected}" in fake.requests[0].split("\r\n")


def test_a_proxy_without_a_scheme_is_http(monkeypatch, connections, proxy):
    fake = proxy()
    monkeypatch.setenv("https_proxy", f"127.0.0.1:{fake.port}")
    assert network.api_proxy() == f"http://127.0.0.1:{fake.port}"
    assert network.is_online(timeout=5) is True


@pytest.mark.parametrize("no_proxy", [API_HOST, ".muse.ai", "*"])
def test_no_proxy_sends_the_probe_direct(monkeypatch, connections, no_proxy):
    monkeypatch.setenv("https_proxy", "http://127.0.0.1:9")
    monkeypatch.setenv("no_proxy", no_proxy)
    assert network.api_proxy() is None
    assert network.is_online(timeout=1) is False
    assert connections == [(API_HOST, 443)]


@pytest.mark.parametrize("url", ["socks5://127.0.0.1:1080", "https://127.0.0.1:3128"])
def test_an_unsupported_proxy_is_offline_without_connecting(monkeypatch, connections, url):
    monkeypatch.setenv("https_proxy", url)
    assert network.proxy_supported(network.api_proxy()) is False
    assert network.is_online(timeout=1) is False
    assert connections == []


def test_a_proxy_without_a_port_is_offline_without_connecting(monkeypatch, connections):
    # urllib's HTTPS requests would use port 443 and websockets port 80.
    monkeypatch.setenv("https_proxy", "http://127.0.0.1")
    assert network.is_online(timeout=1) is False
    assert connections == []


@pytest.mark.parametrize("url,shown", [
    ("http://user:secret@proxy.example:3128", "http://proxy.example:3128"),
    ("http://proxy.example", "http://proxy.example"),
    ("http://user:secret@[2001:db8::1]:8080", "http://[2001:db8::1]:8080"),
])
def test_proxy_display_never_shows_credentials(url, shown):
    assert network.proxy_display(url) == shown
