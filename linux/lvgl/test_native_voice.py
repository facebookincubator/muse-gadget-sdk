"""Verify the native app uploads WAV bytes and queues automatic reply speech."""
import asyncio
import base64
import os
from pathlib import Path
import struct
import tempfile
import unittest
import wave

APP = Path(__file__).resolve().parents[2] / "esp32/simulator/build-spin/muse_linux_app"

@unittest.skipUnless(APP.exists(), "Build muse_linux_app first")
class NativeVoiceTests(unittest.IsolatedAsyncioTestCase):
    async def test_voice_bytes_and_automatic_reply_playback(self):
        await self.check_attachment(voice=True)

    async def test_camera_upload_shows_reply_without_automatic_speech(self):
        await self.check_attachment(voice=False)

    async def check_attachment(self, *, voice):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            directory = Path(temporary)
            audio_path = directory / "voice.wav"
            if voice:
                with wave.open(str(audio_path), "wb") as audio:
                    audio.setnchannels(1); audio.setsampwidth(2); audio.setframerate(16000)
                    audio.writeframes(b"\x01\x00" * 24000)
            else:
                audio_path = directory / "camera.jpg"
                audio_path.write_bytes(b"\xff\xd8" + b"x" * 50000 + b"\xff\xd9")
            token = "a" * 64
            (directory / "token").write_text(token)
            (directory / "host-ready.txt").write_text(str(os.getpid()))
            received = []
            async def peer(reader, writer):
                self.assertEqual((await reader.readline()).decode().strip(), token)
                await reader.readline()  # The app's fresh UUID.
                length = struct.unpack("<I", await reader.readexactly(4))[0]
                received.append(await reader.readexactly(length))
                reply = b"Hello from Muse."
                writer.write(b"R" + struct.pack("<I", len(reply)) + reply + b"D\x00\x00\x00\x00")
                await writer.drain(); writer.close(); await writer.wait_closed()
            server = await asyncio.start_server(peer, "127.0.0.1", 0)
            try:
                environment = dict(os.environ, MUSE_CHAT_HOST="127.0.0.1",
                    MUSE_CHAT_PORT=str(server.sockets[0].getsockname()[1]),
                    MUSE_CHAT_TOKEN_FILE=str(directory / "token"), MUSE_MEDIA_DIR=str(directory))
                process = await asyncio.create_subprocess_exec(str(APP), "--headless",
                    "--voice-file" if voice else "--camera-file", str(audio_path), "--run-ms", "1200", env=environment,
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
                output, error = await asyncio.wait_for(process.communicate(), 10)
                self.assertEqual(process.returncode, 0, error.decode())
                self.assertIn(b"CHAT_DONE", output)
                self.assertEqual(len(received), 1)
                self.assertEqual(received[0][:1], b"\x1f" if voice else b"\x1c")
                self.assertEqual(base64.b64decode(received[0][1:], validate=True), audio_path.read_bytes())
                if voice:
                    request_id, kind, text = (directory / "request.txt").read_text().split("\n", 2)
                    self.assertRegex(request_id, r"^[0-9a-f]{32}$")
                    self.assertEqual((kind, text), ("speak", "Hello from Muse."))
                else:
                    self.assertFalse((directory / "request.txt").exists())
            finally:
                server.close(); await server.wait_closed()

if __name__ == "__main__":
    unittest.main()
