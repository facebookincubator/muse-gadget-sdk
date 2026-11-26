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
"""mg-session-seal-v1.json -> seal_vectors.h for test_seal.c.

    gen_seal_vectors.py VECTORS.json OUT.h
"""
import json
import sys


def c_str(s: str) -> str:
    assert all(32 <= ord(c) < 127 for c in s), "ASCII only"
    return json.dumps(s)


def main() -> int:
    src, out = sys.argv[1], sys.argv[2]
    v = json.load(open(src, encoding="utf-8"))
    rows = []
    for x in v["vectors"]:
        rows.append("{%s, %s, %s, %s, %s, %s, %s, %s, %s, %uu, %uu}" % (
            c_str(x["name"]), c_str(x["K"]), c_str(x["client_nonce"]), c_str(x["device_nonce"]),
            c_str(x["node_id"]), c_str(x["enc_key"]), c_str(x["mac_key"]), c_str(x["sealed"]),
            c_str(x["plaintext"]), x["direction"], x["counter"]))
    # sealed_data: every sealed message of the session, both directions; an older
    # copy of the vectors without it gives an empty table.
    def steps_of(section):
        rows_ = []
        for s in section["steps"] if section else []:
            if s["sealed"]:
                rows_.append("{%s, %s, %uu, %uu, %s, %u}" % (c_str(s["step"]), c_str(s["sealed"]), s["direction"],
                                                             s["counter"], c_str(s["plaintext"]),
                                                             1 if s["outcome"] == "refused" else 0))
        return rows_
    d = v.get("sealed_data")
    data = steps_of(d)
    # sealed_data_large: the same with the device's sealed credentials_saved.
    dl = v.get("sealed_data_large")
    data_large = steps_of(dl)
    # sealed_interleaved (two messages in flight): the client's messages reassembled from its frames
    # per sub-command (a credentials_update and a data message in flight together), each with its counter and whether
    # the receiver refused it; the layer-1 check is the same as sealed_data's (a valid seal opens to its plaintext, a
    # bad tag doesn't open).
    il = v.get("sealed_interleaved")
    inter = []
    if il:
        bufs = {}
        for s in il["steps"]:
            for fr in s["frames"]:
                b = bytes.fromhex(fr)
                sub, flags = b[1], b[2]
                if flags & 1:
                    bufs[sub] = bytearray(b[7:])
                else:
                    bufs.setdefault(sub, bytearray()).extend(b[3:])
                if flags & 2:
                    sealed = bytes(bufs.pop(sub))
                    if s["outcome"] in ("message", "refused") and int.from_bytes(sealed[:4], "big") == s["counter"]:
                        inter.append("{%s, %s, %uu, %uu, %s, %u}" % (c_str(s["step"]), c_str(sealed.hex()),
                                                                     s["direction"], s["counter"],
                                                                     c_str(s["plaintext"]),
                                                                     1 if s["outcome"] == "refused" else 0))
    # sealed_credentials_saved: every sealed answer of the section (counter in its
    # first 4 bytes), the refresh_token of the update it answers, and whether the client counts it as saved; an older
    # copy without it gives an empty table.
    sv = v.get("sealed_credentials_saved")
    saved = []
    if sv:
        for s in sv["steps"][1:]:
            refresh = json.loads(s["update_plaintext"])["refresh_token"]
            for fr, verdict in zip(s["device"], s["verdicts"]):
                b = bytes.fromhex(fr)
                if len(b) > 3:      # a sealed answer: [session, saved, flags, total_len, sealed bytes]
                    saved.append("{%s, %s, %s, %u}" % (c_str(s["step"]), c_str(b[7:].hex()), c_str(refresh),
                                                       1 if verdict == "saved" else 0))
    with open(out, "w", encoding="utf-8") as f:
        f.write("/* Generated from %s by gen_seal_vectors.py. */\n" % src.split("/")[-1])
        f.write("static const vec_t VECTORS[] = {\n\t" + ",\n\t".join(rows) + "\n};\n")
        f.write("#define DATA_K %s\n#define DATA_CN %s\n#define DATA_DN %s\n#define DATA_NODE %s\n" % (
            (c_str(d["K"]), c_str(d["client_nonce"]), c_str(d["device_nonce"]), c_str(d["node_id"])) if d
            else ('""', '""', '""', '""')))
        f.write("#define N_DATA_STEPS %uu\n" % len(data))
        f.write("static const data_t DATA_STEPS[] = {\n\t" + (",\n\t".join(data) if data else '{"", "", 0u, 0u, "", 0}')
                + "\n};\n")
        f.write("#define LARGE_K %s\n#define LARGE_CN %s\n#define LARGE_DN %s\n#define LARGE_NODE %s\n" % (
            (c_str(dl["K"]), c_str(dl["client_nonce"]), c_str(dl["device_nonce"]), c_str(dl["node_id"])) if dl
            else ('""', '""', '""', '""')))
        f.write("#define N_LARGE_STEPS %uu\n" % len(data_large))
        f.write("static const data_t LARGE_STEPS[] = {\n\t" + (",\n\t".join(data_large) if data_large
                                                             else '{"", "", 0u, 0u, "", 0}') + "\n};\n")
        f.write("#define IL_K %s\n#define IL_CN %s\n#define IL_DN %s\n#define IL_NODE %s\n" % (
            (c_str(il["K"]), c_str(il["client_nonce"]), c_str(il["device_nonce"]), c_str(il["node_id"])) if il
            else ('""', '""', '""', '""')))
        f.write("#define N_IL_STEPS %uu\n" % len(inter))
        f.write("static const data_t IL_STEPS[] = {\n\t" + (",\n\t".join(inter) if inter else '{"", "", 0u, 0u, "", 0}')
                + "\n};\n")
        f.write("#define SV_K %s\n#define SV_CN %s\n#define SV_DN %s\n#define SV_NODE %s\n" % (
            (c_str(sv["K"]), c_str(sv["client_nonce"]), c_str(sv["device_nonce"]), c_str(sv["node_id"])) if sv
            else ('""', '""', '""', '""')))
        f.write("#define N_SAVED %uu\n" % len(saved))
        f.write("static const saved_t SAVED[] = {\n\t" + (",\n\t".join(saved) if saved else '{"", "", "", 0}')
                + "\n};\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
