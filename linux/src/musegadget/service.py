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
import base64
import json
import logging
import os
import re
import signal
import socket
import time
from dataclasses import dataclass, field

from musegadget import __version__, config, muse_api
from musegadget.executor import Executor
from musegadget.identity import Identity
from musegadget.link_client import (
    MAX_CONCURRENT_INVOKES, DeviceDescription, HttpStreamError, LinkSession, Outcome,
    acknowledgement, check_image,
)
from musegadget.muse_turn import MuseTurn

log = logging.getLogger(__name__)

DEFAULT_NOISE_HOST = "hatch.metaaivm.com"
BACKOFF_BASE_S = 2.0
BACKOFF_MAX_S = 60.0
AUTH_BACKOFF_MIN_S = 15.0
HEALTHY_SESSION_S = 30.0
UNPAIRED_POLL_S = 30.0
MAX_LOCAL_REQUEST = 4 * 1024 * 1024  # room for a 2 MiB image in base64
MAX_WAIT_S = 600
REPLY_POLL_S = 0.5
CLEANUP_S = 2.0
MAX_HELD_EVENTS = 64
MAX_HELD_TEXT_BYTES = 64 * 1024
_SESSION_ID_RE = re.compile(r"[A-Za-z0-9-]{1,64}")
# Device access tokens live about 4 hours; rotate at 3.
TOKEN_REFRESH_AGE_S = 3 * 3600
TOKEN_RETRY_S = 300


