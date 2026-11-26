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
"""The security draws' one source (components/secure_random).

On an ESP32, esp_fill_random() is a true RNG only while BT or Wi-Fi runs, so
each security draw checks a radio is on and is refused otherwise, never drawn
from a weaker source. This runs the helper itself on the host with the BT
controller and Wi-Fi faked, noise_core's backend as the firmware builds it,
and holds the inventory: every esp_fill_random() / esp_random() call in the
firmware is either the helper or a listed draw that isn't a security one.
Link setup's hello (test_link_pairing_handshake) and the token proof
(test_mg_ble's mg_proto harness) run their refusals in their own tests.
"""

import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
COMP = ROOT / "components" / "secure_random"
NOISE = ROOT / "components" / "noise_core"

sys.path.insert(0, str(TESTS))

# The firmware's draws that aren't security draws, each with why. A new
# esp_fill_random() / esp_random() call anywhere else fails test_inventory:
# a security draw goes through secure_random(); anything else is added here
# with its reason.
NOT_SECURITY = {
    "components/muse/muse_voice_call.cpp":
        "the call's id: a name the end-call repeats so it can't end a newer call; sent inside the session",
    "components/muse/muse_chat_link.c": "request ids: names, sent inside the encrypted session",
    "components/muse/muse_chat_session.cpp":
        "request ids, and WebSocket masking keys (RFC 6455 masking is against proxy cache poisoning on "
        "plain HTTP; these frames travel inside TLS)",
    "main/noise_control.cpp":
        "make_uuid() request ids, and WebSocket masking keys inside the Noise session",
    "components/muse/muse_power_test.c": "the power test's boot seed: no secret, no network",
    "main/pipeline_bench.cpp": "the data-path bench's test data and throwaway keys (CONFIG_HOMEHUB_PIPELINE_BENCH)",
    "components/secure_random/secure_random.c": "the helper itself, behind the radio check",
    "components/muse_apps/muse_apps_sys.c": "a WebAssembly app's sys_random(): a number for the app's own use, no secret",
    "components/muse_apps/muse_apps_vm.c": "an app request's x-request-id: a name, sent inside the Noise session",
    "components_mosaico/muse_mosaico_iris/muse_ptest_iris.c": "the power test's boot id: no secret, no network",
    "main/avatar_sync.c": "the avatar fetch's retry jitter: timing only",
}

# Files that hold security draws: none of them may call the RNG directly.
SECURITY_FILES = [
    "main/link_pairing.c",
    "components/muse_gadget_ble/mg_ble.c",
    "components/muse_gadget_ble/mg_proto.c",
    "components/muse_gadget_ble/mg_unlock.c",
    "components/muse_gadget_ble/mg_token_proof.c",
    "components/muse_gadget_ble/mg_session.c",
    "components/muse_gadget_ble/mg_seal_crypto.c",
    "components/muse/muse_ble.c",
    "components/muse/muse_passcode_esp.c",   # the passcode's salt
    "components/noise_core/src/PsaCryptoBackend.cpp",
    "../portable/mg_device_session/src/mg_session_seal.c",
    "../portable/mg_device_session/src/mg_device_session.c",
]

# Third-party code the scan skips (none of it calls the ESP-IDF RNG).
THIRD_PARTY = ("components/libvpx_vp8/src/", "components/tinyh264/src/", "components/minimp3/",
               "components/liblc3/", "components/libsbc/")

RNG_CALL = re.compile(r"\besp_(?:fill_random|random)\s*\(")


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def _firmware_sources():
    for top in ("main", "components", "components_mosaico"):
        base = ROOT / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            rel = path.relative_to(ROOT).as_posix()
            if path.suffix not in (".c", ".cpp", ".h", ".hpp") or rel.startswith(THIRD_PARTY):
                continue
            yield rel, path


def _function_body(src: str, signature: str) -> str:
    start = src.index(signature)
    brace = src.index("{", start)
    depth = 0
    for i in range(brace, len(src)):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[brace:i + 1]
    raise AssertionError(signature)


