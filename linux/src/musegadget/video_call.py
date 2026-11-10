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

"""One bounded video call: signaling over Noise, media over RTP.

The gadget asks the VM for the call's media endpoint, sends a media-only JOIN
through ``/voice/calls/join`` and then exchanges SRTP directly with that
endpoint: Opus audio both ways and Muse's H.264 video down (about 1 Mbit/s).
The JOIN carries the SRTP key, so there is no DTLS; connectivity is an ICE-lite
check over UDP, falling back to TCP. ``/voice/end-call`` ends the call.

Input and output match ``VoiceCall``: an async iterator of PCM16LE mono 24 kHz
frames and an object with nonblocking write(pcm) and flush(). The video output
takes H.264 access units through a nonblocking write_unit(bytes); see
voice_video.
"""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import struct
import time
import uuid
from collections import deque
from datetime import datetime, timedelta, timezone

from musegadget.noise import Header
from musegadget.rtc.join import ENDPOINT_TYPES, build_join, parse_join_response
from musegadget.rtc.media import MediaSession
from musegadget.rtc.opus import ensure_available as ensure_opus
from musegadget.voice_audio import Pcm48To24, _upsample24_to_48
from musegadget.voice_call import (FRAME_BYTES, MAX_PLAYBACK_BYTES, SAMPLE_RATE, SETTLE_SECONDS, pcm_rms,
                                   positive_seconds)

log = logging.getLogger(__name__)
FRAME_48K_BYTES = 1920  # 20 ms of mono 48 kHz PCM16
FRAME_SECONDS = 0.02
ECHO_TAIL_SECONDS = 0.3
# Muse's audio carries low-level noise between replies; only louder audio is activity.
REPLY_ACTIVITY_RMS = 300


class CallRejected(ConnectionError):
    pass


# Microphone RMS that counts as speech for the idle timer, as in voice calls.
SPEECH_RMS = 300


def _result(reply: dict, what: str, *, acknowledged: bool = False) -> dict:
    response = reply.get("response")
    if (not reply.get("ok") or not isinstance(response, dict) or response.get("ok") is not True
            or not isinstance(response.get("result"), dict)
            or (acknowledged and response["result"].get("ok") is not True)):
        raise CallRejected(f"{what} failed (HTTP {reply.get('status')})")
    return response["result"]


