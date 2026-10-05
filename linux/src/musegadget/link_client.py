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

"""One control session with a Muse VM.

Opens a WebSocket to ``/v1/noise`` with the per-VM bearer in an Authorization
header, runs the Noise XX handshake, then opens a long-lived ``POST
/link-control`` stream. Both directions of that stream carry JSON messages,
each prefixed with its length as a little-endian u32:

* device → VM: ``link.register`` (capabilities), then ``link.result`` for
  each invoke;
* VM → device: the register reply, ``link.invoke`` requests, and events such
  as ``link.unpaired``.

Messages the device sends to the Muse (:meth:`LinkSession.send_chat`) go as
separate ``POST /chat/stream`` requests on the same session.
"""

from __future__ import annotations

import asyncio
import enum
import json
import logging
import struct
import time
import uuid
from dataclasses import dataclass
from typing import Callable
from urllib.parse import quote

from musegadget.muse_api import user_agent
from musegadget.noise import Header, NoiseTransport, NoiseXXInitiator

log = logging.getLogger(__name__)

NOISE_PATH = "/v1/noise"
CONTROL_PATH = "/link-control"
CHAT_PATH = "/chat/stream"
CHAT_SUBSCRIBE_PATH = "/chat/subscribe"
APP_ID = "musegadget"
REQUEST_TIMEOUT_S = 60
CHAT_REPLY_TIMEOUT_S = 120
MAX_RESPONSE_BYTES = 1024 * 1024
MAX_CHAT_EVENT_BYTES = 1024 * 1024
MAX_CHAT_BUFFERED_BYTES = 2 * 1024 * 1024
HANDSHAKE_TIMEOUT_S = 20
PING_INTERVAL_S = 20
MAX_CONCURRENT_INVOKES = 4
MAX_INBOUND_MESSAGE = 4 * 1024 * 1024
# Matches JavaScript's encodeURIComponent, as the firmware does.
_URI_COMPONENT_SAFE = "-_.!~*'()"


class Outcome(enum.Enum):
    CLOSED = "closed"               # connection ended; reconnect normally
    AUTH_REJECTED = "auth_rejected"  # edge refused the VM bearer; re-fetch VMs
    FORBIDDEN = "forbidden"         # authenticated but not allowed right now
    UNPAIRED = "unpaired"           # the Muse removed this device
    STOPPED = "stopped"


@dataclass(frozen=True)
class DeviceDescription:
    node_id: str
    display_name: str
    version: str
    commands: dict

    def register_params(self) -> dict:
        return {
            "node_id": self.node_id,
            "display_name": self.display_name,
            "platform": "linux",
            "version": self.version,
            "device_family": "homehub",
            "model_id": "linux",
            "is_wakeup_supported": False,
            "commands_v2": self.commands,
        }


def encode_message(obj: dict) -> bytes:
    data = json.dumps(obj, separators=(",", ":")).encode()
    return struct.pack("<I", len(data)) + data


class MessageDecoder:
    """Splits the control stream into length-prefixed JSON messages.

    A message may span body chunks, so bytes are buffered until complete.
    """

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> list[dict]:
        self._buf += data
        messages = []
        while len(self._buf) >= 4:
            (length,) = struct.unpack_from("<I", self._buf)
            if length > MAX_INBOUND_MESSAGE:
                raise ValueError(f"inbound message too large: {length}")
            if len(self._buf) < 4 + length:
                break
            raw = bytes(self._buf[4:4 + length])
            del self._buf[:4 + length]
            if not raw:
                continue  # keepalive
            try:
                message = json.loads(raw)
            except json.JSONDecodeError:
                log.warning("dropping malformed control message (%d bytes)", length)
                continue
            if isinstance(message, dict):
                messages.append(message)
        return messages


def noise_url(noise_host: str, vm_id: str) -> str:
    return f"wss://{noise_host}{NOISE_PATH}?vm_id={quote(vm_id, safe=_URI_COMPONENT_SAFE)}"


