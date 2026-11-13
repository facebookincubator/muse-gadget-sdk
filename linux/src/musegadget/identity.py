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

"""Stable device identity.

The identity is a random, locally administered MAC-shaped value generated
once and kept across upgrades and unpairing. It is not read from a network
interface, so it works on machines without Wi-Fi and never exposes a real
hardware address. The derived names follow the Muse Gadget conventions the
apps expect: the node id and the BLE name end in the same six hex digits.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from musegadget import config

NODE_ID_PREFIX = "homelink-"
BLE_NAME_PREFIX = "MuseGadget"
_MAC_RE = re.compile(r"[0-9a-f]{2}(:[0-9a-f]{2}){5}")


@dataclass(frozen=True)
class Identity:
    mac: str

    @property
    def suffix(self) -> str:
        return self.mac.replace(":", "")[-6:]

    @property
    def node_id(self) -> str:
        return NODE_ID_PREFIX + self.suffix

    @property
    def device_id(self) -> str:
        return "hatch-link:" + self.mac

    @property
    def ble_name(self) -> str:
        # No separator: the apps compare the text after the prefix with the
        # text after "homelink-" in the node id.
        return BLE_NAME_PREFIX + self.suffix.upper()


def generate_mac(random_bytes: Callable[[int], bytes] = os.urandom) -> str:
    octets = bytearray(random_bytes(6))
    octets[0] = (octets[0] & 0xFC) | 0x02  # unicast, locally administered
    return ":".join(f"{b:02x}" for b in octets)


def load_or_create(
    directory: Path | None = None,
    random_bytes: Callable[[int], bytes] = os.urandom,
) -> Identity:
    stored = config.load_json(config.IDENTITY_FILE, directory) or {}
    mac = stored.get("mac")
    if isinstance(mac, str) and _MAC_RE.fullmatch(mac):
        return Identity(mac)
    identity = Identity(generate_mac(random_bytes))
    config.save_json(config.IDENTITY_FILE, {"mac": identity.mac}, directory)
    return identity
