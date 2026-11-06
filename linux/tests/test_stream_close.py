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

"""Close intent is not EOS/reset publication; use real Noise and a held send lock."""

import asyncio

import pytest

from test_streaming import connected, response


async def open_stream(session, vm, *, ended=True):
    stream = await session.stream_request("POST", "/synthetic")
    await vm.next_frame()
    await response(vm, stream.stream_id, end=ended)
    await stream.wait_response()
    return stream


async def cancelled(task):
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task


async def healthy_chat(session, vm):
    task = asyncio.create_task(session.send_chat("still connected"))
    frame = await vm.next_frame()
    assert frame.kind == "request" and frame.value.path == "/chat/stream"
    await response(vm, frame.stream_id, b"{}", end=True)
    assert (await task)["ok"]
    assert not session._closed


@pytest.mark.parametrize("end_with_data", [False, True])
def test_cancelled_eof_intent_still_requires_reset(end_with_data):
    async def scenario():
        async with connected() as (session, vm, unused):
            stream = await open_stream(session, vm)
            await session._send_lock.acquire()
            try:
                operation = stream.send_body(b"last", end_body=True) if end_with_data else stream.half_close()
                eof = asyncio.create_task(operation)
                await asyncio.sleep(0)
                assert stream._input_closed and not stream._input_eof_sent
                await cancelled(eof)
                with pytest.raises(ValueError, match="body already closed"):
                    await stream.send_body(b"too late")
            finally:
                session._send_lock.release()
            await stream.aclose()
            frame = await vm.next_frame()
            assert frame.kind == "reset" and frame.stream_id == stream.stream_id
            assert stream._close_complete and stream.stream_id not in session._requests
            await healthy_chat(session, vm)  # No hidden EOS or missing Noise nonce.

    asyncio.run(asyncio.wait_for(scenario(), 5))


def test_eof_retry_and_concurrent_half_close_publish_once():
    async def scenario():
        async with connected() as (session, vm, unused):
            stream = await open_stream(session, vm)
            await session._send_lock.acquire()
            first = asyncio.create_task(stream.half_close())
            second = asyncio.create_task(stream.half_close())
            try:
                await asyncio.sleep(0)
                with pytest.raises(ValueError):
                    await stream.send_body(b"after EOF intent")
                await cancelled(first)
                assert not stream._input_eof_sent
            finally:
                session._send_lock.release()
            await second
            assert stream._input_eof_sent
            frame = await vm.next_frame()
            assert frame.kind == "body_chunk" and frame.value.end_body and not frame.value.data
            await stream.half_close()
            await stream.aclose()
            await healthy_chat(session, vm)  # Neither duplicate EOF nor an unnecessary reset.

    asyncio.run(asyncio.wait_for(scenario(), 5))


@pytest.mark.parametrize("peer_ended", [False, True])
def test_cancelled_close_is_retryable_and_keeps_request_registered(peer_ended):
    async def scenario():
        async with connected() as (session, vm, unused):
            stream = await open_stream(session, vm, ended=peer_ended)
            await session._send_lock.acquire()
            try:
                close = asyncio.create_task(stream.aclose())
                await asyncio.sleep(0)
                await cancelled(close)
                assert stream._closed and not stream._close_complete
                assert session._requests[stream.stream_id] is stream
                with pytest.raises(ConnectionError):
                    await stream.send_body(b"after close intent")
            finally:
                session._send_lock.release()
            await asyncio.gather(stream.aclose(), stream.aclose())
            frame = await vm.next_frame()
            assert frame.kind == "reset" and frame.stream_id == stream.stream_id
            assert stream._close_complete and stream.stream_id not in session._requests
            await healthy_chat(session, vm)

    asyncio.run(asyncio.wait_for(scenario(), 5))


@pytest.mark.parametrize("end_body", [False, True])
def test_queued_body_cannot_publish_after_close_intent(end_body):
    async def scenario():
        async with connected() as (session, vm, unused):
            stream = await open_stream(session, vm)
            await session._send_lock.acquire()
            body = asyncio.create_task(stream.send_body(b"stale", end_body=end_body))
            await asyncio.sleep(0)
            close = asyncio.create_task(stream.aclose())
            await asyncio.sleep(0)
            assert stream._closed
            session._send_lock.release()
            with pytest.raises(ConnectionError, match="stream closed"):
                await body
            await close
            frame = await vm.next_frame()
            assert frame.kind == "reset" and frame.stream_id == stream.stream_id
            await healthy_chat(session, vm)  # Stream recheck didn't consume a nonce or kill chat.

    asyncio.run(asyncio.wait_for(scenario(), 5))


def test_body_queued_before_eof_finishes_but_new_producers_are_rejected():
    async def scenario():
        async with connected() as (session, vm, unused):
            stream = await open_stream(session, vm)
            await session._send_lock.acquire()
            body = asyncio.create_task(stream.send_body(b"last"))
            await asyncio.sleep(0)
            eof = asyncio.create_task(stream.half_close())
            await asyncio.sleep(0)
            with pytest.raises(ValueError):
                await stream.send_body(b"too late")
            session._send_lock.release()
            await asyncio.gather(body, eof)
            frame = await vm.next_frame()
            assert frame.value.data == b"last" and not frame.value.end_body
            assert (await vm.next_frame()).value.end_body
            await stream.aclose()
            await healthy_chat(session, vm)

    asyncio.run(asyncio.wait_for(scenario(), 5))


@pytest.mark.parametrize("operation", ["eof", "close"])
def test_cancellation_after_encryption_fails_session_closed(operation):
    async def scenario():
        async with connected() as (session, vm, unused):
            stream = await open_stream(session, vm)
            entered = asyncio.Event()
            original = session._ws.send

            async def blocked(data):
                await original(data)
                entered.set()
                await asyncio.Future()

            session._ws.send = blocked
            task = asyncio.create_task(stream.half_close() if operation == "eof" else stream.aclose())
            await entered.wait()
            await cancelled(task)
            assert session._closed
            assert not stream._input_eof_sent and not stream._close_complete
            with pytest.raises(ConnectionError):
                await stream.aclose()  # Ambiguous publication must not become silent success.

    asyncio.run(asyncio.wait_for(scenario(), 5))
