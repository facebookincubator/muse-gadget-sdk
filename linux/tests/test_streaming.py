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
import json
from contextlib import asynccontextmanager

import pytest

from musegadget.link_client import LinkSession, noise_url, validate_app_id
from musegadget.noise import ApplicationResponse, BodyChunk, Reset, ServiceFrame
from musegadget.streaming import HTTPStatusError, read_ndjson
from test_link_client import DEVICE, FakeVm, Pipe


@asynccontextmanager
async def connected(*, app_id="hatch-web", register_device=True):
    incoming, outgoing = asyncio.Queue(), asyncio.Queue()
    urls = []

    async def connect(url, headers):
        urls.append(url)
        return Pipe(incoming, outgoing)

    session = LinkSession(noise_host="vm.example", vm_id="test&vm", vm_auth_token="synthetic",
                          device=DEVICE, run_command=lambda *args: {"ok": False},
                          connect=connect, app_id=app_id, register_device=register_device)
    vm = FakeVm(Pipe(outgoing, incoming))
    stop = asyncio.Event()
    task = asyncio.create_task(session.run(stop))
    try:
        await vm.handshake()
        if register_device:
            await vm.accept_control_stream()
            message = await vm.next_message()
            await vm.send_message({"id": message["id"], "ok": True})
            await session.wait_registered()
        else:
            await session.wait_transport_ready()
        yield session, vm, urls
    finally:
        stop.set()
        await asyncio.wait_for(task, 2)


async def response(vm, stream_id, data=b"", *, status=200, end=False):
    await vm.send_frame(ServiceFrame.response(
        stream_id, ApplicationResponse(status=status, body=data, end_body=end),
    ))


async def chunk(vm, stream_id, data=b"", *, end=False):
    await vm.send_frame(ServiceFrame.body_chunk(stream_id, BodyChunk(data=data, end_body=end)))


def test_duplex_fragmented_ndjson_and_independent_upload_eof():
    async def scenario():
        async with connected() as (session, vm, urls):
            assert urls == ["wss://vm.example/v1/noise?vm_id=test%26vm&app_id=hatch-web"]
            async with await session.stream_request("POST", "/synthetic") as stream:
                request = await vm.next_frame()
                assert not request.value.end_body
                await stream.send_body(b"hello\n")
                assert (await vm.next_frame()).value.data == b"hello\n"
                raw = json.dumps({"text": "\u00e9"}, ensure_ascii=False).encode() + b"\r\n\n{\"n\":2}\n"
                await response(vm, stream.stream_id, raw[:10])
                assert await stream.wait_response() == 200
                records = read_ndjson(stream)
                first = asyncio.create_task(records.__anext__())
                await chunk(vm, stream.stream_id, raw[10:])
                assert await asyncio.wait_for(first, 1) == {"text": "\u00e9"}
                # Response available while the request upload is still open.
                await stream.send_body(b"more\n")
                assert (await vm.next_frame()).value.data == b"more\n"
                await stream.half_close()
                assert (await vm.next_frame()).value.end_body
                assert await records.__anext__() == {"n": 2}
                await chunk(vm, stream.stream_id, end=True)
                with pytest.raises(StopAsyncIteration):
                    await records.__anext__()
            assert not session._requests
    asyncio.run(scenario())


@pytest.mark.parametrize("kind", ["non2xx", "reset", "overflow", "body_first", "duplicate"])
def test_stream_failure_is_bounded_and_does_not_block_other_requests(kind):
    async def scenario():
        async with connected() as (session, vm, unused):
            stream = await session.stream_request("POST", "/test",
                                                  max_buffer_bytes=64 if kind == "non2xx" else 16)
            await vm.next_frame()
            if kind == "non2xx":
                await response(vm, stream.stream_id, b'{"error":', status=403)
                await chunk(vm, stream.stream_id, b'"denied"}', end=True)
            elif kind == "reset":
                await vm.send_frame(ServiceFrame.reset(stream.stream_id, Reset()))
            elif kind == "body_first":
                await chunk(vm, stream.stream_id, b"x")
            else:
                await response(vm, stream.stream_id)
                if kind == "duplicate":
                    await response(vm, stream.stream_id)
                else:
                    await chunk(vm, stream.stream_id, b"x" * 17)
            # A second stream is serviced even when the first consumer is slow.
            reply = asyncio.create_task(session.send_chat("synthetic"))
            request = await vm.next_frame()
            await response(vm, request.stream_id, b'{"accepted":true}', end=True)
            assert (await asyncio.wait_for(reply, 1))["ok"]
            with pytest.raises((ConnectionError, ValueError)) as error:
                await asyncio.wait_for(stream.wait_response(), 1)
            if kind == "non2xx":
                assert isinstance(error.value, HTTPStatusError)
                assert error.value.status == 403
                assert json.loads(error.value.body) == {"error": "denied"}
            await stream.aclose()
            assert (await vm.next_frame()).kind == "reset"
            assert not session._requests
    asyncio.run(scenario())


@pytest.mark.parametrize("raw,limit", [(b'{"x":1}', 64), (b'{"x":"\xff"}\n', 64),
                                      (b'[]\n', 64), (b'{bad}\n', 64), (b'xxx', 2)])
def test_ndjson_rejects_malformed_truncated_and_oversized_records(raw, limit):
    async def data():
        for byte in raw:
            yield bytes([byte])

    async def scenario():
        with pytest.raises(ValueError):
            async for _ in read_ndjson(data(), max_line_bytes=limit):
                pass
    asyncio.run(scenario())


@pytest.mark.parametrize("app", ["0", "-1", "+1", "1&other=2", "1\n", " 1", "1.0",
                                  "9223372036854775808", "\u0661", "musegadget"])
def test_invalid_app_id_cannot_inject_query(app):
    with pytest.raises(ValueError):
        noise_url("vm.example", "safe", app)


def test_app_id_int64_boundaries():
    assert validate_app_id("0001") == "1"
    assert validate_app_id("9223372036854775807") == "9223372036854775807"


def test_disconnect_fails_stream_reader_and_cleans_requests():
    async def scenario():
        async with connected() as (session, vm, unused):
            stream = await session.stream_request("POST", "/test")
            await vm.next_frame()
            reader = asyncio.create_task(stream.read())
            await vm.ws.close()
            with pytest.raises(ConnectionError):
                await asyncio.wait_for(reader, 1)
            assert not session._requests
    asyncio.run(scenario())
