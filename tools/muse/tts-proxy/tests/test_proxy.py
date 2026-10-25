"""Run: python3 -m unittest discover -s tools/muse/tts-proxy/tests -v"""
import http.client
import json
import threading
import unittest
from io import BytesIO
from pathlib import Path
from unittest.mock import patch
import importlib.util

source = Path(__file__).resolve().parents[1] / "server.py"
spec = importlib.util.spec_from_file_location("muse_tts_server", source)
mod = importlib.util.module_from_spec(spec)
import sys
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)


class FakeAudio:
    headers = {"Content-Type": "audio/mpeg"}
    def __init__(self, audio: bytes):
        self.io = BytesIO(audio)

    def read(self, n: int) -> bytes:
        return self.io.read(n)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


class ProxyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        mod.Handler.settings = mod.Settings("test-eleven-key", "testVoiceId", "x" * 32)
        cls.server = mod.ThreadingHTTPServer(("127.0.0.1", 0), mod.Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=3)

    def request(self, text="Hola, Muse", token="x" * 32):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        body = json.dumps({"text": text}).encode("utf-8")
        conn.request("POST", "/v1/tts", body=body, headers={
            "Authorization": "Bearer " + token,
            "Content-Type": "application/json",
        })
        response = conn.getresponse()
        output = response.read()
        status = response.status
        conn.close()
        return status, output

    def test_unauthorized_never_calls_provider(self):
        with patch.object(mod, "urlopen") as provider:
            status, _ = self.request(token="bad")
        self.assertEqual(status, 401)
        provider.assert_not_called()

    def test_streams_raw_mp3_bytes(self):
        payload = b"\xff\xfb" + b"x" * 5000
        with patch.object(mod, "urlopen", return_value=FakeAudio(payload)) as provider:
            status, received = self.request("Hola desde Muse")
        self.assertEqual(status, 200)
        self.assertEqual(received, payload)
        upstream = provider.call_args.args[0]
        self.assertIn("/stream?output_format=mp3_22050_32", upstream.full_url)
        self.assertIn(b"Hola desde Muse", upstream.data)
        self.assertEqual(upstream.get_header("Xi-api-key"), "test-eleven-key")

    def test_empty_text_rejected(self):
        with patch.object(mod, "urlopen") as provider:
            status, _ = self.request("")
        self.assertEqual(status, 422)
        provider.assert_not_called()

    def test_provider_error_is_masked(self):
        with patch.object(mod, "urlopen", side_effect=mod.URLError("unavailable")):
            status, output = self.request()
        self.assertEqual(status, 502)
        self.assertNotIn(b"test-eleven-key", output)


if __name__ == "__main__":
    unittest.main()
