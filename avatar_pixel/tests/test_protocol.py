# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

import base64
import json
import unittest

from avatar_pixel import AvatarError
from avatar_pixel.protocol import MAX_BODY_BYTES, compile_request, parse_request
from avatar_pixel.tests.fixtures import avatar, encode


class RequestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with avatar("animal") as image:
            cls.encoded = encode(image)
        cls.text = base64.b64encode(cls.encoded).decode("ascii")

    def assert_error(self, code, body):
        with self.assertRaises(AvatarError) as caught:
            parse_request(body)
        self.assertEqual(caught.exception.code, code)

    def test_response_and_defaults(self):
        request = json.dumps({"image_base64": self.text}).encode()
        parsed = parse_request(request)
        self.assertEqual(parsed["encoded"], self.encoded)
        self.assertEqual(parsed["profile"], 64)
        self.assertEqual(parsed["colors"], 24)
        self.assertFalse(parsed["dither"])
        response = json.loads(compile_request(request))
        self.assertEqual(
            set(response), {"pack_base64", "preview_png_base64", "metadata"}
        )
        self.assertTrue(
            base64.b64decode(response["pack_base64"]).startswith(b"MSPIX01\0")
        )

    def test_duplicate_even_nested_or_escaped_keys(self):
        for body in (
            b'{"image_base64":"x","image_base64":"y"}',
            b'{"profile":64,"profi\\u006ce":96}',
            b'{"unknown":{"nested":1,"nested":2}}',
        ):
            self.assert_error("duplicate_field", body)

    def test_unknown_url_path_and_trusted_overrides_prohibited(self):
        for field in (
            "url",
            "image_url",
            "input",
            "trusted_source",
            "source_path",
            "background",
            "remove_background",
            "prompt",
        ):
            body = json.dumps(
                {"image_base64": self.text, field: "https://example.invalid/private"}
            ).encode()
            self.assert_error("unexpected_field", body)
        for text in (
            "https://example.invalid/avatar.png",
            "data:image/png;base64," + self.text,
            self.text + "=",
            " " + self.text,
        ):
            self.assert_error(
                "invalid_image_base64", json.dumps({"image_base64": text}).encode()
            )

    def test_invalid_json_and_finite_constants(self):
        for body in (
            b"",
            b"{",
            b"\xff",
            b'{"a":NaN}',
            b'{"a":Infinity}',
            b'{"a":-Infinity}',
            b"[" * 2000,
        ):
            self.assert_error("invalid_json", body)
        for body in (b"null", b"[]", b"1", b'"image_base64"'):
            self.assert_error("invalid_request", body)
        self.assert_error("body_bytes_exceeded", b" " * (MAX_BODY_BYTES + 1))

    def test_types_rejected_not_coerced(self):
        for field, values in (
            ("image_base64", (None, False, 42, [], {})),
            ("profile", (None, True, "64", 64.0, 1e300)),
            ("colors", (None, True, "24", 2.0, 65)),
            ("dither", (None, 1, "false", [])),
            ("source_key", (None, 32, "credential", [], {})),
        ):
            for value in values:
                with self.subTest(field=field, value=value):
                    request = {"image_base64": self.text, field: value}
                    code = (
                        "invalid_image_base64"
                        if field == "image_base64"
                        else "invalid_" + field
                    )
                    self.assert_error(code, json.dumps(request).encode())
        self.assert_error("invalid_image_base64", b"{}")

    def test_digest_and_options(self):
        key = "cd" * 32
        request = json.dumps(
            {
                "image_base64": self.text,
                "profile": 96,
                "colors": 2,
                "dither": True,
                "source_key": key,
            }
        ).encode()
        response = json.loads(compile_request(request))
        self.assertEqual(response["metadata"]["source_key"], key)
        self.assertEqual(response["metadata"]["width"], 96)
        self.assertEqual(response["metadata"]["requested_colors"], 2)


if __name__ == "__main__":
    unittest.main()
