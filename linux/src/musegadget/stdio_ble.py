# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""BLE packet transport over private SSH pipes; no Bluetooth on the target."""
from __future__ import annotations

import base64
import json
import queue
import threading
import time

from musegadget.ble_framing import CHUNK_STAGGER_S, MAX_PACKET_BYTES

PROTOCOL_VERSION = 1
MAX_LINE_BYTES = 8192


def packet_data(value) -> bytes:
    if not isinstance(value, str):
        raise ValueError("Invalid relay packet")
    try:
        packet = base64.b64decode(value, validate=True)
    except ValueError:
        raise ValueError("Invalid relay packet") from None
    if not packet or len(packet) > 512:
        raise ValueError("Invalid relay packet size")
    return packet


class JsonPipe:
    """Bounded JSON-line input with cancellable polling and serialized output."""

    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer
        self._inbox = queue.Queue(maxsize=128)
        self._lock = threading.Lock()
        self._closed = threading.Event()
        threading.Thread(target=self._read, daemon=True, name="pairing-pipe").start()

    def _put(self, message):
        while not self._closed.is_set():
            try:
                self._inbox.put(message, timeout=0.1)
                return
            except queue.Full:
                pass

    def _read(self):
        try:
            while not self._closed.is_set():
                line = self.reader.readline(MAX_LINE_BYTES + 1)
                if not line:
                    self._put(None)
                    return
                if len(line.encode("utf-8")) > MAX_LINE_BYTES or not line.endswith("\n"):
                    raise ValueError("Invalid relay line length")
                message = json.loads(line)
                if not isinstance(message, dict):
                    raise ValueError("Invalid relay message")
                self._put(message)
        except (OSError, ValueError, RecursionError):
            # Never include input contents or tokens in an error.
            self._put(ValueError("Invalid or interrupted pairing pipe"))

    def receive(self, timeout=0.1):
        message = self._inbox.get(timeout=timeout)
        if isinstance(message, Exception):
            raise message
        return message

    def send(self, message):
        line = json.dumps(message, separators=(",", ":")) + "\n"
        if len(line.encode("utf-8")) > MAX_LINE_BYTES:
            raise ValueError("Relay message is too large")
        with self._lock:
            self.writer.write(line)
            self.writer.flush()

    def close(self):
        self._closed.set()


class BleServer:
    """The SetupController interface, with CoreBluetooth running on the Mac."""

    def __init__(self, local_name, on_write, on_disconnect, *, channel):
        self.channel = channel
        self.on_write = on_write
        self.on_disconnect = on_disconnect
        self._mtu = MAX_PACKET_BYTES + 3
        self._stopped = threading.Event()
        self._stop_lock = threading.Lock()
        channel.send({"event": "identity", "name": local_name, "version": PROTOCOL_VERSION})

    def mtu(self):
        return self._mtu

    def send_packets(self, packets):
        for index, packet in enumerate(packets):
            if index:
                time.sleep(CHUNK_STAGGER_S)
            self.channel.send({"op": "notify", "data": base64.b64encode(packet).decode("ascii")})

    def disconnect(self, delay):
        self.channel.send({"op": "disconnect", "delay": delay})

    def run(self):
        try:
            while not self._stopped.is_set():
                try:
                    event = self.channel.receive()
                except queue.Empty:
                    continue
                if event is None or event.get("event") == "stop":
                    return
                kind = event.get("event")
                if kind == "write":
                    self.on_write(packet_data(event.get("data")))
                elif kind == "mtu":
                    value = event.get("value")
                    if type(value) is not int or not 23 <= value <= 515:
                        raise ValueError("Invalid relay MTU")
                    self._mtu = value
                elif kind == "disconnect":
                    self._mtu = MAX_PACKET_BYTES + 3
                    self.on_disconnect()
                else:
                    raise ValueError("Unknown relay event")
        finally:
            self.stop()

    def stop(self):
        with self._stop_lock:
            if not self._stopped.is_set():
                self._stopped.set()
                try:
                    self.channel.send({"op": "stop"})
                except OSError:
                    pass  # SSH may already have closed the pipe.
