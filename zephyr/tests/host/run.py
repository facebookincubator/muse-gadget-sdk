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

"""Host tests for the portable Link setup, token proof and device session
code (any OS).

Compiles src/mg_json.c, mg_psa.c, mg_setup_crypto.c, mg_setup_store.c,
mg_setup.c, mg_token_proof.c, mg_identity.c, mg_envelope.c,
mg_vm_session.c, mg_vm_link.c and mg_vm_commands.c with the host C compiler
against small Zephyr shims (tests/host/shim) and the host's PSA Crypto
library (mbedTLS 3.6 or 4.x / TF-PSA-Crypto, found with pkg-config
mbedcrypto; or set PSA_CFLAGS and PSA_LIBS), with AddressSanitizer and
UBSan, and runs them:

  test_setup_crypto  vectors: esp32/tests/vectors/link_pairing_v5.json
  test_json          the JSON reader and writer
  test_seal          the shared seal (portable/mg_device_session) on this app's
                     PSA glue, against protocols/test-vectors/mg-session-seal-v1.json
  test_setup         Link setup as the app drives it, storage, rollback,
                     boot recovery, timeouts, the device key and its VM
                     binding, a new setup ending the device session, and the
                     token proof against
                     protocols/test-vectors/mg-token-proof-v1.json
  test_setup_slot    test_setup again with the SDK token from the image's slot
                     (CONFIG_MG_SDK_TOKEN_SLOT)
  test_setup_board   ... and with the token from the board
                     (CONFIG_MG_SDK_TOKEN_BOARD), the board's setup-state hook
  test_button        the button's events (src/mg_button.c): an edge before the
                     core is up is dropped, then each reaches it in order
  test_gadget        the gadget basics: clock,
                     low battery, boot record, output hold
  test_smp_gate      CONFIG_MG_SMP_GATE_BOARD's gate (src/mg_smp_gate.c) on
                     every group, set up or not, SAFE, and the board's gate
  test_smp_req_gen   that build's own SMP reassembly wraps
                     (src/mg_smp_req_gen.c): a partial request from a
                     connection that's gone dropped, each request stamped with
                     its first fragment's connection
  test_resample      the 32 -> 24 kHz capture resampler: gain,
                     passband, alias rejection, blocks
  test_envelope      the session messages' decoder on bad input, by hand
  test_envelope_xcheck
                     the same against esp32/components/noise_core's
                     ServiceCodec.cpp (built with the host C++ compiler, CXX)
  test_vm_session    the device session against
                     protocols/test-vectors/mg-session-v1.json byte for byte,
                     the token update, retries, the window, link.invoke,
                     /link-control failures, the board's commands
  test_sealed_clear  Forget's clear only sealed (CONFIG_MG_SESSION_SEALED_CLEAR)
  signing_key        tools/check_signing_key.py on keys made with openssl

  python3 tests/host/run.py [-v]
"""

import json
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ZEPHYR = HERE.parents[1]
ROOT = ZEPHYR.parent
SRC = ZEPHYR / "src"


def psa_flags():
    if "PSA_CFLAGS" in os.environ or "PSA_LIBS" in os.environ:
        return shlex.split(os.environ.get("PSA_CFLAGS", "")), shlex.split(os.environ.get("PSA_LIBS", ""))
    try:
        cflags = subprocess.check_output(["pkg-config", "--cflags", "mbedcrypto"], text=True)
        libs = subprocess.check_output(["pkg-config", "--libs", "mbedcrypto"], text=True)
    except (OSError, subprocess.CalledProcessError):
        sys.exit("PSA Crypto not found: install mbedtls (brew install mbedtls / apt install "
                 "libmbedtls-dev) or set PSA_CFLAGS and PSA_LIBS")
    return shlex.split(cflags), shlex.split(libs)


