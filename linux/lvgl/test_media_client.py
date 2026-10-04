"""Hardware requests recover after timeouts and ignore replies from earlier captures."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent

class MediaClientTests(unittest.TestCase):
    def test_repeated_capture_timeout_and_late_response(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            directory = Path(temporary)
            (directory / "lvgl.h").write_text("#include <stdint.h>\nuint32_t lv_tick_get(void);\n")
            (directory / "clock.c").write_text("#include <stdint.h>\nuint32_t now;\nuint32_t lv_tick_get(void) { return now; }\n")
            library = directory / "media.so"
            subprocess.run(["cc", "-shared", "-fPIC", "-I", str(directory), str(ROOT / "media_client.c"),
                            str(directory / "clock.c"), "-o", str(library)], check=True, capture_output=True)
            client = ctypes.CDLL(str(library))
            callback_type = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_bool, ctypes.c_char_p)
            completed = []
            callback = callback_type(lambda kind, ok, text: completed.append((kind.decode(), ok, text.decode())))
            client.media_configure.argtypes = [callback_type, ctypes.c_char_p]
            client.media_start.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            client.media_start.restype = client.media_busy.restype = ctypes.c_bool
            path = str(directory).encode()
            client.media_configure(callback, path)
            clock = ctypes.c_uint32.in_dll(client, "now")
            (directory / "host-ready.txt").write_text("1")

            def begin():
                self.assertTrue(client.media_start(b"camera", b""))
                self.assertTrue(client.media_busy())
                self.assertFalse(client.media_start(b"microphone", b""))
                request_id, kind, text = (directory / "request.txt").read_text().split("\n", 2)
                self.assertEqual((kind, text), ("camera", ""))
                return request_id

            def reply(request_id, status="OK"):
                (directory / "response.txt").write_text(f"{request_id}\n{status}\ncamera\ncaptured")
                client.media_poll()

            first = begin(); reply(first)
            self.assertEqual(completed, [("camera", True, "captured")])
            self.assertFalse(client.media_busy())
            second = begin(); reply(first)
            self.assertTrue(client.media_busy())
            self.assertEqual(len(completed), 1)
            reply(second)
            self.assertFalse(client.media_busy())
            self.assertEqual(len(completed), 2)
            timed_out = begin(); clock.value = 60000; client.media_poll()
            self.assertFalse(client.media_busy())
            self.assertEqual(completed[-1], ("camera", False, "Hardware request timed out. Please try again."))
            retry = begin(); reply(timed_out)
            self.assertTrue(client.media_busy())
            reply(retry, "ERROR")
            self.assertFalse(client.media_busy())
            self.assertEqual(completed[-1], ("camera", False, "captured"))

if __name__ == "__main__":
    unittest.main()
