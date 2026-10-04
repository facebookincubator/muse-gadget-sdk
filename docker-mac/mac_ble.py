# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""Core Bluetooth transport for the unmodified Muse setup controller."""
from __future__ import annotations

import base64
import json
import logging
import subprocess
import threading
import time
from pathlib import Path

from musegadget.ble_framing import CHUNK_STAGGER_S, MAX_PACKET_BYTES

log = logging.getLogger(__name__)
HELPER = Path(__file__).parent / "build/MuseBluetooth.app/Contents/MacOS/MuseBluetooth"


class BleServer:
    def __init__(self, local_name, on_write, on_disconnect):
        self.name = local_name
        self.on_write = on_write
        self.on_disconnect = on_disconnect
        self._mtu = MAX_PACKET_BYTES + 3
        self._process = None
        self._lock = threading.Lock()
        self._stop_lock = threading.Lock()
        self._stopped = threading.Event()

    def mtu(self):
        return self._mtu

    def _send(self, message):
        with self._lock:
            if self._process is None or self._process.poll() is not None:
                raise RuntimeError("Mac Bluetooth helper is not running")
            self._process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
            self._process.stdin.flush()

    def send_packets(self, packets):
        for i, packet in enumerate(packets):
            if i:
                time.sleep(CHUNK_STAGGER_S)
            self._send({"op": "notify", "data": base64.b64encode(packet).decode("ascii")})

    def disconnect(self, delay):
        self._send({"op": "disconnect", "delay": delay})

    def _event(self, event):
        kind = event.get("event")
        if kind == "write":
            self.on_write(base64.b64decode(event["data"], validate=True))
        elif kind == "disconnect":
            self._mtu = MAX_PACKET_BYTES + 3
            self.on_disconnect()
        elif kind == "mtu":
            self._mtu = max(23, min(int(event["value"]), 515))
        elif kind == "ready":
            suffix = " (full-name discovery)" if event.get("nameOnly") else ""
            print(f"Mac Bluetooth advertising: {self.name}{suffix}", flush=True)
        elif kind == "state":
            print(f"Mac Bluetooth: {event['state']}", flush=True)
        elif kind == "error":
            raise RuntimeError(event.get("message", "Mac Bluetooth failed"))

    def run(self):
        if self._stopped.is_set():
            return
        self._process = subprocess.Popen([str(HELPER), self.name], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
        try:
            if self._stopped.is_set():
                self._send({"op": "stop"})
            for line in self._process.stdout:
                self._event(json.loads(line))
            status = self._process.wait()
            if status and not self._stopped.is_set():
                raise RuntimeError("Mac Bluetooth unavailable; allow Bluetooth permission and turn it on")
        finally:
            self.stop()
            self._process.stdout.close()

    def stop(self):
        with self._stop_lock:
            self._stopped.set()
            process = self._process
            if process is None:
                return
            if process.poll() is None:
                try:
                    self._send({"op": "stop"})
                except (OSError, RuntimeError):
                    pass
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
            if process.stdin:
                process.stdin.close()
