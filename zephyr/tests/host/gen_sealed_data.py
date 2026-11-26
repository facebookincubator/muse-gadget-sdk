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
"""The sealed_data and sealed_interleaved exchanges of mg-session-seal-v1.json
as a C header for test_vm_session (SD_ and SI_): keys'
inputs and each step's frames, counter, plaintext, sealed bytes, outcome and
answer."""

import json
import sys


def arr(name, hx):
    b = bytes.fromhex(hx or "")
    body = ", ".join("0x%02x" % x for x in b) or "0"
    return "static const uint8_t %s[] = {%s};\n" % (name, body), len(b)


def emit(d, px, out):
    """One exchange (sealed_data as SD_, sealed_interleaved as SI_): keys' inputs, each step's frames and fields."""
    for k in ("K", "client_nonce", "device_nonce"):
        out.append(arr(px + "_" + k.upper(), d[k])[0])
    out.append('#define %s_NODE_ID "%s"\n#define %s_MAX_WRITE %d\n' % (px, d["node_id"], px, d["max_write"]))
    steps = []
    for i, s in enumerate(d["steps"]):
        fr = []
        for j, f in enumerate(s.get("frames", [])):
            c, n = arr("%s_S%d_F%d" % (px, i, j), f)
            out.append(c)
            fr.append("{%s_S%d_F%d, %d}" % (px, i, j, n))
        out.append("static const struct sd_frame %s_S%d_FRAMES[] = {%s};\n" % (px, i, ", ".join(fr) or "{0}"))
        c, pn = arr("%s_S%d_PT" % (px, i), s.get("plaintext"))
        out.append(c)
        c, sn = arr("%s_S%d_SEALED" % (px, i), s.get("sealed"))
        out.append(c)
        c, an = arr("%s_S%d_ANSWER" % (px, i), s.get("answer"))
        out.append(c)
        ctr = s.get("counter")
        steps.append('{"%s", %d, %d, %s_S%d_FRAMES, %d, %s_S%d_PT, %d, %s_S%d_SEALED, %d, "%s", %s_S%d_ANSWER, %d}'
                     % (s["step"].replace('"', "'"), s["direction"], -1 if ctr is None else ctr, px, i,
                        len(fr), px, i, pn, px, i, sn, s["outcome"], px, i, an))
    out.append("static const struct sd_step %s_STEPS[] = {\n  " % px + ",\n  ".join(steps) + "\n};\n")
    out.append("#define %s_NSTEPS (sizeof(%s_STEPS) / sizeof(%s_STEPS[0]))\n" % (px, px, px))


def main():
    v = json.load(open(sys.argv[1]))
    out = ["/* Generated from mg-session-seal-v1.json sealed_data and sealed_interleaved. */\n"
           "#include <stdint.h>\n#include <stddef.h>\n",
           "struct sd_frame { const uint8_t *p; size_t n; };\n"
           "struct sd_step { const char *name; int dir; long counter; const struct sd_frame *frames; size_t nframes;\n"
           "  const uint8_t *pt; size_t pt_n; const uint8_t *sealed; size_t sealed_n; const char *outcome;\n"
           "  const uint8_t *answer; size_t answer_n; };\n"]
    emit(v["sealed_data"], "SD", out)
    emit(v["sealed_interleaved"], "SI", out)   # two sealed messages in flight
    emit(v["sealed_data_large"], "SLARGE", out)    # credentials_saved sealed, on the shared counter
    # Sealed credentials_saved (SC_): the steps whose device answer is one sealed frame, with
    # the client's update frames, its counter, and the device's frame byte for byte.
    sc = v["sealed_credentials_saved"]
    for k in ("K", "client_nonce", "device_nonce"):
        out.append(arr("SC_" + k.upper(), sc[k])[0])
    out.append('#define SC_NODE_ID "%s"\n' % sc["node_id"])
    steps = []
    for i, st in enumerate(sc["steps"][1:]):
        dev = st["device"]
        if len(dev) != 1 or len(bytes.fromhex(dev[0])) <= 3 or \
                not (st["step"].startswith("saved:") or st["step"].startswith("not saved:")):
            continue  # client-side verdict cases (an unsealed form, another pair's hash)
        fr = []
        for j, f in enumerate(st["client"]):
            c, n = arr("SC_S%d_F%d" % (i, j), f)
            out.append(c)
            fr.append("{SC_S%d_F%d, %d}" % (i, j, n))
        out.append("static const struct sd_frame SC_S%d_FRAMES[] = {%s};\n" % (i, ", ".join(fr)))
        c, an = arr("SC_S%d_ANSWER" % i, dev[0])
        out.append(c)
        steps.append('{"%s", SC_S%d_FRAMES, %d, SC_S%d_ANSWER, %d, %d}'
                     % (st["step"].replace('"', "'"), i, len(fr), i, an, 1 if st["verdicts"] == ["saved"] else 0))
    out.append("struct sc_step { const char *name; const struct sd_frame *frames; size_t nframes;\n"
               "  const uint8_t *answer; size_t answer_n; int saved; };\n")
    out.append("static const struct sc_step SC_STEPS[] = {\n  " + ",\n  ".join(steps) + "\n};\n")
    out.append("#define SC_NSTEPS (sizeof(SC_STEPS) / sizeof(SC_STEPS[0]))\n")
    open(sys.argv[2], "w").write("".join(out))


if __name__ == "__main__":
    main()