@dataclass
class Backoff:
    failures: int = 0
    floor: float = 0.0

    def next_delay(self) -> float:
        # The delay saturates at BACKOFF_MAX_S after a handful of failures, so
        # cap the exponent: 2 ** failures stops converting to a float at 1024,
        # about 17 hours into an outage at the 60 s ceiling.
        delay = min(BACKOFF_BASE_S * 2.0 ** min(self.failures, 16), BACKOFF_MAX_S)
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
    _sdk_token_change_warned: bool = False
    _current: LinkSession | None = None
    _invoke_slots: asyncio.Semaphore = field(
        default_factory=lambda: asyncio.Semaphore(MAX_CONCURRENT_INVOKES), init=False, repr=False,
    )
    # One waiting request at a time per chat: Muse's replies don't name the
    # message they answer, so two waits on one chat would take each other's.
    _reply_waits: dict[str, tuple[asyncio.Lock, int]] = field(default_factory=dict, init=False, repr=False)

    def stop(self) -> None:
        self._stop.set()

    async def run(self) -> None:
        backoff = Backoff()
        token_rejected = False
        while not self._stop.is_set():
            pairing = config.load_json(config.PAIRING_FILE)
            if not pairing:
                log.info("not paired; run `musegadget pair` to set up")
                await self._sleep(UNPAIRED_POLL_S)
                continue
            self._warn_if_sdk_token_changed(pairing)
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
                elif token_rejected:
                    # Allow one immediate retry with the first refreshed token.
                    # Repeated rejection must not spin through successful refreshes.
                    backoff.floor = AUTH_BACKOFF_MIN_S
                    await self._sleep(backoff.next_delay())
                token_rejected = True
                continue
            token_rejected = False
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

    def describe(self) -> DeviceDescription:
        """Describe the device to register with Muse for each session."""
        return DeviceDescription(
            node_id=self.identity.node_id,
            display_name=self.display_name,
            version=__version__,
            commands=self.executor.specs(node_id=self.identity.node_id,
                                         display_name=self.display_name, version=__version__),
        )

    def run_command(self, name: str, params: dict, timeout_ms: int | None = None) -> dict:
        """Handle one command from Muse. LinkSession calls this in a worker thread."""
        return self.executor.run(name, params, timeout_ms)

    async def serve(self, session: LinkSession, vm: dict, stop: asyncio.Event) -> Outcome:
        """Run one session until it ends or ``stop`` is set, and return how it ended.

        Override to run the gadget's own work beside ``session.run(stop)``.
        Clean up that work before returning the session's outcome. An active
        local message socket forwards to this session while it is registered.
        """
        return await session.run(stop)

    async def _session(self, vm: dict, pairing: dict) -> tuple[Outcome, float]:
        session = LinkSession(
            noise_host=pairing.get("noise_host") or DEFAULT_NOISE_HOST,
            vm_id=vm["vm_id"] or vm["vm_name"],
            vm_auth_token=vm["vm_auth_token"],
            device=self.describe(),
            run_command=self.run_command,
            invoke_slots=self._invoke_slots,
        )
        log.info("connecting to %s", vm["vm_name"] or vm["vm_id"])
        started = time.monotonic()
        self._current = session
        try:
            outcome = await self.serve(session, vm, self._stop)
        except Exception as exc:
            log.warning("session failed: %s: %s", type(exc).__name__, exc)
            outcome = Outcome.CLOSED
        finally:
            self._current = None
        lasted = time.monotonic() - (session.registered_at or time.monotonic())
        log.info("session ended: %s after %.0fs", outcome.value, time.monotonic() - started)
        return outcome, lasted

    def _warn_if_sdk_token_changed(self, pairing: dict) -> None:
        """Muse records a device's SDK token only when it pairs, so a token set
        afterwards takes effect only once the device pairs again."""
        paired = pairing.get("sdk_token_hint")
        if self._sdk_token_change_warned or paired is None:
            return
        if paired != config.sdk_token_hint(self.sdk_token):
            self._sdk_token_change_warned = True
            log.warning("paired with SDK token %s, not this one; run `musegadget pair --force` "
                        "to use it", paired)

    async def _maybe_refresh(self, pairing: dict, force: bool = False) -> dict | None:
        """Return current pairing, rotating tokens first if they are due.

        Returns None only when a due refresh failed and the old token should
        not be used yet; the pairing file is removed when the pairing itself
        has been revoked.
        """
        age = time.time() - pairing.get("access_token_saved_at", 0)
        if not force and age < TOKEN_REFRESH_AGE_S:
            return pairing
        if not force and time.monotonic() - self._last_refresh_attempt < TOKEN_RETRY_S:
            return pairing
        self._last_refresh_attempt = time.monotonic()
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
        optional ``"session_id"`` naming a side chat, ``"image"``
        (``{"mime_type", "data_base64", "filename"}``, see
        ``LinkSession.send_image``) and ``"wait"`` (seconds) to hold the
        connection open for the Muse's answer, and gets one JSON line back
        (with ``"reply"`` when it waited). Only root and the command
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
        too_long = False
        try:
            try:
                line = await asyncio.wait_for(reader.readline(), 10)
            except ValueError:  # the line is over the stream limit
                too_long = True
                reply = {"ok": False,
                         "error": f"the request is over {MAX_LOCAL_REQUEST // (1024 * 1024)} MiB"}
            else:
                reply = await self._local_request(line)
        except Exception as exc:
            reply = {"ok": False, "error": f"{type(exc).__name__}: {exc}"}
        writer.write(json.dumps(reply).encode() + b"\n")
        try:
            await writer.drain()
            if too_long:
                # Closing with unread input resets the connection, which can
                # lose the reply before the sender reads it.
                await _discard_line(reader)
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
        image = request.get("image")
        if image is not None:
            if not (isinstance(image, dict) and isinstance(image.get("mime_type"), str)
                    and isinstance(image.get("data_base64"), str)
                    and isinstance(image.get("filename", ""), str)):
                return {"ok": False, "error": "image must be {mime_type, data_base64, filename}"}
            try:
                image_bytes = base64.b64decode(image["data_base64"], validate=True)
            except ValueError:
                return {"ok": False, "error": "image data_base64 is not valid base64"}
            try:
                check_image(image_bytes, image["mime_type"])
            except ValueError as exc:
                return {"ok": False, "error": str(exc)}
        wait = request.get("wait")
        if wait is not None and not (
            isinstance(wait, (int, float)) and not isinstance(wait, bool) and 1 <= wait <= MAX_WAIT_S
        ):
            return {"ok": False, "error": f"wait must be 1 to {MAX_WAIT_S} seconds"}
        session = self._current
        if session is None or session.registered_at is None:
            return {"ok": False, "error": "not connected to the Muse"}
        if image is not None:
            def send():
                log.info("forwarding a %d-character message and an image to the Muse", len(message))
                return session.send_image(
                    image_bytes, image["mime_type"], session_id,
                    message=message, filename=image.get("filename") or None)
        else:
            def send():
                log.info("forwarding a %d-character message to the Muse", len(message))
                return session.send_chat(message, session_id)
        if wait is None:
            return await send()
        return await self._send_and_wait(session, send, session_id, float(wait))

    async def _send_and_wait(self, session, send, session_id, wait_s: float) -> dict:
        """send_and_wait, after any earlier waiting request on the same chat, within ``wait_s``."""
        loop = asyncio.get_running_loop()
        started = loop.time()
        key = session_id or ""
        lock, users = self._reply_waits.get(key, (None, 0))
        lock = lock or asyncio.Lock()
        self._reply_waits[key] = (lock, users + 1)
        try:
            if not await _acquire_within(lock, wait_s):
                return {"ok": False, "error": "another request on this chat was still waiting "
                                              "for its answer; this message was not sent"}
            try:
                return await send_and_wait(session, send, session_id,
                                           wait_s - (loop.time() - started))
            finally:
                lock.release()
        finally:
            lock, users = self._reply_waits[key]
            if users == 1:
                del self._reply_waits[key]
            else:
                self._reply_waits[key] = (lock, users - 1)

    async def _sleep(self, seconds: float) -> None:
        try:
            await asyncio.wait_for(self._stop.wait(), seconds)
        except asyncio.TimeoutError:
            pass


