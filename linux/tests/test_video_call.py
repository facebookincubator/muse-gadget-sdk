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

import asyncio
import base64
import json
import math
import struct
import time
import wave

import pytest
from cryptography import x509
from cryptography.hazmat.primitives import serialization

from musegadget.rtc import stun
from musegadget.rtc.join import _Reader, build_join, parse_join_response
from musegadget.rtc.media import MediaSession
from musegadget.rtc.opus import OpusDecoder, OpusEncoder, OpusUnavailable
from musegadget.rtc.rtp import START_CODE, H264Depacketizer, RtpPacket, sps_dimensions
from musegadget.video_call import VideoCall
from musegadget.voice_audio import WavAudio
from musegadget.voice_video import VideoStream
from rtc_fake_edge import IDR, PPS, SPS, FakeEdge, join_response, offer_fields

THREAD = "f823e827-1022-40ea-b986-1209b491b9e8"


def opus_or_skip():
    try:
        OpusDecoder().close()
    except OpusUnavailable:
        pytest.skip("libopus is not installed")


def test_join_offer_has_the_fields_the_server_requires():
    offer = build_join(thread_id=THREAD, edge_host="media.example", edge_port=3478, vip_type=2, video=True)
    reader = _Reader(offer.payload)
    header, body = reader.struct(), reader.struct()
    assert reader.pos == len(offer.payload)
    assert header[1].decode() == offer.client_session_id and header[1].isdigit()
    join = body[1]
    assert join[1].decode() == offer.client_ufrag and join[2].decode() == offer.server_ufrag
    secrets = join[3]
    assert base64.b64decode(secrets[1]) == offer.srtp_key and len(offer.srtp_key) == 30
    assert secrets[2].decode() == offer.client_pwd and secrets[3].decode() == offer.server_pwd
    assert len(offer.client_pwd) >= 22 and len(offer.server_pwd) >= 22
    server_key = serialization.load_pem_private_key(secrets[4], None)
    server_cert = x509.load_pem_x509_certificate(join[5])
    assert server_cert.public_key().public_numbers() == server_key.public_key().public_numbers()
    assert join[4].startswith(b"sha-256 ") and len(join[4].split()[1].split(b":")) == 32
    assert join[6] == b"media.example:3478"
    assert 7 not in join  # session parameters ride only in field 16
    params = _Reader(join[16]).struct()
    assert params == {1: 0, 2: b"", 3: b"", 4: THREAD.encode()}
    assert (join[8], join[11], join[12], join[13], join[14]) == (1, 1, 2, 2, False)


def test_join_offer_without_thread_and_audio_only():
    offer = build_join(thread_id=None, edge_host="h", edge_port=1, vip_type=8, video=False)
    reader = _Reader(offer.payload)
    reader.struct()
    join = reader.struct()[1]
    assert 4 not in _Reader(join[16]).struct()
    assert (join[12], join[13]) == (8, 8)


def test_join_response_is_decoded():
    offer = offer_fields(build_join(thread_id=THREAD, edge_host="h", edge_port=1, vip_type=2, video=True).payload)
    answer = parse_join_response(join_response(offer, "updatedServerUfrag1"))
    assert (answer.server_ufrag, answer.status, answer.ipv4, answer.ipv6) == ("updatedServerUfrag1", 200, "", "")
    with pytest.raises(ValueError):
        parse_join_response(b"\x00\x00")


def test_stun_check_is_authenticated_and_fingerprinted():
    packet, txid = stun.binding_request("server:client", "p" * 24, 12345, 7)
    assert stun.is_stun(packet) and packet[8:20] == txid
    attrs = {}
    offset = 20
    while offset < len(packet):
        kind, size = struct.unpack_from("!HH", packet, offset)
        attrs[kind] = packet[offset + 4:offset + 4 + size]
        offset += 4 + size + (4 - size % 4) % 4
    assert attrs[stun.USERNAME] == b"server:client" and stun.USE_CANDIDATE in attrs
    assert struct.unpack("!I", attrs[stun.PRIORITY])[0] == 12345
    import binascii
    crc = (binascii.crc32(packet[:-8]) ^ 0x5354554E) & 0xFFFFFFFF
    assert struct.unpack("!I", attrs[stun.FINGERPRINT])[0] == crc
    assert not stun.parse_response(packet, txid, "p" * 24)  # a request is not a success


