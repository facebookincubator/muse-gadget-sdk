# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""Subprocess integration peers using published synthetic pairing fixtures."""
import base64
import json
import sys

from musegadget import cli
from musegadget.ble_framing import ChunkAssembler, encode_chunks
from musegadget.pair_stdio import main
from test_pairing import APP, Mobile, hello, make_device


def emit(message):
    print(json.dumps(message), flush=True)


def phone():
    mobile = Mobile(APP)
    assembler = ChunkAssembler()

    def write(message):
        for packet in encode_chunks(json.dumps(message).encode(), 185):
            emit({"event": "write", "data": base64.b64encode(packet).decode()})

    emit({"event": "state", "state": "poweredOn"})
    emit({"event": "ready", "nameOnly": True})
    emit({"event": "mtu", "value": 185})
    write({"action": "get_device_info"})
    for line in sys.stdin:
        command = json.loads(line)
        if command["op"] == "stop":
            return 0
        if command["op"] != "notify":
            continue
        raw = assembler.feed(base64.b64decode(command["data"]))
        if raw is None:
            continue
        response = json.loads(raw)
        if response["type"] == "device_info":
            assert response["node_id"] == APP["node_id"]
            write(hello())
        elif response["type"] == "pairing_ready":
            assert response["session_id"] == APP["session_id"]
            write(mobile.seal({"action": "pairing_client_finished"}))
        elif response["type"] == "pairing_encrypted":
            plain = mobile.open(response)
            status = plain.get("status")
            if status == "pairing_confirmed":
                assert plain.get("sdk_token") == "mgst_" + "A" * 43
                write(mobile.seal({"action": "wifi_scan"}))
            elif plain.get("type") == "wifi_scan_result":
                write(mobile.seal({
                    "action": "provision_v2", "ssid": "wired", "password": "",
                    "token_type": "device", "access_token": "synthetic-board-access",
                    "refresh_token": "synthetic-board-refresh", "username": "fixture-user",
                }))
            elif status == "auth_ok":
                pass  # The board now owns the saved credentials and stops the helper.
            elif status not in ("wifi_connecting", "wifi_connected"):
                raise AssertionError("Unexpected provisioning status: " + str(status))
    return 1


if __name__ == "__main__":
    if sys.argv[1] == "phone":
        raise SystemExit(phone())
    cli.PairingSession = lambda **kwargs: make_device(sdk_token=kwargs.get("sdk_token"))
    cli.__version__ = APP["firmware_version"]
    cli.BLE_SHUTDOWN_DELAY_S = 0.05
    cli._SystemNetwork.is_online = staticmethod(lambda: True)
    cli._SystemNetwork.current_connection_entry = staticmethod(lambda: {"ssid": "wired", "rssi": 0})
    cli.muse_api.fetch_vms_with_status = lambda *args: ([{"vm_id": "synthetic-vm"}], 200)
    raise SystemExit(main(sys.argv[2:]))