async def send_and_wait(session: LinkSession, send, session_id: str | None,
                        wait_s: float) -> dict:
    """Send a message with ``send()`` and wait up to ``wait_s`` seconds in all for Muse's answer.

    The chat subscription opens before the message goes. As on the ESP32
    firmware, the answer is what follows the message's own ``message.user``
    event, or what names the message as its parent; a :class:`MuseTurn` picks
    the replies out of that. A side chat Muse doesn't know yet is created by
    the message, so the message goes first and the chat's replies are the answer.

    Returns ``send()``'s result, plus ``"reply"`` once Muse acknowledged the
    message: the text of its answer (blank-line separated when it sent several
    messages, "" when it finished without text), or None when no answer came,
    with ``"detail"`` saying why. ``"partial"`` is true when the wait ran out
    while Muse was still answering. A send Muse didn't acknowledge in time
    returns ``"sent": "unknown"``.
    """
    loop = asyncio.get_running_loop()
    deadline = loop.time() + wait_s
    reader = _ReplyReader(MuseTurn(session_id))
    opened = asyncio.Event()

    def follow() -> asyncio.Future:
        return asyncio.ensure_future(_follow(session, session_id, opened, reader))

    follower, sending = follow(), None
    try:
        await _wait_for_event(opened, follower, deadline - loop.time())
        fresh = False
        if not opened.is_set():
            failure = None if not follower.done() or follower.cancelled() else follower.exception()
            if session_id and isinstance(failure, HttpStreamError) and failure.status == 404:
                fresh = True  # Muse doesn't know this side chat; the message creates it
            else:
                why = _stream_problem(follower) if follower.done() else "it did not open in time"
                return {"ok": False,
                        "error": f"could not follow Muse's replies ({why}); the message was not sent"}
        sending = asyncio.ensure_future(send())
        await asyncio.wait({sending}, timeout=max(deadline - loop.time(), 0.0))
        if not sending.done():
            return {"ok": False, "sent": "unknown",
                    "error": f"Muse did not acknowledge the message within {wait_s:g} seconds"}
        result = sending.result()
        if not result.get("ok"):
            return result
        try:
            reader.acknowledge(acknowledgement(result), fresh=fresh)
        except ValueError as exc:
            return {**result, "reply": None, "detail": str(exc)}
        if fresh:
            follower = follow()
        turn = reader.turn
        while True:
            now = time.monotonic()
            # A blank answer waits for settled(), which leaves time for late text.
            if turn.settled(now) or (_reply_text(turn) and turn.complete(now)):
                return {**result, "reply": _reply_text(turn)}
            remaining = deadline - loop.time()
            if follower.done() or remaining <= 0:
                detail = (_stream_problem(follower) if follower.done()
                          else f"no answer within {wait_s:g} seconds")
                text = _reply_text(turn)
                if text:
                    return {**result, "reply": text, "partial": True, "detail": detail}
                return {**result, "reply": None, "detail": detail}
            reader.changed.clear()
            await _wait_for_event(reader.changed, follower, min(remaining, REPLY_POLL_S))
    finally:
        # Bounded, so a stalled transport can't hold the caller or the chat's queue.
        tasks = {task for task in (follower, sending) if task is not None and not task.done()}
        for task in tasks:
            task.cancel()
            task.add_done_callback(_retrieve)
        if tasks:
            await asyncio.wait(tasks, timeout=CLEANUP_S)


async def _follow(session: LinkSession, session_id: str | None, opened: asyncio.Event,
                  reader: _ReplyReader) -> None:
    async for event in session.subscribe_chat(session_id, opened=opened):
        reader.take(event)