def rtp(seq, payload, marker=False):
    return RtpPacket(127, marker, seq, 0, 201, payload)


def test_h264_depacketizer_waits_for_a_keyframe_and_drops_broken_units():
    depacketizer = H264Depacketizer()
    assert depacketizer.push(rtp(1, bytes([0x41, 1, 2]), marker=True)) is None  # P before IDR
    stap = bytes([24]) + struct.pack("!H", len(SPS)) + SPS + struct.pack("!H", len(PPS)) + PPS
    assert depacketizer.push(rtp(2, stap)) is None
    assert depacketizer.push(rtp(3, bytes([0x7C, 0x85]) + IDR[1:100])) is None
    unit = depacketizer.push(rtp(4, bytes([0x7C, 0x45]) + IDR[100:], marker=True))
    assert unit == START_CODE + SPS + START_CODE + PPS + START_CODE + IDR
    assert sps_dimensions(unit) == (448, 768)
    assert depacketizer.push(rtp(5, bytes([0x41, 9]), marker=True)) == START_CODE + bytes([0x41, 9])
    assert depacketizer.push(rtp(7, bytes([0x41, 9]), marker=True)) is None  # seq 6 lost
    assert depacketizer.needs_keyframe


def test_video_stream_starts_at_a_keyframe_and_resyncs_after_overflow():
    import os
    import threading
    read_fd, write_fd = os.pipe()
    stream = VideoStream(write_fd, close_fd=True, max_pending=1)
    big_key = START_CODE + bytes([0x65]) + bytes(1 << 20)  # fills the pipe; the writer blocks
    delta = START_CODE + bytes([0x41, 2])
    stream.write_unit(delta)
    assert stream.dropped == 1 and stream.needs_keyframe  # waits for a keyframe
    stream.write_unit(big_key)
    for _ in range(200):  # until the writer is blocked inside big_key
        if stream._pending.empty():
            break
        threading.Event().wait(0.01)
    stream.write_unit(delta)  # queued behind the blocked write
    stream.write_unit(delta)  # overflow: dropped, and the stream waits for a keyframe
    assert stream.needs_keyframe and stream.dropped == 2
    stream.write_unit(delta)
    assert stream.dropped == 3
    received = bytearray()

    def drain():
        while chunk := os.read(read_fd, 65536):
            received.extend(chunk)

    reader = threading.Thread(target=drain)
    reader.start()
    assert stream.close(timeout=5)
    reader.join(5)
    os.close(read_fd)
    assert bytes(received).startswith(big_key + delta)


def test_h264_depacketizer_caps_unit_size():
    from musegadget.rtc.rtp import MAX_UNIT_BYTES

    depacketizer = H264Depacketizer()
    chunk = bytes([0x7C, 0x05]) + bytes(60000)
    seq = 0
    depacketizer.push(rtp(seq, bytes([0x7C, 0x85]) + bytes(60000)))
    while depacketizer._bytes and depacketizer._bytes <= MAX_UNIT_BYTES:
        seq += 1
        depacketizer.push(rtp(seq, chunk))
    assert depacketizer._bytes == 0 and depacketizer.needs_keyframe  # dropped, not buffered
    seq += 1
    assert depacketizer.push(rtp(seq, bytes([0x7C, 0x45]), marker=True)) is None


def test_join_response_parser_rejects_oversized_containers():
    # A 9-byte list header claiming 2**31 booleans, and a string longer than its message.
    for payload in (bytes.fromhex("0019f1ffffffff07") + b"\x00",
                    bytes([0x18, 0xFF, 0x7F]) + b"x" * 4 + b"\x00"):
        with pytest.raises(ValueError):
            parse_join_response(payload)
    with pytest.raises(ValueError):
        parse_join_response(b"\x00" * (17 * 1024))


