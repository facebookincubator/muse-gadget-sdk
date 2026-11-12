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

"""Call media over ICE-lite: UDP with ICE-TCP (RFC 4571) fallback, SRTP, RTP."""

from __future__ import annotations

import asyncio
import random
import socket
import struct
import time
from collections.abc import Callable
from dataclasses import dataclass, field

from . import stun
from .join import JoinAnswer, JoinOffer
from .opus import OpusDecoder, OpusEncoder
from .rtp import H264Depacketizer, build_pli, build_rtp, is_rtcp, parse_rtp, sps_dimensions
from .srtp import SrtpError, SrtpSession

OPUS_PT = 111
H264_PT = 127
CLIENT_AUDIO_SSRC = 100
CLIENT_VIDEO_SSRC = 101
SERVER_AUDIO_SSRC = 200
SERVER_VIDEO_SSRC = 201
CHECK_INTERVAL = 0.2
CONSENT_INTERVAL = 4.0
CONSENT_TIMEOUT = 30.0  # RFC 7675: stop sending once checks go this long unanswered
CHECK_RESPONSE_WINDOW = 5.0  # an answer older than this is not fresh consent


@dataclass
class MediaStats:
    transport: str = ""
    bytes_in: int = 0
    rtp_audio: int = 0
    rtp_video: int = 0
    rtcp: int = 0
    srtp_errors: int = 0
    decode_errors: int = 0
    late_audio: int = 0
    video_frames: int = 0
    plis_sent: int = 0
    started: float = field(default_factory=time.monotonic)


class _Path:
    def __init__(self, kind: str, session: "MediaSession") -> None:
        self.kind = kind
        self.session = session
        self.send: Callable[[bytes], None] = lambda data: None
        self.close: Callable[[], None] = lambda: None


class _Udp(asyncio.DatagramProtocol):
    def __init__(self, path: _Path) -> None:
        self.path = path

    def datagram_received(self, data: bytes, addr) -> None:
        self.path.session._receive(self.path, data)