class LinkSession:
    def __init__(
        self,
        *,
        noise_host: str,
        vm_id: str,
        vm_auth_token: str,
        device: DeviceDescription,
        run_command: Callable[[str, dict, int | None], dict],
        connect=None,
    ) -> None:
        self._url = noise_url(noise_host, vm_id)
        self._token = vm_auth_token
        self._device = device
        self._run_command = run_command
        self._connect = connect
        self._send_lock = asyncio.Lock()
        self._invokes = asyncio.Semaphore(MAX_CONCURRENT_INVOKES)
        self._tasks: set[asyncio.Task] = set()
        self._stream_id = 0
        self._register_id = ""
        self._requests: dict[int, _Request | _ChatSubscription] = {}
        self._chat_lock = asyncio.Lock()
        self.registered_at: float | None = None

    async def run(self, stop: asyncio.Event) -> Outcome:
        try:
            ws = await self._open()
        except _UpgradeRejected as rejected:
            log.warning("VM refused connection: HTTP %d", rejected.status)
            return Outcome.AUTH_REJECTED if rejected.status == 401 else Outcome.FORBIDDEN
        try:
            self._ws = ws
            self._transport = await asyncio.wait_for(self._handshake(ws), HANDSHAKE_TIMEOUT_S)
            await self._open_control_stream()
            reader = asyncio.ensure_future(self._read_loop())
            stopper = asyncio.ensure_future(stop.wait())
            done, _ = await asyncio.wait({reader, stopper}, return_when=asyncio.FIRST_COMPLETED)
            if stopper in done:
                reader.cancel()
                return Outcome.STOPPED
            stopper.cancel()
            return reader.result()
        finally:
            for task in self._tasks:
                task.cancel()
            for request in self._requests.values():
                if isinstance(request, _Request) and not request.done.done():
                    request.done.set_exception(ConnectionError("session ended"))
                elif isinstance(request, _ChatSubscription):
                    request.fail(ConnectionError("session ended"))
            self._requests.clear()
            await ws.close()

    # -- Connection setup -----------------------------------------------------

    async def _open(self):
        connect = self._connect
        headers = {"Authorization": f"Bearer {self._token}"}
        if connect is not None:
            return await connect(self._url, headers)
        from websockets.asyncio.client import connect
        from websockets.exceptions import InvalidStatus

        try:
            return await connect(
                self._url,
                additional_headers=headers,
                user_agent_header=user_agent(),
                open_timeout=HANDSHAKE_TIMEOUT_S,
                ping_interval=PING_INTERVAL_S,
                ping_timeout=PING_INTERVAL_S,
                max_size=None,
            )
        except InvalidStatus as exc:
            status = exc.response.status_code
            if status in (401, 403):
                raise _UpgradeRejected(status) from None
            raise

    async def _handshake(self, ws) -> NoiseTransport:
        initiator = NoiseXXInitiator()
        initiator.initialize()
        await ws.send(initiator.write_message1())
        msg2 = await ws.recv()
        if isinstance(msg2, str):
            raise ConnectionError("Noise handshake got a text frame")
        initiator.read_message2(bytes(msg2))
        # The bearer already authenticated us at the upgrade; message 3
        # carries an empty payload.
        await ws.send(initiator.write_message3())
        send, recv = initiator.split()
        log.info("Noise session established")
        return NoiseTransport(send, recv)

    async def _open_control_stream(self) -> None:
        encrypted = self._transport.start_stream_request("POST", CONTROL_PATH)
        self._stream_id = encrypted.stream_id
        await self._send_frames(encrypted.frames)
        self._register_id = str(uuid.uuid4())
        await self.send({
            "type": "req",
            "id": self._register_id,
            "method": "link.register",
            "params": self._device.register_params(),
        })
        log.info("sent link.register as %s", self._device.node_id)

    # -- Device-originated requests -------------------------------------------

    async def send_chat(self, message: str, session_id: str | None = None) -> dict:
        """Post a user message on this device's authenticated session.

        The VM attributes the turn to the device registered on this session
        and routes any follow-up device commands back here. ``session_id``
        targets a side chat; an id the Muse has not seen before starts a new
        one. Without it the message goes to the main chat.
        """
        return await self._post_chat(message, session_id)

    async def ask_chat(self, message: str, session_id: str | None = None) -> str:
        """Post a user message and wait for the related assistant reply."""
        async with self._chat_lock:
            subscription = await self._subscribe_chat()
            try:
                result = await self._post_chat(message, session_id)
                if not result["ok"]:
                    raise RuntimeError(f"Muse rejected the message (HTTP {result['status']})")
                response = result.get("response")
                if not isinstance(response, dict):
                    raise RuntimeError("Muse returned an invalid chat acknowledgement")
                acknowledgement = response.get("result", response)
                if not isinstance(acknowledgement, dict):
                    raise RuntimeError("Muse returned an invalid chat acknowledgement")
                message_id = acknowledgement.get("message_id")
                if not isinstance(message_id, str) or not message_id:
                    raise RuntimeError("Muse acknowledgement did not include a message id")
                parent_id = acknowledgement.get("reply_to_message_id")
                if not isinstance(parent_id, str):
                    parent_id = ""
                try:
                    return await asyncio.wait_for(
                        self._wait_for_chat_reply(subscription, message_id, parent_id),
                        CHAT_REPLY_TIMEOUT_S,
                    )
                except asyncio.TimeoutError:
                    seen = "; ".join(subscription.seen_events) or "no event messages received"
                    raise TimeoutError(
                        f"timed out waiting for a related Muse reply; events received: {seen}"
                    ) from None
            finally:
                await self._close_chat_subscription(subscription)

    async def _post_chat(self, message: str, session_id: str | None) -> dict:
        request_body = {
            "message": message,
            "output_modality": "text",
            "device_id": self._device.node_id,
        }
        if session_id:
            request_body["session_id"] = session_id
        body = json.dumps(request_body).encode()
        headers = [
            Header("Content-Type", "application/json"),
            Header("x-request-id", str(uuid.uuid4())),
            Header("x-app-id", APP_ID),
        ]
        encrypted = self._transport.encrypt_http_request("POST", CHAT_PATH, body, headers=headers)
        request = _Request(asyncio.get_running_loop().create_future())
        self._requests[encrypted.stream_id] = request
        try:
            await self._send_frames(encrypted.frames)
            status, response = await asyncio.wait_for(request.done, REQUEST_TIMEOUT_S)
        finally:
            self._requests.pop(encrypted.stream_id, None)
        try:
            decoded = json.loads(response) if response else None
        except json.JSONDecodeError:
            decoded = response.decode("utf-8", errors="replace")[:2000]
        return {"ok": 200 <= status < 300, "status": status, "response": decoded}

    async def _subscribe_chat(self) -> "_ChatSubscription":
        headers = [
            Header("Content-Type", "application/json"),
            Header("Accept", "application/x-ndjson"),
            Header("x-request-id", str(uuid.uuid4())),
            Header("x-app-id", APP_ID),
        ]
        encrypted = self._transport.encrypt_http_request(
            "POST", CHAT_SUBSCRIBE_PATH, b"{}", headers=headers,
        )
        subscription = _ChatSubscription()
        subscription.stream_id = encrypted.stream_id
        self._requests[encrypted.stream_id] = subscription
        try:
            await self._send_frames(encrypted.frames)
        except Exception:
            self._requests.pop(encrypted.stream_id, None)
            raise
        return subscription

    async def _wait_for_chat_reply(
        self, subscription: "_ChatSubscription", message_id: str, parent_id: str,
    ) -> str:
        messages: dict[str, dict] = {}
        rejected_replies: set[str] = set()
        while True:
            event = await subscription.next_event()
            if isinstance(event, Exception):
                raise ConnectionError(f"Muse chat subscription failed: {event}") from event
            if event.get("type") != "event":
                continue
            payload = event.get("payload")
            if not isinstance(payload, dict):
                payload = event
            event_name = event.get("event") or payload.get("event_name") or payload.get("event")
            if event_name not in (
                "delta.message_start", "delta.text_append", "delta.message_done",
                "message.assistant",
            ):
                continue
            reply_id = (
                payload.get("message_id") or payload.get("id")
                or event.get("message_id") or event.get("id")
            )
            if not isinstance(reply_id, str) or not reply_id:
                continue
            if reply_id in rejected_replies:
                continue
            reply_to = (
                payload.get("reply_to_message_id") or payload.get("parent_message_id")
                or event.get("reply_to_message_id") or event.get("parent_message_id") or ""
            )
            if not isinstance(reply_to, str):
                reply_to = ""
            if reply_to and reply_to not in (message_id, parent_id, reply_id):
                rejected_replies.add(reply_id)
                messages.pop(reply_id, None)
                continue
            state = messages.setdefault(reply_id, {"text": "", "reply_to": reply_to, "bytes": 0})
            if reply_to:
                state["reply_to"] = reply_to
            if event_name == "delta.message_start":
                state["text"] = ""
            elif event_name == "delta.text_append":
                text = payload.get("text")
                if not isinstance(text, str):
                    text = event.get("text")
                if isinstance(text, str):
                    state["bytes"] += len(text.encode("utf-8"))
                    if state["bytes"] > MAX_RESPONSE_BYTES:
                        raise ValueError("Muse reply too large")
                    state["text"] += text
            elif event_name == "delta.message_done":
                if state["text"]:
                    return state["text"]
            elif event_name == "message.assistant":
                if payload.get("display_text_ready", event.get("display_text_ready", True)) is False:
                    continue
                text = payload.get("display_text")
                if not isinstance(text, str):
                    text = event.get("display_text")
                if not isinstance(text, str):
                    text = payload.get("content")
                if not isinstance(text, str):
                    text = event.get("content")
                if not isinstance(text, str):
                    text = payload.get("text")
                if not isinstance(text, str):
                    text = event.get("text")
                if isinstance(text, str) and text:
                    if len(text.encode("utf-8")) > MAX_RESPONSE_BYTES:
                        raise ValueError("Muse reply too large")
                    return text
                if state["text"]:
                    return state["text"]

    async def _close_chat_subscription(self, subscription: "_ChatSubscription") -> None:
        stream_id = subscription.stream_id
        if stream_id is None:
            return
        self._requests.pop(stream_id, None)
        try:
            frames = self._transport.encrypt_reset(stream_id, reason="chat reply received")
            await self._send_frames(frames)
        except Exception as exc:
            log.warning("could not close Muse chat subscription: %s", exc)

    # -- Sending --------------------------------------------------------------

    async def send(self, message: dict) -> None:
        frames = self._transport.encrypt_body_chunk(self._stream_id, encode_message(message))
        await self._send_frames(frames)

    async def _send_frames(self, frames) -> None:
        async with self._send_lock:
            for frame in frames:
                await self._ws.send(frame)

    # -- Receiving ------------------------------------------------------------

    async def _read_loop(self) -> Outcome:
        decoder = MessageDecoder()
        while True:
            try:
                raw = await self._ws.recv()
            except Exception as exc:
                log.info("control connection closed: %s", exc)
                return Outcome.CLOSED
            if isinstance(raw, str):
                log.warning("ignoring text frame on Noise connection")
                continue
            frame = self._transport.decrypt_frame(bytes(raw))
            if frame is None:
                continue
            if frame.stream_id != self._stream_id:
                self._requests.get(frame.stream_id, _NO_REQUEST).on_frame(frame)
                continue
            if frame.kind == "reset":
                log.warning("control stream reset: %s", frame.value.reason)
                return Outcome.CLOSED
            if frame.kind == "response":
                if frame.value.status >= 400:
                    log.warning("/link-control refused: HTTP %d", frame.value.status)
                    return Outcome.FORBIDDEN if frame.value.status == 403 else Outcome.CLOSED
                data, ended = frame.value.body, frame.value.end_body
            else:
                data, ended = frame.value.data, frame.value.end_body
            for message in decoder.feed(data):
                outcome = self._handle(message)
                if outcome is not None:
                    return outcome
            if ended:
                log.info("control stream ended by VM")
                return Outcome.CLOSED

    def _handle(self, message: dict) -> Outcome | None:
        if message.get("id") == self._register_id and message.get("method") is None:
            if message.get("error"):
                log.error("link.register rejected: %s", message["error"])
            else:
                self.registered_at = time.monotonic()
                log.info("registered with the Muse")
            return None
        event = message.get("event")
        if event in ("link.unpaired", "node.unpaired"):
            log.warning("the Muse removed this device")
            return Outcome.UNPAIRED
        if message.get("method") == "link.invoke":
            task = asyncio.ensure_future(self._invoke(message))
            self._tasks.add(task)
            task.add_done_callback(self._tasks.discard)
        return None

    async def _invoke(self, message: dict) -> None:
        invoke_id = message.get("id")
        command = message.get("command") or ""
        params = message.get("params") if isinstance(message.get("params"), dict) else {}
        timeout_ms = message.get("timeout_ms") or None
        if not invoke_id:
            return
        shown = printable(command)
        log.info("invoke %s", shown)
        async with self._invokes:
            started = time.monotonic()
            result = await asyncio.get_running_loop().run_in_executor(
                None, self._run_command, command, params, timeout_ms,
            )
            log.info("%s %s in %d ms", shown, describe_result(result),
                     (time.monotonic() - started) * 1000)
        await self.send({"method": "link.result", "id": invoke_id, **result})