@pytest.mark.parametrize("udp,tcp", [(True, True), (False, True), (True, False)])
def test_media_session_connects_and_receives_video(udp, tcp):
    async def scenario():
        offer = build_join(thread_id=THREAD, edge_host="127.0.0.1", edge_port=1, vip_type=2, video=True)
        edge = FakeEdge(udp=udp, tcp=tcp)
        port = await edge.start()
        edge.configure(offer_fields(offer.payload), "updatedServerUfrag1")
        answer = parse_join_response(join_response(offer_fields(offer.payload), "updatedServerUfrag1"))
        units = []
        session = MediaSession(offer, answer, "127.0.0.1", port,
                               on_video=lambda unit, size: units.append((unit, size)))
        try:
            kind = await session.connect(timeout=5)
            await asyncio.wait_for(edge.media_sent.wait(), 5)
            for _ in range(50):
                if units:
                    break
                await asyncio.sleep(0.02)
        finally:
            await session.close()
            edge.close()
        return kind, units, edge

    kind, units, edge = asyncio.run(scenario())
    assert kind.startswith("udp") if udp else kind == "tcp"
    assert units == [(START_CODE + SPS + START_CODE + PPS + START_CODE + IDR, (448, 768))]
    assert edge.plis >= 1  # asked for a keyframe on connect


def test_media_session_fails_cleanly_without_a_reachable_endpoint():
    async def scenario():
        offer = build_join(thread_id=THREAD, edge_host="127.0.0.1", edge_port=1, vip_type=2, video=True)
        edge = FakeEdge(udp=True, tcp=False)
        port = await edge.start()  # never configured: every check goes unanswered
        answer = parse_join_response(join_response(offer_fields(offer.payload), "x"))
        session = MediaSession(offer, answer, "127.0.0.1", port, tcp=False)
        try:
            with pytest.raises(ConnectionError):
                await session.connect(timeout=0.5)
        finally:
            edge.close()

    asyncio.run(scenario())


class FakeSession:
    def __init__(self, edge):
        self.edge = edge
        self.requests = []

    async def request(self, method, path, body=b"", *, headers=None, timeout=30):
        self.requests.append((method, path, json.loads(body) if body else None))
        if path == "/voice/calls/media-endpoint":
            result = {"route": "media", "host": "127.0.0.1", "port": self.edge.port,
                      "vip_type": "META_AI", "enable_media_to_cosmos": True}
        elif path == "/voice/calls/join":
            offer = offer_fields(base64.b64decode(json.loads(body)["payload_b64"]))
            self.edge.configure(offer, "updatedServerUfrag1")
            result = {"payload_b64": base64.b64encode(join_response(offer, "updatedServerUfrag1")).decode(),
                      "call": {"call_id": json.loads(body)["call_id"]}}
        elif path == "/voice/end-call":
            result = {"ok": True}
        else:
            return {"ok": False, "status": 404, "response": None}
        return {"ok": True, "status": 200, "response": {"ok": True, "result": result}}


class Units:
    def __init__(self):
        self.units = []

    def write_unit(self, unit):
        self.units.append(unit)


def test_video_call_signals_over_noise_and_streams_media(tmp_path):
    opus_or_skip()
    tone = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * n / 48000))) for n in range(960))
    encoder = OpusEncoder()
    reply = [encoder.encode(tone) for _ in range(5)]
    in_wav, out_wav = tmp_path / "in.wav", tmp_path / "out.wav"
    with wave.open(str(in_wav), "wb") as wav:
        wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
        wav.writeframes(tone[::2] * 10)

    async def scenario():
        edge = FakeEdge(audio_frames=reply)
        await edge.start()
        session = FakeSession(edge)
        audio, video = WavAudio(str(in_wav), str(out_wav)), Units()
        call = VideoCall(session, device_id="device-1", video_output=video, thread_id=THREAD,
                         idle_timeout=0.5, media_timeout=5)
        try:
            reason = await call.run(audio.frames(), audio)
        finally:
            audio.close()
            edge.close()
        return reason, call, session, edge, video

    reason, call, session, edge, video = asyncio.run(scenario())
    assert reason == "idle_timeout" and call.end_call_acknowledged
    paths = [(method, path) for method, path, _ in session.requests]
    assert paths == [("GET", "/voice/calls/media-endpoint"), ("POST", "/voice/calls/join"),
                     ("POST", "/voice/end-call")]
    join = session.requests[1][2]
    assert (join["call_media_type"], join["device"], join["device_id"], join["thread_id"]) == (
        "video", "mcu-wearable", "device-1", THREAD)
    assert session.requests[2][2]["call_id"] == join["call_id"]
    assert video.units == [START_CODE + SPS + START_CODE + PPS + START_CODE + IDR]
    assert call.summary()["size"] == "448x768"
    assert edge.received_audio >= 10  # the microphone reached the endpoint
    with wave.open(str(out_wav)) as wav:
        assert wav.getnframes() > 0  # Muse's reply was played