class VideoCall:
    def __init__(self, session, *, device_id: str, video_output, thread_id: str | None = None,
                 half_duplex: bool = True, mic_threshold: int = 0, idle_timeout: float = 45,
                 overall_timeout: float = 300, ready_timeout: float = 35,
                 cleanup_timeout: float = 10, media_timeout: float = 10) -> None:
        if not 0 <= mic_threshold <= 32767:
            raise ValueError("mic threshold must be between 0 and 32767")
        self.session, self.device_id, self.video_output = session, device_id, video_output
        self.thread_id = thread_id
        self.half_duplex, self.mic_threshold = half_duplex, mic_threshold
        self.idle_timeout = positive_seconds(idle_timeout)
        self.overall_timeout = positive_seconds(overall_timeout)
        self.ready_timeout = positive_seconds(ready_timeout)
        self.cleanup_timeout = positive_seconds(cleanup_timeout)
        self.media_timeout = positive_seconds(media_timeout)
        self.call_id = str(uuid.uuid4())
        self.started_at = datetime.now(timezone.utc)
        self.reason = ""
        self.end_call_acknowledged = False
        self.media: MediaSession | None = None
        self.video_frames = 0
        self.video_size: tuple[int, int] | None = None
        self.video_stopped = False
        self._video_started = False
        self._admission_sent = False
        self._playing_until = 0.0
        self._activity = time.monotonic()
        self._downsample = Pcm48To24()
        self._previous_sample = 0
        self._playback: deque = deque()  # (24 kHz PCM, audible)
        self._playback_bytes = 0
        self.playback_dropped = 0
        self._write_failed = False
        self._live = False
        self._used = False

    async def run(self, source, output, *, realtime_input: bool = False) -> str:
        if self._used:
            raise ValueError("a VideoCall can only run once")
        self._used = True
        self._output = output
        self._live = realtime_input
        started = time.monotonic()
        tasks = []
        error = None
        try:
            await asyncio.wait_for(self._start(), min(self.ready_timeout, self.overall_timeout))
            self._activity = time.monotonic()
            tasks = [asyncio.create_task(self._input(source, realtime_input)),
                     asyncio.create_task(self._play()),
                     asyncio.create_task(self._watchdog(started))]
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
            return self.reason or "remote_end"
        except BaseException as exc:
            error = exc
            raise
        finally:
            for task in tasks:
                task.cancel()
            if tasks:
                # Workers only await sleeps and the audio source; observe their exit.
                await asyncio.wait(tasks, timeout=min(1.0, self.cleanup_timeout))
                for task in tasks:
                    if task.done() and not task.cancelled():
                        task.exception()
            cleanup = asyncio.create_task(asyncio.wait_for(self._cleanup(), self.cleanup_timeout))
            cancelled = False
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
                if error is None and not cancelled:
                    raise
                log.warning("video call cleanup failed (%s)", type(exc).__name__)
            if cancelled and error is None:
                raise asyncio.CancelledError

    async def _start(self) -> None:
        ensure_opus()  # before any request, so a missing libopus admits no call
        endpoint = _result(await self.session.request("GET", "/voice/calls/media-endpoint"),
                           "media endpoint")
        host, port, vip = endpoint.get("host"), endpoint.get("port"), endpoint.get("vip_type")
        if (not isinstance(host, str) or not host or type(port) is not int or not 0 < port < 65536
                or vip not in ENDPOINT_TYPES):
            raise CallRejected("the VM returned an invalid media endpoint")
        offer = build_join(thread_id=self.thread_id, edge_host=host, edge_port=port,
                           vip_type=ENDPOINT_TYPES[vip], video=True)
        body = {"payload_b64": base64.b64encode(offer.payload).decode(), "call_id": self.call_id,
                "call_media_type": "video", "device": "mcu-wearable", "device_id": self.device_id,
                "proactive_greeting_enabled": False,
                "enable_media_to_cosmos": bool(endpoint.get("enable_media_to_cosmos"))}
        if self.thread_id:
            body["thread_id"] = self.thread_id
        # An ambiguous JOIN may still have admitted the call; cleanup ends it.
        self._admission_sent = True
        joined = _result(await self.session.request(
            "POST", "/voice/calls/join", json.dumps(body).encode(),
            headers=[Header("Content-Type", "application/json")],
        ), "call join")
        if not isinstance(joined.get("payload_b64"), str):
            raise CallRejected("the VM admitted no media for the call")
        answer = parse_join_response(base64.b64decode(joined["payload_b64"]))
        self.media = MediaSession(offer, answer, host, port, on_audio=self._audio, on_video=self._video)
        await self.media.connect(timeout=self.ready_timeout)
        log.info("call media connected over %s", self.media.stats.transport)

    def _audio(self, pcm48: bytes) -> None:
        pcm = self._downsample.feed(pcm48)
        for offset in range(0, len(pcm), FRAME_BYTES):
            part = pcm[offset:offset + FRAME_BYTES]
            # Keep latency bounded: a burst after a stall drops the oldest audio.
            while self._playback and self._playback_bytes + len(part) > MAX_PLAYBACK_BYTES:
                dropped, _ = self._playback.popleft()
                self._playback_bytes -= len(dropped)
                self.playback_dropped += 1
            # Muse's stream carries quiet frames between replies; only audible
            # playback can echo into the microphone, so only it gates the mic.
            self._playback.append((part, pcm_rms(part) > REPLY_ACTIVITY_RMS))
            self._playback_bytes += len(part)

    async def _play(self) -> None:
        """Hand audio to the output at its own pace, one frame of at most 40 ms at a time."""
        deadline = time.monotonic()
        while True:
            if not self._playback:
                await asyncio.sleep(0.01)
                deadline = time.monotonic()
                continue
            pcm, audible = self._playback.popleft()
            self._playback_bytes -= len(pcm)
            duration = len(pcm) / (SAMPLE_RATE * 2)
            try:
                self._output.write(pcm)
            except OSError as exc:
                # A live speaker can hiccup; a file that can't be written ends the call.
                if not self._live:
                    raise
                if not self._write_failed:
                    log.warning("could not play call audio (%s)", type(exc).__name__)
                self._write_failed = True
            if audible:
                self._playing_until = time.monotonic() + duration + SETTLE_SECONDS
                self._activity = time.monotonic()
            deadline = max(deadline + duration, time.monotonic() - 0.2)
            await asyncio.sleep(max(0.0, deadline - time.monotonic()))

    def _video(self, unit: bytes, size) -> None:
        if self.video_stopped:
            return
        self.video_frames += 1
        self.video_size = size or self.video_size
        try:
            self.video_output.write_unit(unit)
        except Exception as exc:
            # Video never ends the call; it just stops.
            log.warning("call video stopped (%s)", type(exc).__name__)
            self.video_stopped = True
            return
        if getattr(self.video_output, "needs_keyframe", False):
            self.media.request_keyframe()
        elif not self._video_started:
            # A player may lose the first keyframe while it starts up; a second
            # one a moment later shows the picture without waiting for loss.
            self._video_started = True
            asyncio.get_running_loop().call_later(1.0, self.media.request_keyframe)

    def _gated(self) -> bool:
        return self.half_duplex and time.monotonic() < self._playing_until + ECHO_TAIL_SECONDS

    def _upsample(self, pcm: bytes) -> bytes:
        samples = list(struct.unpack(f"<{len(pcm) // 2}h", pcm))
        if not samples:
            return b""
        output = _upsample24_to_48(samples, self._previous_sample)
        self._previous_sample = samples[-1]
        return struct.pack(f"<{len(output)}h", *output)

    async def _input(self, source, realtime: bool) -> None:
        pending = bytearray()
        silence = bytes(FRAME_48K_BYTES)
        deadline = time.monotonic()
        last_speech = 0.0

        async def pace(frame: bytes) -> None:
            nonlocal deadline
            self.media.send_audio(frame)
            deadline += FRAME_SECONDS
            if not realtime:
                await asyncio.sleep(max(0.0, deadline - time.monotonic()))

        async def send(frame: bytes) -> None:
            nonlocal last_speech
            if self._gated():
                if realtime:
                    # Live capture during playback is mostly echo: drop it.
                    await pace(silence)
                    return
                # A WAV waits for Muse to finish rather than losing input.
                while self._gated():
                    await pace(silence)
            now = time.monotonic()
            rms = pcm_rms(frame)
            if rms >= max(SPEECH_RMS, self.mic_threshold):
                self._activity = last_speech = now
            elif self.mic_threshold and rms < self.mic_threshold and now - last_speech > 0.4:
                frame = silence
            await pace(frame)

        async for pcm in source:
            pending += self._upsample(pcm)
            while len(pending) >= FRAME_48K_BYTES:
                await send(bytes(pending[:FRAME_48K_BYTES]))
                del pending[:FRAME_48K_BYTES]
            if realtime:
                deadline = time.monotonic()
        if pending:
            await send(bytes(pending).ljust(FRAME_48K_BYTES, b"\0"))
        # Keep a quiet microphone running so Muse can end the turn and reply.
        deadline = time.monotonic()
        while True:
            self.media.send_audio(silence)
            deadline += FRAME_SECONDS
            await asyncio.sleep(max(0.0, deadline - time.monotonic()))

    async def _watchdog(self, started: float) -> None:
        while True:
            now = time.monotonic()
            if self.media.lost.is_set() or now - self.media.last_media > self.media_timeout:
                self.reason = "media_lost"
                return
            if now - started >= self.overall_timeout:
                self.reason = "overall_timeout"
                return
            if now >= self._playing_until and now - self._activity >= self.idle_timeout:
                self.reason = "idle_timeout"
                return
            await asyncio.sleep(0.1)

    async def _cleanup(self) -> None:
        try:
            if self.media is not None:
                await self.media.close()
        finally:
            self._playback.clear()
            self._playback_bytes = 0
            try:
                self._output.flush()
            except Exception:
                log.warning("could not flush call audio")
            if self._admission_sent:
                ended = max(datetime.now(timezone.utc), self.started_at + timedelta(milliseconds=1))
                body = {"call_id": self.call_id,
                        "call_started_at": self.started_at.isoformat(timespec="milliseconds"),
                        "call_ended_at": ended.isoformat(timespec="milliseconds")}
                if self.thread_id:
                    body["thread_id"] = self.thread_id
                reply = await self.session.request(
                    "POST", "/voice/end-call", json.dumps(body).encode(),
                    headers=[Header("Content-Type", "application/json")], timeout=self.cleanup_timeout,
                )
                _result(reply, "end-call", acknowledged=True)
                self.end_call_acknowledged = True

    def summary(self) -> dict:
        stats = self.media.stats if self.media else None
        seconds = time.monotonic() - stats.started if stats else 0.0
        return {
            "frames": self.video_frames,
            "size": "x".join(map(str, self.video_size)) if self.video_size else None,
            "fps": round(self.video_frames / seconds, 1) if seconds > 0 else 0.0,
            "kbit_per_s": round(stats.bytes_in * 8 / seconds / 1000) if seconds > 0 else 0,
            "transport": stats.transport if stats else "",
            "stopped": self.video_stopped,
        }