class Helper(unittest.TestCase):
    """secure_random.c on the host: the BT controller and Wi-Fi faked."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cc = shlex.split(os.environ.get("CC", "cc"))
        cls.exe = {}
        for name, flags in (("wifi", ["-DCONFIG_ESP_WIFI_ENABLED=1", "-DWITH_WIFI_FAKE=1"]),
                            # BLE-only: Wi-Fi exists on the chip but isn't linked, and the
                            # harness leaves esp_wifi_get_max_tx_power() out, so naming it fails the link.
                            ("ble_only", ["-DCONFIG_ESP_WIFI_ENABLED=1", "-DCONFIG_MUSE_GADGET_BLE_STANDALONE=1"])):
            exe = Path(cls.tmp.name) / name
            subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-DCONFIG_BT_CONTROLLER_ENABLED=1", *flags,
                                 "-I", str(TESTS / "secure_random_fakes" / "host"),
                                 "-I", str(TESTS / "secure_random_fakes"), "-I", str(COMP / "include"),
                                 str(TESTS / "secure_random_harness.c"), str(COMP / "secure_random.c"),
                                 "-o", str(exe)], check=True)
            cls.exe[name] = exe

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_lines(self, lines: list[str], build: str = "wifi") -> list[str]:
        r = run_child([str(self.exe[build])], input="\n".join(lines) + "\n", text=True,
                      capture_output=True, check=True)
        return r.stdout.splitlines()

    LOG = "E secure_random: no radio on: refusing a security draw"
    REFUSED = "draw 0 drew=0 zero=16 rng=0 untouched=1"
    DRAWN = "draw 1 drew=1 zero=0 rng=16 untouched=1"

    def test_no_radio_refuses_zeroed_logged_once(self) -> None:
        out = self.run_lines(["draw 16", "draw 16", "draw 16"])
        self.assertEqual(out, [self.LOG, self.REFUSED, self.REFUSED, self.REFUSED])

    def test_bt_enabled_draws(self) -> None:
        self.assertEqual(self.run_lines(["bt on", "draw 16", "draw 16"]), [self.DRAWN, self.DRAWN])

    def test_wifi_started_draws(self) -> None:
        self.assertEqual(self.run_lines(["wifi on", "draw 16"]), [self.DRAWN])

    def test_log_rearms_after_a_radio_spell(self) -> None:
        out = self.run_lines(["draw 16", "bt on", "draw 16", "bt off", "draw 16", "draw 16"])
        self.assertEqual(out, [self.LOG, self.REFUSED, self.DRAWN, self.LOG, self.REFUSED, self.REFUSED])

    def test_ble_only_never_asks_wifi(self) -> None:
        # Linked without the Wi-Fi fake at all; Wi-Fi "on" in the harness changes nothing.
        out = self.run_lines(["wifi on", "draw 16", "bt on", "draw 16"], build="ble_only")
        self.assertEqual(out, [self.LOG, self.REFUSED, self.DRAWN])

    def test_empty_draw(self) -> None:
        self.assertEqual(self.run_lines(["draw 0"]), ["draw 1 drew=0 zero=0 rng=0 untouched=1"])


class NoiseBackend(unittest.TestCase):
    """PsaCryptoBackend as the firmware builds it: its draws gated."""

    def test_draws_refused_without_a_radio(self) -> None:
        from test_noise_core import EMBEDDED_CXX_FLAGS, _cxx_command, _psa_crypto_flags
        cxx = _cxx_command()
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            flags = _psa_crypto_flags(cxx, tmp)
            exe = tmp / "noise_gate"
            r = subprocess.run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *EMBEDDED_CXX_FLAGS,
                                "-DNOISE_CORE_SECURE_RANDOM=1", "-I", str(NOISE / "include"),
                                "-I", str(COMP / "include"), str(TESTS / "secure_random_noise_harness.cpp"),
                                str(NOISE / "src" / "PsaCryptoBackend.cpp"), str(NOISE / "src" / "Status.cpp"),
                                *flags, "-o", str(exe)], text=True, capture_output=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("PASS noise draws gated", r.stdout)


class Inventory(unittest.TestCase):
    def test_inventory(self) -> None:
        found = {}
        for rel, path in _firmware_sources():
            if RNG_CALL.search(_strip_comments(path.read_text(errors="replace"))):
                found[rel] = True
        unlisted = sorted(set(found) - set(NOT_SECURITY))
        self.assertEqual(unlisted, [], "a direct RNG call: a security draw goes through secure_random(), "
                                       "anything else is listed in NOT_SECURITY with its reason")
        stale = sorted(set(NOT_SECURITY) - set(found))
        self.assertEqual(stale, [], "listed but no longer drawing: take it off NOT_SECURITY")

    def test_security_files_never_call_the_rng(self) -> None:
        for rel in SECURITY_FILES:
            src = _strip_comments((ROOT / rel).read_text())
            self.assertNotRegex(src, RNG_CALL, rel)
            self.assertNotIn("esp_random.h", src, rel)
        for rel in ("main/link_pairing.c", "components/muse_gadget_ble/mg_ble.c", "components/muse/muse_ble.c"):
            self.assertIn('#include "secure_random.h"', (ROOT / rel).read_text(), rel)

    def test_psa_and_drbg_draws(self) -> None:
        # PSA's draws (ESP-IDF's PSA RNG is esp_fill_random()) only in noise_core's backend, each after the gate.
        for rel, path in _firmware_sources():
            src = _strip_comments(path.read_text(errors="replace"))
            if re.search(r"\bpsa_generate_(?:random|key)\s*\(", src):
                self.assertEqual(rel, "components/noise_core/src/PsaCryptoBackend.cpp")
            if re.search(r"\bmbedtls_ctr_drbg_\w+\s*\(", src):
                # The host-only backend: not in the firmware's noise_core build.
                self.assertEqual(rel, "components/noise_core/src/MbedtlsCryptoBackend.cpp")
        self.assertNotIn("MbedtlsCryptoBackend.cpp", (NOISE / "CMakeLists.txt").read_text())
        psa = (NOISE / "src" / "PsaCryptoBackend.cpp").read_text()
        for sig, draw in (("Status PsaCryptoBackend::Random(", "psa_generate_random("),
                          ("Status PsaCryptoBackend::X25519GenerateKeypair(", "psa_generate_key(")):
            body = _function_body(psa, sig)
            self.assertLess(body.index("secure_random_ready()"), body.index(draw), sig)

    def test_phone_setup_passkey(self) -> None:
        src = (ROOT / "components" / "muse" / "muse_ble.c").read_text()
        i = src.index("case BLE_GAP_EVENT_PASSKEY_ACTION:")
        body = src[i:src.index("case BLE_GAP_EVENT_ENC_CHANGE:", i)]
        self.assertIn("if (!secure_random(&r, sizeof(r))) {", body)
        self.assertLess(body.index("ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_AUTH_FAIL);"),
                        body.index("ble_sm_inject_io("))


if __name__ == "__main__":
    unittest.main()
