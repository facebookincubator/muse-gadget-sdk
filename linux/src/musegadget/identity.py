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

Node ids: a new pairing offers ``gadget-xxxxxx`` when
``MUSEGADGET_GADGET_NODE_ID=1`` (off by default), else ``homelink-xxxxxx``,
and stores it with the device tokens, which are issued for it. The stored id
is the one in use until the device is paired again; a pairing made before ids
were stored keeps ``homelink-xxxxxx``, the id its tokens were issued for. A
stored id is never renamed.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Callable

from musegadget import config

NODE_ID_PREFIX = "gadget-"
# The node ids new pairings offer while gadget ids are off, and those of
# pairings made before ids were stored, which keep them until the device is
# paired again.
LEGACY_NODE_ID_PREFIX = "homelink-"
BLE_NAME_PREFIX = "MuseGadget"
_MAC_RE = re.compile(r"[0-9a-f]{2}(:[0-9a-f]{2}){5}")
_NODE_ID_RE = re.compile(r"[a-z]+-[0-9a-f]{6}")


@dataclass(frozen=True)
class Identity:
    mac: str
    # The node id of the pairing this identity was loaded with (for_pairing()).
    paired: str | None = field(default=None, compare=False, repr=False)

    @property
    def suffix(self) -> str:
        return self.mac.replace(":", "")[-6:]

    @property
    def node_id(self) -> str:
        """The ID Muse knows the device by: its pairing's, else a new pairing's."""
        return self.paired or self.setup_node_id

    @property
    def setup_node_id(self) -> str:
        """The node id a new pairing offers and stores (config.gadget_node_ids())."""
        prefix = NODE_ID_PREFIX if config.gadget_node_ids() else LEGACY_NODE_ID_PREFIX
        return prefix + self.suffix

    @property
    def legacy_node_id(self) -> str:
        return LEGACY_NODE_ID_PREFIX + self.suffix

    def paired_node_id(self, pairing: dict) -> str:
        """The node id ``pairing`` was made for.

        The id stored with its tokens, or, for a pairing made before ids were
        stored, the ``homelink-`` id its tokens were issued for.
        """
        stored = pairing.get("node_id")
        if isinstance(stored, str) and _NODE_ID_RE.fullmatch(stored):
            return stored
        return self.legacy_node_id

    def for_pairing(self, pairing: dict | None) -> Identity:
        """This identity with ``pairing``'s node id (none: not paired)."""
        return replace(self, paired=self.paired_node_id(pairing) if pairing else None)

    @property
    def device_id(self) -> str:
        return "hatch-link:" + self.mac

    @property
    def ble_name(self) -> str:
        # No separator: the apps compare the text after the prefix with the
        # six hex digits that end the node id.
        return BLE_NAME_PREFIX + self.suffix.upper()


def generate_mac(random_bytes: Callable[[int], bytes] = os.urandom) -> str:
    octets = bytearray(random_bytes(6))
    octets[0] = (octets[0] & 0xFC) | 0x02  # unicast, locally administered
    return ":".join(f"{b:02x}" for b in octets)


def load_or_create(
    directory: Path | None = None,
    random_bytes: Callable[[int], bytes] = os.urandom,
) -> Identity:
    """The device's identity, with the node id of its current pairing, if any."""
    stored = config.load_json(config.IDENTITY_FILE, directory) or {}
    mac = stored.get("mac")
    if isinstance(mac, str) and _MAC_RE.fullmatch(mac):
        identity = Identity(mac)
    else:
        identity = Identity(generate_mac(random_bytes))
        config.save_json(config.IDENTITY_FILE, {"mac": identity.mac}, directory)
    return identity.for_pairing(config.load_json(config.PAIRING_FILE, directory))