def test_quiet_frames_from_muse_do_not_mute_the_microphone(tmp_path):
    opus_or_skip()
    tone = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * n / 48000))) for n in range(960))
    encoder = OpusEncoder()
    quiet = [encoder.encode(bytes(1920)) for _ in range(100)]  # 2 s of near-silence
    in_wav, out_wav = tmp_path / "in.wav", tmp_path / "out.wav"
    with wave.open(str(in_wav), "wb") as wav:
        wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
        wav.writeframes(tone[::2] * 10)

    async def scenario():
        edge = FakeEdge(audio_frames=quiet)
        await edge.start()
        audio = WavAudio(str(in_wav), str(out_wav))
        call = VideoCall(FakeSession(edge), device_id="d", video_output=Units(), idle_timeout=0.5)
        try:
            await call.run(audio.frames(), audio)
        finally:
            audio.close()
            edge.close()
        return edge

    assert asyncio.run(scenario()).loud_frames_received >= 8  # of the 20 tone frames; Opus ramps up


def test_missing_libopus_admits_no_call(monkeypatch):
    from musegadget.rtc import opus

    def missing():
        raise OpusUnavailable("libopus not found")

    monkeypatch.setattr(opus, "_opus", missing)

    async def scenario():
        edge = FakeEdge()
        await edge.start()
        session = FakeSession(edge)
        call = VideoCall(session, device_id="d", video_output=Units())

        async def frames():
            yield bytes(1920)

        class Out:
            def write(self, pcm):
                pass

            def flush(self):
                pass

        try:
            with pytest.raises(OpusUnavailable):
                await call.run(frames(), Out())
        finally:
            edge.close()
        return session

    assert asyncio.run(scenario()).requests == []


def test_video_call_ends_the_call_when_media_never_connects():
    opus_or_skip()

    async def scenario():
        edge = FakeEdge(udp=True, tcp=False)
        await edge.start()
        session = FakeSession(edge)
        session.request_original = session.request

        async def request(method, path, body=b"", **kwargs):
            reply = await session.request_original(method, path, body, **kwargs)
            if path == "/voice/calls/join":
                edge.server_pwd = None  # the endpoint ignores every check
            return reply

        session.request = request
        call = VideoCall(session, device_id="d", video_output=Units(), ready_timeout=0.5)

        async def frames():
            yield bytes(1920)

        class Out:
            def write(self, pcm):
                pass

            def flush(self):
                pass

        try:
            with pytest.raises((ConnectionError, asyncio.TimeoutError)):
                await call.run(frames(), Out())
        finally:
            edge.close()
        return session, call

    session, call = asyncio.run(scenario())
    assert session.requests[-1][1] == "/voice/end-call" and call.end_call_acknowledged


def test_unacknowledged_end_call_is_an_error():
    async def scenario():
        edge = FakeEdge()
        await edge.start()
        session = FakeSession(edge)
        original = session.request

        async def request(method, path, body=b"", **kwargs):
            reply = await original(method, path, body, **kwargs)
            if path == "/voice/end-call":
                reply["response"]["result"] = {"ok": False}
            return reply

        session.request = request
        call = VideoCall(session, device_id="d", video_output=Units(), idle_timeout=0.3)

        async def frames():
            yield bytes(1920)

        class Out:
            def write(self, pcm):
                pass

            def flush(self):
                pass

        try:
            with pytest.raises(ConnectionError):
                await call.run(frames(), Out())
        finally:
            edge.close()
        return call

    opus_or_skip()
    assert not asyncio.run(scenario()).end_call_acknowledged


