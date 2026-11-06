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

"""One bounded, half-duplex-by-default MCU voice call over Noise.

Input is an async iterator of PCM16LE mono 24 kHz frames (up to 960 samples).
Output implements write(pcm) and flush(), both nonblocking. The WAV and optional
sounddevice adapters are in voice_audio. No device commands are advertised.
"""

from __future__ import annotations

import asyncio
import base64
import binascii
import json
import logging
import math
import struct
import time
import uuid
from collections import deque
from datetime import datetime, timedelta, timezone
from urllib.parse import urlencode

from musegadget.noise import Header
from musegadget.streaming import read_ndjson
from musegadget.voice_gate import DROP, HOLD, ReplyGate, identifier

log = logging.getLogger(__name__)
SAMPLE_RATE = 24000
FRAME_SAMPLES = 960
FRAME_BYTES = FRAME_SAMPLES * 2
FRAME_SECONDS = FRAME_SAMPLES / SAMPLE_RATE
MAX_AUDIO_BYTES = SAMPLE_RATE * 2 * 4
MAX_PLAYBACK_BYTES = SAMPLE_RATE * 2 * 5
SETTLE_SECONDS = 0.2
MAX_TRACKED_TURNS = 64
MAX_SETTLED_TURNS = 1024
END_CALL_STOP_OFFER = {"type": "hatch.voice.capability.offer", "end_call_stop": 1}


def positive_seconds(value) -> float:
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise ValueError("timeout must be finite and positive")
    return result


