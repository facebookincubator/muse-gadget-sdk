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

"""Keep VM selection and its credential paired throughout the connection path.

The client must preserve three invariants:

  1. Send the selected fetch_vms entry's vm_auth_token without changing its bytes.
  2. Refuse a missing token; do not substitute an account-wide or device credential.
  3. Keep credentials isolated between VM entries, including across VM switches.

Each token is bound to a VM through its env_id. The edge proxy rejects a token
for a different VM before the connection reaches that VM, so mixing entries
causes a connection failure and repeated retries across affected devices.

The rules are enforced in three places, and each is covered by running the real
code under a host harness:

  tests/link_vm_api_harness.c      parsing a fetch_vms response — one
                                   independent token per entry, stored verbatim
  tests/link_vm_connect_harness.c  choosing what to connect with — the id and
                                   the token come from one entry, or we refuse
  tests/link_noise_upgrade_harness.c  what goes on the wire — the exact token,
                                   in a header, for this vm_id

The first two are run by tests/test_link_vm_api.py and this module; the third by
tests/test_link_noise_session.py.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"


def _cc_command() -> list[str]:
    cmd = shlex.split(os.environ.get("CC", "cc"))
    if not cmd or shutil.which(cmd[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return cmd


def _sanitizer_flags(cc: list[str], tmp: Path) -> list[str]:
    probe = tmp / "asan_probe"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    proc = run_child(
        [*cc, "-x", "c", "-std=c11", *flags, "-o", str(probe), "-"],
        input=b"int main(void) { return 0; }\n",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        return []
    run = subprocess.run([str(probe)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return flags if run.returncode == 0 else []


class VmConnectHarnessTest(unittest.TestCase):
    """Runs tests/link_vm_connect_harness.c against the real vm_connect.h.

    That harness is where the three rules are actually asserted: it resolves
    real vm_info_t entries and checks the id and token that come back.
    """

    def test_vm_connect_harness(self) -> None:
        cc = _cc_command()
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            binary = tmp / "link_vm_connect_harness"
            compiled = subprocess.run(
                [
                    *cc,
                    "-std=c11",
                    "-D_GNU_SOURCE",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-g",
                    "-O1",
                    *_sanitizer_flags(cc, tmp),
                    "-I",
                    str(MAIN),
                    str(ROOT / "tests" / "link_vm_connect_harness.c"),
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                compiled.returncode, 0, msg=compiled.stdout + compiled.stderr
            )
            env = {**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"}
            ran = subprocess.run(
                [str(binary)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
            )
            self.assertEqual(ran.returncode, 0, msg=ran.stdout + ran.stderr)


class NoLegacyCredentialLintTest(unittest.TestCase):
    """The one rule with nothing to execute.

    Link has no legacy account-wide credential to fall back to. Keep its exact
    identifier in the lint pattern so reintroducing it in firmware is detected.
    There is no runtime path to exercise; this is the only source-level
    assertion in this file.
    """

    def test_no_legacy_credential_exists_anywhere_in_the_firmware(self) -> None:
        sources = sorted(MAIN.glob("*.[ch]")) + sorted(MAIN.glob("*.cpp"))
        self.assertTrue(sources)
        for path in sources:
            self.assertIsNone(
                re.search(r"(?i)\babra\b", path.read_text()),
                msg=f"{path.name} references a legacy account-wide credential",
            )


if __name__ == "__main__":
    unittest.main()