class MediaSession:
    def __init__(
        self,
        offer: JoinOffer,
        answer: JoinAnswer,
        host: str,
        port: int,
        *,
        on_audio: Callable[[bytes], None] | None = None,
        on_video: Callable[[bytes, tuple[int, int] | None], None] | None = None,
        tcp: bool = True,
        udp: bool = True,
    ) -> None:
        self.offer, self.answer = offer, answer
        self.host, self.port = host, port
        self.on_audio, self.on_video = on_audio, on_video
        self.use_tcp, self.use_udp = tcp, udp
        self.stats = MediaStats()
        self._srtp = SrtpSession(offer.srtp_key)
        self._username = f"{answer.server_ufrag}:{offer.client_ufrag}"
        self._tie = random.getrandbits(64)
        self._paths: list[_Path] = []
        self._pending: dict[bytes, tuple[_Path, float]] = {}  # txid -> (path, sent at)
        self._selected: _Path | None = None
        self._connected = asyncio.Event()
        self._tasks: list[asyncio.Task] = []
        self._h264 = H264Depacketizer()
        self._decoder = OpusDecoder() if on_audio else None
        self._encoder: OpusEncoder | None = None
        self._seq = random.randrange(1 << 16)
        self._ts = random.getrandbits(32)
        self._last_pli = 0.0
        self._dims: tuple[int, int] | None = None
        self._audio_seq: int | None = None
        self._closed = False
        self.last_media = time.monotonic()
        self.last_consent = time.monotonic()
        self.lost = asyncio.Event()

    async def connect(self, timeout: float = 10.0) -> str:
        infos = await asyncio.get_running_loop().getaddrinfo(self.host, self.port, type=socket.SOCK_STREAM)
        addrs = list(dict.fromkeys((info[0], info[4][:2]) for info in infos))
        if self.answer.ipv6:
            addrs.insert(0, (socket.AF_INET6, (self.answer.ipv6, self.port)))
        if self.answer.ipv4:
            addrs.insert(0, (socket.AF_INET, (self.answer.ipv4, self.port)))
        for family, addr in addrs:
            if self.use_udp:
                self._tasks.append(asyncio.create_task(self._open_udp(family, addr)))
            if self.use_tcp:
                self._tasks.append(asyncio.create_task(self._open_tcp(addr)))
        checks = asyncio.create_task(self._checks())
        try:
            await asyncio.wait_for(self._connected.wait(), timeout)
        except asyncio.TimeoutError:
            await self.close()
            raise ConnectionError("no ICE connectivity to the media edge") from None
        finally:
            checks.cancel()
        for path in self._paths:
            if path is not self._selected:
                path.close()
        self._paths = [self._selected]
        self.stats.transport = self._selected.kind
        self.last_media = self.last_consent = time.monotonic()
        # Racing checks only test paths; nominate the one that won.
        self._check(self._selected, nominate=True)
        self._tasks.append(asyncio.create_task(self._consent()))
        self.request_keyframe()
        return self._selected.kind

    async def _open_udp(self, family: int, addr) -> None:
        path = _Path(f"udp{'6' if family == socket.AF_INET6 else '4'}", self)
        try:
            transport, _ = await asyncio.get_running_loop().create_datagram_endpoint(
                lambda: _Udp(path), remote_addr=addr, family=family
            )
        except OSError:
            return
        path.send = transport.sendto
        path.close = transport.close
        if self._selected is not None or self._closed:
            path.close()
            return
        self._paths.append(path)

    async def _open_tcp(self, addr) -> None:
        path = _Path("tcp", self)
        try:
            reader, writer = await asyncio.wait_for(asyncio.open_connection(addr[0], addr[1]), 5)
        except (OSError, asyncio.TimeoutError):
            return
        path.send = lambda data: writer.write(struct.pack("!H", len(data)) + data)
        path.close = writer.close
        if self._selected is not None or self._closed:
            path.close()
            return
        self._paths.append(path)
        try:
            while True:
                size = struct.unpack("!H", await reader.readexactly(2))[0]
                self._receive(path, await reader.readexactly(size))
        except (asyncio.IncompleteReadError, OSError):
            if path is self._selected and not self._closed:
                self._selected = None
                self.lost.set()

    def _check(self, path: _Path, *, nominate: bool = False) -> None:
        priority = (110 if path.kind.startswith("udp") else 90) << 24 | 0xFFFF << 8 | 255
        packet, txid = stun.binding_request(self._username, self.offer.server_pwd, priority, self._tie,
                                            nominate=nominate)
        self._pending[txid] = (path, time.monotonic())
        if len(self._pending) > 64:
            self._pending.pop(next(iter(self._pending)))
        path.send(packet)

    async def _checks(self) -> None:
        while True:
            for path in list(self._paths):
                self._check(path)
            await asyncio.sleep(CHECK_INTERVAL)

    async def _consent(self) -> None:
        while not self._closed:
            await asyncio.sleep(CONSENT_INTERVAL)
            if self._selected is None or time.monotonic() - self.last_consent > CONSENT_TIMEOUT:
                self.lost.set()
                return
            self._check(self._selected, nominate=True)

    def _receive(self, path: _Path, data: bytes) -> None:
        self.stats.bytes_in += len(data)
        if stun.is_stun(data):
            txid = data[8:20]
            sent_path, sent_at = self._pending.get(txid, (None, 0.0))
            if (sent_path is path and time.monotonic() - sent_at <= CHECK_RESPONSE_WINDOW
                    and stun.parse_response(data, txid, self.offer.server_pwd)):
                del self._pending[txid]
                if path is self._selected:
                    self.last_consent = time.monotonic()
                if self._selected is None:
                    self._selected = path
                    self._connected.set()
            return
        if path is not self._selected or len(data) < 12:
            return
        if is_rtcp(data):
            # The endpoint's reports carry nothing this client acts on.
            self.stats.rtcp += 1
            return
        # Media comes only from the endpoint's two streams. That also drops our
        # own packets reflected back, which share the call's one SRTP key.
        if struct.unpack_from("!I", data, 8)[0] not in (SERVER_AUDIO_SSRC, SERVER_VIDEO_SSRC):
            return
        try:
            packet = parse_rtp(self._srtp.unprotect_rtp(data))
        except (SrtpError, ValueError):
            self.stats.srtp_errors += 1
            return
        self.last_media = time.monotonic()
        if packet.payload_type == OPUS_PT:
            self.stats.rtp_audio += 1
            # Over UDP a packet can arrive after a later one; play in order by dropping it.
            if self._audio_seq is not None and (packet.seq - self._audio_seq) & 0xFFFF >= 0x8000:
                self.stats.late_audio += 1
                return
            self._audio_seq = packet.seq
            if self._decoder and self.on_audio:
                try:
                    pcm = self._decoder.decode(packet.payload)
                except RuntimeError:
                    self.stats.decode_errors += 1
                    return
                self.on_audio(pcm)
        elif packet.payload_type == H264_PT:
            self.stats.rtp_video += 1
            unit = self._h264.push(packet)
            if self._h264.needs_keyframe:
                self.request_keyframe()
            if unit:
                self.stats.video_frames += 1
                self._dims = sps_dimensions(unit) or self._dims
                if self.on_video:
                    self.on_video(unit, self._dims)

    def _send(self, data: bytes) -> None:
        if self._selected:
            self._selected.send(data)

    def request_keyframe(self) -> None:
        if self._closed:
            return
        now = time.monotonic()
        if now - self._last_pli < 1.0:
            return
        self._last_pli = now
        self.stats.plis_sent += 1
        self._send(self._srtp.protect_rtcp(build_pli(CLIENT_VIDEO_SSRC, SERVER_VIDEO_SSRC)))

    def send_audio(self, pcm48_20ms: bytes) -> None:
        """Send one 20 ms frame of mono 48 kHz PCM16."""
        if self._encoder is None:
            self._encoder = OpusEncoder()
        packet = build_rtp(OPUS_PT, self._seq, self._ts, CLIENT_AUDIO_SSRC, self._encoder.encode(pcm48_20ms))
        self._seq += 1
        self._ts += OpusEncoder.FRAME_SAMPLES
        self._send(self._srtp.protect_rtp(packet))

    async def close(self) -> None:
        self._closed = True
        for task in self._tasks:
            task.cancel()
        for path in self._paths:
            path.close()
        self._paths = []
        for codec in (self._encoder, self._decoder):
            if codec:
                codec.close()
