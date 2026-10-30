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

"""BLE setup commands: the protocol logic behind the GATT characteristics.

Independent of BlueZ: a transport delivers reassembled writes and sends
framed notifications. Messages are handled one at a time on a worker thread,
in arrival order, and every send happens under one lock so encrypted record
counters reach the phone in order.
"""

from __future__ import annotations

import json
import logging
import queue
import threading
from dataclasses import dataclass
from typing import Callable, Protocol

from musegadget.ble_framing import ChunkAssembler, encode_chunks
from musegadget.identity import Identity
from musegadget.pairing import PairingError, PairingSession

log = logging.getLogger(__name__)

SENSITIVE_ACTIONS = frozenset({
    "provision", "provision_v2", "wifi_scan", "ota", "device.ota",
    "unpair", "set_wifi", "set_auth",
})
PLAINTEXT_STATUSES = frozenset({
    "error_encryption_required",
    "error_pairing_invalid_hello",
    "error_pairing_unavailable",
    "error_pairing_decrypt",
})
DISCONNECT_AFTER_ERROR_S = 0.3


class Transport(Protocol):
    def send_packets(self, packets: list[bytes]) -> None:
        """Notify each packet in order; blocks until all are queued."""

    def mtu(self) -> int: ...

    def disconnect(self, delay: float) -> None: ...


class Network(Protocol):
    def is_online(self) -> bool: ...

    def current_connection_entry(self) -> dict: ...


@dataclass(frozen=True)
class Credentials:
    access_token: str
    refresh_token: str
    username: str
    api_url: str
    api_url_v2: str
    noise_host: str


class ProvisionFailed(Exception):
    """Setup could not finish; ``status`` is what the app is told."""

    def __init__(self, status: str) -> None:
        super().__init__(status)
        self.status = status


def _compact(obj: dict) -> str:
    return json.dumps(obj, separators=(",", ":"))


