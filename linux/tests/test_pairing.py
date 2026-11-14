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

import base64
import hashlib
import json
from pathlib import Path

import pytest
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from musegadget import pairing
from musegadget.pairing import PairingError, PairingSession, PairingState

VECTORS = json.loads(
    (Path(__file__).parent / "vectors" / "link_pairing_v5.json").read_text()
)["vectors"]
APP = next(v for v in VECTORS if v["name"] == "community_app_v5")
# The same device offering a new pairing's node id, gadget-000001.
APP_GADGET = next(v for v in VECTORS if v["name"] == "community_app_gadget_v5")


def b64(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def unb64(text: str) -> bytes:
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


class FakeClock:
    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now


class Mobile:
    """Mobile side of the record layer, written from the contract, not the device code."""

    def __init__(self, vector: dict) -> None:
        self.tx = AESGCM(bytes.fromhex(vector["mobile_tx_key_hex"]))
        self.rx = AESGCM(bytes.fromhex(vector["mobile_rx_key_hex"]))
        self.session_id = vector["session_id"]
        self.tx_counter = 0

    def _nonce(self, direction: int, counter: int) -> bytes:
        return bytes([direction]) + b"\0\0\0" + counter.to_bytes(8, "big")

    def _aad(self, arrow: str, counter: int) -> bytes:
        return f"hatch-link ble setup v1|{self.session_id}|{arrow}|{counter}".encode()

    def seal(self, command: dict, counter: int | None = None) -> dict:
        if counter is None:
            counter = self.tx_counter
            self.tx_counter += 1
        sealed = self.tx.encrypt(
            self._nonce(0, counter), json.dumps(command).encode(), self._aad("m2d", counter)
        )
        return {
            "action": "pairing_encrypted",
            "session_id": self.session_id,
            "counter": str(counter),
            "ciphertext": b64(sealed[:-16]),
            "tag": b64(sealed[-16:]),
        }

    def open(self, envelope: dict) -> dict:
        assert envelope["type"] == "pairing_encrypted"
        assert envelope["session_id"] == self.session_id
        counter = int(envelope["counter"])
        plain = self.rx.decrypt(
            self._nonce(1, counter),
            unb64(envelope["ciphertext"]) + unb64(envelope["tag"]),
            self._aad("d2m", counter),
        )
        return json.loads(plain)


def make_device(
    vector: dict = APP, clock: FakeClock | None = None, sdk_token: str | None = None,
) -> PairingSession:
    scalar = int(vector["device_private_scalar_hex"], 16)
    return PairingSession(
        node_id=vector["node_id"],
        device_id=vector["device_id"],
        mac=vector["mac"],
        firmware_version=vector["firmware_version"],
        sdk_token=sdk_token,
        clock=clock or FakeClock(),
        generate_key=lambda: ec.derive_private_key(scalar, ec.SECP256R1()),
        random_bytes=lambda n: unb64(vector["device_nonce"]),
    )


def hello(vector: dict = APP, **overrides) -> dict:
    message = {
        "action": "pairing_client_hello",
        "version": 5,
        "pairing_auth": "none",
        "pairing_policy": "confirm_app",
        "mobile_pub": vector["mobile_pub"],
        "mobile_nonce": vector["mobile_nonce"],
    }
    message.update(overrides)
    return message


def client_finished_record(vector: dict = APP) -> dict:
    return {
        "action": "pairing_encrypted",
        "session_id": vector["session_id"],
        "counter": "0",
        "ciphertext": vector["client_finished_ciphertext"],
        "tag": vector["client_finished_tag"],
    }


def confirmed_device(clock: FakeClock | None = None) -> tuple[PairingSession, Mobile, int]:
    device = make_device(clock=clock)
    device.handle_hello(hello())
    command = json.loads(device.decrypt(client_finished_record()))
    generation = device.handle_client_finished(command)
    mobile = Mobile(APP)
    mobile.tx_counter = 1
    return device, mobile, generation


# --- Vector conformance --------------------------------------------------------


@pytest.mark.parametrize("vector", VECTORS, ids=lambda v: v["name"])
def test_transcript_matches_every_vector(vector):
    transcript = pairing.build_transcript(
        community=vector["community"],
        auth_epoch=vector["pairing_auth_epoch"],
        policy=vector["pairing_policy"],
        device_id=vector["device_id"],
        node_id=vector["node_id"],
        mac=vector["mac"],
        firmware_version=vector["firmware_version"],
        mobile_pub=vector["mobile_pub"],
        device_pub=vector["device_pub"],
        mobile_nonce=vector["mobile_nonce"],
        device_nonce=vector["device_nonce"],
    )
    assert transcript == vector["transcript"]
    assert b64(hashlib.sha256(transcript.encode()).digest()) == vector["transcript_hash"]


@pytest.mark.parametrize("vector", VECTORS, ids=lambda v: v["name"])
def test_key_schedule_matches_every_vector(vector):
    tx, rx, session_id = pairing.derive_session_keys(
        bytes.fromhex(vector["ecdh_secret_hex"]),
        unb64(vector["mobile_nonce"]),
        unb64(vector["device_nonce"]),
        unb64(vector["transcript_hash"]),
    )
    assert tx.hex() == vector["mobile_tx_key_hex"]
    assert rx.hex() == vector["mobile_rx_key_hex"]
    assert b64(session_id) == vector["session_id"]


def test_transcript_rejects_app_policy_for_official_devices():
    with pytest.raises(ValueError):
        pairing.build_transcript(
            community=False, auth_epoch=1, policy="confirm_app",
            device_id="d", node_id="n", mac="m", firmware_version="1",
            mobile_pub="a", device_pub="b", mobile_nonce="c", device_nonce="e",
        )


def test_device_info_advertises_community_app_confirmation():
    info = make_device().device_info()
    assert info == {
        "device_id": APP["device_id"],
        "mac": APP["mac"],
        "model": "hatch_link",
        "pairing_protocol": 5,
        "pairing_auth": "none",
        "pairing_auth_epoch": 0,
        "pairing_policy": "confirm_app",
    }


def test_hello_produces_the_vector_pairing_ready():
    ready = make_device().handle_hello(hello())
    assert ready == {
        "type": "pairing_ready",
        "version": 5,
        "device_id": APP["device_id"],
        "node_id": APP["node_id"],
        "mac": APP["mac"],
        "model": "hatch_link",
        "firmware_version": APP["firmware_version"],
        "pairing_auth": "none",
        "pairing_auth_epoch": 0,
        "pairing_policy": "confirm_app",
        "device_pub": APP["device_pub"],
        "device_nonce": APP["device_nonce"],
        "transcript_hash": APP["transcript_hash"],
        "session_id": APP["session_id"],
    }


def test_full_app_confirmed_handshake():
    device = make_device()
    device.handle_hello(hello())
    assert device.state is PairingState.WAIT_CLIENT_FINISHED

    plaintext = device.decrypt(client_finished_record())
    assert plaintext == APP["client_finished_plaintext"]

    generation = device.handle_client_finished(json.loads(plaintext))
    assert generation
    assert device.confirmed

    mobile = Mobile(APP)
    confirmed = mobile.open(device.encrypt_status("pairing_confirmed", generation))
    assert confirmed == {"type": "status", "status": "pairing_confirmed"}

    scan = device.decrypt(mobile.seal({"action": "wifi_scan"}, counter=1))
    assert json.loads(scan) == {"action": "wifi_scan"}


def test_pairing_confirmed_carries_the_sdk_token():
    token = "mgst_" + "A" * 43
    device = make_device(sdk_token=token)
    device.handle_hello(hello())
    generation = device.handle_client_finished(json.loads(device.decrypt(client_finished_record())))
    mobile = Mobile(APP)
    assert mobile.open(device.encrypt_status("pairing_confirmed", generation)) == {
        "type": "status", "status": "pairing_confirmed", "sdk_token": token,
    }
    assert mobile.open(device.encrypt_status("wifi_connecting", generation)) == {
        "type": "status", "status": "wifi_connecting",
    }


def test_device_records_count_up_from_zero():
    device, mobile, generation = confirmed_device()
    first = device.encrypt_status("pairing_confirmed", generation)
    second = device.encrypt_json('{"type":"wifi_scan_result","networks":[]}', generation)
    assert (first["counter"], second["counter"]) == ("0", "1")
    assert mobile.open(second) == {"type": "wifi_scan_result", "networks": []}


# --- Hello validation -----------------------------------------------------------


@pytest.mark.parametrize(
    "overrides",
    [
        {"version": 4},
        {"version": "5"},
        {"version": True},
        {"pairing_auth": "fleet_ecdsa_p256_v1"},
        {"pairing_policy": "confirm_press"},
        {"pairing_policy": None},
        {"mobile_pub": ""},
        {"mobile_pub": b64(b"\x04" + b"\0" * 64)},
        {"mobile_pub": b64(b"\x02" + b"\0" * 32)},
        {"mobile_pub": APP["mobile_pub"] + "="},
        {"mobile_nonce": b64(b"\0" * 15)},
        {"mobile_nonce": 7},
    ],
)
def test_hello_rejects_invalid_input(overrides):
    device = make_device()
    with pytest.raises(PairingError) as err:
        device.handle_hello(hello(**overrides))
    assert err.value.status == "error_pairing_invalid_hello"
    assert device.state is PairingState.IDLE


def test_new_hello_replaces_the_previous_session():
    device, mobile, generation = confirmed_device()
    device.handle_hello(hello())
    assert not device.is_current(generation)
    assert device.state is PairingState.WAIT_CLIENT_FINISHED


# --- Record layer ---------------------------------------------------------------


def expect_decrypt_failure(device: PairingSession, envelope: dict) -> None:
    with pytest.raises(PairingError) as err:
        device.decrypt(envelope)
    assert err.value.status == "error_pairing_decrypt"
    assert device.state is PairingState.IDLE


def test_replayed_record_clears_the_session():
    device, mobile, _ = confirmed_device()
    record = mobile.seal({"action": "wifi_scan"})
    device.decrypt(record)
    expect_decrypt_failure(device, record)


def test_skipped_counter_clears_the_session():
    device, mobile, _ = confirmed_device()
    expect_decrypt_failure(device, mobile.seal({"action": "wifi_scan"}, counter=2))


@pytest.mark.parametrize("counter", ["-1", "+1", " 1", "1.0", "", "18446744073709551616", 1])
def test_malformed_counter_clears_the_session(counter):
    device, mobile, _ = confirmed_device()
    record = mobile.seal({"action": "wifi_scan"})
    record["counter"] = counter
    expect_decrypt_failure(device, record)


def test_tampered_ciphertext_clears_the_session():
    device, mobile, _ = confirmed_device()
    record = mobile.seal({"action": "wifi_scan"})
    raw = bytearray(unb64(record["ciphertext"]))
    raw[0] ^= 1
    record["ciphertext"] = b64(bytes(raw))
    expect_decrypt_failure(device, record)


def test_wrong_session_id_clears_the_session():
    device, mobile, _ = confirmed_device()
    record = mobile.seal({"action": "wifi_scan"})
    record["session_id"] = b64(b"\0" * 16)
    expect_decrypt_failure(device, record)


def test_short_tag_clears_the_session():
    device, mobile, _ = confirmed_device()
    record = mobile.seal({"action": "wifi_scan"})
    record["tag"] = record["tag"][:-2]
    expect_decrypt_failure(device, record)


def test_decrypt_without_session_fails():
    expect_decrypt_failure(make_device(), client_finished_record())


# --- Client finished ------------------------------------------------------------


@pytest.mark.parametrize(
    "command",
    [
        {"action": "wifi_scan"},
        {"action": "pairing_client_finished", "extra": True},
        {"action": "provision_v2", "ssid": "x"},
    ],
)
def test_first_record_must_be_exact_client_finished(command):
    device = make_device()
    device.handle_hello(hello())
    plaintext = device.decrypt(Mobile(APP).seal(command))
    assert device.handle_client_finished(json.loads(plaintext)) == 0
    assert device.state is PairingState.IDLE


def test_client_finished_only_counts_as_the_first_record():
    device, mobile, _ = confirmed_device()
    plaintext = device.decrypt(mobile.seal({"action": "pairing_client_finished"}))
    assert device.handle_client_finished(json.loads(plaintext)) == 0
    assert device.state is PairingState.IDLE


# --- Timeouts and generations ---------------------------------------------------


def test_client_finished_times_out_after_60_seconds():
    clock = FakeClock()
    device = make_device(clock=clock)
    device.handle_hello(hello())
    clock.now += 60.5
    expect_decrypt_failure(device, client_finished_record())


def test_confirmed_session_expires_after_120_seconds():
    clock = FakeClock()
    device, mobile, generation = confirmed_device(clock)
    clock.now += 119
    assert device.confirmed
    clock.now += 2
    assert not device.confirmed
    assert device.encrypt_status("wifi_connected") is None
    assert device.is_current(generation)


def test_stale_generation_cannot_encrypt():
    device, mobile, generation = confirmed_device()
    provisioning = device.mark_provisioning()
    assert provisioning and provisioning != generation
    assert device.encrypt_status("wifi_connecting", generation) is None
    assert mobile.open(device.encrypt_status("wifi_connecting", provisioning)) == {
        "type": "status",
        "status": "wifi_connecting",
    }


def test_provisioning_requires_confirmation():
    device = make_device()
    device.handle_hello(hello())
    assert device.mark_provisioning() == 0


def test_commit_runs_only_for_the_current_provisioning_session():
    clock = FakeClock()
    device, _, _ = confirmed_device(clock)
    generation = device.mark_provisioning()
    committed = []
    assert not device.commit_provisioning(generation + 1, lambda: committed.append(1) or True)
    assert device.commit_provisioning(generation, lambda: committed.append(2) or True)
    assert committed == [2]

    clock.now += 119
    assert device.extend_provisioning(generation)
    clock.now += 119
    assert device.commit_provisioning(generation, lambda: True)
    clock.now += 2
    assert not device.commit_provisioning(generation, lambda: True)


# --- Encoding helpers -----------------------------------------------------------


@pytest.mark.parametrize("text", ["", "a", "ab=c", "ab+c", "ab/c", "a b", 5, None])
def test_b64url_decode_rejects_what_the_firmware_rejects(text):
    with pytest.raises(ValueError):
        pairing.b64url_decode(text)


def test_b64url_round_trip_is_unpadded():
    assert pairing.b64url_encode(b"\xfb\xff") == "-_8"
    assert pairing.b64url_decode("-_8") == b"\xfb\xff"
