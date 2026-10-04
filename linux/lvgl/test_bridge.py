"""Protocol checks use a local fake SDK; no Muse account or messages are used."""
import asyncio
import base64
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import wave
from unittest.mock import patch
from bridge import handle

TOKEN = "a" * 64
SESSION = "f048a1b5-5ebd-41aa-8b2d-55a42a771f46"

class BridgeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = str(Path(self.tmp.name) / "sdk.sock")
        self.requests = []
        self.sdk_closed = asyncio.Event()
        async def sdk(reader, writer):
            request = json.loads(await reader.readline())
            self.requests.append(request)
            if request["message"] == "wait":
                await reader.read()
                self.sdk_closed.set()
            else:
                events = [dict(type="ack"), dict(type="reply", message_id="one", text="Hello"),
                          dict(type="reply", message_id="one", text="Hello, Muse!"),
                          dict(type="reply", message_id="two", text="Second reply"), dict(type="done")]
                for event in events:
                    writer.write(json.dumps(event).encode() + b"\n")
                    await writer.drain()
            writer.close()
            await writer.wait_closed()
        self.sdk = await asyncio.start_unix_server(sdk, self.path, limit=1024 * 1024)
        self.server = await asyncio.start_server(
            lambda r, w: handle(r, w, token=TOKEN, socket_path=self.path), "127.0.0.1", 0, limit=256)
        self.port = self.server.sockets[0].getsockname()[1]

    async def asyncTearDown(self):
        self.server.close(); self.sdk.close()
        await self.server.wait_closed(); await self.sdk.wait_closed()
        self.tmp.cleanup()

    async def request(self, message=b"hello", *, token=TOKEN, size=None):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        writer.write(f"{token}\n{SESSION}\n".encode() + struct.pack("<I", len(message) if size is None else size) + message)
        await writer.drain()
        return reader, writer

    async def read_frame(self, reader):
        header = await asyncio.wait_for(reader.readexactly(5), 2)
        return chr(header[0]), (await reader.readexactly(struct.unpack("<I", header[1:])[0])).decode()

    async def test_stream_coalesces_replies_without_duplication(self):
        reader, writer = await self.request()
        frames = [await self.read_frame(reader) for _ in range(5)]
        self.assertEqual(frames[-2], ("R", "Hello, Muse!\n\nSecond reply"))
        self.assertEqual(frames[-1], ("D", ""))
        self.assertEqual(self.requests, [dict(stream=True, session_id=SESSION, message="hello")])
        writer.close(); await writer.wait_closed()

    async def test_invalid_authorization_never_reaches_sdk(self):
        reader, writer = await self.request(token="b" * 64)
        self.assertEqual((await self.read_frame(reader))[0], "E")
        self.assertFalse(self.requests)
        writer.close(); await writer.wait_closed()

    async def test_non_uuid_session_never_reaches_sdk(self):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        writer.write(f"{TOKEN}\nnot-a-uuid\n".encode() + struct.pack("<I", 5) + b"hello")
        await writer.drain()
        self.assertEqual((await self.read_frame(reader))[0], "E")
        self.assertFalse(self.requests)
        writer.close(); await writer.wait_closed()

    async def test_sdk_action_uses_executor_without_posting_chat(self):
        with patch("bridge.sdk_action", return_value="actual SDK result") as execute:
            reader, writer = await self.request(b"\x1ehealth")
            self.assertEqual(await self.read_frame(reader), ("R", "actual SDK result"))
            self.assertEqual(await self.read_frame(reader), ("D", ""))
            execute.assert_called_once_with("health")
            self.assertFalse(self.requests)
            writer.close(); await writer.wait_closed()

    async def test_voice_note_sends_original_wav_to_muse_without_transcription(self):
        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as audio:
            audio.setnchannels(1); audio.setsampwidth(2); audio.setframerate(16000)
            audio.writeframes(b"\x00\x00" * 32000)
        encoded = base64.b64encode(buffer.getvalue())
        reader, writer = await self.request(b"\x1f" + encoded)
        frames = [await self.read_frame(reader) for _ in range(5)]
        self.assertEqual(frames[-1], ("D", ""))
        request = self.requests[0]
        self.assertEqual(request["message"], "")
        self.assertEqual(request["items"], [dict(type="file", mime_type="audio/wav",
            filename="voice_note.wav", data_base64=encoded.decode())])
        writer.close(); await writer.wait_closed()

    async def test_invalid_voice_note_never_reaches_sdk(self):
        for prompt in (b"\x1f!!!!", b"\x1f" + base64.b64encode(b"not a WAV")):
            reader, writer = await self.request(prompt)
            self.assertEqual((await self.read_frame(reader))[0], "E")
            writer.close(); await writer.wait_closed()
        self.assertFalse(self.requests)

    async def test_camera_photo_is_uploaded_for_analysis(self):
        picture = b"\xff\xd8" + b"x" * 50000 + b"\xff\xd9"
        encoded = base64.b64encode(picture)
        reader, writer = await self.request(b"\x1c" + encoded)
        frames = [await self.read_frame(reader) for _ in range(5)]
        self.assertEqual(frames[-1], ("D", ""))
        request = self.requests[0]
        self.assertIn("Analyze this camera photo", request["message"])
        self.assertEqual(request["items"], [dict(type="file", mime_type="image/jpeg",
            filename="camera.jpg", data_base64=encoded.decode())])
        writer.close(); await writer.wait_closed()

    async def test_invalid_camera_photo_never_reaches_sdk(self):
        reader, writer = await self.request(b"\x1c" + base64.b64encode(b"not a JPEG"))
        self.assertEqual((await self.read_frame(reader))[0], "E")
        self.assertFalse(self.requests)
        writer.close(); await writer.wait_closed()

    async def test_oversized_message_never_reaches_sdk(self):
        reader, writer = await self.request(size=32001)
        self.assertEqual((await self.read_frame(reader))[0], "E")
        self.assertFalse(self.requests)
        writer.close(); await writer.wait_closed()

    async def test_disconnect_cancels_sdk_subscription(self):
        reader, writer = await self.request(b"wait")
        for _ in range(50):
            if self.requests: break
            await asyncio.sleep(0.01)
        self.assertTrue(self.requests)
        writer.close(); await writer.wait_closed()
        await asyncio.wait_for(self.sdk_closed.wait(), 2)

if __name__ == "__main__":
    unittest.main()
