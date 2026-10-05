#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""macOS pairing and direct Muse connection, controlled over private stdio."""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
from pathlib import Path
import signal
import sys
import tempfile
import threading
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent / "sdk"))
from musegadget import __version__, config, identity, network
from musegadget.ble_setup import SetupController
from musegadget.cli import _SystemNetwork, _verify_and_save
from musegadget.executor import Account
from musegadget.link_client import DeviceDescription, LinkSession, Outcome
from musegadget.pairing import PairingSession
from musegadget.service import Service
import mac_ble

_output_lock = threading.Lock()

def emit(event):
    if event.get("type") in {"pairing_status", "paired", "pairing_error"}:
        logging.info("%s: %s", event["type"], event.get("text") or event.get("error") or "complete")
    with _output_lock:
        print(json.dumps(event, ensure_ascii=False), flush=True)

def save_sdk_token(token):
    """Use the SDK's validator; never place credentials in process arguments."""
    if not isinstance(token, str) or not token.strip():
        raise ValueError("Enter your SDK token from gadgets.muse.ai.")
    previous = os.environ.get(config.SDK_TOKEN_ENV)
    os.environ[config.SDK_TOKEN_ENV] = token.strip()
    try:
        valid = config.sdk_token()
    finally:
        if previous is None: os.environ.pop(config.SDK_TOKEN_ENV, None)
        else: os.environ[config.SDK_TOKEN_ENV] = previous
    directory = config.state_dir()
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    directory.chmod(0o700)
    with tempfile.NamedTemporaryFile(mode="w", dir=directory, delete=False) as temporary:
        path = Path(temporary.name)
        try:
            temporary.write(valid + "\n"); temporary.flush(); os.fsync(temporary.fileno())
            os.replace(path, directory / config.SDK_TOKEN_FILE)
        finally: path.unlink(missing_ok=True)
    return valid

class CompanionExecutor:
    account = Account.current()
    def run(self, name, params, timeout_ms=None):
        return {"ok": False, "error": "This companion supports chat only."}

class MacDescription(DeviceDescription):
    def register_params(self):
        return {**super().register_params(), "platform": "macos", "model_id": "macos"}

class MacService(Service):
    async def _sleep(self, seconds):
        stopping = asyncio.create_task(self._stop.wait())
        waking = asyncio.create_task(self.wake.wait())
        try:
            await asyncio.wait((stopping, waking), timeout=seconds, return_when=asyncio.FIRST_COMPLETED)
        finally:
            self.wake.clear()
            stopping.cancel(); waking.cancel()
            await asyncio.gather(stopping, waking, return_exceptions=True)

    async def _session(self, vm, pairing):
        session = LinkSession(noise_host=pairing.get("noise_host") or "hatch.metaaivm.com",
            vm_id=vm["vm_id"] or vm["vm_name"], vm_auth_token=vm["vm_auth_token"],
            device=MacDescription(self.identity.node_id, "Muse Companion on Mac", __version__, {}),
            run_command=self.executor.run)
        self._current = session
        try:
            outcome = await session.run(self._stop)
        except Exception as exc:
            logging.warning("Muse connection failed: %s", type(exc).__name__)
            outcome = Outcome.CLOSED
        finally:
            self._current = None
        return outcome, time.monotonic() - (session.registered_at or time.monotonic())

