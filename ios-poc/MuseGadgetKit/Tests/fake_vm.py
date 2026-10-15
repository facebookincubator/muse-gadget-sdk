"""A fake Muse VM over a real WebSocket, built from the Linux SDK's own Noise code.

Run from linux/:  uv run --with . python ../ios-poc/MuseGadgetKit/Tests/fake_vm.py PORT
Serves exactly two connections, then exits 0 if the Swift client behaved:
  1. a wrong bearer, which must be refused with HTTP 401;
  2. the right bearer: Noise XX, /link-control, link.register, link.heartbeat,
     a link.invoke answered by link.result, a /chat/stream request, then
     link.unpaired.
"""

import asyncio
import json
import sys

from websockets.asyncio.server import serve

from musegadget.link_client import MessageDecoder, encode_message
from musegadget.noise import (
    ApplicationResponse, BodyChunk, NoiseFrameDecoder, NoiseXXResponder, ServiceFrame,
    encode_noise_frames,
)
from musegadget.noise.transport import decode_request_envelope, encode_response_envelope

BEARER = "Bearer vm-token"
done = asyncio.Event()
results = {"refused_401": False, "checks": []}


def check(name, ok):
    results["checks"].append([name, bool(ok)])
    print(("PASS " if ok else "FAIL ") + name, flush=True)


async def process_request(connection, request):
    if request.headers.get("Authorization") != BEARER:
        results["refused_401"] = True
        return connection.respond(401, "bad bearer\n")
    return None


async def handler(ws):
    check("vm_id query", ws.request.path == "/v1/noise?vm_id=vm%201%26x")
    responder = NoiseXXResponder(payload=b"")
    responder.initialize()
    await ws.send(responder.read_message1_and_write_message2(await ws.recv()))
    responder.read_message3(await ws.recv())
    send_cipher, recv_cipher = responder.split()
    frames, messages = NoiseFrameDecoder(), MessageDecoder()

    async def next_frame():
        while True:
            assembled = frames.decode(recv_cipher.decrypt_with_ad(b"", await ws.recv()))
            if assembled is not None:
                return decode_request_envelope(assembled)

    async def send_frame(frame):
        for chunk in encode_noise_frames(encode_response_envelope(frame)):
            await ws.send(send_cipher.encrypt_with_ad(b"", chunk))

    async def next_message():
        while True:
            frame = await next_frame()
            if frame.kind == "request" and frame.value.path == "/chat/stream":
                return {"_chat": frame}
            got = messages.feed(frame.value.data)
            if got:
                return got[0]

    control = await next_frame()
    check("control stream request", (control.kind, control.value.verb, control.value.path,
                                     control.value.end_body) == ("request", "POST", "/link-control", False))
    sid = control.stream_id
    await send_frame(ServiceFrame.response(sid, ApplicationResponse(status=200)))

    async def send_message(msg):
        await send_frame(ServiceFrame.body_chunk(sid, BodyChunk(data=encode_message(msg))))

    register = await next_message()
    params = register.get("params", {})
    check("link.register", register.get("method") == "link.register" and register.get("type") == "req")
    check("register params", params.get("node_id") == "homelink-abcdef"
          and params.get("platform") == "linux" and params.get("device_family") == "homehub"
          and "device.health" in params.get("commands_v2", {}))
    await send_message({"type": "res", "id": register["id"], "result": {"status": "registered"}})

    heartbeat = await next_message()
    check("link.heartbeat", heartbeat.get("method") == "link.heartbeat")

    await send_message({"type": "req", "id": "inv-1", "method": "link.invoke",
                        "command": "device.health", "params": {"verbose": True}})
    result = await next_message()
    check("link.result", result.get("method") == "link.result" and result.get("id") == "inv-1"
          and result.get("ok") is True and result.get("payload", {}).get("echo") == {"verbose": True})

    chat = (await next_message())["_chat"]
    body = json.loads(chat.value.body)
    headers = {h.key: h.value for h in chat.value.headers}
    check("chat request", chat.value.verb == "POST" and chat.value.end_body
          and body == {"message": "hello from iPhone", "output_modality": "text",
                       "device_id": "homelink-abcdef"}
          and headers.get("x-app-id") == "musegadget")
    await send_frame(ServiceFrame.response(chat.stream_id, ApplicationResponse(
        status=200, body=b'{"message_id":"m-1"}', end_body=True)))

    await send_message({"type": "event", "event": "link.unpaired"})
    try:
        await asyncio.wait_for(ws.recv(), 5)
    except Exception:
        pass
    done.set()


async def main(port):
    async with serve(handler, "127.0.0.1", port, process_request=process_request):
        print("READY", flush=True)
        await asyncio.wait_for(done.wait(), 60)
    check("wrong bearer refused with 401", results["refused_401"])
    ok = all(passed for _, passed in results["checks"])
    print("ALL PASS" if ok else "SOME FAILED", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    asyncio.run(main(int(sys.argv[1])))
