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

"""Pair this device with the Muse app over Bluetooth and save the pairing."""

from __future__ import annotations

import enum
import logging
import threading
import time
from dataclasses import dataclass
from typing import Callable

from musegadget import __version__, config, identity, muse_api, network
from musegadget.ble_setup import Credentials, ProvisionFailed, SetupController
from musegadget.pairing import PairingSession

log = logging.getLogger("musegadget")

SETUP_WINDOW_S = 600
BLE_SHUTDOWN_DELAY_S = 1.5


class PairOutcome(enum.Enum):
    PAIRED = "paired"
    ALREADY_PAIRED = "already_paired"      # pass force=True to pair again
    INVALID_SDK_TOKEN = "invalid_sdk_token"
    WINDOW_CLOSED = "window_closed"        # the setup window ended without pairing


@dataclass(frozen=True)
class PairResult:
    outcome: PairOutcome
    sdk_token_problem: str | None = None  # why the SDK token was rejected, for INVALID_SDK_TOKEN


@dataclass(frozen=True)
class SetupWindow:
    """What the user needs once Bluetooth setup is open."""

    ble_name: str           # the device to choose in the Muse app
    timeout_s: int          # how long setup stays open
    has_sdk_token: bool     # False: the device pairs now, but gadgets without a token will stop pairing


class _SystemNetwork:
    is_online = staticmethod(network.is_online)
    current_connection_entry = staticmethod(network.current_connection_entry)


def _verify_and_save(credentials: Credentials, commit: Callable[[Callable[[], bool]], bool]) -> None:
    api_url = credentials.api_url if credentials.api_url.startswith("https://") else ""
    api_url_v2 = credentials.api_url_v2 if credentials.api_url_v2.startswith("https://") else ""
    vms, status = muse_api.fetch_vms_with_status(
        credentials.access_token, muse_api.api_root(api_url_v2),
    )
    if not vms:
        log.warning("device token check failed (HTTP %s, %d VMs)", status, len(vms))
        raise ProvisionFailed("auth_failed")

    record = {
        "access_token": credentials.access_token,
        "refresh_token": credentials.refresh_token,
        "token_type": "device",
        "username": credentials.username,
        "api_url": api_url,
        "api_url_v2": api_url_v2,
        "noise_host": credentials.noise_host,
        "access_token_saved_at": int(time.time()),
    }

    def save() -> bool:
        config.save_json(config.PAIRING_FILE, record)
        return True

    if not commit(save):
        raise ProvisionFailed("error_storage")


def pair(*, on_open: Callable[[SetupWindow], None], force: bool = False,
         timeout_s: int = SETUP_WINDOW_S) -> PairResult:
    """Open Bluetooth setup until the Muse app pairs this device or ``timeout_s`` passes.

    Reads the SDK token and the device identity from the state directory and
    saves the pairing on success. It prints nothing. ``on_open`` runs once
    setup is about to open, so the caller can tell the user what to do.
    """
    # BleServer needs D-Bus, which only a device that pairs has to install.
    from musegadget.ble_server import BleServer

    if config.load_json(config.PAIRING_FILE) and not force:
        return PairResult(PairOutcome.ALREADY_PAIRED)

    try:
        sdk_token = config.sdk_token()
    except ValueError as exc:
        return PairResult(PairOutcome.INVALID_SDK_TOKEN, sdk_token_problem=str(exc))
    ident = identity.load_or_create()
    pairing = PairingSession(
        node_id=ident.node_id,
        device_id=ident.device_id,
        mac=ident.mac,
        firmware_version=__version__,
        sdk_token=sdk_token,
    )
    completed = threading.Event()
    controller: SetupController | None = None

    server = BleServer(
        ident.ble_name,
        on_write=lambda packet: controller.on_write(packet),
        on_disconnect=lambda: controller.on_disconnect(),
    )

    def finish() -> None:
        completed.set()
        threading.Timer(BLE_SHUTDOWN_DELAY_S, server.stop).start()

    controller = SetupController(
        pairing=pairing,
        identity=ident,
        version=__version__,
        transport=server,
        network=_SystemNetwork(),
        provision=_verify_and_save,
        on_complete=finish,
    )
    window = threading.Timer(timeout_s, server.stop)
    window.daemon = True

    on_open(SetupWindow(ident.ble_name, timeout_s, has_sdk_token=bool(sdk_token)))
    controller.start()
    window.start()
    try:
        server.run()
    except KeyboardInterrupt:
        pass
    finally:
        window.cancel()
        controller.stop()

    return PairResult(PairOutcome.PAIRED if completed.is_set() else PairOutcome.WINDOW_CLOSED)
