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
import base64
import enum
import json
import logging
import struct
import time
import uuid
from collections import deque
from dataclasses import dataclass
from contextlib import asynccontextmanager
from typing import AsyncIterator, Callable, Sequence
from urllib.parse import quote

from musegadget.muse_api import user_agent
from musegadget.noise import Header, NoiseTransport, NoiseXXInitiator

log = logging.getLogger(__name__)

NOISE_PATH = "/v1/noise"
CONTROL_PATH = "/link-control"
CHAT_PATH = "/chat/stream"
APP_ID = "musegadget"
REQUEST_TIMEOUT_S = 60
MAX_RESPONSE_BYTES = 1024 * 1024
HANDSHAKE_TIMEOUT_S = 20
PING_INTERVAL_S = 20
MAX_CONCURRENT_INVOKES = 4
MAX_INBOUND_MESSAGE = 4 * 1024 * 1024
MAX_CHAT_EVENT_BYTES = 256 * 1024
MAX_STREAM_BUFFER_BYTES = 1024 * 1024
MAX_TTS_BUFFER_BYTES = 8 * 1024 * 1024
STREAM_CHUNK_BYTES = 16 * 1024
MAX_VOICE_BYTES = 2 * 1024 * 1024
REQUEST_CHUNK_BYTES = 16 * 1024
MAX_INLINE_REQUEST_BYTES = 64 * 1024
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
            except ValueError:  # bad JSON, or bytes that aren't UTF-8
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
        self._requests: dict[int, _Request] = {}
        self._streams: dict[int, HttpStream] = {}
        self._running = False
        self.registered = asyncio.Event()
        self.chat_subscribed = asyncio.Event()
        self.registered_at: float | None = None

    async def run(self, stop: asyncio.Event) -> Outcome:
        self.registered.clear()
        self.chat_subscribed.clear()
        try:
            ws = await self._open()
        except _UpgradeRejected as rejected:
            log.warning("VM refused connection: HTTP %d", rejected.status)
            return Outcome.AUTH_REJECTED if rejected.status == 401 else Outcome.FORBIDDEN
        reader = stopper = None
        try:
            self._ws = ws
            self._transport = await asyncio.wait_for(self._handshake(ws), HANDSHAKE_TIMEOUT_S)
            self._running = True
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
            self._running = False
            self.registered.clear()
            self.chat_subscribed.clear()
            background = []
            for pending in (reader, stopper):
                if pending is not None:
                    pending.cancel()
                    background.append(pending)
            for task in self._tasks:
                task.cancel()
                background.append(task)
            for request in self._requests.values():
                if not request.done.done():
                    request.done.set_exception(ConnectionError("session ended"))
            self._requests.clear()
            for stream in self._streams.values():
                stream.fail(ConnectionError("session ended"))
            self._streams.clear()
            # Closing the socket wakes writers stalled by network backpressure.
            await ws.close()
            if background:
                await asyncio.gather(*background, return_exceptions=True)

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

    async def send_chat(
        self, message: str, session_id: str | None = None, *, output_modality: str = "text",
    ) -> dict:
        """Post a user message to the Muse as coming from this device.

        Sent on this session, so the VM attributes the turn to the device
        registered on it (``device_id``) and routes any follow-up device
        commands back here. ``session_id`` targets a side chat; an id the Muse
        has not seen before starts a new one. Without it the message goes to
        the main chat. Pass ``output_modality="voice"`` to get a reply that
        ``stream_tts`` can speak.
        """
        request_body = {
            "message": message,
            "output_modality": output_modality,
            "device_id": self._device.node_id,
        }
        if session_id:
            request_body["session_id"] = session_id
        return await self._post_chat(request_body)

    async def send_voice(
        self, wav_bytes: bytes, session_id: str | None = None, *, message: str = "",
        output_modality: str | None = None,
    ) -> dict:
        """Post a WAV voice note, optionally including instructions in ``message``.

        Muse chooses its usual reply format unless ``output_modality`` is set.
        Its TTS endpoint requires a voice-modality assistant reply.
        """
        if not wav_bytes or len(wav_bytes) > MAX_VOICE_BYTES:
            raise ValueError("voice note must contain at most 2 MiB of WAV audio")
        request_body = {
            "message": message,
            "device_id": self._device.node_id,
            "items": [{
                "type": "file", "mime_type": "audio/wav", "filename": "voice_note.wav",
                "data_base64": base64.b64encode(wav_bytes).decode("ascii"),
            }],
        }
        if session_id:
            request_body["session_id"] = session_id
        if output_modality is not None:
            request_body["output_modality"] = output_modality
        return await self._post_chat(request_body)

    @staticmethod
    def _http_headers(headers: Sequence[Header] = ()) -> list[Header]:
        return [
            *headers,
            Header("x-request-id", str(uuid.uuid4())),
            Header("x-app-id", APP_ID),
        ]

    async def _post_chat(self, request_body: dict) -> dict:
        if not self._running:
            raise ConnectionError("Muse session is not connected")
        body = json.dumps(request_body).encode()
        headers = self._http_headers([
            Header("Content-Type", "application/json"),
        ])
        upload_in_parts = len(body) > MAX_INLINE_REQUEST_BYTES
        if upload_in_parts:
            encrypted = self._transport.start_stream_request("POST", CHAT_PATH, headers=headers)
        else:
            encrypted = self._transport.encrypt_http_request("POST", CHAT_PATH, body, headers=headers)
        request = _Request(asyncio.get_running_loop().create_future())
        self._requests[encrypted.stream_id] = request
        try:
            await self._send_frames(encrypted.frames)
            if upload_in_parts:
                for offset in range(0, len(body), REQUEST_CHUNK_BYTES):
                    part = body[offset:offset + REQUEST_CHUNK_BYTES]
                    frames = self._transport.encrypt_body_chunk(
                        encrypted.stream_id, part, end_body=offset + len(part) == len(body),
                    )
                    await self._send_frames(frames)
            status, response = await asyncio.wait_for(request.done, REQUEST_TIMEOUT_S)
        except BaseException:
            request.done.cancel()
            await self._reset_stream(encrypted.stream_id)
            raise
        finally:
            self._requests.pop(encrypted.stream_id, None)
        try:
            decoded = json.loads(response) if response else None
        except json.JSONDecodeError:
            decoded = response.decode("utf-8", errors="replace")[:2000]
        return {"ok": 200 <= status < 300, "status": status, "response": decoded}

    @asynccontextmanager
    async def stream_http(
        self, method: str, path: str, body: bytes = b"", *, headers: Sequence[Header] = (),
        max_buffer_bytes: int = MAX_STREAM_BUFFER_BYTES,
    ) -> AsyncIterator[HttpStream]:
        """Open a bounded HTTP response stream; closing it cancels its request.

        The response status and headers are available on entry. Consumers
        iterate byte chunks without holding up the control stream. Pending
        chunks coalesce into 16 KiB buffers. A consumer that exceeds its byte
        budget fails independently; the default budget is 1 MiB.
        """
        if not self._running:
            raise ConnectionError("Muse session is not connected")
        encrypted = self._transport.encrypt_http_request(
            method, path, body, headers=self._http_headers(headers),
        )
        stream = HttpStream(max_buffer_bytes)
        self._streams[encrypted.stream_id] = stream
        try:
            await self._send_frames(encrypted.frames)
            await asyncio.wait_for(stream.ready, REQUEST_TIMEOUT_S)
            yield stream
        finally:
            self._streams.pop(encrypted.stream_id, None)
            if not stream.ended and not stream.reset_requested:
                stream.reset_requested = True
                await self._reset_stream(encrypted.stream_id)
            stream.close()

    async def subscribe_chat(self, session_id: str | None = None) -> AsyncIterator[dict]:
        """Yield Muse chat events from its NDJSON subscription."""
        body = json.dumps({"session_id": session_id} if session_id else {}).encode()
        path = "/chat/subscribe"
        async with self.stream_http("POST", path, body, headers=[
            Header("Content-Type", "application/json"), Header("Accept", "application/x-ndjson"),
        ]) as stream:
            if not 200 <= stream.status < 300:
                raise HttpStreamError(stream.status, path)
            self.chat_subscribed.set()
            try:
                buffered = bytearray()
                async for chunk in stream:
                    for part in chunk.splitlines(keepends=True):
                        buffered += part
                        if len(buffered) > MAX_CHAT_EVENT_BYTES:
                            raise ValueError("Muse chat event exceeds 256 KiB")
                        if not buffered.endswith(b"\n"):
                            continue
                        event = self._chat_event(bytes(buffered))
                        buffered.clear()
                        if event is not None:
                            yield event
                if buffered:
                    event = self._chat_event(bytes(buffered))
                    if event is not None:
                        yield event
            finally:
                self.chat_subscribed.clear()

    @staticmethod
    def _chat_event(raw: bytes) -> dict | None:
        if not raw.strip():
            return None
        try:
            event = json.loads(raw)
        except (json.JSONDecodeError, UnicodeDecodeError):
            log.warning("dropping malformed chat event (%d bytes)", len(raw))
            return None
        return event if isinstance(event, dict) and event.get("type") == "event" else None

    async def stream_tts(self, message_id: str) -> AsyncIterator[bytes]:
        """Yield MP3 for a voice-modality assistant reply, with the legacy fallback."""
        encoded_id = quote(message_id, safe=_URI_COMPONENT_SAFE)
        for prefix in ("/api/voice/tts-stream", "/voice/tts-stream"):
            path = f"{prefix}?message_id={encoded_id}"
            async with self.stream_http("GET", path, headers=[Header("Accept", "audio/mpeg")],
                                        max_buffer_bytes=MAX_TTS_BUFFER_BYTES) as stream:
                if stream.status == 404 and prefix == "/api/voice/tts-stream":
                    continue
                if not 200 <= stream.status < 300:
                    raise HttpStreamError(stream.status, prefix)
                async for chunk in stream:
                    yield chunk
                return

    async def _reset_stream(self, stream_id: int) -> None:
        if not self._running:
            return
        try:
            await self._send_frames(self._transport.encrypt_reset(stream_id, reason="consumer closed"))
        except Exception:
            log.debug("could not cancel closed Muse stream", exc_info=True)

    # -- Sending --------------------------------------------------------------

    async def send(self, message: dict) -> None:
        frames = self._transport.encrypt_body_chunk(self._stream_id, encode_message(message))
        await self._send_frames(frames)

    async def _send_frames(self, frames) -> None:
        # Encryption has already advanced the Noise nonce. Finish this batch
        # before cancellation so the next sender cannot leave a nonce gap.
        sender = asyncio.ensure_future(self._write_frames(frames))
        cancelled = False
        while True:
            try:
                await asyncio.shield(sender)
                break
            except asyncio.CancelledError:
                cancelled = True
                if sender.done():
                    break
            except Exception:
                if not cancelled:
                    raise
                break
        if cancelled:
            if not sender.cancelled():
                sender.exception()
            raise asyncio.CancelledError

    async def _write_frames(self, frames) -> None:
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
                stream = self._streams.get(frame.stream_id)
                if stream is not None:
                    if not stream.on_frame(frame):
                        stream.reset_requested = True
                        task = asyncio.ensure_future(self._reset_stream(frame.stream_id))
                        self._tasks.add(task)
                        task.add_done_callback(self._tasks.discard)
                else:
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
            if message.get("error") or message.get("ok") is False:
                log.error("link.register rejected")
                return Outcome.FORBIDDEN
            else:
                self.registered_at = time.monotonic()
                self.registered.set()
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


