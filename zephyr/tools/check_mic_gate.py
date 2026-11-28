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
"""Post-link check: the mic's token-proof gate is linked into the image, and a
release image carries no path around it over a wireless link (a build option
or a missing file must never compile the gate out unnoticed).

  check_mic_gate.py --nm NM --elf zephyr.elf --bin zephyr.bin [--release]

Every image: mg_core_output_ok() linked (the gate start_mic, the capture's
output and the press's turn ask) and start_mic's proof_required refusal linked.
--release (CONFIG_MG_AUTOMATION off): additionally the wireless bench switch's
release lock (mg_core_ble_bench_locked) linked, the bench switch's press path
not linked, and no memory reader over a wireless link (MCUmgr shell, fs,
settings or stat groups, a shell on the Nordic UART, a log backend or MCUmgr's
log group) linked. The USB bench console (every build here) is wired, the
owner's, and allowed. Fails (exit 1) on any miss.
"""
import argparse
import subprocess
import sys

GATE_SYMBOLS = ("mg_core_output_ok", "mg_token_proof_matched")
GATE_STRINGS = (b"start_mic before the token proof\0",)
# The bench switch's release lock, by its own symbol (not a log string a
# lower log level compiles out).
RELEASE_SYMBOLS = ("mg_core_ble_bench_locked",)
# A CONFIG_MG_BENCH_MODE image carries this symbol; --release refuses it.
BENCH_MODE_MARKER = "mg_bench_mode_image"
# The bench switch's press path: a release build takes it over BLE with the
# token proof only.
RELEASE_ABSENT = ("mg_core_press_window_open",)
# Raw memory readers count as mic output: no MCUmgr group or shell that reads
# memory, files or settings over a wireless link in a release image.
RELEASE_ABSENT_PREFIXES = ("shell_mgmt_", "fs_mgmt_", "settings_mgmt_", "stat_mgmt_",
                           "shell_backend_bt", "shell_bt_nus", "bt_nus_shell",
                           # logging over a wireless link runs before every gate
                           "log_backend_ble", "log_mgmt_")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--release", action="store_true")
    # A release-configured image built with CONFIG_MG_BENCH_MODE: the release gates are
    # checked, the bench paths are allowed, and the image is never a release (the release
    # tooling runs --release alone, which refuses it).
    ap.add_argument("--bench-mode", action="store_true")
    a = ap.parse_args()
    out = subprocess.run([a.nm, a.elf], capture_output=True, text=True, check=True).stdout
    text = {ln.split()[-1] for ln in out.splitlines() if len(ln.split()) >= 3 and ln.split()[1] in "Tt"}
    syms = {ln.split()[-1] for ln in out.splitlines() if len(ln.split()) >= 3}
    img = open(a.bin, "rb").read()
    bad = []
    for s in GATE_SYMBOLS:
        if s not in text:
            bad.append(f"the mic gate's {s}() isn't linked")
    for s in GATE_STRINGS:
        if s not in img:
            bad.append(f"start_mic's proof refusal isn't in the image ({s[:-1].decode()!r})")
    if a.release:
        for s in RELEASE_SYMBOLS:
            if s not in text:
                bad.append(f"the bench switch's release lock {s}() isn't linked")
        if BENCH_MODE_MARKER in syms and not a.bench_mode:
            bad.append("a bench-mode image (CONFIG_MG_BENCH_MODE, recovery reason bench): it "
                       "never ships")
        for s in RELEASE_ABSENT:
            if s in text and not a.bench_mode:
                bad.append(f"a release image links {s}() (a bench path around the mic gate)")
        for name in sorted(text):
            if name.startswith(RELEASE_ABSENT_PREFIXES):
                bad.append(f"a release image links {name}() (a memory reader over a wireless link)")
    if bad:
        print("check_mic_gate: refused:", file=sys.stderr)
        for b in bad:
            print("  " + b, file=sys.stderr)
        return 1
    print("check_mic_gate: the mic's proof gate is linked" +
          ("; bench mode: never a release" if a.bench_mode
           else "; no bench path around it (release)" if a.release else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
