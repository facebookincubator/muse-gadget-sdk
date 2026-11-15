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

import os
import struct

import pytest

from musegadget.rtc.srtp import SrtpError, SrtpSession

def rtp(seq, ssrc=201, payload=b"x" * 100, ext=False):
    first = 0x90 if ext else 0x80
    header = struct.pack("!BBHII", first, 127, seq, seq * 3000, ssrc)
    if ext:
        header += struct.pack("!HH", 0xBEDE, 1) + b"\x31\x00\x01\x00"
    return header + payload


def lib(key, direction):
    pylibsrtp = pytest.importorskip("pylibsrtp")
    policy = pylibsrtp.Policy(key=key, ssrc_type=direction)
    return pylibsrtp.Session(policy=policy)


def test_rfc3711_key_derivation_vector():
    # RFC 3711 B.3
    from musegadget.rtc import srtp

    mk = bytes.fromhex("E1F97A0D3E018BE0D64FA32C06DE4139")
    ms = bytes.fromhex("0EC675AD498AFEEBB6960B3AABE6")
    assert srtp._derive(mk, ms, 0, 16).hex().upper() == "C61E7A93744F39EE10734AFE3FF7A087"
    assert srtp._derive(mk, ms, 2, 14).hex().upper() == "30CBBC08863D8C85D49DB34A9AE1"


@pytest.mark.parametrize("ext", [False, True])
def test_rtp_interop_with_libsrtp(ext):
    pylibsrtp = pytest.importorskip("pylibsrtp")
    key = os.urandom(30)
    ours = SrtpSession(key)
    tx = lib(key, pylibsrtp.Policy.SSRC_ANY_OUTBOUND)
    rx = lib(key, pylibsrtp.Policy.SSRC_ANY_INBOUND)
    for seq in (65530, 65531, 65535, 0, 1, 2):
        packet = rtp(seq, ext=ext)
        assert rx.unprotect(ours.protect_rtp(packet)) == packet
        assert ours.unprotect_rtp(tx.protect(packet)) == packet


def test_rtcp_interop_with_libsrtp():
    pylibsrtp = pytest.importorskip("pylibsrtp")
    key = os.urandom(30)
    ours = SrtpSession(key)
    tx = lib(key, pylibsrtp.Policy.SSRC_ANY_OUTBOUND)
    rx = lib(key, pylibsrtp.Policy.SSRC_ANY_INBOUND)
    pli = struct.pack("!BBHII", 0x81, 206, 2, 101, 201)
    assert rx.unprotect_rtcp(ours.protect_rtcp(pli)) == pli
    sr = struct.pack("!BBHI", 0x80, 200, 6, 201) + os.urandom(20)
    assert ours.unprotect_rtcp(tx.protect_rtcp(sr)) == sr


def test_known_answers_from_libsrtp():
    ours = SrtpSession(bytes(range(30)))
    packet = struct.pack("!BBHII", 0x80, 111, 4660, 960 * 7, 100) + b"opus-payload-0123456789"
    protected = bytes.fromhex("806f123400001a40000000649c5fa8537a5563ed445b2da0c83e0b8d1fa94d9f7f3e264882c67696106f276ab8")
    assert ours.protect_rtp(packet) == protected
    assert SrtpSession(bytes(range(30))).unprotect_rtp(protected) == packet
    pli = struct.pack("!BBHII", 0x81, 206, 2, 101, 201)
    assert ours.protect_rtcp(pli) == bytes.fromhex("81ce000200000065270b4d4180000001179c9c93e63b478aa918")


def test_tampering_is_rejected():
    ours = SrtpSession(os.urandom(30))
    protected = bytearray(ours.protect_rtp(rtp(7)))
    protected[20] ^= 1
    with pytest.raises(SrtpError):
        ours.unprotect_rtp(bytes(protected))


def test_replayed_and_stale_packets_are_rejected():
    tx, rx = SrtpSession(bytes(range(30))), SrtpSession(bytes(range(30)))
    first = tx.protect_rtp(rtp(10))
    rx.unprotect_rtp(first)
    with pytest.raises(SrtpError):
        rx.unprotect_rtp(first)
    late = [tx.protect_rtp(rtp(seq)) for seq in range(11, 100)]
    rx.unprotect_rtp(late[-1])
    rx.unprotect_rtp(late[-2])  # reordered but inside the window
    with pytest.raises(SrtpError):
        rx.unprotect_rtp(late[0])  # 88 behind the newest
    pli = struct.pack("!BBHII", 0x81, 206, 2, 101, 201)
    sent = tx.protect_rtcp(pli)
    rx.unprotect_rtcp(sent)
    with pytest.raises(SrtpError):
        rx.unprotect_rtcp(sent)


def test_stream_count_is_bounded():
    from musegadget.rtc.srtp import MAX_STREAMS

    tx, rx = SrtpSession(bytes(range(30))), SrtpSession(bytes(range(30)))
    for ssrc in range(MAX_STREAMS):
        rx.unprotect_rtp(tx.protect_rtp(rtp(1, ssrc=ssrc)))
    with pytest.raises(SrtpError):
        rx.unprotect_rtp(tx.protect_rtp(rtp(1, ssrc=MAX_STREAMS)))