class HttpStreamError(ConnectionError):
    """Muse refused an HTTP request carried by the Noise session."""

    def __init__(self, status: int, path: str) -> None:
        super().__init__(f"Muse request {path} failed: HTTP {status}")
        self.status = status
        self.path = path


class HttpStream:
    """Response headers and bounded byte chunks dispatched by the reader."""

    def __init__(self, max_buffer_bytes: int = MAX_STREAM_BUFFER_BYTES) -> None:
        self.ready = asyncio.get_running_loop().create_future()
        self.status = 0
        self.headers: list[Header] = []
        self.ended = False
        self.reset_requested = False
        self._finished = False
        self._chunks: deque[bytearray] = deque()
        self._available = asyncio.Event()
        self._error: Exception | None = None
        self._buffered = 0
        self._max_buffer_bytes = max_buffer_bytes

    def on_frame(self, frame) -> bool:
        if self._finished:
            return True
        if frame.kind == "reset":
            self.ended = True
            self.fail(ConnectionError("Muse response stream reset"))
            return True
        if frame.kind == "response":
            self.status = frame.value.status
            self.headers = list(frame.value.headers)
            if not self.ready.done():
                self.ready.set_result(None)
            data, ended = frame.value.body, frame.value.end_body
        else:
            if not self.ready.done():
                self.fail(ConnectionError("Muse response body arrived without headers"))
                return False
            data, ended = frame.value.data, frame.value.end_body
        if data:
            if self._buffered + len(data) > self._max_buffer_bytes:
                self.fail(BufferError("Muse response consumer fell behind"))
                return False
            self._buffered += len(data)
            offset = 0
            if self._chunks and len(self._chunks[-1]) < STREAM_CHUNK_BYTES:
                take = min(len(data), STREAM_CHUNK_BYTES - len(self._chunks[-1]))
                self._chunks[-1].extend(data[:take])
                offset = take
            while offset < len(data):
                self._chunks.append(bytearray(data[offset:offset + STREAM_CHUNK_BYTES]))
                offset += STREAM_CHUNK_BYTES
            self._available.set()
        if ended:
            self.ended = self._finished = True
            self._available.set()
        return True

    def fail(self, error: Exception) -> None:
        if self._finished:
            return
        self._finished = True
        if not self.ready.done():
            self.ready.set_exception(error)
        self._drain()
        self._error = error
        self._available.set()

    def _drain(self) -> None:
        self._chunks.clear()
        self._buffered = 0

    def close(self) -> None:
        if not self.ready.done():
            self.ready.cancel()
        self._finished = True
        self._error = None
        self._drain()
        self._available.set()

    def __aiter__(self) -> HttpStream:
        return self

    async def __anext__(self) -> bytes:
        while True:
            if self._chunks:
                chunk = self._chunks.popleft()
                self._buffered -= len(chunk)
                return bytes(chunk)
            if self._error is not None:
                raise self._error
            if self._finished:
                raise StopAsyncIteration
            self._available.clear()
            await self._available.wait()


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


class _NoRequest:
    def on_frame(self, frame) -> None:
        pass


_NO_REQUEST = _NoRequest()


class _UpgradeRejected(Exception):
    def __init__(self, status: int) -> None:
        super().__init__(status)
        self.status = status
