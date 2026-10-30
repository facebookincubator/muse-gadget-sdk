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
import threading
import time

import pytest

from musegadget.ble_framing import ChunkAssembler, encode_chunks
from musegadget.ble_setup import ProvisionFailed, SetupController
from musegadget.identity import Identity
from musegadget.pairing import PairingState

from test_pairing import APP, Mobile, client_finished_record, hello, make_device


class FakeTransport:
    def __init__(self, mtu: int = 185) -> None:
        self._mtu = mtu
        self.messages: list = []
        self.disconnects: list[float] = []
        self._assembler = ChunkAssembler()
        self._lock = threading.Lock()

    def mtu(self) -> int:
        return self._mtu

    def send_packets(self, packets: list[bytes]) -> None:
        with self._lock:
            for packet in packets:
                assert len(packet) <= max(self._mtu - 3, 20)
                message = self._assembler.feed(packet)
                if message is None:
                    continue
                if packet[0] == 0xFE:
                    self.messages.append(json.loads(message))
                else:
                    self.messages.append(message.decode())

    def disconnect(self, delay: float) -> None:
        self.disconnects.append(delay)


class FakeNetwork:
    def __init__(self, online: bool = True) -> None:
        self.online = online

    def is_online(self) -> bool:
        return self.online

    def current_connection_entry(self) -> dict:
        return {"ssid": "HomeNet", "rssi": -40, "secure": False}


IDENTITY = Identity(APP["mac"])


class Harness:
    def __init__(self, online: bool = True, provision=None, mtu: int = 185) -> None:
        self.transport = FakeTransport(mtu)
        self.network = FakeNetwork(online)
        self.pairing = make_device()
        self.saved: list = []
        self.completed = threading.Event()

        def default_provision(credentials, commit):
            if not commit(lambda: self.saved.append(credentials) or True):
                raise ProvisionFailed("error_storage")

        self.controller = SetupController(
            pairing=self.pairing,
            identity=IDENTITY,
            version=APP["firmware_version"],
            transport=self.transport,
            network=self.network,
            provision=provision or default_provision,
            on_complete=self.completed.set,
        )
        self.mobile = Mobile(APP)

    def send(self, obj: dict) -> None:
        self.controller.handle_message(json.dumps(obj).encode())

    def send_encrypted(self, command: dict) -> None:
        self.send(self.mobile.seal(command))

    def pair(self) -> None:
        self.send(hello())
        self.send(client_finished_record())
        self.mobile.tx_counter = 1

    def opened(self) -> list:
        return [self.mobile.open(m) for m in self.transport.messages
                if isinstance(m, dict) and m.get("type") == "pairing_encrypted"]

    def statuses(self) -> list[str]:
        return [m["status"] for m in self.opened() if m.get("type") == "status"]

    def wait_for_status(self, status: str, timeout: float = 2.0) -> None:
        deadline = time.monotonic() + timeout
        while status not in self.statuses():
            assert time.monotonic() < deadline, f"no {status}; got {self.statuses()}"
            time.sleep(0.01)


PROVISION = {
    "action": "provision_v2",
    "ssid": "HomeNet",
    "password": "",
    "access_token": "device-access",
    "refresh_token": "device-refresh",
    "token_type": "device",
    "username": "someone",
    "api_url": "https://legacy-api.example",
    "api_url_v2": "https://api.example",
}


def test_device_info_matches_the_pairing_transcript_fields():
    h = Harness()
    h.send({"action": "get_device_info"})
    info = h.transport.messages[-1]
    assert info["type"] == "device_info"
    assert info["node_id"] == APP["node_id"]
    assert info["version"] == APP["firmware_version"]
    assert info["model"] == "hatch_link"
    assert info["pairing_protocol"] == 5
    assert info["pairing_auth"] == "none"
    assert info["pairing_policy"] == "confirm_app"
    assert info["network_ready"] is True


def test_hello_returns_pairing_ready_in_chunks_at_minimum_mtu():
    h = Harness(mtu=23)
    h.send(hello())
    ready = h.transport.messages[-1]
    assert ready["type"] == "pairing_ready"
    assert ready["session_id"] == APP["session_id"]


def test_full_setup_flow():
    h = Harness()
    h.pair()
    assert h.statuses() == ["pairing_confirmed"]

    h.send_encrypted({"action": "wifi_scan"})
    scan = h.opened()[-1]
    assert scan == {"type": "wifi_scan_result",
                    "networks": [{"ssid": "HomeNet", "rssi": -40, "secure": False}]}

    h.send_encrypted(PROVISION)
    h.wait_for_status("auth_ok")
    assert h.statuses() == ["pairing_confirmed", "wifi_connecting", "wifi_connected", "auth_ok"]
    assert h.completed.is_set()
    saved = h.saved[0]
    assert (saved.access_token, saved.refresh_token, saved.username) == (
        "device-access", "device-refresh", "someone")
    assert (saved.api_url, saved.api_url_v2) == (
        "https://legacy-api.example", "https://api.example")
    assert h.pairing.state is PairingState.PROVISIONING