def test_quiet_live_microphone_lets_the_idle_timer_fire():
    opus_or_skip()
    hiss = struct.pack("<h", 120) * 960  # steady low noise, below speech level

    async def scenario():
        edge = FakeEdge()
        await edge.start()
        call = VideoCall(FakeSession(edge), device_id="d", video_output=Units(), idle_timeout=0.5)

        async def live():
            while True:
                await asyncio.sleep(0.04)
                yield hiss

        class Out:
            def write(self, pcm):
                pass

            def flush(self):
                pass

        try:
            return await asyncio.wait_for(call.run(live(), Out(), realtime_input=True), 10)
        finally:
            edge.close()

    assert asyncio.run(scenario()) == "idle_timeout"


def test_malformed_sps_has_no_size():
    long_zeros = bytes([0x67, 0x42, 0xC0, 0x29]) + bytes(64)  # an Exp-Golomb code with no end
    assert sps_dimensions(START_CODE + long_zeros) is None


def test_live_playback_is_paced_in_device_sized_frames():
    opus_or_skip()
    encoder = OpusEncoder()
    tone = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * n / 48000))) for n in range(960))
    burst = [encoder.encode(tone) for _ in range(50)]  # 1 s of reply sent at once

    class Device:
        """Like LiveAudio: at most 960 samples per write and 8 frames queued."""

        def __init__(self):
            self.frames = []
            self.started = None

        def write(self, pcm):
            assert 0 < len(pcm) <= 1920
            self.started = self.started or time.monotonic()
            self.frames.append((time.monotonic(), len(pcm)))

        def flush(self):
            pass

    async def scenario():
        edge = FakeEdge(audio_frames=burst)
        await edge.start()
        device = Device()
        call = VideoCall(FakeSession(edge), device_id="d", video_output=Units(), idle_timeout=0.5)

        async def frames():
            yield bytes(1920)

        try:
            await call.run(frames(), device)
        finally:
            edge.close()
        return device, call

    device, call = asyncio.run(scenario())
    played = sum(n for _, n in device.frames) / 48000
    assert played >= 0.9 and call.playback_dropped == 0
    assert device.frames[-1][0] - device.started >= 0.8  # paced, not written at once


def test_media_session_ignores_reports_reflections_and_late_audio():
    opus_or_skip()

    async def scenario():
        offer = build_join(thread_id=THREAD, edge_host="127.0.0.1", edge_port=1, vip_type=2, video=True)
        fields = offer_fields(offer.payload)
        edge = FakeEdge()
        port = await edge.start()
        edge.configure(fields, "u")
        answer = parse_join_response(join_response(fields, "u"))
        heard = []
        session = MediaSession(offer, answer, "127.0.0.1", port, on_audio=heard.append)
        try:
            await session.connect(timeout=5)
            from musegadget.rtc.rtp import build_rtp
            from musegadget.rtc.srtp import SrtpSession
            peer = SrtpSession(fields["srtp_key"])
            opus = OpusEncoder().encode(bytes(1920))
            path = session._selected
            for seq, ssrc in ((10, 200), (9, 200), (11, 100)):  # in order, late, reflected
                session._receive(path, peer.protect_rtp(build_rtp(111, seq, 960 * seq, ssrc, opus)))
            session._receive(path, peer.protect_rtcp(struct.pack("!BBHI", 0x80, 201, 1, 1)))
        finally:
            await session.close()
            edge.close()
        return heard, session.stats

    heard, stats = asyncio.run(scenario())
    assert len(heard) == 1 and stats.late_audio == 1 and stats.rtcp == 1


def test_refused_join_fails_fast():
    offer = offer_fields(build_join(thread_id=THREAD, edge_host="h", edge_port=1, vip_type=2, video=True).payload)
    accepted = join_response(offer, "u")
    status_200 = bytes([0x15, 0x90, 0x03])  # header field 4, zigzag varint of 200
    assert status_200 in accepted
    with pytest.raises(ValueError):
        parse_join_response(accepted.replace(status_200, bytes([0x15, 0xA0, 0x06])))  # 400


