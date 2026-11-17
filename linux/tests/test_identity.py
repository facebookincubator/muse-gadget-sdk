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

from __future__ import annotations

import os
import stat

from musegadget import config, identity


def test_names_share_the_suffix(monkeypatch):
    ident = identity.Identity("02:00:00:ab:cd:ef")
    # Gadget ids off (the default): new pairings offer homelink- as before.
    monkeypatch.delenv(config.GADGET_NODE_ID_ENV, raising=False)
    assert ident.node_id == "homelink-abcdef"   # not paired: a new pairing's
    assert ident.setup_node_id == "homelink-abcdef"
    monkeypatch.setenv(config.GADGET_NODE_ID_ENV, "1")
    assert ident.node_id == "gadget-abcdef"
    assert ident.setup_node_id == "gadget-abcdef"
    assert ident.legacy_node_id == "homelink-abcdef"
    assert ident.device_id == "hatch-link:02:00:00:ab:cd:ef"
    assert ident.ble_name == "MuseGadgetABCDEF"


def test_generated_mac_is_locally_administered_unicast():
    mac = identity.generate_mac(lambda n: b"\xff" * n)
    first = int(mac.split(":")[0], 16)
    assert first & 0x01 == 0
    assert first & 0x02 == 2


def test_identity_persists(tmp_path):
    first = identity.load_or_create(tmp_path)
    assert identity.load_or_create(tmp_path) == first
    mode = stat.S_IMODE(os.stat(tmp_path / config.IDENTITY_FILE).st_mode)
    assert mode == 0o600


def test_corrupt_identity_is_replaced(tmp_path):
    (tmp_path / config.IDENTITY_FILE).write_text('{"mac": "nope"}')
    ident = identity.load_or_create(tmp_path, lambda n: bytes(range(n)))
    assert ident.mac == "02:01:02:03:04:05"


def test_the_node_id_is_the_pairings(monkeypatch):
    monkeypatch.setenv(config.GADGET_NODE_ID_ENV, "1")
    # The id stored with the tokens is the one in use, a homelink- one
    # included; a pairing made before ids were stored keeps homelink-.
    ident = identity.Identity("02:00:00:ab:cd:ef")
    assert ident.for_pairing({"node_id": "gadget-abcdef"}).node_id == "gadget-abcdef"
    assert ident.for_pairing({"node_id": "homelink-abcdef"}).node_id == "homelink-abcdef"
    assert ident.for_pairing({"access_token": "a"}).node_id == "homelink-abcdef"
    for bad in (None, "", 7, "gadget-ABCDEF", "gadget-abc", "gadget-abcdef\n"):
        assert ident.for_pairing({"node_id": bad}).node_id == "homelink-abcdef"
    assert ident.for_pairing(None).node_id == "gadget-abcdef"
    assert ident.for_pairing({"node_id": "homelink-abcdef"}) == ident   # the same device


def test_load_reads_the_pairings_node_id(tmp_path):
    first = identity.load_or_create(tmp_path)
    assert first.node_id == first.setup_node_id
    config.save_json(config.PAIRING_FILE, {"access_token": "a"}, tmp_path)
    assert identity.load_or_create(tmp_path).node_id == first.legacy_node_id
    config.save_json(config.PAIRING_FILE, {"access_token": "a", "node_id": first.setup_node_id}, tmp_path)
    assert identity.load_or_create(tmp_path).node_id == first.setup_node_id
