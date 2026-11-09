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

import pytest

from musegadget.noise import ApplicationResponse, BodyChunk, ServiceFrame
from test_streaming import connected


@pytest.mark.parametrize("kind, chunk_bytes, count", [("request", 1, 300), ("http", 64000, 20)])
def test_ready_consumer_receives_a_burst_larger_than_its_buffer(kind, chunk_bytes, count):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            async def consume():
                if kind == "request":
                    async with await session.stream_request("POST", "/synthetic") as stream:
                        await stream.wait_response()
                        return b"".join([chunk async for chunk in stream])
                async with session.stream_http("GET", "/synthetic") as stream:
                    return b"".join([chunk async for chunk in stream])

            consumer = asyncio.create_task(consume())
            try:
                request = await vm.next_frame()
                await vm.send_frame(ServiceFrame.response(
                    request.stream_id, ApplicationResponse(status=200),
                ))
                for index in range(count):
                    await vm.send_frame(ServiceFrame.body_chunk(
                        request.stream_id,
                        BodyChunk(data=bytes([index % 256]) * chunk_bytes, end_body=index == count - 1),
                    ))
                assert await consumer == b"".join(
                    bytes([index % 256]) * chunk_bytes for index in range(count)
                )
            finally:
                consumer.cancel()
                await asyncio.gather(consumer, return_exceptions=True)

    asyncio.run(asyncio.wait_for(scenario(), 5))
