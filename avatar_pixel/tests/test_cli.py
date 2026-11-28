# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

import base64
import http.client
import json
import os
from pathlib import Path
import select
import signal
import stat
import subprocess
import sys
import tempfile
import unittest

from avatar_pixel import validate_pack
from avatar_pixel.tests.fixtures import avatar, encode

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT.parent


def environment():
    return {"PYTHONPATH": str(SDK), "PYTHONDONTWRITEBYTECODE": "1"}


class CLITests(unittest.TestCase):
    def run_cli(self, directory, *args, env=None):
        return subprocess.run(
            [sys.executable, "-B", "-m", "avatar_pixel", *args],
            cwd=directory,
            env=env or environment(),
            capture_output=True,
            timeout=15,
            check=False,
        )

    def test_cli_private_files_and_no_overwrite(self):
        with tempfile.TemporaryDirectory(prefix=".test-cli-", dir=ROOT) as scratch:
            directory = Path(scratch)
            source = directory / "synthetic.png"
            with avatar("robot") as image:
                source.write_bytes(encode(image))
            args = (
                "compile",
                "--input",
                str(source),
                "--output",
                "robot.mspix",
                "--preview",
                "robot.png",
                "--manifest",
                "robot.json",
                "--profile",
                "96",
                "--colors",
                "24",
                "--source-key",
                "ef" * 32,
            )
            result = self.run_cli(directory, *args)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, b"compiled\n")
            self.assertEqual(result.stderr, b"")
            metadata = validate_pack((directory / "robot.mspix").read_bytes())
            self.assertEqual(metadata["width"], 96)
            self.assertEqual(metadata["source_key"], "ef" * 32)
            self.assertEqual(
                json.loads((directory / "robot.json").read_bytes())["source_key"],
                "ef" * 32,
            )
            for name in ("robot.mspix", "robot.png", "robot.json"):
                self.assertEqual(stat.S_IMODE((directory / name).stat().st_mode), 0o600)
            before = (directory / "robot.mspix").read_bytes()
            result = self.run_cli(directory, *args)
            self.assertEqual(result.returncode, 2)
            self.assertEqual((directory / "robot.mspix").read_bytes(), before)
            self.assertNotIn(str(source).encode(), result.stderr)

    def test_cli_rolls_back_only_its_new_files(self):
        with tempfile.TemporaryDirectory(prefix=".test-cli-", dir=ROOT) as scratch:
            directory = Path(scratch)
            with avatar("plant") as image:
                (directory / "source.png").write_bytes(encode(image))
            (directory / "existing.png").write_bytes(b"preserve existing work")
            result = self.run_cli(
                directory,
                "compile",
                "--input",
                "source.png",
                "--output",
                "new.mspix",
                "--preview",
                "existing.png",
            )
            self.assertEqual(result.returncode, 2)
            self.assertFalse((directory / "new.mspix").exists())
            self.assertEqual(
                (directory / "existing.png").read_bytes(), b"preserve existing work"
            )
            (directory / "linked.png").symlink_to(directory / "existing.png")
            result = self.run_cli(
                directory, "compile", "--input", "source.png", "--output", "linked.png"
            )
            self.assertEqual(result.returncode, 2)
            self.assertEqual(
                (directory / "existing.png").read_bytes(), b"preserve existing work"
            )

    def test_cli_no_device_fifo_or_urls(self):
        with tempfile.TemporaryDirectory(prefix=".test-cli-", dir=ROOT) as scratch:
            directory = Path(scratch)
            os.mkfifo(directory / "fifo")
            for path in ("fifo", "https://example.invalid/image.png"):
                result = self.run_cli(
                    directory, "compile", "--input", path, "--output", "never.mspix"
                )
                self.assertEqual(result.returncode, 2)
                self.assertNotIn(path.encode(), result.stderr)
                self.assertFalse((directory / "never.mspix").exists())

    def test_cli_nonloopback_refused_before_bind_and_no_token_leak(self):
        with tempfile.TemporaryDirectory(prefix=".test-cli-", dir=ROOT) as scratch:
            result = self.run_cli(scratch, "serve", "--host", "0.0.0.0", "--port", "0")
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stderr, b"nonloopback_requires_auth\n")
            self.assertEqual(result.stdout, b"")
            env = environment()
            env["AVATAR_PIXEL_BEARER_TOKEN"] = "private-short-token"
            result = self.run_cli(scratch, "serve", env=env)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stderr, b"invalid_bearer_token\n")
            self.assertNotIn(env["AVATAR_PIXEL_BEARER_TOKEN"].encode(), result.stderr)

    def test_genuine_ephemeral_cli_http_process_no_saved_uploads(self):
        with tempfile.TemporaryDirectory(prefix=".test-http-", dir=ROOT) as scratch:
            process = subprocess.Popen(
                [sys.executable, "-B", "-m", "avatar_pixel", "serve", "--port", "0"],
                cwd=scratch,
                env=environment(),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                self.assertTrue(
                    select.select([process.stdout], [], [], 5)[0],
                    "server did not become ready",
                )
                ready = process.stdout.readline()
                self.assertTrue(
                    ready.startswith(b"avatar_pixel listening on 127.0.0.1:"), ready
                )
                port = int(ready.rstrip().split(b":")[-1])
                with avatar("logo") as image:
                    source = encode(image)
                body = json.dumps(
                    {
                        "image_base64": base64.b64encode(source).decode(),
                        "profile": 64,
                        "colors": 24,
                    }
                ).encode()
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                try:
                    connection.request(
                        "POST",
                        "/v1/compile",
                        body=body,
                        headers={"Content-Type": "application/json"},
                    )
                    response = connection.getresponse()
                    self.assertEqual(response.status, 200)
                    result = json.loads(response.read())
                    metadata = validate_pack(base64.b64decode(result["pack_base64"]))
                    self.assertEqual(metadata["width"], 64)
                finally:
                    connection.close()
                self.assertEqual(list(Path(scratch).iterdir()), [])
            finally:
                process.send_signal(signal.SIGINT)
                try:
                    stdout, stderr = process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                    raise AssertionError("test HTTP process did not stop")
            self.assertEqual(process.returncode, 0, stderr)
            self.assertEqual(stdout, b"")
            self.assertEqual(stderr, b"")


if __name__ == "__main__":
    unittest.main()
