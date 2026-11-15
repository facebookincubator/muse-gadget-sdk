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

import json
import sys
import threading
import types

import pytest

from musegadget import cli, config
from musegadget import pair as pair_module
from musegadget.pair import PairOutcome, PairResult, SetupWindow, pair


class FakeBleServer:
    def __init__(self, name, *, on_write, on_disconnect):
        self.name = name
        self.stopped = threading.Event()

    def run(self):
        self.stopped.wait(5)

    def stop(self):
        self.stopped.set()


class CompletingController:
    def __init__(self, *, transport, on_complete, **kwargs):
        self.on_complete = on_complete
        self.transport = transport

    def start(self):
        self.on_complete()

    def stop(self):
        pass


class SilentController(CompletingController):
    def start(self):
        self.transport.stop()


@pytest.fixture
def state(tmp_path, monkeypatch):
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path))
    monkeypatch.delenv(config.SDK_TOKEN_ENV, raising=False)
    monkeypatch.setitem(sys.modules, "musegadget.ble_server",
                        types.SimpleNamespace(BleServer=FakeBleServer))
    monkeypatch.setattr(pair_module, "BLE_SHUTDOWN_DELAY_S", 0)
    (tmp_path / config.IDENTITY_FILE).write_text(json.dumps({"mac": "02:00:00:ab:cd:ef"}))
    return tmp_path


def test_a_paired_device_is_left_alone_unless_forced(state, monkeypatch, capsys):
    (state / config.PAIRING_FILE).write_text('{"access_token": "kept"}')
    monkeypatch.setattr(pair_module, "SetupController", CompletingController)

    assert cli.main(["pair", "--timeout", "60"]) == 1
    assert capsys.readouterr().err == "Already paired. Run `musegadget unpair` first, or pass --force.\n"
    assert cli.main(["pair", "--force", "--timeout", "60"]) == 0


def test_an_invalid_sdk_token_stops_pairing_before_setup_opens(state, monkeypatch, capsys):
    monkeypatch.setenv(config.SDK_TOKEN_ENV, "not-a-token")
    monkeypatch.setattr(pair_module, "SetupController", CompletingController)

    assert cli.main(["pair", "--timeout", "60"]) == 1
    output = capsys.readouterr()
    assert output.err == "Can't pair: the SDK token is not valid; copy it again from gadgets.muse.ai.\n"
    assert output.out == ""


def test_completed_setup_reports_paired(state, monkeypatch, capsys):
    monkeypatch.setattr(pair_module, "SetupController", CompletingController)

    assert cli.main(["pair", "--timeout", "120"]) == 0
    output = capsys.readouterr()
    assert output.out == ("Setup open for 2 minutes. In the Muse app, add a device\n"
                          "and choose MuseGadgetABCDEF.\nPaired.\n")
    assert output.err == ("No SDK token yet. Get one at gadgets.muse.ai and run "
                          "`bash install.sh --sdk-token mgst_…`; gadgets without one will stop pairing.\n")


def test_a_setup_window_that_closes_unpaired_says_so(state, monkeypatch, capsys):
    monkeypatch.setattr(pair_module, "SetupController", SilentController)

    assert cli.main(["pair", "--timeout", "60"]) == 1
    assert capsys.readouterr().err.endswith("Setup window closed without pairing.\n")


def test_pair_prints_nothing_and_hands_the_setup_window_to_the_caller(state, monkeypatch, capsys):
    monkeypatch.setattr(pair_module, "SetupController", CompletingController)
    windows = []

    assert pair(on_open=windows.append, timeout_s=120) == PairResult(PairOutcome.PAIRED)
    assert windows == [SetupWindow("MuseGadgetABCDEF", 120, has_sdk_token=False)]
    (state / config.PAIRING_FILE).write_text('{"access_token": "kept"}')
    assert pair(on_open=windows.append, timeout_s=120) == PairResult(PairOutcome.ALREADY_PAIRED)
    monkeypatch.setenv(config.SDK_TOKEN_ENV, "not-a-token")
    assert pair(on_open=windows.append, force=True, timeout_s=120) == PairResult(
        PairOutcome.INVALID_SDK_TOKEN,
        sdk_token_problem="the SDK token is not valid; copy it again from gadgets.muse.ai")
    assert len(windows) == 1
    assert capsys.readouterr() == ("", "")