def main():
    verbose = "-v" in sys.argv
    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    pc, pl = psa_flags()
    session_json = ROOT / "protocols/test-vectors/mg-session-v1.json"
    device_hello = json.loads(session_json.read_text())["device_hello"]
    # _GNU_SOURCE: memmem() on glibc.
    common = ["-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-O1", "-g",
              "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="mg-host-") as tmp:
        tmp = Path(tmp)
        subprocess.check_call([sys.executable, str(HERE / "gen_vectors.py"),
                               str(ROOT / "esp32/tests/vectors/link_pairing_v5.json"),
                               str(ROOT / "protocols/test-vectors/mg-token-proof-v1.json"),
                               str(tmp / "vectors.h")])
        subprocess.check_call([sys.executable, str(HERE / "gen_update_vectors.py"),
                               str(ROOT / "protocols/test-vectors/mg-update-v2.json"),
                               str(tmp / "update_vectors.h")])
        subprocess.check_call([sys.executable, str(HERE / "gen_session_vectors.py"),
                               str(session_json), str(tmp / "session_vectors.h")])
        subprocess.check_call([sys.executable, str(ZEPHYR / "tools" / "check_nus_words.py"),
                               str(SRC / "mg_nus.c"), str(SRC / "mg_nus_policy.h")])
        subprocess.check_call([sys.executable, str(ZEPHYR / "tools" / "check_owner_reads.py"), str(SRC)])
        subprocess.check_call([sys.executable, str(ZEPHYR / "tools" / "check_log_secrets.py"), str(SRC)])
        subprocess.check_call([sys.executable, str(ZEPHYR / "tools" / "check_button_pin.py"), str(SRC)])
        subprocess.check_call([sys.executable, str(HERE / "gen_sealed_data.py"),
                               str(ROOT / "protocols/test-vectors/mg-session-seal-v1.json"),
                               str(tmp / "sealed_data_vectors.h")])
        seal_dir = ROOT / "portable/mg_device_session"
        subprocess.check_call([sys.executable, str(seal_dir / "tests/gen_seal_vectors.py"),
                               str(ROOT / "protocols/test-vectors/mg-session-seal-v1.json"),
                               str(tmp / "seal_vectors.h")])
        inc = ["-I" + str(HERE / "shim"), "-I" + str(tmp), "-I" + str(SRC),
               "-I" + str(ROOT / "protocols"), "-I" + str(ROOT / "esp32/main"),
               # After src: this app's own mg_token_proof.h comes first.
               "-I" + str(seal_dir / "include"),
               "-I" + str(ROOT / "portable/mg_link_setup/include"),
               # mgls_crypto.h's PSA types, as the board builds it.
               "-DMGLS_CRYPTO_PSA",
               # The test hooks (fixed keys and nonces), never in firmware.
               "-DMG_HOST_TEST",
               "-DCONFIG_MG_LOG_LEVEL=3", '-DCONFIG_MG_SDK_TOKEN="mgst_host_test"',
               # The device session as the vectors' device: its hello's limits.
               "-DCONFIG_MG_VM_SESSION_MAX_MESSAGE=%d" % device_hello["max_message"],
               "-DCONFIG_MG_VM_SESSION_WINDOW=%d" % device_hello["window"],
               "-DCONFIG_MG_VM_SESSION_TX_QUEUE=4096",
               "-DCONFIG_MG_VM_LINK_CONTROL_MAX=2048", "-DCONFIG_MG_VM_LINK_COMMANDS_MAX=8",
               "-DCONFIG_MG_VM_LINK_HEARTBEAT_S=86400", "-DCONFIG_MG_VM_KEEPALIVE_MS=15000", "-DCONFIG_MG_VM_IDLE_CLOSE_MS=20000", "-DCONFIG_MG_VM_COMMAND_LED=1"]
        transcript = str(ROOT / "esp32/main/pairing_transcript.c")
        crypto = [str(SRC / "mg_setup_crypto.c"), str(SRC / "mg_psa.c")]
        # The vendored seal on this app's PSA Crypto (src/mg_seal_crypto.c).
        seal = [str(seal_dir / "src/mg_session_seal.c"), str(SRC / "mg_seal_crypto.c")]
        setup = [str(HERE / "host_zephyr.c"), str(SRC / "mg_setup.c"),
                 str(SRC / "mg_setup_store.c"), str(SRC / "mg_token_proof.c"),
                 str(SRC / "mg_json.c"), str(SRC / "mg_identity.c"), transcript] + crypto + seal
        tests = {
            "test_setup_crypto": [str(HERE / "test_setup_crypto.c"), transcript] + crypto,
            "test_json": [str(HERE / "test_json.c"), str(SRC / "mg_json.c")],
            # The shared seal's own vectors, on this app's crypto glue.
            "test_seal": [str(seal_dir / "tests/test_seal.c"), str(SRC / "mg_psa.c")] + seal,
            # With the device session, which a new setup's commit ends.
            "test_setup": [str(HERE / "test_setup.c"), str(SRC / "mg_vm_session.c"),
                           str(SRC / "mg_vm_link.c"), str(SRC / "mg_envelope.c")] + setup,
            "test_gadget": [str(HERE / "test_gadget.c"), str(HERE / "host_zephyr.c"),
                            str(SRC / "mg_clock.c"), str(SRC / "mg_power.c"),
                            str(SRC / "mg_led_pattern.c"), str(SRC / "mg_boot.c"), "-lm"],
            # CONFIG_MG_SMP_GATE_BOARD's gate through its MCUmgr callback.
            # The button's events from boot: none before the core is up.
            "test_button": [str(HERE / "test_button.c"), str(HERE / "host_zephyr.c"),
                            str(SRC / "mg_button.c")],
            "test_smp_gate": [str(HERE / "test_smp_gate.c"), str(HERE / "host_zephyr.c"),
                              str(SRC / "mg_smp_gate.c")],
            # ... and that build's reassembly wraps, in smp_bt's order.
            "test_smp_req_gen": [str(HERE / "test_smp_req_gen.c"), str(HERE / "host_zephyr.c"),
                                 str(SRC / "mg_smp_req_gen.c")],
            "test_resample": [str(HERE / "test_resample.c"), str(SRC / "mg_resample.c"), "-lm"],
            "test_envelope": [str(HERE / "test_envelope.c"), str(SRC / "mg_envelope.c")],
            "test_vm_session": [str(HERE / "test_vm_session.c"), str(SRC / "mg_vm_session.c"),
                                str(SRC / "mg_vm_link.c"), str(SRC / "mg_vm_commands.c"),
                                str(SRC / "mg_envelope.c")] + setup,
        }
        noise_core = ROOT / "esp32/components/noise_core"
        # ServiceCodec.cpp itself, beside mg_envelope.c (C compiled as C).
        xcheck_obj = tmp / "mg_envelope.o"
        cxx_ok = subprocess.run(cc + common + inc + ["-c", str(SRC / "mg_envelope.c"), "-o",
                                                     str(xcheck_obj)],
                                capture_output=True, text=True).returncode == 0
        failed = 0
        # The session's own build: mg_setup.c and mg_token_proof.c end it.
        # Link setup again with the SDK token from the image's slot
        # (CONFIG_MG_SDK_TOKEN_SLOT): empty, filled, longest, malformed.
        tests["test_setup_slot"] = tests["test_setup"]
        # ... and with the token from the board (CONFIG_MG_SDK_TOKEN_BOARD).
        tests["test_setup_board"] = tests["test_setup"]
        # Forget's clear only sealed (CONFIG_MG_SESSION_SEALED_CLEAR, off by default).
        tests["test_sealed_clear"] = tests["test_vm_session"]
        extra = {"test_smp_req_gen": ["-DCONFIG_MCUMGR_TRANSPORT_REASSEMBLY=1",
                                      "-DCONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE=24",
                                      "-DCONFIG_MCUMGR_TRANSPORT_NETBUF_MIN_USER_DATA_SIZE=8"],
                 "test_vm_session": ["-DCONFIG_MG_VM_SESSION=1",
                                     '-DCONFIG_MG_DEVICE_PRODUCT_CODE="XN54"',
                                     '-DCONFIG_BOARD="xiao_nrf54l15"'],
                 "test_sealed_clear": ["-DCONFIG_MG_VM_SESSION=1", "-DCONFIG_MG_SESSION_SEALED_CLEAR=1",
                                       '-DCONFIG_MG_DEVICE_PRODUCT_CODE="XN54"',
                                       '-DCONFIG_BOARD="xiao_nrf54l15"',
                                       "-Wno-unused-function"],  # runs only its own test
                 "test_setup": ["-DCONFIG_MG_VM_SESSION=1"],
                 "test_setup_slot": ["-DCONFIG_MG_VM_SESSION=1", "-DCONFIG_MG_SDK_TOKEN_SLOT=1"],
                 "test_setup_board": ["-DCONFIG_MG_VM_SESSION=1", "-DCONFIG_MG_SDK_TOKEN_BOARD=1"]}
        builds = {name: cc + common + inc + extra.get(name, []) + pc + srcs + pl
                  for name, srcs in tests.items()}
        builds["test_envelope_xcheck"] = cxx + [
            "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer",
            "-I" + str(SRC), "-I" + str(noise_core / "include"),
            str(HERE / "test_envelope_xcheck.cpp"), str(noise_core / "src/ServiceCodec.cpp"),
            str(noise_core / "src/Status.cpp"), str(xcheck_obj)]
        for name, cmd in builds.items():
            exe = tmp / name
            if name == "test_envelope_xcheck" and not cxx_ok:
                print(f"{name}: BUILD FAILED (mg_envelope.c)")
                failed += 1
                continue
            build = subprocess.run(cmd + ["-o", str(exe)], capture_output=True, text=True)
            if build.returncode:
                print(f"{name}: BUILD FAILED\n{build.stdout}{build.stderr}")
                failed += 1
                continue
            env = dict(os.environ, HOST_VERBOSE="1") if verbose else os.environ
            run = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
            out = (run.stdout + run.stderr).strip()
            last = out.splitlines()[-1] if out else ""
            print(f"{name}: {'ok' if run.returncode == 0 else 'FAILED'} ({last})")
            if run.returncode or verbose:
                print(out)
            failed += run.returncode != 0
        failed += window_check(cc + common + pc, inc, tmp, device_hello["window"])
        failed += signing_key_check()
        failed += image_id_check()
        sys.exit(1 if failed else 0)


def window_check(cc, inc, tmp, vectors_window):
    """CONFIG_MG_VM_SESSION_WINDOW: the device's hello is always sealed, and a
    sealed hello's window is at least 4 (mgcommands.h, Opening), so a window
    of 1 to 3 doesn't build (mg_vm_session.c's #error); 4 and the vectors' do."""
    flags = [f for f in inc if not f.startswith("-DCONFIG_MG_VM_SESSION_WINDOW=")]
    bad = []
    for w, want in ((1, False), (3, False), (4, True), (vectors_window, True)):
        r = subprocess.run(cc + flags + ["-DCONFIG_MG_VM_SESSION=1",
                                         "-DCONFIG_MG_VM_SESSION_WINDOW=%d" % w, "-c",
                                         str(SRC / "mg_vm_session.c"), "-o",
                                         str(tmp / "window.o")],
                           capture_output=True, text=True)
        built = r.returncode == 0
        if built != want or (not want and "CONFIG_MG_VM_SESSION_WINDOW must be 4" not in r.stderr):
            bad.append(f"window {w}: {'built' if built else 'refused'}")
    print(f"window: {'ok' if not bad else 'FAILED'} (4 windows{': ' + '; '.join(bad) if bad else ''})")
    return 1 if bad else 0


def signing_key_check():
    """tools/check_signing_key.py: a build signed with one
    of MCUboot's public development keys (in its tree, or a copy elsewhere,
    re-encoded too) fails unless it's a development build; a private key, or
    the dev key with --dev-ok, passes; no key, a missing one, one that doesn't
    parse or an MCUboot directory that can't be read fails. Keys made with
    openssl."""
    import shutil
    import subprocess
    import tempfile
    tool = HERE.parent.parent / "tools" / "check_signing_key.py"
    d = Path(tempfile.mkdtemp())
    try:
        mcuboot = d / "mcuboot"
        mcuboot.mkdir()
        dev = mcuboot / "root-ec-p256.pem"
        mine = d / "mine.pem"
        copy = d / "copied.pem"
        for k in (dev, mine):
            subprocess.run(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt",
                            "ec_paramgen_curve:P-256", "-out", str(k)],
                           check=True, capture_output=True)
        # The same key in another encoding (PKCS#8 or SEC1, whichever it isn't).
        subprocess.run(["openssl", "pkey", "-in", str(dev), "-out", str(copy)],
                       check=True, capture_output=True)
        if copy.read_bytes() == dev.read_bytes():
            subprocess.run(["openssl", "ec", "-in", str(dev), "-out", str(copy)],
                           check=True, capture_output=True)
        junk = d / "junk.pem"
        junk.write_text("-----BEGIN EC PRIVATE KEY-----\nnot-a-key\n-----END EC PRIVATE KEY-----\n")
        secret = [k.read_text().splitlines()[1] for k in (dev, mine)]
        cases = [((dev,), 1), ((dev, "--dev-ok"), 0), ((copy,), 1), ((mine,), 0),
                 (("",), 1), ((d / "missing.pem",), 1), ((mine, "--dev-ok"), 0),
                 ((junk,), 1), ((mine, "--mcuboot", str(d / "none")), 1)]
        # A blocklist file (--blocklist: public-key sha256 per line): a real
        # key listed there by its public half counts as public. The list is
        # this test's own, made here.
        listed = d / "listed.pem"
        try:
            made = subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey",
                                   "-noout", "-out", str(listed)],
                                  capture_output=True).returncode == 0
        except OSError:
            made = False
        if made:
            der = subprocess.run(["openssl", "pkey", "-in", str(listed), "-pubout", "-outform",
                                  "DER"], capture_output=True).stdout
            import hashlib
            bl = d / "blocklist.sha256"
            bl.write_text("# test\n%s  test-key  test\n" % hashlib.sha256(der).hexdigest())
            cases += [((listed,), 0), ((listed, "--blocklist", bl), 1),
                      ((listed, "--blocklist", bl, "--dev-ok"), 0), ((mine, "--blocklist", bl), 0)]
        # No openssl, no blocklist rows: a failure, never a silent skip.
        bad = [] if made else ["blocklist rows: openssl couldn't make a key"]
        for args, want in cases:
            rest = [str(x) for x in args[1:]]
            cmd = [sys.executable, str(tool), "--key", str(args[0])]
            cmd += [] if "--mcuboot" in rest else ["--mcuboot", str(mcuboot)]
            cmd += rest
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != want or any(x in r.stdout + r.stderr for x in secret):
                bad.append(f"{args}: rc {r.returncode}, want {want}")
    finally:
        shutil.rmtree(d)
    print(f"signing_key: {'ok' if not bad else 'FAILED'} ({len(cases)} cases"
          f"{': ' + '; '.join(bad) if bad else ''})")
    return 1 if bad else 0


def image_id_check():
    """cmake/mg_image_id.cmake: the plain, token, bench and dirty builds of a
    commit get different MCUboot image ids, each variant rises with N, the
    variant is read from every way an option reaches the image and held to
    the configuration after Kconfig (tests/host/image_id_check.cmake)."""
    import shutil
    import subprocess
    import tempfile
    cmake = shutil.which("cmake")
    if not cmake:
        print("image_id: FAILED (no cmake)")
        return 1
    d = tempfile.mkdtemp(prefix="mg-image-id-")
    try:
        r = subprocess.run([cmake, "-DTMP=" + d, "-P", str(HERE / "image_id_check.cmake")],
                           capture_output=True, text=True)
    finally:
        shutil.rmtree(d)
    ok = r.returncode == 0 and "image_id: ok" in (r.stdout + r.stderr)
    print("image_id: ok" if ok else f"image_id: FAILED {r.stdout}{r.stderr}")
    return 0 if ok else 1


if __name__ == "__main__":
    main()
