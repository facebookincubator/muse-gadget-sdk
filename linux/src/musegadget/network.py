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

import re
import shutil
import socket
import subprocess
from pathlib import Path

from musegadget.muse_api import API_BASE

CURRENT_CONNECTION_LABEL = "Use current connection"


def is_online(timeout: float = 5.0) -> bool:
    """True if the Muse API host accepts a TCP connection."""
    host = API_BASE.split("://", 1)[-1].split("/", 1)[0]
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
            capture_output=True, text=True, timeout=5, check=False,
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
                capture_output=True, text=True, timeout=5, check=False,
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
