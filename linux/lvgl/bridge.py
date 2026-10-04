#!/usr/bin/env python3
"""Authenticated bridge from the native LVGL app to the enrolled Linux SDK."""
import argparse
import asyncio
import hmac
import json
from pathlib import Path
import re
import struct
import uuid
import base64

def sdk_action(name):
    """Run a bounded showcase through the SDK's existing command executor."""
    from musegadget.executor import Account, Executor
    executor = Executor(Account.lookup("muse"))
    if name == "health":
        result = executor.run("device.health", {})
    elif name == "system":
        result = executor.run("system.run", {"command": "uname -m && id -un && uptime"})
    elif name == "files":
        text = "Hello from the native Linux LVGL app. This file was written through the Muse SDK.\n"
        path = "/home/muse/lvgl-feature-demo.txt"
        written = executor.run("file.write", dict(path=path, data_b64=base64.b64encode(text.encode()).decode(),
                                                 final=True, overwrite=True))
        if not written.get("ok"):
            raise ValueError(str(written.get("error", "SDK file write failed")))
        result = executor.run("file.read", dict(path=path))
        if result.get("ok"):
            payload = result["payload"]
            return "SDK file.write + file.read\n\n" + path + "\n\n" + base64.b64decode(payload["data_b64"]).decode()
    else:
        raise ValueError("Unknown SDK showcase action")
    if not result.get("ok"):
        raise ValueError(str(result.get("error", "SDK action failed")))
    payload = result["payload"]
    if name == "system":
        return "SDK system.run\n\n" + payload["stdout"] + "\nExit code: " + str(payload["exit_code"])
    return "SDK device.health\n\n" + json.dumps(payload, indent=2)

LIMIT = 1024 * 1024

async def frame(writer, kind, text=""):
    raw = text.encode("utf-8")
    if len(raw) > LIMIT:
        raise ValueError("Muse reply exceeds the display transport limit")
    writer.write(kind.encode("ascii") + struct.pack("<I", len(raw)) + raw)
    await writer.drain()

async def handle(reader, writer, *, token, socket_path):
    local = None
    request_ready = asyncio.Event()
    async def forward():
        nonlocal local
        supplied = (await reader.readline()).decode("ascii").strip()
        session = (await reader.readline()).decode("ascii").strip()
        if not hmac.compare_digest(supplied, token):
            raise ValueError("Connection authorization failed")
        if not re.fullmatch(r"[a-zA-Z0-9-]{1,64}", session):
            raise ValueError("Invalid conversation identifier")
        if str(uuid.UUID(session)) != session.lower():
            raise ValueError("Conversation identifier must be a UUID")
        size = struct.unpack("<I", await reader.readexactly(4))[0]
        if not 0 < size <= 256 * 1024:
            raise ValueError("Muse request exceeds 256 KB")
        first = await reader.readexactly(1)
        voice = first == b"\x1f"
        photo = first == b"\x1c"
        if size > 32000 and not (voice or photo):
            raise ValueError("Please keep your message under 32 KB")
        prompt = (first + await reader.readexactly(size - 1)).decode("utf-8")
        if not prompt.strip():
            raise ValueError("Enter a message first")
        request_ready.set()
        if prompt.startswith("\x1e"):
            result = await asyncio.to_thread(sdk_action, prompt[1:])
            await frame(writer, "R", result)
            await frame(writer, "D")
            return
        request = dict(stream=True, session_id=session, message=prompt)
        if voice:
            audio = base64.b64decode(prompt[1:], validate=True)
            if not (44 <= len(audio) <= 180000 and audio[:4] == b"RIFF" and audio[8:12] == b"WAVE"):
                raise ValueError("Send a WAV voice note of up to five seconds")
            request.update(message="", items=[dict(type="file", mime_type="audio/wav",
                filename="voice_note.wav", data_base64=prompt[1:])])
        elif photo:
            picture = base64.b64decode(prompt[1:], validate=True)
            if not (4 <= len(picture) <= 180000 and picture[:2] == b"\xff\xd8" and picture[-2:] == b"\xff\xd9"):
                raise ValueError("Send a camera JPEG smaller than 180 KB")
            request.update(message="Analyze this camera photo. Describe what you can see, including notable objects and details. Be clear about anything uncertain.",
                items=[dict(type="file", mime_type="image/jpeg", filename="camera.jpg", data_base64=prompt[1:])])
        sdk_reader, local = await asyncio.open_unix_connection(socket_path, limit=LIMIT + 1)
        local.write(json.dumps(request).encode() + b"\n")
        await local.drain()
        replies = {}
        while raw := await sdk_reader.readline():
            event = json.loads(raw)
            kind = event.get("type")
            if kind == "reply":
                mid, text = event.get("message_id"), event.get("text")
                if not isinstance(mid, str) or not isinstance(text, str):
                    raise ValueError("Muse returned an invalid reply")
                replies[mid] = text
                await frame(writer, "R", "\n\n".join(replies.values()))
            elif kind == "ack":
                await frame(writer, "S", "Muse is thinking...")
            elif kind == "done":
                if not any(replies.values()):
                    raise ValueError("Muse ended this turn without a reply")
                await frame(writer, "D")
                return
            elif kind == "error" or event.get("ok") is False:
                await frame(writer, "E", str(event.get("error", "Muse could not answer")))
                return
        raise ConnectionError("Muse disconnected before finishing the reply")

    task = asyncio.create_task(forward())
    # Leave the write side open: EOF here cancels the SDK subscription.
    async def disconnected():
        await request_ready.wait()
        while await reader.read(1):
            pass
    watch = asyncio.create_task(disconnected())
    try:
        done, _ = await asyncio.wait((task, watch), timeout=210, return_when=asyncio.FIRST_COMPLETED)
        if task in done:
            await task
        elif not done:
            raise TimeoutError("Muse reply timed out")
    except (OSError, ValueError, UnicodeError, TimeoutError, asyncio.IncompleteReadError) as exc:
        try:
            await frame(writer, "E", str(exc))
        except (ConnectionError, OSError):
            pass
    finally:
        task.cancel(); watch.cancel()
        await asyncio.gather(task, watch, return_exceptions=True)
        if local:
            local.close()
            await local.wait_closed()
        writer.close()
        try:
            await writer.wait_closed()
        except (ConnectionError, OSError):
            pass

async def main(args):
    token = Path(args.token_file).read_text().strip()
    if not re.fullmatch(r"[0-9a-f]{64}", token):
        raise ValueError("Invalid bridge authorization file")
    server = await asyncio.start_server(
        lambda r, w: handle(r, w, token=token, socket_path=args.socket), args.host, args.port, limit=256)
    print("Muse LVGL bridge ready", flush=True)
    async with server:
        await server.serve_forever()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--socket", default="/run/musegadget/musegadget.sock")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    asyncio.run(main(parser.parse_args()))
