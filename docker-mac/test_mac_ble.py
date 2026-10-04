# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""Check BLE forwarding without hardware; upstream tests cover cryptography."""
import base64
import io
import json
import subprocess

import pytest

import mac_ble

from mac_ble import BleServer


class FakeHelper:
    stdin = None

    def __init__(self):
        self.stdin = io.StringIO()

    def poll(self):
        return None


def test_transport_preserves_packets_and_mtu():
    writes = []
    disconnects = []
    transport = BleServer("MuseGadget123456", writes.append, lambda: disconnects.append(True))
    helper = FakeHelper()
    transport._process = helper
    packets = [bytes(range(160)), b"\xfe\x01\x02\x00\xff"]
    transport.send_packets(packets)
    messages = [json.loads(line) for line in helper.stdin.getvalue().splitlines()]
    assert [base64.b64decode(message["data"]) for message in messages] == packets
    for message in messages:
        transport._event({"event": "write", "data": message["data"]})
    assert writes == packets
    transport._event({"event": "mtu", "value": 185})
    assert transport.mtu() == 185
    transport._event({"event": "disconnect"})
    assert disconnects == [True]
    assert transport.mtu() == mac_ble.MAX_PACKET_BYTES + 3


def test_disconnect_command_preserves_delay():
    transport = BleServer("MuseGadget123456", lambda packet: None, lambda: None)
    transport._process = FakeHelper()
    transport.disconnect(0.3)
    assert json.loads(transport._process.stdin.getvalue()) == {"op": "disconnect", "delay": 0.3}


@pytest.mark.parametrize("reported,expected", [(1, 23), (9999, 515), (185, 185)])
def test_mtu_bounds(reported, expected):
    transport = BleServer("MuseGadget123456", lambda packet: None, lambda: None)
    transport._event({"event": "mtu", "value": reported})
    assert transport.mtu() == expected


def test_stop_before_run_does_not_launch_helper(monkeypatch):
    transport = BleServer("MuseGadget123456", lambda packet: None, lambda: None)
    monkeypatch.setattr(mac_ble.subprocess, "Popen", lambda *a, **kw: pytest.fail("helper launched"))
    transport.stop()
    transport.run()


class LifecycleHelper(FakeHelper):
    def __init__(self, status=0, timeouts=0):
        super().__init__()
        self.stdout = io.StringIO()
        self.status = status
        self.timeouts = timeouts
        self.actions = []
        self.done = False

    def poll(self):
        return self.status if self.done else None

    def wait(self, timeout=None):
        self.actions.append(("wait", timeout))
        if self.timeouts:
            self.timeouts -= 1
            raise subprocess.TimeoutExpired("fake-helper", timeout)
        self.done = True
        return self.status

    def terminate(self):
        self.actions.append(("terminate",))

    def kill(self):
        self.actions.append(("kill",))


def test_close_window_ends_transport_and_closes_pipes(monkeypatch):
    helper = LifecycleHelper()
    transport = BleServer("MuseGadget123456", lambda packet: None, lambda: None)
    calls = []

    def launch(argv, **kwargs):
        calls.append(argv)
        return helper

    monkeypatch.setattr(mac_ble.subprocess, "Popen", launch)
    transport.run()
    assert calls == [[str(mac_ble.HELPER), "MuseGadget123456"]]
    assert helper.stdin.closed and helper.stdout.closed


def test_permission_failure_is_reported(monkeypatch):
    helper = LifecycleHelper(status=2)
    monkeypatch.setattr(mac_ble.subprocess, "Popen", lambda *a, **kw: helper)
    transport = BleServer("MuseGadget123456", lambda packet: None, lambda: None)
    with pytest.raises(RuntimeError, match="allow Bluetooth permission"):
        transport.run()
    assert helper.stdin.closed and helper.stdout.closed


def test_helper_error_always_stops_child(monkeypatch):
    helper = LifecycleHelper()
    helper.stdout = io.StringIO(json.dumps({"event": "error", "message": "GATT unavailable"}) + "\n")
    monkeypatch.setattr(mac_ble.subprocess, "Popen", lambda *a, **kw: helper)
    transport = BleServer("MuseGadget123456", lambda packet: None, lambda: None)
    with pytest.raises(RuntimeError, match="GATT unavailable"):
        transport.run()
    assert helper.done
    assert helper.stdin.closed and helper.stdout.closed


def test_unresponsive_helper_shutdown_is_bounded_and_repeatable():
    helper = LifecycleHelper(timeouts=2)
    transport = BleServer("MuseGadget123456", lambda packet: None, lambda: None)
    transport._process = helper
    transport.stop()
    transport.stop()
    assert helper.actions == [("wait", 3), ("terminate",), ("wait", 3), ("kill",), ("wait", 3)]
    assert helper.stdin.closed
