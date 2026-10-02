#!/usr/bin/env python3
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

"""Preset round trips, renderer output, sprite exports, and local HTTP access."""
import hashlib
import importlib.util
from contextlib import closing
import http.client
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile
from http.server import HTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "muse"))
import customize  # noqa: E402
import make_gifs  # noqa: E402


def digest(animations):
    return {name: hashlib.sha256(b"".join(f.tobytes() for f in seq)).hexdigest()
            for name, seq in animations.items()}


class PresetTests(unittest.TestCase):
    def test_round_trip_and_missing_fields(self):
        p = customize.validate({"name": "My plush"})
        self.assertEqual(customize.validate(json.loads(json.dumps(p))), p)
        self.assertEqual(p["width"], 1.0)

    def test_reject_invalid_values_and_code_injection(self):
        cases = [[], {"unknown": 1}, {"version": True}, {"version": 2},
                 {"pack": "../../evil"}, {"pack": []}, {"name": "\n"}, {"name": "x" * 65},
                 {"fur": "#ffffff\n#error"}, {"width": True}, {"width": float("nan")},
                 {"height": float("inf")}, {"face_x": -3}, {"height": 0},
                 {"accessory": "#include"}, {"accessory": {}}]
        for value in cases:
            with self.subTest(value=value), self.assertRaises(ValueError):
                customize.validate(value)

    def test_defaults_preserve_shading_exactly(self):
        for shade in ("3a2b22", "ae987e", "e6d7bd", "f8eedc"):
            self.assertEqual(customize.tint("#cfbc9f", "cfbc9f", shade), "0x" + shade)

    def test_source_preserves_copyright_and_is_standalone(self):
        code = customize.source({"name": "*/ malicious name /*", "accessory": "scarf"})
        self.assertTrue(code.startswith("// Copyright (c) Meta Platforms, Inc. and affiliates."))
        self.assertNotIn("malicious name", code)
        self.assertIn("#define MUSE_STYLE_ACCESSORY 1", code)
        self.assertIn('#include "muse_avatar_style.h"', code)

    def test_invalid_install_keeps_existing_renderer(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "muse_pixel.c"
            path.write_text("previous renderer")
            with patch.object(make_gifs, "CUSTOM_SRC", str(path)), self.assertRaises(ValueError):
                customize.install({"width": 0})
            self.assertEqual(path.read_text(), "previous renderer")
            self.assertFalse(Path(str(path) + ".prev").exists())


@unittest.skipUnless(importlib.util.find_spec("PIL"), "Pillow needed for avatar previews")
class RendererTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = make_gifs.render_frames(make_gifs.DEFAULT_SRC, scale=1)
        cls.default = customize.frames(customize.DEFAULT)

    def test_default_preset_matches_unmodified_template_in_every_state(self):
        self.assertEqual(digest(self.original), digest(self.default))

    def test_all_states_and_native_dimensions(self):
        self.assertEqual(set(self.default), set(customize.ANIMATIONS))
        expected = {"boot": 75, "idle": 150, "listening": 100, "thinking": 100,
                    "speaking": 100, "happy": 75, "off": 50, "error": 75}
        self.assertEqual({name: len(seq) for name, seq in self.default.items()}, expected)
        self.assertTrue(all(frame.size == (64, 64) for seq in self.default.values() for frame in seq))
        self.assertGreater(len(set(digest(self.default).values())), 6)

    def test_accessories_and_shape_change_the_rendered_character(self):
        base = digest(self.default)
        for p in ({"accessory": "scarf"}, {"accessory": "bow"},
                  {"width": 1.15, "height": .90, "face_x": -2, "face_y": 2, "fur": "#80bed0"}):
            with self.subTest(preset=p):
                result = digest(customize.frames(p))
                self.assertNotEqual(result["idle"], base["idle"])
                self.assertNotEqual(result["speaking"], base["speaking"])

    def test_pack_defaults_and_extreme_values_render_all_states(self):
        for key in customize.packs():
            if key == "jollybot":
                continue
            for settings in ({}, {"width": .85, "height": 1.08, "face_x": 2, "face_y": -2,
                                  "accessory": "scarf", "accessory_color": "#ffffff"}):
                with self.subTest(pack=key, settings=settings):
                    result = customize.frames({"pack": key, **settings})
                    self.assertEqual(set(result), set(customize.ANIMATIONS))
                    self.assertNotEqual(digest(result)["idle"], digest(self.default)["idle"])

    def test_export_round_trip_and_sprite_frames(self):
        from PIL import Image

        with patch.object(customize, "frames", return_value=self.default):
            archive = customize.bundle({"name": "Test export"})
        with zipfile.ZipFile(io.BytesIO(archive)) as z:
            self.assertIsNone(z.testzip())
            p = customize.validate(json.loads(z.read("preset.json")))
            self.assertEqual(z.read("muse_pixel.c").decode(), customize.source(p))
            manifest = json.loads(z.read("sprites/manifest.json"))
            self.assertEqual(manifest["frame_ms"], 40)
            self.assertEqual(manifest["background"], "opaque")
            for name, entry in manifest["states"].items():
                with Image.open(io.BytesIO(z.read(entry["file"]))) as atlas:
                    self.assertEqual(atlas.size, (64 * entry["columns"], 64 * entry["rows"]))
                    for i in (0, entry["frames"] - 1):
                        x, y = (i % entry["columns"]) * 64, (i // entry["columns"]) * 64
                        self.assertEqual(atlas.crop((x, y, x + 64, y + 64)).tobytes(), self.default[name][i].tobytes())
                with Image.open(io.BytesIO(z.read(f"gifs/{name}.gif"))) as gif:
                    self.assertEqual(gif.size, (64, 64))
            self.assertFalse(manifest["states"]["off"]["loop"])
            self.assertTrue(manifest["states"]["idle"]["loop"])

    def test_install_backs_up_previous_renderer(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "muse_pixel.c"
            path.write_text("previous renderer")
            with patch.object(make_gifs, "CUSTOM_SRC", str(path)), patch.object(customize, "frames", return_value=self.default):
                customize.install({"accessory": "bow"})
            self.assertEqual(Path(str(path) + ".prev").read_text(), "previous renderer")
            self.assertEqual(path.read_text(), customize.source({"accessory": "bow"}))
            self.assertEqual(json.loads((path.parent / "preset.json").read_text())["accessory"], "bow")


class HTTPTests(unittest.TestCase):
    def setUp(self):
        self.server = HTTPServer(("127.0.0.1", 0), customize.handler("test-session"))
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.worker.start()
        self.host = f"127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.worker.join(timeout=5)
        self.server.server_close()

    def request(self, path="/api/preview", body="{}", **headers):
        with closing(http.client.HTTPConnection(self.host, timeout=5)) as conn:
            conn.request("POST", path, body, {"Content-Type": "application/json", "X-Muse-Session": "test-session", **headers})
            response = conn.getresponse()
            return response.status, response.read()

    def test_reject_cross_origin_wrong_host_and_missing_session(self):
        for headers in ({"Origin": "https://example.com"}, {"Host": "evil.example"},
                        {"X-Muse-Session": ""}):
            with self.subTest(headers=headers):
                self.assertEqual(self.request(**headers)[0], 403)

    def test_validate_before_compile(self):
        with patch.object(customize, "preview") as render:
            self.assertEqual(self.request(body='{"width":0}')[0], 400)
            self.assertEqual(self.request(body="x" * 8193)[0], 400)
            self.assertEqual(self.request(path="/api/install")[0], 404)
            render.assert_not_called()

    def test_valid_request_reaches_renderer_and_page_loads(self):
        with patch.object(customize, "preview", return_value={"manifest": {}}):
            self.assertEqual(self.request(Origin="http://" + self.host)[0], 200)
        with closing(http.client.HTTPConnection(self.host, timeout=5)) as conn:
            conn.request("GET", "/")
            response = conn.getresponse()
            self.assertEqual(response.status, 200)
            self.assertIn(b"test-session", response.read())

    def test_exports_are_bounded_download_links_with_attachment_headers(self):
        urls = []
        with patch.object(customize, "bundle", return_value=b"test archive"):
            for _ in range(5):
                status, data = self.request(path="/api/export")
                self.assertEqual(status, 200)
                urls.append(json.loads(data)["download"])
        with closing(http.client.HTTPConnection(self.host, timeout=5)) as conn:
            conn.request("GET", urls[-1])
            response = conn.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.getheader("Content-Type"), "application/zip")
            self.assertEqual(response.getheader("Content-Disposition"), 'attachment; filename="muse-character.zip"')
            self.assertEqual(response.read(), b"test archive")
        with closing(http.client.HTTPConnection(self.host, timeout=5)) as conn:
            conn.request("GET", urls[0])
            response = conn.getresponse()
            self.assertEqual(response.status, 404)
            response.read()

    def test_preset_download_round_trip(self):
        status, data = self.request(path="/api/preset", body='{"name":"Saved look","accessory":"bow"}')
        self.assertEqual(status, 200)
        with closing(http.client.HTTPConnection(self.host, timeout=5)) as conn:
            conn.request("GET", json.loads(data)["download"])
            response = conn.getresponse()
            self.assertEqual(response.status, 200)
            p = customize.validate(json.loads(response.read()))
            self.assertEqual(p["name"], "Saved look")
            self.assertEqual(p["accessory"], "bow")


if __name__ == "__main__":
    unittest.main()