class _ReplyReader:
    """Passes MuseTurn the events that follow one request's own message.

    Events repeating an earlier ``seq`` are dropped. Until Muse acknowledges
    the message, events wait here (bounded). Then a ``message.user`` event
    for the acknowledged message opens the answer, and one for any other
    message closes it again; events of a reply already taken, and events
    naming the message as their parent, always count.
    """

    def __init__(self, turn: MuseTurn) -> None:
        self.turn = turn
        self.changed = asyncio.Event()
        self.user_ids: set[str] | None = None
        self.after_note = False
        self.last_seq = 0
        self.held: list[dict] = []
        self.held_text = 0

    def take(self, event: dict) -> None:
        seq = event.get("seq")
        if isinstance(seq, int) and not isinstance(seq, bool):
            if seq <= self.last_seq:
                return
            self.last_seq = seq
        if self.user_ids is None:
            if len(self.held) >= MAX_HELD_EVENTS:
                raise ValueError("too many chat events before Muse acknowledged the message")
            self.held_text += _text_bytes(event)
            if self.held_text > MAX_HELD_TEXT_BYTES:
                raise ValueError("more than 64 KiB of chat text before Muse acknowledged the message")
            self.held.append(event)
            return
        self._route(event)
        self.changed.set()

    def acknowledge(self, response: dict, *, fresh: bool) -> None:
        self.turn.acknowledge(response)
        result = response.get("result", response)
        self.user_ids = {result[key] for key in ("message_id", "reply_to_message_id")
                         if isinstance(result.get(key), str) and result[key]}
        self.after_note = fresh
        held, self.held = self.held, []
        for event in held:
            self._route(event)
        self.changed.set()

    def _route(self, event: dict) -> None:
        payload = event.get("payload")
        if event.get("type") != "event" or not isinstance(payload, dict):
            return
        message_id = payload.get("message_id") or event.get("message_id") or payload.get("id")
        message_id = message_id if isinstance(message_id, str) else None
        if event.get("event") == "message.user":
            self.after_note = message_id in self.user_ids
            return
        parent = payload.get("reply_to_message_id") or payload.get("parent_message_id")
        parent = parent if isinstance(parent, str) else None
        if self.after_note or parent in self.user_ids or message_id in self.turn.replies:
            self.turn.feed(event)


def _text_bytes(event: dict) -> int:
    payload = event.get("payload")
    if not isinstance(payload, dict):
        return 0
    return sum(len(payload[key].encode("utf-8")) for key in ("text", "display_text", "content", "transcript")
               if isinstance(payload.get(key), str))


def _retrieve(task: asyncio.Future) -> None:
    if not task.cancelled():
        task.exception()


def _reply_text(turn: MuseTurn) -> str:
    return "\n\n".join(reply.text.strip() for reply in turn.replies.values() if reply.text.strip())


def _stream_problem(follower: asyncio.Future) -> str:
    """Why the reply stream ended, without text a server could have put there."""
    exc = None if follower.cancelled() else follower.exception()
    if exc is None:
        return "Muse closed the reply stream"
    if isinstance(exc, HttpStreamError):
        return f"HTTP {exc.status}"
    if isinstance(exc, ValueError):  # the size bounds in MuseTurn and subscribe_chat
        return str(exc)
    return f"the reply stream failed ({type(exc).__name__})"


async def _wait_for_event(event: asyncio.Event, task: asyncio.Future, timeout: float) -> None:
    """Wait until ``event`` is set, ``task`` ends or ``timeout`` passes."""
    if event.is_set() or task.done() or timeout <= 0:
        return
    waiter = asyncio.ensure_future(event.wait())
    try:
        await asyncio.wait({waiter, task}, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
    finally:
        waiter.cancel()


async def _acquire_within(lock: asyncio.Lock, timeout: float) -> bool:
    """Acquire ``lock`` within ``timeout`` seconds; a lock acquired just as time ran out is kept."""
    acquire = asyncio.ensure_future(lock.acquire())
    try:
        await asyncio.wait({acquire}, timeout=max(timeout, 0.0))
    except BaseException:
        if acquire.done() and not acquire.cancelled():
            lock.release()
        else:
            acquire.cancel()
        raise
    if not acquire.done():
        acquire.cancel()  # a cancelled acquire never holds the lock
        return False
    return True


async def _discard_line(reader, timeout_s: float = 10) -> None:
    async def discard():
        while True:
            data = await reader.read(64 * 1024)
            if not data or b"\n" in data:
                return

    try:
        await asyncio.wait_for(discard(), timeout_s)
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