def audio_from_event(event: dict) -> bytes | None:
    kind = event.get("type")
    if kind == "continuous_audio_video.delta":
        audio = event.get("audio_delta")
        if audio is None:
            return None
        if not isinstance(audio, dict):
            raise ValueError("invalid audio delta")
        fmt = audio.get("format")
        if fmt is not None and (not isinstance(fmt, dict)
                                or fmt.get("type", "") not in ("", "audio/pcm")
                                or fmt.get("rate") != SAMPLE_RATE):
            raise ValueError("unsupported audio format; expected PCM16LE mono 24 kHz")
        if audio.get("num_channels", 1) != 1:
            raise ValueError("voice audio must be mono")
        encoded = audio.get("audio_frames")
    elif kind in ("response.output_audio.delta", "response.audio.delta"):
        encoded = event.get("delta")
    else:
        return None
    if not isinstance(encoded, str) or len(encoded) > ((MAX_AUDIO_BYTES + 2) // 3) * 4:
        raise ValueError("invalid or oversized audio delta")
    try:
        pcm = base64.b64decode(encoded, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise ValueError("invalid audio base64") from exc
    if len(pcm) % 2 or len(pcm) > MAX_AUDIO_BYTES:
        raise ValueError("invalid PCM16 audio length")
    return pcm


def pcm_rms(pcm: bytes) -> float:
    samples = struct.unpack("<" + "h" * (len(pcm) // 2), pcm)
    return math.sqrt(sum(value * value for value in samples) / len(samples)) if samples else 0


class _InputGated(Exception):
    """A queued input frame lost admission before encryption."""


class VoiceCall:
    def __init__(self, session, *, device_id: str, thread_id: str | None = None,
                 half_duplex: bool = True,
                 mic_threshold: int = 0, idle_timeout: float = 45,
                 overall_timeout: float = 300, ready_timeout: float = 35,
                 cleanup_timeout: float = 10) -> None:
        if not 0 <= mic_threshold <= 32767:
            raise ValueError("mic threshold must be between 0 and 32767")
        self.session, self.device_id = session, device_id
        self.thread_id = thread_id
        self.session_id: str | None = None  # learned from this fresh call's readiness
        self.half_duplex, self.mic_threshold = half_duplex, mic_threshold
        self.idle_timeout = positive_seconds(idle_timeout)
        self.overall_timeout = positive_seconds(overall_timeout)
        self.ready_timeout = positive_seconds(ready_timeout)
        self.cleanup_timeout = positive_seconds(cleanup_timeout)
        self.call_id = str(uuid.uuid4())
        self.started_at = datetime.now(timezone.utc)
        self.ready = asyncio.Event()
        self.held = False
        self._mic_epoch = 0
        self.reason = ""
        self.cleanup_error: Exception | None = None
        self.end_call_acknowledged = False
        self.media_drain_incomplete = False
        self.advisory_error_count = 0
        self._active_turns: dict[str, str] = {}  # submission ID -> caller input item
        self._settled_turns: dict[str, str] = {}  # retain submission -> item for the whole call
        self._stream = None
        self._admission_sent = False
        self._remote_ended = False
        self._reply_gate = ReplyGate()
        self._waiting_pcm = bytearray()
        self._playback = deque()
        self._playback_bytes = 0
        self._audible_frames = 0
        self._received_audio = False
        self._playing_until = 0.0
        self._activity = time.monotonic()
        self._used = False

    async def run(self, source, output, *, realtime_input: bool = False) -> str:
        """Run until remote stop/EOF, idle/overall deadline, or cancellation.

        WAV input pauses while gated; a live input discards gated capture rather
        than replaying stale microphone samples. EOF keeps a paced quiet-microphone
        stream alive for provider endpointing until the call ends, without a
        manual commit. Quiet PCM never counts as activity. Returns the termination reason.
        """
        if self._used:
            raise ValueError("a VoiceCall can only run once")
        self._used = True
        self._output = output
        self._started_mono = time.monotonic()
        self._ready_deadline = self._started_mono + min(self.ready_timeout, self.overall_timeout)
        tasks = []
        error = None
        try:
            tasks = [asyncio.create_task(self._receive()),
                     asyncio.create_task(self._input(source, realtime_input)),
                     asyncio.create_task(self._play()),
                     asyncio.create_task(self._watchdog())]
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            # Prefer a real failure when EOF and another task finish together.
            for task in done:
                task.result()
            return self.reason or "remote_end"
        except BaseException as exc:
            error = exc
            raise
        finally:
            self._remote_ended = True  # Invalidate queued input before shutdown's first await.

            async def shutdown():
                # Shield the entire shutdown, including asynchronous source
                # finalizers. A second signal must not skip explicit end-call.
                for task in tasks:
                    task.cancel()
                pending = set()
                if tasks:
                    done, pending = await asyncio.wait(
                        tasks, timeout=min(1.0, self.cleanup_timeout),
                    )
                    for task in done:
                        if not task.cancelled():
                            task.exception()
                    # A source may await in its async finalizer. Cancel that
                    # wait too, but never let it delay the remote end-call.
                    for task in pending:
                        task.cancel()
                        task.add_done_callback(
                            lambda finished: None if finished.cancelled() else finished.exception(),
                        )
                try:
                    self._flush()
                except Exception:
                    log.warning("could not flush voice playback")
                await self._cleanup()
                if pending:
                    raise asyncio.TimeoutError("voice worker shutdown timed out")

            cleanup = asyncio.create_task(shutdown())
            cancelled = False
            # Cancellation must not strand the call. Repeated signals do not
            # cancel shutdown; worker joins and remote cleanup have deadlines.
            while not cleanup.done():
                try:
                    await asyncio.shield(cleanup)
                except asyncio.CancelledError:
                    cancelled = True
                except Exception:
                    break
            try:
                cleanup.result()
            except Exception as exc:
                self.cleanup_error = exc
                if error is None and not cancelled:
                    raise
                log.warning("voice cleanup failed (%s)", type(exc).__name__)
            if cancelled and error is None:
                raise asyncio.CancelledError

    async def _start(self) -> None:
        query = {"call_id": self.call_id}
        self._stream = await self.session.stream_request(
            "POST", "/voice/realtime?" + urlencode(query),
            headers=[Header("Content-Type", "application/x-ndjson; charset=utf-8")],
        )
        start = {"type": "hatch.voice.call.start", "call_id": self.call_id,
                 "device": "mcu-wearable", "device_id": self.device_id,
                 "call_media_type": "audio", "proactive_greeting_enabled": False}
        if self.thread_id:
            start["thread_id"] = self.thread_id
        # Even an ambiguous send or a rejected readiness record may have made
        # admission side effects; cleanup uses our own call id, never the peer's.
        self._admission_sent = True
        await self._event(start)
        # This client ends the call on a session_control stop (drain, then
        # end-call), so Muse may end a call that way when the caller says
        # goodbye. A VM that doesn't know the key ignores the offer.
        await self._event(END_CALL_STOP_OFFER)
        await self._stream.wait_response()

    async def _event(self, event: dict) -> None:
        data = json.dumps(event, separators=(",", ":")).encode() + b"\n"
        epoch = self._mic_epoch

        def input_admissible():
            if self._gated() or epoch != self._mic_epoch:
                raise _InputGated

        check = input_admissible if event["type"] in (
            "input_audio_buffer.append", "input_audio_buffer.commit",
        ) else None
        await self._stream.send_body(data, check=check)

    def _confirm(self, event: dict) -> None:
        if self.ready.is_set() or event.get("call_id") != self.call_id:
            raise ValueError("duplicate or mismatched voice call identity")
        session_id = event.get("session_id")
        thread_id = event.get("thread_id")
        if not isinstance(session_id, str) or not session_id.strip():
            raise ValueError("voice readiness is missing session identity")
        if self.thread_id and self.thread_id != thread_id:
            raise ValueError("voice thread identity mismatch")
        if thread_id is not None and (not isinstance(thread_id, str) or not thread_id.strip()):
            raise ValueError("invalid voice thread identity")
        self.session_id, self.thread_id = session_id, thread_id
        self._activity = time.monotonic()
        self.ready.set()

    def _track_turn(self, event: dict) -> bool:
        if event["type"] == "hatch.voice.turn_started":
            message_id = identifier(event.get("submission_message_id"))
            item_id = identifier(event.get("item_id"))
            if message_id is None or item_id is None:
                raise ValueError("invalid voice turn identity")
            previous = self._active_turns.get(message_id) or self._settled_turns.get(message_id)
            if previous is not None:
                if previous != item_id:
                    raise ValueError("conflicting voice turn identity")
                return True  # Valid replay: the gate still needs its caller lineage.
            if len(self._active_turns) >= MAX_TRACKED_TURNS:
                raise ValueError("too many active voice turns")
            self._active_turns[message_id] = item_id
            self._activity = time.monotonic()
            return True
        message_id = identifier(event.get("response_message_id"))
        if message_id is None:
            return False  # An unidentified settlement cannot retire newer work.
        item_id = self._active_turns.get(message_id)
        if item_id is None:
            return False
        # A delayed response has its own submission ID but aliases the original
        # input item. Settle that item, never a later caller's unrelated work.
        completed = []
        for key, item in self._active_turns.items():
            if item == item_id:
                completed.append(key)
            if key == message_id:
                break  # newer aliases for this item still own active work
        if len(self._settled_turns) + len(completed) > MAX_SETTLED_TURNS:
            raise ValueError("too many settled voice turns")
        for key in completed:
            self._settled_turns[key] = self._active_turns.pop(key)
        self._activity = time.monotonic()  # a fresh idle window after settlement
        return True

    def _server_error(self, event: dict) -> None:
        error = event.get("error")
        if not isinstance(error, dict):
            raise ValueError("invalid voice error event")
        if not self.ready.is_set() or error.get("code") == "session_dead":
            raise ConnectionError("voice session failed")
        # Operation/turn errors do not mean this media session ended. In
        # particular, a broad server_error/backend_error type isn't terminal.
        # Never log the server message: it may contain user data or credentials.
        self.advisory_error_count += 1
        if self.advisory_error_count == 1:
            log.warning("voice operation reported an error; session remains open")

    async def _receive(self) -> None:
        await self._start()
        async for event in read_ndjson(self._stream):
            kind = event.get("type")
            if not isinstance(kind, str):
                raise ValueError("voice event has no type")
            if kind == "error":
                self._server_error(event)
                continue
            if kind == "hatch.voice.session":
                self._confirm(event)
            elif kind == "hatch.voice.switch_input_hold":
                if not isinstance(event.get("held"), bool):
                    raise ValueError("invalid input hold event")
                if self.held != event["held"]:
                    self._mic_epoch += 1
                self.held = event["held"]
            elif kind == "input_audio_buffer.speech_started":
                self._activity = time.monotonic()
            elif kind == "conversation.item.added":
                item = event.get("item")
                control_id = item.get("call_id") if isinstance(item, dict) else None
                if (isinstance(item, dict) and item.get("type") == "session_control"
                        and isinstance(control_id, str) and 0 < len(control_id) <= 128
                        and str(item.get("name", "")).strip().lower() == "stop"):
                    if not self.ready.is_set():
                        raise ValueError("stop received before voice readiness")
                    # call_id here is a CONTROL id, not the admission call id.
                    # This dedicated response stream fences the call. Returning
                    # after drain handles the control once; cleanup ignores all
                    # later records, including duplicates.
                    await self._drain_playback()
                    self.reason = "remote_stop"
                    return
            elif kind in ("hatch.voice.turn_started", "hatch.voice.turn_settled"):
                if not self._track_turn(event):
                    continue
            elif kind in ("response.created", "response.done", "response.completed"):
                # A response (including cancelled/failed) is NOT the call and
                # does not settle a caller turn's asynchronous work.
                self._activity = time.monotonic()
            was_waiting = self._reply_gate.waiting
            self._reply_gate.event(event)
            self._apply_reply_flush()
            if was_waiting and not self._reply_gate.waiting:
                if self._reply_gate.playing and self._waiting_pcm:
                    self._queue_pcm(bytes(self._waiting_pcm))
                self._waiting_pcm.clear()
            pcm = audio_from_event(event)
            if pcm:
                # Validated zero keepalives may arrive before readiness. Drop
                # them without activating audio or renewing either deadline.
                if not any(pcm) and not self._received_audio and not self._waiting_pcm:
                    if self.ready.is_set():
                        self._reply_gate.audio(event)
                        self._apply_reply_flush()
                    continue
                if not self.ready.is_set():
                    raise ValueError("audio received before voice readiness")
                verdict = self._reply_gate.audio(event)
                self._apply_reply_flush()
                if verdict == DROP:
                    continue
                if verdict == HOLD:
                    if len(self._waiting_pcm) + len(pcm) > MAX_PLAYBACK_BYTES:
                        raise ValueError("unconfirmed voice reply buffer full")
                    self._waiting_pcm.extend(pcm)
                else:
                    self._queue_pcm(pcm)
        if not self.ready.is_set():
            raise ConnectionError("voice response ended before readiness")
        await self._drain_playback()
        self.reason = "remote_end"

    def _queue_pcm(self, pcm: bytes) -> None:
        self._received_audio = True
        if self._playback_bytes + len(pcm) > MAX_PLAYBACK_BYTES:
            raise ValueError("voice playback buffer full")
        for offset in range(0, len(pcm), FRAME_BYTES):
            part = pcm[offset:offset + FRAME_BYTES]
            audible = pcm_rms(part) >= 300
            self._playback.append((part, audible))
            self._audible_frames += int(audible)
            if audible:
                if self.half_duplex:
                    self._mic_epoch += 1
                self._activity = time.monotonic()
        self._playback_bytes += len(pcm)

    def _apply_reply_flush(self) -> None:
        playing, waiting = self._reply_gate.take_flush()
        if playing:
            self._flush()
        elif waiting:
            self._waiting_pcm.clear()

    async def _drain_playback(self) -> None:
        self._remote_ended = True
        # Keep final speech, but a stalled sink cannot postpone hangup forever.
        deadline = time.monotonic() + MAX_PLAYBACK_BYTES / (SAMPLE_RATE * 2) + SETTLE_SECONDS
        while ((self._playback or time.monotonic() < self._playing_until)
               and time.monotonic() < deadline):
            await asyncio.sleep(FRAME_SECONDS)

    async def barge_in(self) -> None:
        """Fence interrupted audio until a new caller turn; cancel agent work best-effort."""
        response_id = self._reply_gate.open_response
        self._reply_gate.interrupt()
        self._apply_reply_flush()
        if response_id and not self._remote_ended:
            try:
                await asyncio.wait_for(self._event({"type": "response.cancel",
                                                   "response_id": response_id}), 1)
            except Exception:
                log.warning("could not cancel interrupted response")

    def _flush(self) -> None:
        self._waiting_pcm.clear()
        self._playback.clear()
        self._playback_bytes = 0
        self._audible_frames = 0
        self._received_audio = False
        self._playing_until = 0
        self._output.flush()

    def _gated(self) -> bool:
        return self._remote_ended or self.held or (self.half_duplex and
                             (self._audible_frames > 0 or time.monotonic() < self._playing_until))

    async def _input(self, source, realtime: bool) -> None:
        # Let the watchdog own the readiness deadline. On Python 3.9,
        # wait_for(Event.wait()) can swallow cancellation racing with readiness.
        await self.ready.wait()
        last_speech = 0.0
        next_send = time.monotonic()
        frames = source.__aiter__()
        eof = False
        while True:
            if not eof:
                try:
                    pcm = await frames.__anext__()
                except StopAsyncIteration:
                    eof = True
            if eof:
                # Automatic endpointing needs a continuing quiet microphone.
                # Generate one bounded frame at a time.
                pcm = b"\0" * FRAME_BYTES
            if not pcm or len(pcm) > FRAME_BYTES or len(pcm) % 2:
                raise ValueError("input must contain 1–960 PCM16LE samples per frame")
            if realtime and not eof and self._gated():
                continue
            while self._gated():
                await asyncio.sleep(FRAME_SECONDS)
            epoch = self._mic_epoch
            now = time.monotonic()
            rms = pcm_rms(pcm)
            if rms >= max(300, self.mic_threshold):
                self._activity = last_speech = now
            muted = eof or (self.mic_threshold and rms < self.mic_threshold
                            and now - last_speech > 0.4)
            if muted:
                # Provider VAD owns turn boundaries. A manual commit can assign
                # the final transcript to a different item than speech_started.
                pcm = b"\0" * len(pcm)
            await asyncio.sleep(max(0, next_send - now))
            # A hold/playback cycle during pacing invalidates this frame even
            # if the gate reopened before we woke up.
            if epoch != self._mic_epoch:
                continue
            if self._gated():
                if realtime and not eof:
                    continue
                while self._gated():
                    await asyncio.sleep(FRAME_SECONDS)
            try:
                await self._event({"type": "input_audio_buffer.append",
                                   "audio": base64.b64encode(pcm).decode("ascii"),
                                   "send_timestamp_ms": int(time.time() * 1000)})
            except _InputGated:
                continue  # Never replay a frame invalidated while it waited for the wire.
            next_send = time.monotonic() + len(pcm) / (SAMPLE_RATE * 2)

    async def _play(self) -> None:
        while True:
            if not self._playback:
                await asyncio.sleep(0.01)
                continue
            pcm, audible = self._playback.popleft()
            self._playback_bytes -= len(pcm)
            self._audible_frames -= int(audible)
            self._output.write(pcm)
            duration = len(pcm) / (SAMPLE_RATE * 2)
            if audible:
                self._playing_until = time.monotonic() + duration + SETTLE_SECONDS
                self._activity = time.monotonic()
            await asyncio.sleep(duration)

    async def _watchdog(self) -> None:
        while True:
            now = time.monotonic()
            if now - self._started_mono >= self.overall_timeout:
                self.reason = "overall_timeout"
                return
            if not self.ready.is_set() and now >= self._ready_deadline:
                raise asyncio.TimeoutError("voice readiness timed out")
            if (self.ready.is_set() and not self._active_turns
                    and now - self._activity >= self.idle_timeout):
                self.reason = "idle_timeout"
                return
            await asyncio.sleep(min(0.1, self.idle_timeout, self.overall_timeout))

    async def _cleanup(self) -> None:
        async def finish():
            try:
                if self._admission_sent:
                    ended = max(datetime.now(timezone.utc), self.started_at + timedelta(milliseconds=1))
                    body = {"call_id": self.call_id,
                            "call_started_at": self.started_at.isoformat(timespec="milliseconds"),
                            "call_ended_at": ended.isoformat(timespec="milliseconds")}
                    if self.thread_id:
                        body["thread_id"] = self.thread_id
                    result = await self.session.request(
                        "POST", "/voice/end-call", json.dumps(body).encode(),
                        headers=[Header("Content-Type", "application/json")],
                        timeout=self.cleanup_timeout,
                    )
                    response = result.get("response")
                    if (not result.get("ok") or not isinstance(response, dict)
                            or response.get("ok") is not True
                            or not isinstance(response.get("result"), dict)
                            or response["result"].get("ok") is not True):
                        raise ConnectionError("voice end-call was not acknowledged")
                    self.end_call_acknowledged = True
                if self._stream is not None:
                    await self._stream.half_close()
                    async for _ in self._stream:
                        pass
            finally:
                try:
                    self._flush()
                except Exception:
                    log.warning("could not flush voice playback")
        drain_error = None
        try:
            await asyncio.wait_for(finish(), self.cleanup_timeout)
        except Exception as exc:
            if not self.end_call_acknowledged:
                raise
            # The application has ended the call. A delayed media EOF must not
            # undo that ACK if the residual stream can be reset within a bound.
            self.media_drain_incomplete = True
            drain_error = exc
            log.warning("call ended; closing residual voice media stream")
        finally:
            if self._stream is not None:
                try:
                    await asyncio.wait_for(self._stream.aclose(), 1)
                except Exception:
                    if drain_error is not None:
                        raise
                    # The caller sees the original failure, not a reset failure.
                    log.warning("could not close voice stream")
