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
Output implements write(pcm) and flush(), both nonblocking. Buffered outputs
can return queued bytes from write() and discarded bytes from flush() for exact
flush events; None assumes immediate playback. The WAV and optional sounddevice
adapters are in voice_audio. No device commands are advertised.
An optional on_event callback receives the reply's lifecycle, transcript and
accepted audio as VoiceEvent values.
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
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from numbers import Integral
from typing import Callable, Optional, Tuple, Union
from urllib.parse import urlencode

from musegadget.noise import Header
from musegadget.streaming import read_ndjson
from musegadget.voice_gate import DROP, HOLD, MAX_IDS, PLAY, STALE, ReplyGate, identifier, provider_id

log = logging.getLogger(__name__)
SAMPLE_RATE = 24000
FRAME_SAMPLES = 960
FRAME_BYTES = FRAME_SAMPLES * 2
FRAME_SECONDS = FRAME_SAMPLES / SAMPLE_RATE
MAX_AUDIO_BYTES = SAMPLE_RATE * 2 * 4
MAX_PLAYBACK_BYTES = SAMPLE_RATE * 2 * 5
# Words held with an unconfirmed reply have their own budget, so observing a
# call never shrinks its held audio, and a count bound covers tiny entries.
MAX_HELD_TEXT_BYTES = 64 * 1024
MAX_HELD_ITEMS = 4096
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


@dataclass(frozen=True)
class ResponseStarted:
    """Muse opened a response (``response.created``)."""

    response_id: Optional[str]


@dataclass(frozen=True)
class TranscriptDelta:
    """More of the words Muse speaks.

    ``response_id`` is that of the latest ``ResponseStarted`` when Muse sent
    the words. Held words keep that ID. Words are dropped with the reply's
    audio, or held with it until its caller's turn confirms it.
    """

    response_id: Optional[str]
    text: str


@dataclass(frozen=True)
class TranscriptDone:
    """The whole transcript of Muse's speech.

    ``response_id`` is that of the latest ``ResponseStarted`` when Muse sent
    the words. Held words keep that ID.
    """

    response_id: Optional[str]
    text: str


@dataclass(frozen=True)
class ResponseDone:
    """Muse closed a response. ``status`` is as reported, such as ``"completed"``
    or ``"cancelled"``, or None. Audio and held words of the response can
    still follow."""

    response_id: Optional[str]
    status: Optional[str]


@dataclass(frozen=True)
class AudioChunk:
    """Reply PCM accepted for playback, delivered before the output writes it.

    ``response_id`` is the latest response when the chunk's provider audio
    stream was first heard, so a reply still playing when the next response
    opens keeps its own. ``offset`` is the number of samples delivered for
    ``response_id`` before this chunk, so ``offset / SAMPLE_RATE`` is where the
    chunk starts in that response's speech.
    """

    response_id: Optional[str]
    offset: int
    pcm: bytes


@dataclass(frozen=True)
class Working:
    """Whether Muse is working on a caller turn: one has not settled, and Muse
    is not waiting on the caller at a conversational boundary."""

    working: bool


@dataclass(frozen=True)
class PlaybackFlushed:
    """Delivered audio that was still queued or playing was discarded.
    ``response_ids`` names the responses it belonged to, in playback order."""

    response_ids: Tuple[Optional[str], ...]


VoiceEvent = Union[ResponseStarted, TranscriptDelta, TranscriptDone, ResponseDone,
                   AudioChunk, Working, PlaybackFlushed]


@dataclass(frozen=True)
class _HeldWords:
    event: TranscriptDelta | TranscriptDone
    providers: list[str]