class SetupController:
    """Handles one BLE setup client at a time.

    ``provision`` runs on its own thread with the saved credentials. It must
    verify them and persist them through ``commit`` (which runs under the
    pairing lock), raising :class:`ProvisionFailed` on failure.
    """

    def __init__(
        self,
        *,
        pairing: PairingSession,
        identity: Identity,
        version: str,
        transport: Transport,
        network: Network,
        provision: Callable[[Credentials, Callable[[Callable[[], bool]], bool]], None],
        on_complete: Callable[[], None] = lambda: None,
    ) -> None:
        self._pairing = pairing
        self._identity = identity
        self._version = version
        self._transport = transport
        self._network = network
        self._provision = provision
        self._on_complete = on_complete
        self._assembler = ChunkAssembler()
        self._inbox: queue.Queue[bytes | None] = queue.Queue()
        self._tx_lock = threading.Lock()
        self._state_lock = threading.Lock()
        self._plaintext_blocked = False
        self._provisioning = False
        self._worker: threading.Thread | None = None

    # -- Transport callbacks (any thread) ---------------------------------------

    def on_write(self, packet: bytes) -> None:
        message = self._assembler.feed(packet)
        if message is not None:
            self._inbox.put(message)

    def on_disconnect(self) -> None:
        log.info("BLE client disconnected; clearing pairing session")
        self._assembler.reset()
        with self._state_lock:
            self._plaintext_blocked = False
        self._pairing.reset()

    # -- Lifecycle --------------------------------------------------------------

    def start(self) -> None:
        self._worker = threading.Thread(target=self._run, name="ble-setup", daemon=True)
        self._worker.start()

    def stop(self) -> None:
        self._inbox.put(None)

    def _run(self) -> None:
        while (message := self._inbox.get()) is not None:
            try:
                self.handle_message(message)
            except Exception:
                log.exception("setup command failed")

    # -- Dispatch ---------------------------------------------------------------

    def handle_message(self, raw: bytes, decrypted: bool = False) -> None:
        try:
            command = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError, RecursionError):
            log.warning("invalid command JSON (%d bytes)", len(raw))
            self.send_status("error_invalid_command")
            return
        if not isinstance(command, dict):
            self.send_status("error_invalid_command")
            return
        action = command.get("action")
        action = action if isinstance(action, str) else ""
        log.info("RX action: %s%s", action or "?", " (encrypted)" if decrypted else "")

        with self._state_lock:
            plaintext_blocked = self._plaintext_blocked

        if not decrypted and action == "pairing_client_hello":
            self._handle_hello(command)
        elif not decrypted and action == "pairing_encrypted":
            self._handle_record(command)
        elif action == "get_device_info":
            # Public metadata, safe in plaintext at any point. Apps re-read it
            # when they restart a handshake on the same connection.
            self.send_json(self.device_info())
        elif not decrypted and plaintext_blocked:
            log.warning("plaintext command ignored after pairing started: %s", action)
        elif not decrypted and action in SENSITIVE_ACTIONS:
            self.send_status("error_encryption_required")
        elif decrypted and action == "pairing_client_finished":
            self._handle_client_finished(command)
        elif decrypted and action in SENSITIVE_ACTIONS and not self._pairing.confirmed:
            self.send_status("error_pairing_confirm_required")
        elif decrypted and action == "wifi_scan":
            self._handle_wifi_scan()
        elif decrypted and action == "provision_v2":
            self._handle_provision(command)
        else:
            self.send_status("error_unknown_action")

    def device_info(self) -> dict:
        info = {
            "type": "device_info",
            "node_id": self._identity.node_id,
            "version": self._version,
            **self._pairing.device_info(),
            "build_sha": "",
            "network_ready": self._network.is_online(),
        }
        return info

    def _handle_hello(self, command: dict) -> None:
        try:
            ready = self._pairing.handle_hello(command)
        except PairingError as err:
            self.send_status(err.status)
            return
        with self._state_lock:
            self._plaintext_blocked = True
        self.send_json(ready)

    def _handle_record(self, envelope: dict) -> None:
        try:
            plaintext = self._pairing.decrypt(envelope)
        except PairingError as err:
            self.send_status(err.status)
            self._transport.disconnect(DISCONNECT_AFTER_ERROR_S)
            return
        self.handle_message(plaintext.encode("utf-8"), decrypted=True)

    def _handle_client_finished(self, command: dict) -> None:
        generation = self._pairing.handle_client_finished(command)
        if not generation:
            self.send_status("error_pairing_decrypt")
            self._transport.disconnect(DISCONNECT_AFTER_ERROR_S)
            return
        log.info("pairing confirmed (app consent)")
        self.send_status("pairing_confirmed", generation)

    def _handle_wifi_scan(self) -> None:
        networks = [self._network.current_connection_entry()] if self._network.is_online() else []
        self.send_encrypted_json({"type": "wifi_scan_result", "networks": networks})

    def _handle_provision(self, command: dict) -> None:
        def text(key: str) -> str:
            value = command.get(key)
            return value if isinstance(value, str) else ""

        if (
            not isinstance(command.get("ssid"), str)
            or not isinstance(command.get("password"), str)
            or not text("access_token")
            or not text("refresh_token")
            or text("token_type") != "device"
        ):
            self.send_status("error_missing_credentials")
            return
        generation = 0
        with self._state_lock:
            if self._provisioning:
                refusal = "error_operation_in_progress"
            else:
                generation = self._pairing.mark_provisioning()
                refusal = None if generation else "error_pairing_confirm_required"
                self._provisioning = bool(generation)
        # Sent after the lock is released: send_status() takes _state_lock
        # itself when the session has no keys, and the lock is not reentrant.
        if refusal:
            self.send_status(refusal)
            return
        # The Wi-Fi fields are deliberately dropped: this device only sets up
        # when it is already online.
        credentials = Credentials(
            access_token=text("access_token"),
            refresh_token=text("refresh_token"),
            username=text("username"),
            api_url=text("api_url"),
            api_url_v2=text("api_url_v2"),
            noise_host=text("noise_host"),
        )
        threading.Thread(
            target=self._run_provision, args=(credentials, generation),
            name="provision", daemon=True,
        ).start()

    def _run_provision(self, credentials: Credentials, generation: int) -> None:
        try:
            self.send_status("wifi_connecting", generation)
            if not self._network.is_online():
                # Stay in provisioning so the app can retry, as the firmware does.
                self._pairing.extend_provisioning(generation)
                self.send_status("wifi_failed", generation)
                return
            self.send_status("wifi_connected", generation)

            def commit(save: Callable[[], bool]) -> bool:
                return self._pairing.commit_provisioning(generation, save)

            try:
                self._provision(credentials, commit)
            except ProvisionFailed as err:
                log.warning("provisioning failed: %s", err.status)
                self.send_status(err.status, generation)
                self._transport.disconnect(0.5)
                return
            self.send_status("auth_ok", generation)
            log.info("setup complete")
            self._on_complete()
        finally:
            with self._state_lock:
                self._provisioning = False

    # -- Sending ----------------------------------------------------------------

    def send_status(self, status: str, generation: int = 0) -> None:
        with self._tx_lock:
            envelope = self._pairing.encrypt_status(status, generation)
            if envelope is not None:
                self._send_locked(_compact(envelope))
                log.info("TX status (encrypted #%s): %s", envelope["counter"], status)
                return
            with self._state_lock:
                blocked = self._plaintext_blocked
            if generation or blocked or status not in PLAINTEXT_STATUSES:
                log.info("TX status suppressed: %s", status)
                return
            self._transport.send_packets([status.encode()])
            log.info("TX status: %s", status)

    def send_json(self, obj: dict) -> None:
        with self._tx_lock:
            self._send_locked(_compact(obj))
            log.info("TX %s", obj.get("type"))

    def send_encrypted_json(self, obj: dict, generation: int = 0) -> None:
        with self._tx_lock:
            envelope = self._pairing.encrypt_json(_compact(obj), generation)
            if envelope is None:
                log.info("TX %s suppressed: no session", obj.get("type"))
                return
            self._send_locked(_compact(envelope))
            log.info("TX %s (encrypted #%s)", obj.get("type"), envelope["counter"])

    def _send_locked(self, text: str) -> None:
        packets = encode_chunks(text.encode("utf-8"), self._transport.mtu())
        log.debug("TX %d bytes in %d packets (mtu %d)", len(text), len(packets), self._transport.mtu())
        self._transport.send_packets(packets)
