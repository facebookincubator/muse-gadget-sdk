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

"""tsan_run's answer when ThreadSanitizer can't run here: a skip with the
reason on a desk, a failure in CI (the CI environment variable set, as GitHub
Actions sets it and the host-tests job passes into its container), so CI never
passes with its race checks skipped."""

import os
import unittest
from pathlib import Path
from unittest import mock

import tsan_run

ROOT = Path(__file__).resolve().parents[1]


class Inner(unittest.TestCase):
    def runTest(self) -> None:   # the TSan test tsan_run.run answers for
        pass


class TsanUnavailable(unittest.TestCase):
    def setUp(self) -> None:
        saved = tsan_run._probe_reason
        self.addCleanup(setattr, tsan_run, "_probe_reason", saved)
        tsan_run._probe_reason = "the ThreadSanitizer runtime doesn't run here (a test's reason)"   # a failed probe

    def run_with(self, ci) -> str:
        """What tsan_run.run did with the failed probe: "failed: ...", "skipped: ..." or "ran"."""
        env = {k: v for k, v in os.environ.items() if k != "CI"}
        if ci is not None:
            env["CI"] = ci
        with mock.patch.dict(os.environ, env, clear=True):
            try:
                tsan_run.run(Inner(), ["true"])
            except unittest.SkipTest as e:
                return f"skipped: {e}"
            except AssertionError as e:
                return f"failed: {e}"
        return "ran"

    def test_a_failed_probe_fails_in_ci(self) -> None:
        for ci in ("true", "1", "TRUE"):
            with self.subTest(ci=ci):
                self.assertEqual(self.run_with(ci), "failed: ThreadSanitizer must run in CI, but the ThreadSanitizer "
                                                    "runtime doesn't run here (a test's reason)")

    def test_a_failed_probe_skips_elsewhere_with_its_reason(self) -> None:
        for ci in (None, "", "0", "false"):
            with self.subTest(ci=ci):
                self.assertEqual(self.run_with(ci),
                                 "skipped: the ThreadSanitizer runtime doesn't run here (a test's reason)")

    def test_the_host_tests_container_gets_ci(self) -> None:
        # GitHub sets CI on the runner; the ESP-IDF action's docker run passes only what extra_docker_args names.
        yml = (ROOT.parent / ".github" / "workflows" / "esp32.yml").read_text()
        job = yml[yml.index("  host-tests:"):]
        step = job[job.index("- name: Run host tests in ESP-IDF"):]
        step = step[:step.index("command: |")]
        self.assertRegex(step, r"extra_docker_args: [^\n]*-e CI\b")


if __name__ == "__main__":
    unittest.main()