class VoiceCall:
    def __init__(self, session, *, device_id: str, thread_id: str | None = None,
                 half_duplex: bool = True,
                 mic_threshold: int = 0, idle_timeout: float = 45,
                 overall_timeout: float = 300, ready_timeout: float = 35,
                 cleanup_timeout: float = 10,
                 on_event: Optional[Callable[[VoiceEvent], None]] = None) -> None:
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
        self._barge_in_failure = None
        self._reply_gate = ReplyGate()
        self._on_event = on_event
        self._response_id: str | None = None  # the latest response.created
        self._audio_sources: dict[str, str | None] = {}  # provider id -> response it started in
        self._audio_offsets: dict[str | None, int] = {}
        # Working follows the ESP32 client's rule (muse_vc_call_working), only
        # while observed. It never changes the idle tracking above.
        self._working = False
        self._work_retired: set[str] = set()  # open turns a nothing-pending settlement retired
        self._awaiting_caller = False  # settled at a boundary with work open
        self._awaiting_reply = False  # the latest new caller turn's reply hasn't finished
        self._reply_kept = False
        self._reply_item: str | None = None
        self._reply_response: str | None = None
        self._open_response: str | None = None
        self._open_unmarked = False
        self._waiting: list[tuple[bytearray, str | None] | _HeldWords] = []
        self._waiting_bytes = 0  # held PCM
        self._waiting_text_bytes = 0
        self._waiting_audio = False
        self._playback = deque()
        self._pending_output = deque()
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
            self._barge_in_failure = asyncio.get_running_loop().create_future()
            tasks = [asyncio.create_task(self._receive()),
                     asyncio.create_task(self._input(source, realtime_input)),
                     asyncio.create_task(self._play()),
                     asyncio.create_task(self._watchdog()),
                     self._barge_in_failure]
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
            # Nothing can hand a new failure to run() after it stops waiting.
            self._barge_in_failure.cancel()

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
                finally:
                    # An observer raising CancelledError on PlaybackFlushed
                    # must not skip the explicit end-call.
                    await self._cleanup()
                if pending:
                    raise asyncio.TimeoutError("voice worker shutdown timed out")

            try:
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
            except BaseException as exc:
                error = exc
                raise
            finally:
                if not self._barge_in_failure.cancelled():
                    failure = self._barge_in_failure.exception()
                    if failure is not error:
                        log.warning("voice barge-in flush failed (%s)", type(failure).__name__)

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
                if self._on_event is None:
                    if not self._track_turn(event):
                        continue
                else:
                    message_id = identifier(event.get("response_message_id"))
                    repeat = kind == "hatch.voice.turn_settled" and message_id is not None and (
                        message_id in self._work_retired
                        or message_id in self._settled_turns and message_id not in self._active_turns)
                    turns = len(self._active_turns)
                    tracked = self._track_turn(event)
                    self._follow_work(event, len(self._active_turns) != turns, repeat)
                    if not tracked:
                        continue
            elif kind in ("response.created", "response.done", "response.completed"):
                # A response (including cancelled/failed) is NOT the call and
                # does not settle a caller turn's asynchronous work.
                self._activity = time.monotonic()
                if self._on_event is not None:
                    self._response_event(kind, event)
            elif kind == "response.output_audio_transcript.delta":
                delta = event.get("delta")
                if self._on_event is not None and isinstance(delta, str) and delta:
                    self._words(TranscriptDelta(self._response_id, delta))
            elif kind == "response.output_audio_transcript.done":
                if self._on_event is not None and isinstance(event.get("transcript"), str):
                    self._words(TranscriptDone(self._response_id, event["transcript"]))
            was_holding = self._reply_gate.holding
            self._reply_gate.event(event)
            self._apply_reply_flush()
            if was_holding and not self._reply_gate.holding:
                if self._reply_gate.confirmed and self._waiting:
                    if self._playback_bytes + self._waiting_bytes > MAX_PLAYBACK_BYTES:
                        raise ValueError("voice playback buffer full")
                    for held in self._waiting:
                        if isinstance(held, tuple):
                            self._queue_pcm(bytes(held[0]), held[1])
                        else:
                            self._emit(held.event)
                self._clear_waiting()
            pcm = audio_from_event(event)
            if pcm:
                # Validated zero keepalives may arrive before readiness. Drop
                # them without activating audio or renewing either deadline.
                if not any(pcm) and not self._received_audio and not self._waiting_audio:
                    if self.ready.is_set():
                        if self._reply_gate.audio(event) != DROP:
                            self._audio_response_id(event)
                        self._apply_reply_flush()
                    continue
                if not self.ready.is_set():
                    raise ValueError("audio received before voice readiness")
                verdict = self._reply_gate.audio(event)
                response_id = None if verdict == DROP else self._audio_response_id(event)
                self._apply_reply_flush()
                if verdict == DROP:
                    continue
                if verdict == HOLD:
                    self._hold((pcm, response_id), len(pcm))
                else:
                    self._queue_pcm(pcm, response_id)
        if not self.ready.is_set():
            raise ConnectionError("voice response ended before readiness")
        await self._drain_playback()
        self.reason = "remote_end"

    def _emit(self, event) -> None:
        if self._on_event is not None:
            self._on_event(event)

    def _follow_work(self, event: dict, new_turn: bool, repeat: bool) -> None:
        if event["type"] == "hatch.voice.turn_started":
            item = identifier(event.get("item_id"))
            if new_turn:
                # A replay changes nothing. The new turn is the newest entry.
                self._awaiting_caller = False
                older = list(self._active_turns.values())[:-1]
                if (item != self._reply_item and item not in older
                        and item not in self._settled_turns.values()):
                    # A new caller turn; a delayed reply's turn names an earlier item.
                    self._awaiting_reply, self._reply_kept, self._reply_item = True, False, item
                    self._reply_response = self._open_response if self._open_unmarked else None
        elif not repeat:
            pending = event.get("async_handoffs_pending")
            if isinstance(pending, int) and not isinstance(pending, bool) and pending >= 0:
                # pending >= 1 is a boundary, such as a spoken approval prompt:
                # Muse waits on the caller until a new caller turn starts.
                self._awaiting_caller = pending > 0
                open_turns = [key for key in self._active_turns if key not in self._work_retired]
                if pending == 0 and open_turns:
                    # Muse doesn't settle every caller turn by name. Retire the
                    # open ones, except, once, the latest turn awaiting its reply.
                    keep = self._awaiting_reply and not self._reply_kept
                    self._work_retired.update(key for key in open_turns
                                              if not keep or self._active_turns[key] != self._reply_item)
                    if keep:
                        self._reply_kept = True
                    else:
                        self._awaiting_reply = False
        self._work_retired.intersection_update(self._active_turns)
        working = not self._awaiting_caller and any(
            key not in self._work_retired for key in self._active_turns)
        if working != self._working:
            self._working = working
            self._emit(Working(working))

    def _response_event(self, kind: str, event: dict) -> None:
        response = event.get("response")
        if not isinstance(response, dict):
            response = {}
        response_id = identifier(response.get("id"))
        if kind == "response.created":
            self._response_id = response_id
            submission = identifier(response.get("submission_message_id"))
            self._open_response, self._open_unmarked = response_id, submission is None
            if self._awaiting_reply and response_id is not None and (
                    submission is None
                    or submission not in self._work_retired
                    and self._active_turns.get(submission) == self._reply_item):
                self._reply_response = response_id
            self._emit(ResponseStarted(response_id))
        else:
            if response_id is not None:
                if response_id == self._open_response:
                    self._open_response = None
                if self._awaiting_reply and response_id == self._reply_response:
                    self._awaiting_reply = False
            status = response.get("status")
            self._emit(ResponseDone(response_id, status if isinstance(status, str) else None))

    def _words(self, event: TranscriptDelta | TranscriptDone) -> None:
        verdict = self._reply_gate.words()
        if verdict == HOLD:
            # Each response.created replaces this list, even if its ID repeats.
            held = _HeldWords(event, self._reply_gate.words_ids)
            self._hold(held, len(event.text.encode("utf-8", "surrogatepass")))
        elif verdict == PLAY:
            self._emit(event)

    def _hold(self, held, size: int) -> None:
        audio = isinstance(held, tuple)
        # Held PCM joins the run before it, as one buffer did before events
        # existed: playback sleeps once per queued fragment, and fragments
        # don't count against the item bound.
        joins = (audio and self._waiting and isinstance(self._waiting[-1], tuple)
                 and self._waiting[-1][1] == held[1])
        if (not joins and len(self._waiting) >= MAX_HELD_ITEMS
                or audio and self._waiting_bytes + size > MAX_PLAYBACK_BYTES
                or not audio and self._waiting_text_bytes + size > MAX_HELD_TEXT_BYTES):
            raise ValueError("unconfirmed voice reply buffer full")
        if joins:
            self._waiting[-1][0].extend(held[0])
        elif audio:
            self._waiting.append((bytearray(held[0]), held[1]))
        else:
            self._waiting.append(held)
        if audio:
            self._waiting_bytes += size
            self._waiting_audio = True
        else:
            self._waiting_text_bytes += size

    def _audio_response_id(self, event: dict) -> str | None:
        if self._on_event is None:
            return None
        source = provider_id(event)
        if source is None:
            return self._response_id
        return self._audio_sources.setdefault(source, self._response_id)

    def _clear_waiting(self) -> None:
        self._waiting.clear()
        self._waiting_bytes = self._waiting_text_bytes = 0
        self._waiting_audio = False

    def _queue_pcm(self, pcm: bytes, response_id: str | None) -> None:
        self._received_audio = True
        if self._playback_bytes + len(pcm) > MAX_PLAYBACK_BYTES:
            raise ValueError("voice playback buffer full")
        for offset in range(0, len(pcm), FRAME_BYTES):
            part = pcm[offset:offset + FRAME_BYTES]
            audible = pcm_rms(part) >= 300
            self._playback.append((part, audible, response_id))
            self._audible_frames += int(audible)
            if audible:
                if self.half_duplex:
                    self._mic_epoch += 1
                self._activity = time.monotonic()
        self._playback_bytes += len(pcm)
        if self._on_event is not None:
            # Queued first: if the callback raises, cleanup still reports
            # this audio in PlaybackFlushed. Nothing plays before it returns.
            offset = self._audio_offsets.pop(response_id, 0)
            self._audio_offsets[response_id] = offset + len(pcm) // 2
            if len(self._audio_offsets) > MAX_IDS:
                self._trim_offsets()
            self._emit(AudioChunk(response_id, offset, pcm))

    def _trim_offsets(self) -> None:
        # Forget the oldest responses that can get no more audio: none of it
        # queued, playing or held, no provider stream that can still play,
        # and not the latest. Until enough qualify, keep more than MAX_IDS.
        live = {self._response_id}
        live.update(response_id for _, _, response_id in self._playback)
        live.update(response_id for _, response_id in self._pending_output)
        live.update(held[1] for held in self._waiting if isinstance(held, tuple))
        live.update(response_id for source, response_id in self._audio_sources.items()
                    if self._reply_gate.ids.get(source, STALE) != STALE)
        excess = len(self._audio_offsets) - MAX_IDS
        for response_id in [key for key in self._audio_offsets if key not in live][:excess]:
            del self._audio_offsets[response_id]

    def _apply_reply_flush(self) -> None:
        playing, waiting = self._reply_gate.take_flush()
        if playing:
            self._flush()
        elif waiting:
            if self._reply_gate.holding:
                self._waiting = [held for held in self._waiting
                                 if not isinstance(held, tuple)
                                 and (not held.providers or any(
                                     self._reply_gate.ids[source] != STALE
                                     for source in held.providers))]
                self._waiting_bytes = 0
                self._waiting_text_bytes = sum(len(held.event.text.encode("utf-8", "surrogatepass"))
                                               for held in self._waiting)
                self._waiting_audio = False
            else:
                self._clear_waiting()

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
        error = None
        try:
            self._apply_reply_flush()
        except BaseException as exc:
            error = exc
            if isinstance(exc, StopIteration):
                # Future.set_exception() rejects StopIteration before Python 3.12.
                error = RuntimeError("coroutine raised StopIteration")
                error.__cause__ = exc
        try:
            if response_id and not self._remote_ended:
                try:
                    await asyncio.wait_for(self._event({"type": "response.cancel",
                                                       "response_id": response_id}), 1)
                except Exception:
                    log.warning("could not cancel interrupted response")
        finally:
            if error is not None:
                # Once run() stops waiting, the caller owns the failure.
                if self._barge_in_failure is None or self._barge_in_failure.done():
                    raise error
                self._barge_in_failure.set_exception(error)

    def _flush(self) -> None:
        queued = [response_id for _, _, response_id in self._playback]
        self._clear_waiting()
        self._playback.clear()
        self._playback_bytes = 0
        self._audible_frames = 0
        self._received_audio = False
        self._playing_until = 0
        error = None
        try:
            discarded = self._output.flush()
            if self._on_event is not None:
                self._trim_output(discarded if isinstance(discarded, Integral)
                                  and not isinstance(discarded, bool) else 0)
                queued = [response_id for _, response_id in self._pending_output] + queued
            self._pending_output.clear()
        except BaseException as exc:
            error = exc
        if self._on_event is not None and queued:
            try:
                self._emit(PlaybackFlushed(tuple(dict.fromkeys(queued))))
            except BaseException as exc:
                if error is None:
                    raise
                log.warning("voice playback observer failed (%s)", type(exc).__name__)
        if error is not None:
            raise error

    def _trim_output(self, pending: int) -> None:
        kept = deque()
        for size, response_id in reversed(self._pending_output):
            if pending <= 0:
                break
            count = min(size, pending)
            kept.appendleft((count, response_id))
            pending -= count
        self._pending_output = kept

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
            pcm, audible, response_id = self._playback[0]
            pending = self._output.write(pcm)
            self._playback.popleft()
            self._playback_bytes -= len(pcm)
            self._audible_frames -= int(audible)
            if (self._on_event is not None and isinstance(pending, Integral)
                    and not isinstance(pending, bool)):
                self._pending_output.append((len(pcm), response_id))
                self._trim_output(pending)
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
