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

from websockets.asyncio.server import serve

from musegadget import tls
from musegadget.link_client import LinkSession, Outcome
from musegadget.noise import ApplicationResponse, ServiceFrame
from test_link_client import DEVICE, FakeVm


def test_noise_connection_omits_compression_and_carries_chat(monkeypatch):
    # The loopback peer is plain ws://, which takes no TLS context.
    monkeypatch.setattr(tls, "client_context", lambda: None)

    async def scenario():
        received = asyncio.get_running_loop().create_future()

        async def handler(ws):
            try:
                vm = FakeVm(ws)
                await vm.handshake()
                await vm.accept_control_stream()
                registration = await vm.next_message()
                await vm.send_message({"id": registration["id"], "ok": True})
                request = await vm.next_frame()
                await vm.send_frame(ServiceFrame.response(
                    request.stream_id,
                    ApplicationResponse(status=200, body=b'{"accepted":true}', end_body=True),
                ))
                received.set_result((ws.request.headers, ws.protocol.extensions, request))
                await ws.wait_closed()
            except Exception as exc:
                if not received.done():
                    received.set_exception(exc)
                raise

        async with serve(handler, "127.0.0.1", 0, compression="deflate") as server:
            port = server.sockets[0].getsockname()[1]
            session = LinkSession(
                noise_host="vm.example", vm_id="vm-1", vm_auth_token="synthetic",
                device=DEVICE, run_command=lambda *args: {"ok": True},
            )
            session._url = f"ws://127.0.0.1:{port}/v1/noise?vm_id=vm-1"
            stop = asyncio.Event()
            task = asyncio.create_task(session.run(stop))
            try:
                await session.wait_registered()
                assert await session.send_chat("porch light on", "side-1") == {
                    "ok": True, "status": 200, "response": {"accepted": True},
                }
                headers, extensions, request = await received
                assert "Sec-WebSocket-Extensions" not in headers
                assert extensions == []
                assert headers["Authorization"] == "Bearer synthetic"
                assert (request.value.verb, request.value.path) == ("POST", "/chat/stream")
                assert json.loads(request.value.body) == {
                    "message": "porch light on", "session_id": "side-1",
                    "output_modality": "text", "device_id": "homelink-abcdef",
                }
            finally:
                stop.set()
                assert await task is Outcome.STOPPED

    asyncio.run(asyncio.wait_for(scenario(), 5))
