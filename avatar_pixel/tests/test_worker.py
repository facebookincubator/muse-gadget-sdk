# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

import base64
import io
import json
from types import ModuleType, SimpleNamespace
import unittest
from unittest import mock

from avatar_pixel import _worker
from avatar_pixel.protocol import compile_request
from avatar_pixel.tests.fixtures import avatar, encode


def fake_resource(failure=None):
    resource = ModuleType("resource")
    for name in ("CORE", "FSIZE", "CPU", "AS"):
        setattr(resource, "RLIMIT_" + name, name)

    def set_limit(limit, values):
        if limit == failure:
            raise ValueError("private resource error")

    resource.setrlimit = mock.Mock(side_effect=set_limit)
    return resource


class WorkerResourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with avatar("robot") as image:
            data = encode(image)
        cls.body = json.dumps(
            {"image_base64": base64.b64encode(data).decode("ascii")}
        ).encode()
        cls.expected = compile_request(cls.body)

    def run_worker(self, platform, resource, body=None):
        source = mock.Mock(wraps=io.BytesIO(self.body if body is None else body))
        output = io.BytesIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(_worker.sys, "platform", platform),
            mock.patch.dict("sys.modules", {"resource": resource}),
            mock.patch.object(_worker.sys, "stdin", SimpleNamespace(buffer=source)),
            mock.patch.object(_worker.sys, "stdout", SimpleNamespace(buffer=output)),
            mock.patch.object(_worker.sys, "stderr", stderr),
        ):
            status = _worker.main()
        self.assertEqual(stderr.getvalue(), "")
        return status, output.getvalue(), source

    def test_darwin_valid_image_skips_only_unsupported_as(self):
        # Model the real macOS ValueError if RLIMIT_AS is attempted. It must
        # never be attempted, and valid image bytes must still be compiled.
        resource = fake_resource(failure="AS")
        status, output, source = self.run_worker("darwin", resource)
        self.assertEqual(status, 0)
        self.assertEqual(output, self.expected)
        self.assertEqual(source.read.call_count, 1)
        self.assertEqual(
            resource.setrlimit.call_args_list,
            [
                mock.call("CORE", (0, 0)),
                mock.call("FSIZE", (0, 0)),
                mock.call("CPU", (30, 30)),
            ],
        )

    def test_darwin_invalid_json_reaches_validation_without_private_errors(self):
        resource = fake_resource(failure="AS")
        status, output, source = self.run_worker("darwin", resource, b"{private")
        self.assertEqual(status, 2)
        self.assertEqual(output, b'{"error":"invalid_json"}')
        self.assertEqual(source.read.call_count, 1)

    def test_linux_valid_image_retains_hard_as_limit(self):
        resource = fake_resource()
        status, output, _source = self.run_worker("linux", resource)
        self.assertEqual(status, 0)
        self.assertEqual(output, self.expected)
        resource.setrlimit.assert_any_call("AS", (512 * 1024 * 1024,) * 2)
        for limit, values in (("CORE", (0, 0)), ("FSIZE", (0, 0)), ("CPU", (30, 30))):
            resource.setrlimit.assert_any_call(limit, values)

    def test_linux_as_failure_is_not_silently_relaxed(self):
        resource = fake_resource(failure="AS")
        status, output, source = self.run_worker("linux", resource)
        self.assertEqual(status, 3)
        self.assertEqual(output, b'{"error":"compile_failed"}')
        source.read.assert_not_called()
        resource.setrlimit.assert_any_call("AS", (512 * 1024 * 1024,) * 2)

    def test_missing_unix_resource_is_fail_closed(self):
        for platform in ("linux", "darwin", "freebsd"):
            with self.subTest(platform=platform):
                status, output, source = self.run_worker(platform, None)
                self.assertEqual(status, 3)
                self.assertEqual(output, b'{"error":"compile_failed"}')
                source.read.assert_not_called()

    def test_darwin_required_limits_still_fail_closed(self):
        for limit in ("CORE", "FSIZE", "CPU"):
            with self.subTest(limit=limit):
                resource = fake_resource(failure=limit)
                status, output, source = self.run_worker("darwin", resource)
                self.assertEqual(status, 3)
                self.assertEqual(output, b'{"error":"compile_failed"}')
                source.read.assert_not_called()


if __name__ == "__main__":
    unittest.main()