def printable(text: str) -> str:
    """text with control characters replaced, so it can't forge log lines."""
    return "".join(ch if ch.isprintable() else "?" for ch in text)


def describe_result(result: dict) -> str:
    """How an invoke ended, for the log. Never its parameters, output or error,
    which can echo them."""
    if not result.get("ok"):
        return "failed"
    payload = result.get("payload")
    if isinstance(payload, dict) and isinstance(payload.get("exit_code"), int):
        timed_out = ", timed out" if payload.get("timed_out") else ""
        return f"ok, exit {payload['exit_code']}{timed_out}"
    return "ok"


class _Request:
    """Collects the response to one request stream."""

    def __init__(self, done: asyncio.Future) -> None:
        self.done = done
        self.status = 0
        self.body = bytearray()

    def on_frame(self, frame) -> None:
        if self.done.done():
            return
        if frame.kind == "reset":
            self.done.set_exception(ConnectionError(f"stream reset: {frame.value.reason}"))
            return
        if frame.kind == "response":
            self.status = frame.value.status
            data, ended = frame.value.body, frame.value.end_body
        else:
            data, ended = frame.value.data, frame.value.end_body
        self.body += data
        if len(self.body) > MAX_RESPONSE_BYTES:
            self.done.set_exception(ValueError("response too large"))
        elif ended:
            self.done.set_result((self.status, bytes(self.body)))