def test_media_is_lost_when_consent_checks_go_unanswered(monkeypatch):
    from musegadget.rtc import media

    monkeypatch.setattr(media, "CONSENT_INTERVAL", 0.05)
    monkeypatch.setattr(media, "CONSENT_TIMEOUT", 0.2)

    async def scenario():
        offer = build_join(thread_id=THREAD, edge_host="127.0.0.1", edge_port=1, vip_type=2, video=True)
        fields = offer_fields(offer.payload)
        edge = FakeEdge(udp=False, tcp=True)
        port = await edge.start()
        edge.configure(fields, "u")
        session = MediaSession(offer, parse_join_response(join_response(fields, "u")), "127.0.0.1", port)
        try:
            await session.connect(timeout=5)
            edge.server_pwd = None  # the endpoint stops answering
            await asyncio.wait_for(session.lost.wait(), 5)
        finally:
            await session.close()
            edge.close()

    asyncio.run(scenario())


def test_a_recording_that_cannot_be_written_ends_the_call():
    opus_or_skip()
    tone = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * n / 48000))) for n in range(960))
    reply = [OpusEncoder().encode(tone) for _ in range(5)]

    class FullDisk:
        def write(self, pcm):
            raise OSError("No space left on device")

        def flush(self):
            pass

    async def scenario():
        edge = FakeEdge(audio_frames=reply)
        await edge.start()
        session = FakeSession(edge)
        call = VideoCall(session, device_id="d", video_output=Units(), idle_timeout=5)

        async def frames():
            yield bytes(1920)

        try:
            with pytest.raises(OSError):
                await call.run(frames(), FullDisk())
        finally:
            edge.close()
        return session

    assert asyncio.run(scenario()).requests[-1][1] == "/voice/end-call"


def test_h264_fragment_edge_cases_request_a_keyframe():
    stap = bytes([24]) + struct.pack("!H", len(SPS)) + SPS + struct.pack("!H", len(PPS)) + PPS
    depacketizer = H264Depacketizer()
    depacketizer.push(rtp(1, stap))
    depacketizer.push(rtp(2, bytes([0x7C, 0x85]) + IDR[1:]))
    unit = depacketizer.push(rtp(3, bytes([0x7C, 0x45]), marker=True))  # empty end fragment
    assert unit == START_CODE + SPS + START_CODE + PPS + START_CODE + IDR and not depacketizer.needs_keyframe
    assert depacketizer.push(rtp(4, bytes([0x7C, 0x01]) + b"x", marker=True)) is None  # no start
    assert depacketizer.needs_keyframe
    depacketizer.push(rtp(5, stap))
    depacketizer.push(rtp(6, bytes([0x7C, 0x85]) + IDR[1:]))
    assert depacketizer.push(rtp(7, bytes([0x41, 9]), marker=True)) is None  # unfinished at the marker
    assert depacketizer.needs_keyframe


def test_stale_check_responses_are_not_consent(monkeypatch):
    from musegadget.rtc import media

    async def scenario():
        offer = build_join(thread_id=THREAD, edge_host="127.0.0.1", edge_port=1, vip_type=2, video=True)
        fields = offer_fields(offer.payload)
        edge = FakeEdge(udp=False, tcp=True)
        port = await edge.start()
        edge.configure(fields, "u")
        session = MediaSession(offer, parse_join_response(join_response(fields, "u")), "127.0.0.1", port)
        try:
            await session.connect(timeout=5)
            consent = session.last_consent
            monkeypatch.setattr(media, "CHECK_RESPONSE_WINDOW", 0.0)
            session._check(session._selected)
            await asyncio.sleep(0.2)
            return consent, session.last_consent
        finally:
            await session.close()
            edge.close()

    before, after = asyncio.run(scenario())
    assert after == before


def test_a_second_keyframe_is_requested_after_video_starts():
    opus_or_skip()

    async def scenario():
        edge = FakeEdge()
        await edge.start()
        call = VideoCall(FakeSession(edge), device_id="d", video_output=Units(), idle_timeout=1.6)

        async def frames():
            yield bytes(1920)

        class Out:
            def write(self, pcm):
                pass

            def flush(self):
                pass

        try:
            await call.run(frames(), Out())
        finally:
            edge.close()
        return edge

    assert asyncio.run(scenario()).plis >= 2  # one on connect, one after the first frame