def test_setup_commands_need_encryption():
    h = Harness()
    h.send({"action": "wifi_scan"})
    h.send({"action": "provision_v2", **PROVISION})
    assert h.transport.messages == ["error_encryption_required", "error_encryption_required"]


def test_plaintext_is_ignored_once_pairing_starts():
    h = Harness()
    h.send(hello())
    h.send({"action": "wifi_scan"})
    assert len(h.transport.messages) == 1


def test_device_info_is_answered_mid_session_and_a_new_hello_restarts():
    h = Harness()
    h.pair()
    h.send({"action": "get_device_info"})
    assert h.transport.messages[-1]["type"] == "device_info"
    assert h.pairing.confirmed
    h.send(hello())
    assert h.transport.messages[-1]["type"] == "pairing_ready"
    assert h.pairing.state is PairingState.WAIT_CLIENT_FINISHED


def test_bad_hello_gets_a_plaintext_error():
    h = Harness()
    h.send(hello(pairing_policy="confirm_press"))
    assert h.transport.messages == ["error_pairing_invalid_hello"]


def test_decrypt_failure_disconnects_without_a_plaintext_reply():
    h = Harness()
    h.send(hello())
    record = client_finished_record()
    record["counter"] = "1"
    h.send(record)
    assert len(h.transport.messages) == 1  # just pairing_ready
    assert h.transport.disconnects == [0.3]
    assert h.pairing.state is PairingState.IDLE


def test_wrong_first_record_means_the_session_can_never_confirm():
    h = Harness()
    h.send(hello())
    h.send_encrypted({"action": "wifi_scan"})
    assert h.statuses() == ["error_pairing_confirm_required"]
    h.send_encrypted({"action": "pairing_client_finished"})
    assert h.transport.disconnects == [0.3]
    assert h.pairing.state is PairingState.IDLE


@pytest.mark.parametrize(
    "change",
    [
        {"refresh_token": ""},
        {"token_type": "user"},
        {"access_token": None},
        {"password": None},
    ],
)
def test_provision_requires_device_tokens(change):
    h = Harness()
    h.pair()
    h.send_encrypted({**PROVISION, **change})
    assert h.statuses()[-1] == "error_missing_credentials"
    assert h.pairing.state is PairingState.READY


def test_offline_device_reports_wifi_failure_and_can_retry():
    h = Harness(online=False)
    h.pair()
    h.send_encrypted({"action": "wifi_scan"})
    assert h.opened()[-1]["networks"] == []

    h.send_encrypted(PROVISION)
    h.wait_for_status("wifi_failed")
    assert not h.transport.disconnects

    h.network.online = True
    h.send_encrypted(PROVISION)
    h.wait_for_status("auth_ok")


def test_rejected_token_reports_auth_failed_and_disconnects():
    def reject(credentials, commit):
        raise ProvisionFailed("auth_failed")

    h = Harness(provision=reject)
    h.pair()
    h.send_encrypted(PROVISION)
    h.wait_for_status("auth_failed")
    assert h.transport.disconnects == [0.5]
    assert not h.completed.is_set()


def test_disconnect_clears_the_session():
    h = Harness()
    h.pair()
    h.controller.on_disconnect()
    assert h.pairing.state is PairingState.IDLE
    h.send({"action": "wifi_scan"})
    assert h.transport.messages[-1] == "error_encryption_required"


def test_writes_are_reassembled_before_dispatch():
    h = Harness()
    h.controller.start()
    try:
        for packet in encode_chunks(json.dumps(hello()).encode(), 23):
            h.controller.on_write(packet)
        deadline = time.monotonic() + 2
        while not h.transport.messages:
            assert time.monotonic() < deadline
            time.sleep(0.01)
    finally:
        h.controller.stop()
    assert h.transport.messages[0]["type"] == "pairing_ready"


def test_provision_refusal_without_session_keys_does_not_deadlock():
    # The session can expire between the dispatcher's confirmed check and
    # mark_provisioning(). The refusal then goes out without keys, and that
    # path of send_status() takes the state lock itself.
    h = Harness()
    h.pair()
    sealed = h.mobile.seal(PROVISION)

    def expire_then_refuse() -> int:
        h.pairing.reset()
        return 0

    h.pairing.mark_provisioning = expire_then_refuse
    worker = threading.Thread(target=h.send, args=(sealed,), daemon=True)
    worker.start()
    worker.join(2.0)
    assert not worker.is_alive(), "provision refusal deadlocked on the state lock"
    assert h.pairing.encrypt_status("probe") is None