class _ChatSubscription:
    """Collect bounded NDJSON events from a live Muse chat subscription."""

    def __init__(self) -> None:
        self.events: asyncio.Queue[tuple[dict | Exception, int]] = asyncio.Queue(maxsize=128)
        self.status = 0
        self.stream_id: int | None = None
        self._line = bytearray()
        self._failed = False
        self._queued_bytes = 0
        self.seen_events: list[str] = []

    async def next_event(self) -> dict | Exception:
        event, size = await self.events.get()
        self._queued_bytes -= size
        return event

    def on_frame(self, frame) -> None:
        if frame.kind == "reset":
            self._fail(ConnectionError(f"stream reset: {frame.value.reason}"))
            return
        if frame.kind == "response":
            self.status = frame.value.status
            data, ended = frame.value.body, frame.value.end_body
            if self.status >= 400:
                self._fail(ConnectionError(f"HTTP {self.status}"))
                return
        else:
            data, ended = frame.value.data, frame.value.end_body
        if self._failed:
            return
        offset = 0
        while offset < len(data):
            newline = data.find(b"\n", offset)
            if newline < 0:
                self._line.extend(data[offset:])
                if len(self._line) > MAX_CHAT_EVENT_BYTES:
                    self._fail(ValueError("Muse chat event too large"))
                break
            self._line.extend(data[offset:newline])
            if len(self._line) > MAX_CHAT_EVENT_BYTES:
                self._fail(ValueError("Muse chat event too large"))
                return
            line = bytes(self._line)
            self._line.clear()
            offset = newline + 1
            if not self._queue_event_line(line):
                return
        if ended:
            if self._line.strip():
                self._fail(ConnectionError("Muse chat subscription ended mid-event"))
            else:
                self._fail(ConnectionError("Muse chat subscription ended"))

    def _fail(self, error: Exception) -> None:
        if self._failed:
            return
        self._failed = True
        while not self.events.empty():
            self.events.get_nowait()
        self._queued_bytes = 0
        self.events.put_nowait((error, 0))

    def _queue_event_line(self, line: bytes) -> bool:
        if not line.strip():
            return True
        try:
            event = json.loads(line)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            self._fail(ValueError(f"invalid Muse chat event: {exc}"))
            return False
        if not isinstance(event, dict):
            return True
        payload = event.get("payload")
        if not isinstance(payload, dict):
            payload = event
        event_name = event.get("event") or payload.get("event_name") or payload.get("event")
        if event.get("type") == "event":
            message_id = payload.get("message_id") or payload.get("id")
            if not isinstance(message_id, str):
                message_id = event.get("message_id") or event.get("id")
            parent_id = (
                payload.get("reply_to_message_id") or payload.get("parent_message_id")
                or event.get("reply_to_message_id") or event.get("parent_message_id")
            )
            self.seen_events.append(
                f"{event_name or 'unknown'}(id={message_id or '-'}, parent={parent_id or '-'})"
            )
            del self.seen_events[:-8]
        if event.get("type") != "event" or event_name not in (
            "delta.message_start", "delta.text_append", "delta.message_done",
            "message.assistant",
        ):
            return True
        event_size = len(line)
        if self._queued_bytes + event_size > MAX_CHAT_BUFFERED_BYTES:
            self._fail(ValueError("Muse chat reply buffer is full"))
            return False
        try:
            self.events.put_nowait((event, event_size))
        except asyncio.QueueFull:
            self._fail(ValueError("Muse chat event queue is full"))
            return False
        self._queued_bytes += event_size
        return True

    def fail(self, error: Exception) -> None:
        self._fail(error)


class _NoRequest:
    def on_frame(self, frame) -> None:
        pass


_NO_REQUEST = _NoRequest()


class _UpgradeRejected(Exception):
    def __init__(self, status: int) -> None:
        super().__init__(status)
        self.status = status