class Backend:
    def __init__(self):
        self.identity = identity.load_or_create()
        self.service = MacService(self.identity, CompanionExecutor(), sdk_token=config.sdk_token())
        self.service.wake = asyncio.Event()
        self.pair_task = self.chat_task = None
        self.ble = None
        self.stopped = asyncio.Event()
        self.last_status = None
        self.pair_cancelled = threading.Event()

    @property
    def online(self):
        session = self.service._current
        return session is not None and session.registered_at is not None

    def status(self, force=False):
        paired = config.load_json(config.PAIRING_FILE) is not None
        pairing = self.pair_task is not None and not self.pair_task.done()
        event = dict(type="connection", paired=paired, online=self.online, pairing=pairing,
                     ble_name=self.identity.ble_name, sdk_token_saved=bool(config.sdk_token()))
        if force or event != self.last_status:
            self.last_status = event; emit(event)

    def pair_sync(self):
        if self.pair_cancelled.is_set(): return False
        completed = threading.Event()
        controller = None
        server = mac_ble.BleServer(self.identity.ble_name,
            lambda packet: controller.on_write(packet), lambda: controller.on_disconnect())
        self.ble = server
        if self.pair_cancelled.is_set(): server.stop()
        server.status = lambda text: emit(dict(type="pairing_status", text=text))
        def finish():
            completed.set()
            threading.Timer(2, server.stop).start()
        controller = SetupController(pairing=PairingSession(node_id=self.identity.node_id,
            device_id=self.identity.device_id, mac=self.identity.mac, firmware_version=__version__,
            sdk_token=self.service.sdk_token), identity=self.identity, version=__version__,
            transport=server, network=_SystemNetwork(), provision=_verify_and_save, on_complete=finish)
        timer = threading.Timer(600, server.stop); timer.daemon = True
        controller.start(); timer.start()
        try:
            server.run()
        finally:
            timer.cancel(); controller.stop(); server.stop(); self.ble = None
        return completed.is_set()

    async def pair(self):
        try:
            if not await asyncio.to_thread(network.is_online):
                raise ConnectionError("Connect your Mac to the internet before pairing.")
            if self.pair_cancelled.is_set() or self.stopped.is_set(): return
            emit(dict(type="pairing_status", text=f"In the Muse phone app, add {self.identity.ble_name}."))
            if await asyncio.to_thread(self.pair_sync):
                self.service.wake.set()
                emit(dict(type="paired"))
            else:
                emit(dict(type="pairing_error", error="Pairing closed without completing. You can try again."))
        except Exception as exc:
            emit(dict(type="pairing_error", error=str(exc)))
        finally:
            self.pair_task = None; self.status(True)

    async def chat(self, text, session_id):
        try:
            session = self.service._current
            if not self.online: raise ConnectionError("Wait for your Muse to connect.")
            async for event in session.chat_events(text, session_id): emit(event)
        except asyncio.CancelledError:
            emit(dict(type="error", error="Conversation cancelled."))
        except Exception as exc:
            emit(dict(type="error", error=str(exc)))
    def chat_finished(self, task):
        if self.chat_task is task:
            emit(dict(type="turn_finished")); self.chat_task = None

    def command(self, request):
        op = request.get("op")
        if op == "pair":
            if config.load_json(config.PAIRING_FILE): raise ValueError("This Mac is already paired.")
            if self.pair_task is not None: raise ValueError("Pairing is already open.")
            supplied = request.get("sdk_token")
            self.service.sdk_token = save_sdk_token(supplied) if supplied else config.sdk_token()
            if not self.service.sdk_token: raise ValueError("Enter your SDK token from gadgets.muse.ai.")
            self.pair_cancelled.clear()
            self.pair_task = asyncio.create_task(self.pair()); self.status(True)
        elif op == "cancel_pair":
            self.pair_cancelled.set()
            if self.ble: threading.Thread(target=self.ble.stop, daemon=True).start()
        elif op == "chat":
            if self.chat_task is not None: raise ValueError("Wait for the current reply.")
            text, session_id = request.get("message"), request.get("session_id")
            if not isinstance(text, str) or not text.strip() or len(text.encode()) > 32000:
                raise ValueError("Enter a message under 32 KB.")
            if not isinstance(session_id, str) or str(uuid.UUID(session_id)) != session_id:
                raise ValueError("Invalid conversation identifier.")
            self.chat_task = asyncio.create_task(self.chat(text, session_id))
            self.chat_task.add_done_callback(self.chat_finished)
        elif op == "cancel":
            if self.chat_task: self.chat_task.cancel()
        elif op == "check": self.status(True)
        elif op == "stop": self.stopped.set()
        else: raise ValueError("Unknown app operation.")

    async def run(self):
        reader = asyncio.StreamReader(limit=64 * 1024)
        protocol = asyncio.StreamReaderProtocol(reader)
        transport, _ = await asyncio.get_running_loop().connect_read_pipe(lambda: protocol, sys.stdin)
        async def commands():
            while line := await reader.readline():
                try:
                    request = json.loads(line)
                    if not isinstance(request, dict): raise ValueError("Invalid app request.")
                    self.command(request)
                except Exception as exc:
                    emit(dict(type="command_error", error=str(exc)))
            self.stopped.set()
        async def statuses():
            while not self.stopped.is_set():
                self.status(); await asyncio.sleep(0.5)
        tasks = [asyncio.create_task(self.service.run()), asyncio.create_task(commands()), asyncio.create_task(statuses())]
        loop = asyncio.get_running_loop()
        for signum in (signal.SIGTERM, signal.SIGINT): loop.add_signal_handler(signum, self.stopped.set)
        try:
            await self.stopped.wait()
        finally:
            self.service.stop()
            self.pair_cancelled.set()
            if self.ble: await asyncio.to_thread(self.ble.stop)
            if self.chat_task:
                self.chat_task.cancel(); await asyncio.gather(self.chat_task, return_exceptions=True)
            if self.pair_task: await asyncio.gather(self.pair_task, return_exceptions=True)
            for task in tasks: task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            transport.close()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", required=True)
    args = parser.parse_args()
    os.environ[config.STATE_DIR_ENV] = args.state_dir
    directory = config.state_dir(); directory.mkdir(mode=0o700, parents=True, exist_ok=True); directory.chmod(0o700)
    descriptor = os.open(directory / "connection.log", os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    logfile = os.fdopen(descriptor, "a")
    logging.basicConfig(level=logging.INFO, handlers=[logging.StreamHandler(sys.stderr), logging.StreamHandler(logfile)])
    asyncio.run(Backend().run())

if __name__ == "__main__": main()
