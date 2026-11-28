# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

import base64
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

from avatar_pixel import AvatarError, compile_image, validate_pack
from avatar_pixel.protocol import compile_request, parse_request
from avatar_pixel.tests.test_api import request, running
from avatar_pixel.tests.test_cli import environment
from avatar_pixel.tests.test_compiler import parts
from avatar_pixel.tests.test_matte import outlined_white
from avatar_pixel.tests.fixtures import encode

ROOT = Path(__file__).resolve().parent.parent


class MatteInterfaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with outlined_white() as image:
            cls.encoded = encode(image)
        cls.text = base64.b64encode(cls.encoded).decode("ascii")

    def body(self, **options):
        return json.dumps({"image_base64": self.text, **options}).encode()

    def test_json_false_or_omitted_is_identical(self):
        self.assertFalse(parse_request(self.body())["remove_white_background"])
        self.assertEqual(
            compile_request(self.body()),
            compile_request(self.body(remove_white_background=False)),
        )
        self.assertNotIn(
            "white_background", json.loads(compile_request(self.body()))["metadata"]
        )

    def test_json_option_rejects_wrong_types_duplicates_and_override_fields(self):
        for value in (None, 0, 1, "true", "false", [], {}, 1.0, 1e300):
            with self.subTest(value=value):
                with self.assertRaisesRegex(
                    AvatarError, "invalid_remove_white_background"
                ):
                    parse_request(self.body(remove_white_background=value))
        duplicate = (
            b'{"image_base64":"not decoded",'
            b'"remove_white_background":true,"remove_white_background":false}'
        )
        with self.assertRaisesRegex(AvatarError, "duplicate_field"):
            parse_request(duplicate)
        for field in ("remove_background", "matte_threshold", "background_url"):
            with self.assertRaisesRegex(AvatarError, "unexpected_field"):
                parse_request(self.body(**{field: True}))

    def test_real_http_new_option_matches_library_with_private_bounded_response(self):
        with running() as server:
            body = self.body(remove_white_background=True)
            status, headers, output = request(
                server,
                "POST",
                "/v1/compile",
                body,
                {"Content-Type": "application/json"},
            )
            self.assertEqual(status, 200, output)
            self.assertEqual(headers["Cache-Control"], "no-store")
            result = json.loads(output)
            pack = base64.b64decode(result["pack_base64"], validate=True)
            expected = compile_image(self.encoded, remove_white_background=True)
            self.assertEqual(pack, expected.pack)
            self.assertEqual(
                base64.b64decode(result["preview_png_base64"], validate=True),
                expected.preview_png,
            )
            self.assertTrue(result["metadata"]["white_background"]["applied"])
            self.assertEqual(result["metadata"]["white_background"]["removed"], 3072)
            self.assertTrue(any(parts(pack)[2]))
            self.assertIn(0, parts(pack)[2])
            validate_pack(pack)
            self.assertLess(len(output), 800 * 1024)
            # Reject explicit null via genuine HTTP, without decoding/echoing it.
            status, _, output = request(
                server,
                "POST",
                "/v1/compile",
                self.body(remove_white_background=None),
                {"Content-Type": "application/json"},
            )
            self.assertEqual(status, 400)
            self.assertEqual(output, b'{"error":"invalid_remove_white_background"}')

    def test_cli_opt_in_and_default_private_files(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-matte-cli-", dir=ROOT
        ) as scratch:
            directory = Path(scratch)
            (directory / "synthetic.png").write_bytes(self.encoded)
            args = [
                sys.executable,
                "-B",
                "-m",
                "avatar_pixel",
                "compile",
                "--input",
                "synthetic.png",
                "--output",
                "matted.mspix",
                "--manifest",
                "matted.json",
                "--remove-white-background",
            ]
            done = subprocess.run(
                args,
                cwd=scratch,
                env=environment(),
                capture_output=True,
                timeout=15,
                check=False,
            )
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(done.stdout, b"compiled\n")
            self.assertEqual(done.stderr, b"")
            pack = (directory / "matted.mspix").read_bytes()
            self.assertEqual(
                pack, compile_image(self.encoded, remove_white_background=True).pack
            )
            self.assertTrue(
                json.loads((directory / "matted.json").read_bytes())[
                    "white_background"
                ]["applied"]
            )
            self.assertEqual(
                stat.S_IMODE((directory / "matted.mspix").stat().st_mode), 0o600
            )
            done = subprocess.run(
                [
                    sys.executable,
                    "-B",
                    "-m",
                    "avatar_pixel",
                    "compile",
                    "--input",
                    "synthetic.png",
                    "--output",
                    "default.mspix",
                ],
                cwd=scratch,
                env=environment(),
                capture_output=True,
                timeout=15,
                check=False,
            )
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(
                (directory / "default.mspix").read_bytes(),
                compile_image(self.encoded).pack,
            )


if __name__ == "__main__":
    unittest.main()
