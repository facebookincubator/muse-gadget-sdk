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

"""TLS session resumption for the VM WebSockets (CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS).

Link's tls_resume_* helpers, sliced from main/noise_control.cpp, run on the
host: a connect that failed before TLS began (DNS, TCP) keeps the session; a
TLS-level failure drops it. Resuming must never weaken the server check: both
connects (Link's control session and Muse's voice session) keep the CA bundle
attached (a server that doesn't resume gets a full, verified handshake), a
session is offered only to the host it was made with, and no build keeps
whole peer certificates in RAM for it.
"""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTROL = ROOT / "main/noise_control.cpp"


class LinkTlsResume(unittest.TestCase):
    def test_session_kept_across_network_failures(self):
        source = CONTROL.read_text()
        start = source.index("// TLS session resumption for the VM WebSocket")
        end = source.index("#endif", source.index("static void tls_resume_failed(", start))
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "tls_resume.inc").write_text(source[start:end])
            binary = Path(tmp) / "tls_resume"
            built = subprocess.run(
                [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", *sanitize, "-I", tmp,
                 str(ROOT / "tests/link_tls_resume_harness.cpp"), "-o", str(binary)],
                capture_output=True, text=True, env=env)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), "ok")



SITES = {
    "Link control": ROOT / "main" / "noise_control.cpp",
    "Muse voice": ROOT / "components" / "muse" / "muse_chat_session.cpp",
}


class TlsResumeTest(unittest.TestCase):
    def test_both_connects_keep_the_ca_bundle(self) -> None:
        for name, path in SITES.items():
            src = path.read_text()
            with self.subTest(name):
                offer = src.index("tls_resume_offer(", src.index("esp_tls_cfg_t cfg = {};"))
                head = src[src.index("esp_tls_cfg_t cfg = {};"):offer]
                self.assertIn("cfg.crt_bundle_attach = esp_crt_bundle_attach;", head)
                self.assertNotIn("skip_common_name", src)
                self.assertNotIn("use_global_ca_store", src)

    def test_offered_only_to_its_own_host(self) -> None:
        for name, path in SITES.items():
            src = path.read_text()
            with self.subTest(name):
                body = src[src.index("tls_resume_offer("):]
                body = body[:body.index("\n}\n")]
                self.assertRegex(body, r"strcmp\([^)]*host[^)]*\) != 0\)")
                self.assertIn("tls_resume_forget", body)

    def test_defaults_on_and_no_peer_certificates_kept(self) -> None:
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        self.assertRegex(defaults, r"(?m)^CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS=y$")
        for path in [ROOT / "sdkconfig.defaults", *sorted((ROOT / "devices").glob("sdkconfig.*"))]:
            text = path.read_text()
            with self.subTest(path.name):
                self.assertNotRegex(text, r"(?m)^CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=y")
                if re.search(r"(?m)^CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS=y", text):
                    self.assertNotRegex(text, r"(?m)^CONFIG_ESP_TLS_(INSECURE|SKIP_SERVER_CERT_VERIFY)=y")


if __name__ == "__main__":
    unittest.main()