def test_the_pairing_remembers_the_sdk_token_it_presented(state, monkeypatch):
    monkeypatch.setattr(pair_module.muse_api, "fetch_vms_with_status",
                        lambda *args: ([{"vm_id": "vm"}], 200))
    credentials = types.SimpleNamespace(
        access_token="a", refresh_token="r", username="u", noise_host="",
        api_url="https://api.example", api_url_v2="https://api.example")

    pair_module._verify_and_save(credentials, lambda save: save(), "mgst_AbC1234token")
    assert config.load_json(config.PAIRING_FILE)["sdk_token_hint"] == "mgst_AbC1234"
    pair_module._verify_and_save(credentials, lambda save: save())
    assert config.load_json(config.PAIRING_FILE)["sdk_token_hint"] == "none"


class CapturingController(CompletingController):
    """Records what pair() hands the setup controller."""

    seen: dict = {}

    def __init__(self, *, pairing, identity, provision, **kwargs):
        super().__init__(**kwargs)
        CapturingController.seen = {"pairing": pairing, "identity": identity, "provision": provision}


def _credentials(access: str = "a"):
    return types.SimpleNamespace(
        access_token=access, refresh_token="r", username="u", noise_host="",
        api_url="https://api.example", api_url_v2="https://api.example")


@pytest.mark.parametrize("gadget_ids, offered", [("1", "gadget-abcdef"), (None, "homelink-abcdef")])
def test_a_new_pairing_offers_and_stores_its_node_id(state, monkeypatch, gadget_ids, offered):
    # A new pairing offers gadget- + the identity's six hex digits in
    # get_device_info and the transcript, and stores it in the same write as
    # the tokens it was issued for.
    # Off (the default), the offer is homelink- as before; stored either way.
    if gadget_ids:
        monkeypatch.setenv(config.GADGET_NODE_ID_ENV, gadget_ids)
    monkeypatch.setattr(pair_module, "SetupController", CapturingController)
    monkeypatch.setattr(pair_module.muse_api, "fetch_vms_with_status",
                        lambda *args: ([{"vm_id": "vm"}], 200))
    assert pair(on_open=lambda window: None, timeout_s=60).outcome is PairOutcome.PAIRED
    seen = CapturingController.seen
    assert seen["identity"].setup_node_id == offered
    seen["provision"](_credentials(), lambda save: save())
    saved = config.load_json(config.PAIRING_FILE)
    assert saved["node_id"] == offered and saved["access_token"] == "a"


def test_pairing_again_replaces_a_homelink_node_id(state, monkeypatch, capsys):
    # A pairing made before ids were stored keeps homelink- until the device is
    # paired again; the new pairing stores the gadget id with its tokens.
    (state / config.PAIRING_FILE).write_text(json.dumps({"access_token": "old", "refresh_token": "r"}))
    assert cli.main(["info"]) == 0
    assert "node id:   homelink-abcdef\n" in capsys.readouterr().out
    monkeypatch.setattr(pair_module.muse_api, "fetch_vms_with_status",
                        lambda *args: ([{"vm_id": "vm"}], 200))
    monkeypatch.setenv(config.GADGET_NODE_ID_ENV, "1")
    pair_module._verify_and_save(_credentials("new"), lambda save: save(), None, "gadget-abcdef")
    assert config.load_json(config.PAIRING_FILE)["node_id"] == "gadget-abcdef"
    assert cli.main(["info"]) == 0
    assert "node id:   gadget-abcdef\n" in capsys.readouterr().out
    assert cli.main(["unpair"]) == 0
    capsys.readouterr()
    assert cli.main(["info"]) == 0
    assert "node id:   gadget-abcdef (when paired)\n" in capsys.readouterr().out
