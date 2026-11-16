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

"""Connectivity checks used during setup."""

from __future__ import annotations

import base64
import re
import shutil
import socket
import subprocess
import urllib.parse
import urllib.request
from pathlib import Path

from musegadget.muse_api import API_BASE

CURRENT_CONNECTION_LABEL = "Use current connection"


def _api_host() -> str:
    return API_BASE.split("://", 1)[-1].split("/", 1)[0]


def api_proxy() -> str | None:
    """The proxy URL for HTTPS to the Muse API, or None to connect directly.

    The same rule urllib and websockets (15 and later) apply: ``https_proxy``
    from the environment, unless ``no_proxy`` covers the host. A URL without
    a scheme is an http:// one, as urllib reads it.
    """
    proxy = urllib.request.getproxies().get("https")
    if not proxy or urllib.request.proxy_bypass(_api_host()):
        return None
    return proxy if "://" in proxy else f"http://{proxy}"


def proxy_supported(proxy: str) -> bool:
    """True for an http:// proxy, the only kind every connection here can use."""
    return urllib.parse.urlsplit(proxy).scheme.lower() == "http"


def proxy_display(proxy: str) -> str:
    """The proxy URL without any user name or password, safe to print."""
    parts = urllib.parse.urlsplit(proxy)
    host = parts.hostname or ""
    if ":" in host:
        host = f"[{host}]"
    try:
        port = parts.port
    except ValueError:
        port = None
    return f"{parts.scheme}://{host}" + (f":{port}" if port else "")


def _tunnel_opens(proxy: str, host: str, timeout: float) -> bool:
    """True if the proxy answers 2xx to an HTTP CONNECT for host:443."""
    parts = urllib.parse.urlsplit(proxy)
    try:
        port = parts.port or 80   # urllib's default for a proxy without a port
    except ValueError:
        return False
    if parts.scheme.lower() != "http" or not parts.hostname:
        return False
    request = f"CONNECT {host}:443 HTTP/1.1\r\nHost: {host}:443\r\n"
    if parts.username is not None:
        # urllib sends Basic credentials from the proxy URL the same way.
        user_pass = (f"{urllib.parse.unquote(parts.username)}:"
                     f"{urllib.parse.unquote(parts.password or '')}")
        token = base64.b64encode(user_pass.encode()).decode("ascii")
        request += f"Proxy-Authorization: Basic {token}\r\n"
    request += "\r\n"
    try:
        with socket.create_connection((parts.hostname, port), timeout=timeout) as sock:
            sock.sendall(request.encode())
            with sock.makefile("rb") as reply:
                status_line = reply.readline(1024)
    except OSError:
        return False
    fields = status_line.split()
    return (len(fields) >= 2 and fields[0].startswith(b"HTTP/")
            and len(fields[1]) == 3 and fields[1].startswith(b"2"))


def is_online(timeout: float = 5.0) -> bool:
    """True if the Muse API host is reachable on port 443.

    Through the proxy when one applies (see ``api_proxy``), since the API and
    the Muse connection go through it too; otherwise with a direct TCP
    connection.
    """
    host = _api_host()
    proxy = api_proxy()
    if proxy is not None:
        return _tunnel_opens(proxy, host, timeout)
    try:
        with socket.create_connection((host, 443), timeout=timeout):
            return True
    except OSError:
        return False


def _ssid_from_nmcli() -> str | None:
    """SSID of the active Wi-Fi connection, via NetworkManager if present."""
    if not shutil.which("nmcli"):
        return None
    try:
        out = subprocess.run(
            ["nmcli", "-t", "-f", "ACTIVE,SSID", "device", "wifi", "list", "--rescan", "no"],
            capture_output=True, text=True, errors="replace", timeout=5, check=False,
        ).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in out.splitlines():
        # ACTIVE is yes or no, so the first colon ends it. In terse mode nmcli
        # escapes the SSID's colons and backslashes with a backslash.
        active, _, ssid = line.partition(":")
        if active == "yes" and ssid:
            return re.sub(r"\\(.)", r"\1", ssid)
    return None


def _wireless_interfaces() -> list[str]:
    """Interface names that have a wireless sysfs node, e.g. ``["wlan0"]``."""
    try:
        return sorted(
            p.name for p in Path("/sys/class/net").iterdir()
            if (p / "wireless").is_dir()
        )
    except OSError:
        return []


_WPA_ESCAPE = re.compile(r'\\(x[0-9a-fA-F]{2}|[\\"enrt])')
_WPA_SIMPLE = {"\\": 0x5C, '"': 0x22, "e": 0x1B, "n": 0x0A, "r": 0x0D, "t": 0x09}


def _wpa_unescape(text: str) -> str:
    """An SSID as wpa_cli prints it, decoded.

    wpa_supplicant's printf_encode() writes the SSID's bytes as \\xNN, except
    printable ASCII, and \\", \\\\, \\e, \\n, \\r and \\t. SSIDs are bytes,
    nearly always UTF-8; others get replacement characters.
    """
    raw = bytearray()
    pos = 0
    for m in _WPA_ESCAPE.finditer(text):
        raw += text[pos:m.start()].encode()
        esc = m.group(1)
        raw.append(int(esc[1:], 16) if esc[0] == "x" else _WPA_SIMPLE[esc])
        pos = m.end()
    raw += text[pos:].encode()
    return raw.decode("utf-8", errors="replace")


def _ssid_from_wpa_cli() -> str | None:
    """SSID from wpa_supplicant, for hosts NetworkManager does not manage.

    netplan and systemd-networkd drive wpa_supplicant directly, so every
    device reads as ``unmanaged`` to nmcli and the query above finds nothing
    even while Wi-Fi is associated. wpa_cli needs root, which the service has.
    """
    if not shutil.which("wpa_cli"):
        return None
    for iface in _wireless_interfaces():
        try:
            out = subprocess.run(
                ["wpa_cli", "-i", iface, "status"],
                capture_output=True, text=True, errors="replace", timeout=5, check=False,
            ).stdout
        except (OSError, subprocess.TimeoutExpired):
            continue
        state = ssid = ""
        for line in out.splitlines():
            key, _, value = line.partition("=")
            if key == "wpa_state":
                state = value.strip()
            elif key == "ssid":
                # Not stripped: an SSID can start or end with a space.
                ssid = _wpa_unescape(value)
        if ssid and state == "COMPLETED":
            return ssid
    return None


def active_wifi_ssid() -> str | None:
    """SSID of the active Wi-Fi connection, via NetworkManager or wpa_supplicant."""
    return _ssid_from_nmcli() or _ssid_from_wpa_cli()


def current_connection_entry() -> dict:
    """The single scan entry offered when the device is already online.

    It is marked open so the apps skip the password field. The device ignores
    whatever credentials come back. Using the real SSID lets the app preselect
    it when the phone is on the same network.
    """
    return {
        "ssid": active_wifi_ssid() or CURRENT_CONNECTION_LABEL,
        "rssi": -40,
        "secure": False,
    }
