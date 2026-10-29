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

"""Realtime streams coexist with chat, TTS and control on the shared transport."""

import asyncio

import pytest

from musegadget.link_client import LinkSession
from test_streaming import connected, response


def test_all_request_types_wait_for_lock_before_advancing_noise(monkeypatch):
    async def scenario():
        async with connected() as (session, vm, unused):
            encrypted = []
            for name in ("encrypt_http_request", "start_stream_request", "encrypt_body_chunk"):
                original = getattr(session._transport, name)

                def record(*args, original=original, name=name, **kwargs):
                    encrypted.append(name)
                    return original(*args, **kwargs)

                monkeypatch.setattr(session._transport, name, record)

            async def download():
                async with session.stream_http("GET", "/synthetic-tts") as stream:
                    return b"".join([part async for part in stream])

            await session._send_lock.acquire()
            chat = asyncio.create_task(session.send_chat("synthetic"))
            tts = asyncio.create_task(download())
            duplex = asyncio.create_task(session.stream_request("POST", "/synthetic-voice"))
            control = asyncio.create_task(session.send({"method": "synthetic"}))
            try:
                for _ in range(4):
                    await asyncio.sleep(0)
                assert encrypted == []
            finally:
                session._send_lock.release()

            # Every frame decrypts, regardless of which request acquired the lock first.
            paths = set()
            for _ in range(4):
                frame = await vm.next_frame()
                if frame.kind == "request":
                    paths.add(frame.value.path)
                    await response(vm, frame.stream_id,
                                   b"speech" if frame.value.path == "/synthetic-tts" else b"{}",
                                   end=True)
                else:
                    assert frame.stream_id == session._stream_id
            assert paths == {"/chat/stream", "/synthetic-tts", "/synthetic-voice"}
            assert (await chat)["ok"]
            assert await tts == b"speech"
            await control
            stream = await duplex
            assert await stream.wait_response() == 200
            assert await stream.read() == b"{}"
            await stream.half_close()
            assert (await vm.next_frame()).value.end_body
            await stream.aclose()
            assert not session._requests and not session._streams

    asyncio.run(asyncio.wait_for(scenario(), 5))


def test_cancelled_duplex_send_closes_cipher_and_fails_chat_consumer():
    async def scenario():
        async with connected() as (session, vm, unused):
            async def download():
                async with session.stream_http("GET", "/synthetic-tts") as stream:
                    return await stream.__anext__()

            waiting = asyncio.create_task(download())
            request = await vm.next_frame()
            await response(vm, request.stream_id)
            stream = await session.stream_request("POST", "/synthetic-voice")
            await vm.next_frame()
            blocked = asyncio.Event()
            original = session._ws.send

            async def interrupted(data):
                await original(data)
                blocked.set()
                await asyncio.Future()

            session._ws.send = interrupted
            upload = asyncio.create_task(stream.send_body(b"synthetic\n"))
            await blocked.wait()
            upload.cancel()
            with pytest.raises(asyncio.CancelledError):
                await upload
            assert session._closed
            with pytest.raises(ConnectionError):
                await waiting
            with pytest.raises(ConnectionError):
                await session.request("POST", "/voice/end-call", b"{}")
            with pytest.raises(ConnectionError):
                await stream.send_body(b"stale")

    asyncio.run(asyncio.wait_for(scenario(), 5))


def test_voice_transport_is_one_shot_even_after_handshake_failure():
    async def scenario():
        opens = []

        async def fail_open(*unused):
            opens.append(True)
            raise ConnectionError("synthetic failure")

        session = LinkSession(noise_host="synthetic.example", vm_id="synthetic", vm_auth_token="fake",
                              device=None, run_command=None, register_device=False, connect=fail_open)
        with pytest.raises(ConnectionError):
            await session.run(asyncio.Event())
        with pytest.raises(ConnectionError):
            await session.wait_transport_ready()
        with pytest.raises(ValueError, match="only run once"):
            await session.run(asyncio.Event())
        assert opens == [True]

    asyncio.run(scenario())
