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

"""A loopback ICE-lite media endpoint for video call tests."""

from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import struct

from musegadget.rtc import stun
from musegadget.rtc.join import _Reader, _Writer
from musegadget.rtc.rtp import build_rtp, is_rtcp, parse_rtp
from musegadget.rtc.srtp import SrtpSession

# A real 448x768 Constrained Baseline SPS/PPS from a Muse call.
SPS = bytes.fromhex("6742c0298c648701848078442324")
PPS = bytes.fromhex("68ce3c80")
IDR = bytes([0x65]) + bytes(range(256)) * 8


def offer_fields(payload: bytes) -> dict:
    reader = _Reader(payload)
    header, body = reader.struct(), reader.struct()
    join = body[1]
    return {"session": header[1], "transaction": header[3], "client_ufrag": join[1].decode(),
            "srtp_key": base64.b64decode(join[3][1]), "server_pwd": join[3][3].decode(), "join": join}


def join_response(offer: dict, server_ufrag: str) -> bytes:
    w = _Writer()
    w.binary(1, offer["session"])
    w.i32(3, offer["transaction"])
    w.i32(4, 200)
    w.out.append(0)
    w._last = [0]
    w.begin(2)
    w.binary(1, server_ufrag)
    w.binary(2, b"")
    w.binary(3, b"")
    w.end()
    w.out.append(0)
    return bytes(w.out)


def _verified_username(data: bytes, password: str) -> str | None:
    length = struct.unpack_from("!H", data, 2)[0]
    offset, username = 20, None
    while offset + 4 <= 20 + length:
        kind, size = struct.unpack_from("!HH", data, offset)
        if kind == stun.USERNAME:
            username = data[offset + 4:offset + 4 + size].decode()
        if kind == stun.MESSAGE_INTEGRITY:
            prefix = bytearray(data[:offset])
            struct.pack_into("!H", prefix, 2, offset - 20 + 24)
            mac = hmac.new(password.encode(), bytes(prefix), hashlib.sha1).digest()
            return username if hmac.compare_digest(mac, data[offset + 4:offset + 24]) else None
        offset += 4 + size + (4 - size % 4) % 4
    return None


def _success(request: bytes, password: str) -> bytes:
    header = struct.pack("!HHI", stun.BINDING_SUCCESS, 24, stun.MAGIC) + request[8:20]
    mac = hmac.new(password.encode(), header, hashlib.sha1).digest()
    return header + struct.pack("!HH", stun.MESSAGE_INTEGRITY, 20) + mac


class FakeEdge:
    def __init__(self, *, udp: bool = True, tcp: bool = True, audio_frames=()) -> None:
        self.udp, self.tcp = udp, tcp
        self.audio_frames = list(audio_frames)  # Opus packets to send after connecting
        self.port = 0
        self.expected_username = None
        self.server_pwd = None
        self.srtp = None
        self.checks = []  # transport of each valid check
        self.received_audio = 0
        self.loud_frames_received = 0  # decoded microphone frames with speech-level RMS
        self.plis = 0
        self.media_sent = asyncio.Event()
        self._sent = False
        self._servers = []
        self._decoder = None

    def configure(self, offer: dict, server_ufrag: str) -> None:
        self.expected_username = f"{server_ufrag}:{offer['client_ufrag']}"
        self.server_pwd = offer["server_pwd"]
        self.srtp = SrtpSession(offer["srtp_key"])

    async def start(self) -> int:
        loop = asyncio.get_running_loop()
        sock = None
        if self.tcp:
            server = await asyncio.start_server(self._tcp, "127.0.0.1", 0)
            self._servers.append(server)
            self.port = server.sockets[0].getsockname()[1]
        if self.udp:
            edge = self

            class Udp(asyncio.DatagramProtocol):
                def connection_made(self, transport):
                    self.transport = transport

                def datagram_received(self, data, addr):
                    edge._handle(data, lambda out: self.transport.sendto(out, addr), "udp")

            transport, _ = await loop.create_datagram_endpoint(Udp, local_addr=("127.0.0.1", self.port))
            sock = transport
            self._servers.append(transport)
            self.port = transport.get_extra_info("sockname")[1]
        return self.port

    async def _tcp(self, reader, writer):
        def send(out):
            writer.write(struct.pack("!H", len(out)) + out)
        try:
            while True:
                size = struct.unpack("!H", await reader.readexactly(2))[0]
                self._handle(await reader.readexactly(size), send, "tcp")
        except (asyncio.IncompleteReadError, ConnectionError):
            pass

    def _handle(self, data: bytes, send, kind: str) -> None:
        if stun.is_stun(data):
            if self.server_pwd and _verified_username(data, self.server_pwd) == self.expected_username:
                self.checks.append(kind)
                send(_success(data, self.server_pwd))
                if not self._sent:
                    self._sent = True
                    for packet in self._media():
                        send(packet)
                    self.media_sent.set()
            return
        if is_rtcp(data):
            if self.srtp.unprotect_rtcp(data)[1] == 206:
                self.plis += 1
            return
        packet = parse_rtp(self.srtp.unprotect_rtp(data))
        if packet.payload_type == 111:
            self.received_audio += 1
            if self._decoder is None:
                from musegadget.rtc.opus import OpusDecoder
                self._decoder = OpusDecoder()
            from musegadget.voice_call import pcm_rms
            if pcm_rms(self._decoder.decode(packet.payload)) > 1000:
                self.loud_frames_received += 1

    def _media(self):
        stap = bytes([24]) + struct.pack("!H", len(SPS)) + SPS + struct.pack("!H", len(PPS)) + PPS
        body = IDR[1:]
        fus = [body[i:i + 900] for i in range(0, len(body), 900)]
        packets = [build_rtp(127, 1, 90000, 201, stap)]
        for i, chunk in enumerate(fus):
            fu_header = (0x80 if i == 0 else 0) | (0x40 if i == len(fus) - 1 else 0) | 5
            packets.append(build_rtp(127, 2 + i, 90000, 201, bytes([0x7C, fu_header]) + chunk,
                                     marker=i == len(fus) - 1))
        for i, opus in enumerate(self.audio_frames):
            packets.append(build_rtp(111, 100 + i, 960 * i, 200, opus))
        return [self.srtp.protect_rtp(packet) for packet in packets]

    def close(self) -> None:
        for server in self._servers:
            server.close()
