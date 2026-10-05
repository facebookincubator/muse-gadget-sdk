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

"""Keep a paired device connected to its Muse.

Each round fetches the leased VMs with the device token (which also yields a
fresh per-VM bearer), connects to the default VM and serves commands until
the connection ends. Failures back off exponentially. A session that stayed
up for a while resets the backoff. The device token is rotated before it
expires, and immediately if the API rejects it.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import re
import signal
import socket
import time
from dataclasses import dataclass, field

from musegadget import __version__, config, muse_api
from musegadget.executor import COMMAND_SPECS, Executor
from musegadget.identity import Identity
from musegadget.link_client import DeviceDescription, LinkSession, Outcome

log = logging.getLogger(__name__)

DEFAULT_NOISE_HOST = "hatch.metaaivm.com"
BACKOFF_BASE_S = 2.0
BACKOFF_MAX_S = 60.0
AUTH_BACKOFF_MIN_S = 15.0
HEALTHY_SESSION_S = 30.0
UNPAIRED_POLL_S = 30.0
MAX_LOCAL_REQUEST = 64 * 1024
MAX_WAIT_S = 600
_SESSION_ID_RE = re.compile(r"[A-Za-z0-9-]{1,64}")
# Device access tokens live about 4 hours; rotate at 3.
TOKEN_REFRESH_AGE_S = 3 * 3600
TOKEN_RETRY_S = 300


@dataclass
class Backoff:
    failures: int = 0
    floor: float = 0.0

    def next_delay(self) -> float:
        delay = min(BACKOFF_BASE_S * (2 ** self.failures), BACKOFF_MAX_S)
        self.failures += 1
        return max(delay, self.floor)

    def reset(self) -> None:
        self.failures = 0
        self.floor = 0.0


@dataclass
class Service:
    identity: Identity
    executor: Executor
    sdk_token: str | None = None
    display_name: str = field(default_factory=socket.gethostname)
    _stop: asyncio.Event = field(default_factory=asyncio.Event)
    _last_refresh_attempt: float = float("-inf")
    # The SDK token reaches Muse only in refresh bodies until apps forward it at
    # mint, so each start with a token attempts one refresh to report it.
    _sdk_token_report_attempted: bool = False
    _current: LinkSession | None = None

    def stop(self) -> None:
        self._stop.set()

    async def run(self) -> None:
        backoff = Backoff()
        while not self._stop.is_set():
            pairing = config.load_json(config.PAIRING_FILE)
            if not pairing:
                log.info("not paired; run `musegadget pair` to set up")
                await self._sleep(UNPAIRED_POLL_S)
                continue
            pairing = await self._maybe_refresh(pairing)
            if pairing is None:
                await self._sleep(TOKEN_RETRY_S)
                continue

            api = muse_api.api_root(pairing.get("api_url_v2", ""))
            vms, status = await asyncio.to_thread(
                muse_api.fetch_vms_with_status, pairing["access_token"], api,
            )
            if status == 401:
                log.warning("device token rejected by the API; refreshing")
                if await self._maybe_refresh(pairing, force=True) is None:
                    await self._sleep(TOKEN_RETRY_S)
                continue
            vm = next((v for v in vms if v["is_default"]), vms[0] if vms else None)
            if vm is None:
                await self._sleep(backoff.next_delay())
                continue

            outcome, lasted = await self._session(vm, pairing)
            if outcome is Outcome.STOPPED:
                return
            if outcome is Outcome.UNPAIRED:
                config.delete_json(config.PAIRING_FILE)
                log.warning("pairing removed; run `musegadget pair` to set up again")
                continue
            if lasted >= HEALTHY_SESSION_S:
                backoff.reset()
            if outcome in (Outcome.AUTH_REJECTED, Outcome.FORBIDDEN):
                backoff.floor = AUTH_BACKOFF_MIN_S
            delay = backoff.next_delay()
            log.info("reconnecting in %.0fs", delay)
            await self._sleep(delay)

    async def _session(self, vm: dict, pairing: dict) -> tuple[Outcome, float]:
        device = DeviceDescription(
            node_id=self.identity.node_id,
            display_name=self.display_name,
            version=__version__,
            commands=COMMAND_SPECS,
        )
        session = LinkSession(
            noise_host=pairing.get("noise_host") or DEFAULT_NOISE_HOST,
            vm_id=vm["vm_id"] or vm["vm_name"],
            vm_auth_token=vm["vm_auth_token"],
            device=device,
            run_command=self.executor.run,
        )
        log.info("connecting to %s", vm["vm_name"] or vm["vm_id"])
        started = time.monotonic()
        self._current = session
        try:
            outcome = await session.run(self._stop)
        except Exception as exc:
            log.warning("session failed: %s: %s", type(exc).__name__, exc)
            outcome = Outcome.CLOSED
        finally:
            self._current = None
        lasted = time.monotonic() - (session.registered_at or time.monotonic())
        log.info("session ended: %s after %.0fs", outcome.value, time.monotonic() - started)
        return outcome, lasted

    async def _maybe_refresh(self, pairing: dict, force: bool = False) -> dict | None:
        """Return current pairing, rotating tokens first if they are due.

        Returns None only when a due refresh failed and the old token should
        not be used yet; the pairing file is removed when the pairing itself
        has been revoked.
        """
        age = time.time() - pairing.get("access_token_saved_at", 0)
        report_due = bool(self.sdk_token) and not self._sdk_token_report_attempted
        due = force or age >= TOKEN_REFRESH_AGE_S
        if not due and not report_due:
            return pairing
        if not force and time.monotonic() - self._last_refresh_attempt < TOKEN_RETRY_S:
            return pairing
        self._last_refresh_attempt = time.monotonic()
        if report_due:
            self._sdk_token_report_attempted = True
            log.info("refreshing device token to report the SDK token")
        tokens, status = await asyncio.to_thread(
            muse_api.refresh_device_token,
            pairing["refresh_token"], self.identity.node_id,
            muse_api.api_root(pairing.get("api_url_v2", "")),
            self.sdk_token,
        )
        if tokens:
            pairing = {
                **pairing,
                "access_token": tokens["access_token"],
                "refresh_token": tokens["refresh_token"],
                "access_token_saved_at": int(time.time()),
            }
            config.save_json(config.PAIRING_FILE, pairing)
            log.info("device token rotated")
            return pairing
        if not due:
            # Only reporting the SDK token: nothing has rejected the current
            # token, so a refusal here must never unpair the device.
            log.warning("SDK token report refresh failed (HTTP %s); keeping the pairing", status)
            return pairing
        if status == 401:
            config.delete_json(config.PAIRING_FILE)
            log.error("pairing revoked; run `musegadget pair` to set up again")
            return None
        # Transient failure: keep using the current token while it still works.
        return None if force else pairing

    # -- Local socket -----------------------------------------------------------

    async def serve_local(self, path) -> asyncio.AbstractServer:
        """Accept messages for the Muse from programs on this device.

        Each connection sends one JSON line, ``{"message": "..."}`` plus an
        optional ``"session_id"`` naming a side chat and ``"wait"`` (seconds) to
        hold the connection open for the Muse's answer, and gets one JSON line
        back (with ``"reply"`` when it waited). Only root and the command
        account's group can connect.
        """
        path.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
        if path.exists():
            path.unlink()
        server = await asyncio.start_unix_server(self._handle_local, str(path),
                                                 limit=MAX_LOCAL_REQUEST)
        if os.geteuid() == 0:
            os.chown(path, 0, self.executor.account.gid)
        os.chmod(path, 0o660)
        log.info("accepting messages on %s", path)
        return server

    async def _handle_local(self, reader, writer) -> None:
        try:
            line = await asyncio.wait_for(reader.readline(), 10)
            reply = await self._local_request(line)
        except Exception as exc:
            reply = {"ok": False, "error": f"{type(exc).__name__}: {exc}"}
        writer.write(json.dumps(reply).encode() + b"\n")
        try:
            await writer.drain()
        finally:
            writer.close()

    async def _local_request(self, line: bytes) -> dict:
        request = json.loads(line)
        message = request.get("message") if isinstance(request, dict) else None
        if not isinstance(message, str) or not message.strip():
            return {"ok": False, "error": "expected {\"message\": \"...\"}"}
        session_id = request.get("session_id")
        if session_id is not None and not (
            isinstance(session_id, str) and _SESSION_ID_RE.fullmatch(session_id)
        ):
            return {"ok": False, "error": "session_id must be letters, digits and dashes"}
        session = self._current
        if session is None or session.registered_at is None:
            return {"ok": False, "error": "not connected to the Muse"}
        wait = request.get("wait")
        if wait is not None and not (
            isinstance(wait, (int, float)) and not isinstance(wait, bool) and 0 < wait <= MAX_WAIT_S
        ):
            return {"ok": False, "error": f"wait must be 1..{MAX_WAIT_S} seconds"}
        log.info("forwarding a %d-character message to the Muse", len(message))
        if wait:
            return await session.send_chat(message, session_id, wait_reply=wait)
        return await session.send_chat(message, session_id)

    async def _sleep(self, seconds: float) -> None:
        try:
            await asyncio.wait_for(self._stop.wait(), seconds)
        except asyncio.TimeoutError:
            pass


def run_service(identity: Identity, executor: Executor, sdk_token: str | None = None) -> None:
    async def main() -> None:
        service = Service(identity=identity, executor=executor, sdk_token=sdk_token)
        loop = asyncio.get_running_loop()
        for signum in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(signum, service.stop)
        server = await service.serve_local(config.socket_path())
        try:
            await service.run()
        finally:
            server.close()

    asyncio.run(main())
