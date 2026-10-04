# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
import base64
import io
import json
import threading
import time

import pytest

from musegadget import stdio_ble


def make_transport(events):
    writer = io.StringIO()
    channel = stdio_ble.JsonPipe(io.StringIO("".join(json.dumps(e) + "\n" for e in events)), writer)
    writes, disconnects = [], []
    server = stdio_ble.BleServer("MuseGadget123456", writes.append,
                               lambda: disconnects.append(True), channel=channel)
    return server, channel, writer, writes, disconnects


def test_stdio_packets_mtu_and_disconnect_match_ble_interface():
    payload = bytes(range(185))
    server, channel, output, writes, disconnects = make_transport([
        {"event": "mtu", "value": 185},
        {"event": "write", "data": base64.b64encode(payload).decode()},
        {"event": "disconnect"},
    ])
    server.send_packets([payload])
    server.disconnect(0.3)
    server.run()
    messages = [json.loads(line) for line in output.getvalue().splitlines()]
    assert messages == [
        {"event": "identity", "name": "MuseGadget123456", "version": 1},
        {"op": "notify", "data": base64.b64encode(payload).decode()},
        {"op": "disconnect", "delay": 0.3}, {"op": "stop"},
    ]
    assert writes == [payload] and disconnects == [True]
    assert server.mtu() == stdio_ble.MAX_PACKET_BYTES + 3
    channel.close()


@pytest.mark.parametrize("line", ["[]\n", "garbage\n", "{}", "x" * 8193 + "\n"])
def test_invalid_pipe_lines_fail_without_echoing_input(line):
    channel = stdio_ble.JsonPipe(io.StringIO(line), io.StringIO())
    try:
        with pytest.raises(ValueError, match="Invalid or interrupted pairing pipe"):
            channel.receive(timeout=1)
    finally:
        channel.close()


@pytest.mark.parametrize("event", [
    {"event": "write", "data": "not-base64"},
    {"event": "write", "data": base64.b64encode(b"x" * 513).decode()},
    {"event": "mtu", "value": 517}, {"event": "mtu", "value": True},
    {"event": "unexpected"},
])
def test_invalid_remote_input_stops_transport(event):
    server, channel, output, writes, _ = make_transport([event])
    try:
        with pytest.raises(ValueError):
            server.run()
        assert not writes
        assert json.loads(output.getvalue().splitlines()[-1]) == {"op": "stop"}
    finally:
        channel.close()


def test_stop_interrupts_idle_input_poll_without_waiting_for_eof():
    class IdleChannel:
        def __init__(self):
            self.messages = []

        def send(self, event):
            self.messages.append(event)

        def receive(self):
            time.sleep(0.01)
            raise stdio_ble.queue.Empty

    channel = IdleChannel()
    server = stdio_ble.BleServer("MuseGadget123456", lambda packet: None, lambda: None,
                               channel=channel)
    worker = threading.Thread(target=server.run)
    worker.start()
    server.stop()
    server.stop()
    worker.join(timeout=1)
    assert not worker.is_alive()
    assert channel.messages.count({"op": "stop"}) == 1
