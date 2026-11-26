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

"""The device session (mg_command_session, protocols/mgcommands.h, Device
session) on the host, against protocols/test-vectors/mg-session-v1.json.

Two levels:

- The BLE side (components/muse_gadget_ble/mg_session.c through mg_proto.c):
  what the vectors don't reach: a full BLE stack, the window, disconnects,
  the JSON both ways.
- The whole gadget, byte for byte: every exchange of the vectors through
  mg_proto.c and mg_session.c and main/noise_control.cpp's own link code
  (link.register, link.invoke, link.result, request streams, compiled from
  the source as the other noise_control tests do) over the device session, and the same senders
  over Wi-Fi's ClientSession, which must frame the same envelopes.
"""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_mg_ble import MG, PROTOCOLS, ROOT, SEAL_INCS, SEAL_SRCS, codec_build, compile_and_run  # noqa: E402

VECTORS = PROTOCOLS / "test-vectors/mg-session-v1.json"
SEAL_VECTORS = PROTOCOLS / "test-vectors/mg-session-seal-v1.json"
PROOF_VECTORS = PROTOCOLS / "test-vectors/mg-token-proof-v1.json"
CONTROL = ROOT / "main/noise_control.cpp"
CJSON = ROOT / "managed_components/espressif__cjson/cJSON"


def reference():
    """protocols/test-vectors/mg_session_ref.py, the reference device."""
    sys.path.insert(0, str(PROTOCOLS / "test-vectors"))
    import mg_session_ref  # noqa: E402
    return mg_session_ref


def c_str(data: bytes) -> str:
    return '"' + "".join(f"\\x{b:02x}" for b in data) + '"'


def c_hex(data: bytes) -> str:
    return '"' + data.hex() + '"'


def recorded_exchanges() -> list[dict]:
    """Each exchange of the vectors with, per step, the messages the reference
    device queued to the VM during that write (what the session code above
    mg_session hands it)."""
    ref = reference()

    class Log(list):
        def __init__(self, log):
            super().__init__()
            self.log = log

        def append(self, item):
            self.log.append(item)
            super().append(item)

    class Recording(ref.Device):
        def __init__(self, mtu, **flags):
            self.log = []
            super().__init__(mtu, **flags)

        def reset_session(self):
            super().reset_session()
            self.outq = Log(self.log)

    data = json.loads(VECTORS.read_text(encoding="utf-8"))
    k = bytes.fromhex(data["session_seal"]["K"])
    out = []
    for ex in data["exchanges"]:
        if any(st.get("write", "").startswith("%02x%02x" % (0x89, 11)) for st in ex["steps"]):
            # mg_session_wake: a client sends it only to a device that declares mg_session_cap_wake
            # (mgcommands.h), which this one doesn't.
            continue
        dev = Recording(ex["mtu"], **ex["device"])
        steps = []
        for step in ex["steps"]:
            dev.log.clear()
            if step.get("event") == "proof":
                # A new matching token proof (the session reset at its challenge): its nonces key what follows.
                cn, dn = bytes.fromhex(step["client_nonce"]), bytes.fromhex(step["device_nonce"])
                got = [f.hex() for f in dev.new_proof(k, cn, dn)]
                assert got == step["notify"], f"the reference disagrees with its vectors: {ex['name']}"
                steps.append({"write": b"", "proof": (cn, dn), "notify": [bytes.fromhex(h) for h in step["notify"]],
                              "send": list(dev.log), "task": -1})
                continue
            if step.get("event") == "counters":
                # A stand-in for a long session: the device's next counters jump.
                dev.rx, dev.tx = step.get("rx", dev.rx), step.get("tx", dev.tx)
                steps.append({"write": b"", "proof": None, "notify": [], "send": [], "task": -1,
                              "counters": (step.get("rx", -1), step.get("tx", -1))})
                continue
            if step.get("event") == "nudge":
                # The device's keepalive (mg_session_nudge): the credit it owes, or credit 0 when it owes none.
                got = [f.hex() for f in dev.nudge()]
                assert got == step["notify"], f"the reference disagrees with its vectors: {ex['name']}"
                steps.append({"write": b"", "proof": None, "notify": [bytes.fromhex(h) for h in step["notify"]],
                              "send": list(dev.log), "task": -1, "nudge": True})
                continue
            if step.get("event") == "buffer":
                # The board's stack has room again (mg_proto_session_pump): credentials that waited go out.
                got = [f.hex() for f in dev.buffer()]
                assert got == step["notify"], f"the reference disagrees with its vectors: {ex['name']}"
                steps.append({"write": b"", "proof": None, "notify": [bytes.fromhex(h) for h in step["notify"]],
                              "send": list(dev.log), "task": -1, "buffer": True})
                continue
            assert "event" not in step or step["event"] == "task", \
                f"mg-session-v1.json: an event this replay doesn't know: {step['event']!r} in {ex['name']!r}"
            if step.get("event") == "task":
                # A task starts or ends on the device: it asks for a session only while one lasts.
                got = [f.hex() for f in dev.set_task(step["active"])]
                assert got == step["notify"], f"the reference disagrees with its vectors: {ex['name']}"
                steps.append({"write": b"", "proof": None, "notify": [bytes.fromhex(h) for h in step["notify"]],
                              "send": list(dev.log), "task": int(step["active"])})
                continue
            frame = bytes.fromhex(step["write"])
            got = [f.hex() for f in dev.write(frame)]
            assert got == step["notify"], f"the reference disagrees with its vectors: {ex['name']}"
            steps.append({"write": frame, "proof": None, "notify": [bytes.fromhex(h) for h in step["notify"]],
                          "send": list(dev.log), "task": -1})
        flags = {"holds_token": True, "proof": True, "encrypted": True, "vm_binding": False, "task": False,
                 "credentials_wait": False, **ex["device"]}
        out.append({"name": ex["name"], "mtu": ex["mtu"], "flags": flags, "steps": steps})
    return out


def json_cases() -> list[tuple[bytes, bool, str, str, int]]:
    """credentials_update bodies and what a JSON reader makes of them (the spec's rule: an object
    with non-empty string access_token and refresh_token, '{' its first byte)."""
    texts = [
        '{"access_token":"a","refresh_token":"r"}',
        '{"refresh_token" : "r2" ,\n "access_token":"a2", "extra":2.5e-3,"n":null,"t":true,"f":false} ',
        # Leading whitespace: refused (mgcommands.h).
        ' {"refresh_token" : "r2" ,\n "access_token":"a2", "extra":2.5e-3,"n":null,"t":true,"f":false} ',
        '\n{"access_token":"a","refresh_token":"r"}',
        # Unknown flat members ignored (the apps send access_expires_at, a number).
        '{"access_token":"a3","refresh_token":"r3","access_expires_at":1793860000,"note":"x"}',
        '{"access_expires_at":1793860000,"refresh_token":"r4","access_token":"a4"}',
        # Its forms (layer 2's rule): an unsigned integer that fits 64 bits is
        # kept; anything else is none; a duplicate is refused.
        '{"access_token":"a","refresh_token":"r","access_expires_at":18446744073709551615}',
        '{"access_token":"a","refresh_token":"r","access_expires_at":18446744073709551616}',
        '{"access_token":"a","refresh_token":"r","access_expires_at":0}',
        '{"access_token":"a","refresh_token":"r","access_expires_at":-5}',
        '{"access_token":"a","refresh_token":"r","access_expires_at":1.5e9}',
        '{"access_token":"a","refresh_token":"r","access_expires_at":"1793860000"}',
        '{"access_token":"a","refresh_token":"r","access_expires_at":null}',
        '{"access_token":"a","refresh_token":"r","access_expires_at":true}',
        '{"access_token":"a","refresh_token":"r","access_expires_at":1,"access_expires_at":2}',
        # Both tokens still required.
        '{"refresh_token":"r","access_expires_at":1793860000}',
        '{"access_token":"a","access_expires_at":1793860000}',
        # A flat object: anything nested is refused.
        '{"access_token":"a","refresh_token":"r","extra":[1,2]}',
        '{"access_token":"a","refresh_token":"r","o":{"x":"y"}}',
        '{"access_token":"a","refresh_token":"r","extra":[1,{"x":null}]}',
        '{"access_token":"a","refresh_token":"r","o":{"p":{}}}',
        '{"access_token":"a","refresh_token":"r","x":' + '[' * 64 + ']' * 64 + '}',
        '{"access_token":"a\\"b\\\\c\\/d\\u00e9\\ud83d\\ude00","refresh_token":"\\n\\t"}',
        '{"access_token":"first","access_token":"last","refresh_token":"r"}',
        '{"access_token":"","refresh_token":"r"}',
        '{"access_token":"a","refresh_token":1}',
        '{"access_token":"a"}',
        '["not","an","object"]',
        '{"access_token":"a","refresh_token":"r"} x',
        '{"access_token":"a","refresh_token":"r",}',
        '{"access_token":"a" "refresh_token":"r"}',
        '{"access_token":"a\u0001","refresh_token":"r"}',
        '{"access_token":"a\\x","refresh_token":"r"}',
        '{"access_token":"a","refresh_token":"r","n":01}',
        '{"access_token":"a","refresh_token":"r","n":-0.5E+2}',
        '{"access_token":"a\\u0000b","refresh_token":"r"}',
        '{"access_token":"a","refresh_token":"r"',
        '',
        '{}',
    ]
    def depth(v) -> int:
        kids = v.values() if isinstance(v, dict) else v if isinstance(v, list) else None
        return 0 if kids is None else 1 + max((depth(k) for k in kids), default=0)

    def no_dup_expiry(pairs):
        if sum(1 for k, _ in pairs if k == "access_expires_at") > 1:
            raise ValueError("access_expires_at twice")
        return dict(pairs)

    cases = []
    for t in texts:
        raw = t.encode("utf-8")
        try:
            v = json.loads(t, object_pairs_hook=no_dup_expiry)
        except ValueError:
            v = None
        ok = (raw[:1] == b"{" and isinstance(v, dict)
              and all(isinstance(v.get(k), str) and v[k] for k in ("access_token", "refresh_token"))
              and "\0" not in v["access_token"] + v["refresh_token"] and depth(v) <= 1)
        e = v.get("access_expires_at") if ok else None
        exp = e if isinstance(e, int) and not isinstance(e, bool) and 0 < e < 2 ** 64 else 0
        cases.append((raw, ok, v["access_token"] if ok else "", v["refresh_token"] if ok else "", exp))
    return cases


def creds_cases(ref) -> list[tuple[dict, bytes]]:
    """Credentials and the JSON the device sends for them (compact, the spec's key order)."""
    plain = dict(ref.TEST_CREDENTIALS)
    bound = dict(ref.TEST_CREDENTIALS_BOUND)
    kind_only = dict(ref.TEST_CREDENTIALS, vm_kind="svm")
    odd = {"node_id": "né", "access_token": 'q"b\\s\x01\n', "refresh_token": "\x1f\x7f/"}
    # A binding without a key isn't sent (setup stores none).
    unbound = (dict(odd, vm_id="vm-1", vm_kind="cvm"), ref.compact(odd).encode())
    return [(c, ref.compact(c).encode()) for c in (plain, bound, kind_only, odd)] + [unbound]


def vectors_header(directory: Path) -> Path:
    """mg_session_vectors.h: the exchanges, the device's fixed credential, a token proof, the JSON cases."""
    ref = reference()
    data = json.loads(VECTORS.read_text(encoding="utf-8"))
    proof = json.loads(PROOF_VECTORS.read_text(encoding="utf-8"))["vectors"][0]
    lines = ["#pragma once", "/* Generated from protocols/test-vectors/mg-session-v1.json by test_mg_session.py. */",
             "typedef struct { const char *write; int n_notify; const char *const *notify; int n_send;",
             "                 const char *const *send; const char *proof_cn, *proof_dn;",
             "                 int task; int counters; long long rx, tx; int nudge; int buffer; } sv_step_t;   /* task: -1 a "
             "write or proof; 0/1 a task ends/starts; counters: the next counters jump (-1 kept); nudge: the device's "
             "keepalive; buffer: the board's stack has room again */",
             "typedef struct { const char *name; int mtu; bool holds_token, proof, encrypted, vm_binding; int n_steps;",
             "                 const sv_step_t *steps; bool task; bool credentials_wait; } sv_exchange_t;   /* task: one "
             "active at the start; credentials_wait: no room for the credentials when the hello is answered */"]
    exchanges = recorded_exchanges()
    for e, ex in enumerate(exchanges):
        for s, step in enumerate(ex["steps"]):
            lines.append(f"static const char *const sv_n{e}_{s}[] = {{ " +
                         ", ".join(c_hex(n) for n in step["notify"]) + (", " if step["notify"] else "") + "0 };")
            lines.append(f"static const char *const sv_s{e}_{s}[] = {{ " +
                         ", ".join(c_hex(m) for m in step["send"]) + (", " if step["send"] else "") + "0 };")
        lines.append(f"static const sv_step_t sv_steps{e}[] = {{")
        for s, step in enumerate(ex["steps"]):
            pf = step["proof"]
            nonces = f"{c_hex(pf[0])}, {c_hex(pf[1])}" if pf else "0, 0"
            ctr = step.get("counters")
            lines.append(f"    {{ {c_hex(step['write'])}, {len(step['notify'])}, sv_n{e}_{s}, "
                         f"{len(step['send'])}, sv_s{e}_{s}, {nonces}, {step['task']}, {int(bool(ctr))}, "
                         f"{ctr[0] if ctr else -1}LL, {ctr[1] if ctr else -1}LL, {int(bool(step.get('nudge')))}, "
                         f"{int(bool(step.get('buffer')))} }},")
        lines.append("};")
    lines.append("static const sv_exchange_t SV_EXCHANGES[] = {")
    for e, ex in enumerate(exchanges):
        f = ex["flags"]
        lines.append(f"    {{ {json.dumps(ex['name'])}, {ex['mtu']}, {int(f['holds_token'])}, {int(f['proof'])}, "
                     f"{int(f['encrypted'])}, {int(f['vm_binding'])}, {len(ex['steps'])}, sv_steps{e}, "
                     f"{int(f['task'])}, {int(f['credentials_wait'])} }},")
    lines.append("};")
    lines.append(f"#define SV_NEXCHANGES {len(exchanges)}")
    creds = data["test_credentials"]
    for key, name in (("node_id", "NODE_ID"), ("access_token", "ACCESS"), ("refresh_token", "REFRESH"),
                      ("device_key", "DEVICE_KEY"), ("key_id", "KEY_ID")):
        lines.append(f"#define SV_{name} {c_str(creds[key].encode())}")
    bound = data["test_credentials_bound"]
    lines.append(f"#define SV_VM_ID {c_str(bound['vm_id'].encode())}")
    lines.append(f"#define SV_VM_KIND {c_str(bound['vm_kind'].encode())}")
    lines.append(f"#define SV_REGISTER {c_str(ref.compact(ref.register_request()).encode())}")
    lines.append(f"#define SV_REGISTER_ID {c_str(data['register_id'].encode())}")
    lines.append(f"#define SV_PAYLOAD_CARD {c_str(ref.compact(data['test_payloads']['display.card']).encode())}")
    lines.append(f"#define SV_PAYLOAD_HEALTH {c_str(ref.compact(data['test_payloads']['device.health']).encode())}")
    lines.append(f"#define SV_MAX_MESSAGE {data['device_hello']['max_message']}u")
    lines.append(f"#define SV_WINDOW {data['device_hello']['window']}u")
    for key in ("K", "client_nonce", "device_nonce"):
        lines.append(f"#define PV_{key.upper()} {c_hex(bytes.fromhex(proof[key]))}")
    for key, name in (("K", "K"), ("client_nonce", "CN"), ("device_nonce", "DN")):   # the exchanges' proof
        lines.append(f"#define SV_SEAL_{name} {c_hex(bytes.fromhex(data['session_seal'][key]))}")
    for key in ("challenge", "confirm"):
        lines.append(f"#define PV_{key.upper()} {c_hex(bytes.fromhex(proof['frames'][key]))}")
    lines.append("static const struct { const char *json; int len; bool ok; const char *access, *refresh; "
                 "unsigned long long exp; } SV_JSON[] = {")
    for raw, ok, a, r, exp in json_cases():
        lines.append(f"    {{ {c_str(raw)}, {len(raw)}, {int(ok)}, {c_str(a.encode())}, {c_str(r.encode())}, {exp}ull }},")
    lines.append("};")
    lines.append("static const struct { const char *node, *access, *refresh, *device_key, *key_id, *vm_id, "
                 "*vm_kind, *json; } SV_CREDS[] = {")
    for c, out in creds_cases(ref):
        lines.append("    { " + ", ".join(c_str(c.get(k, "").encode()) for k in
                                         ("node_id", "access_token", "refresh_token", "device_key", "key_id",
                                          "vm_id", "vm_kind"))
                     + f", {c_str(out)} }},")
    lines.append("};")
    header = directory / "mg_session_vectors.h"
    header.write_text("\n".join(lines) + "\n")
    return header


def mg_build(gen: Path) -> tuple[list[Path], list[str], list[Path], list[Path]]:
    """The protocol's C sources (SBC only), defines, includes and vendored codec sources."""
    vendor, defs, incs = codec_build(False)
    defs = [*defs, "MG_SESSION_TEST_HOOKS=1"]   # the vectors' counters event
    srcs = [MG / "mg_proto.c", MG / "mg_session.c", MG / "mg_ui.c", MG / "mg_queue.c", MG / "mg_codec.c",
            MG / "mg_play.c", MG / "mg_resample.c", ROOT / "components/muse_vstream/muse_vstream.c",
            ROOT / "tests/mg_token_proof_ref.c", *SEAL_SRCS]
    includes = [gen, MG, MG / "include", ROOT / "components/muse_vstream/include", PROTOCOLS, *incs, *SEAL_INCS]
    return srcs, defs, includes, vendor


def seal_reference():
    """protocols/test-vectors/mg_session_seal_ref.py, the seal's reference."""
    sys.path.insert(0, str(PROTOCOLS / "test-vectors"))
    import mg_session_seal_ref  # noqa: E402
    return mg_session_seal_ref


def sealed_case() -> dict:
    """The sealed exchange the "sealed" harness runs: two token proofs (the
    token proof vector's nonces, then the seal vectors' re-proof nonces), and
    the sealed credentials_updates the reference seals for the client. Every
    pair the harness stores, in order."""
    seal = seal_reference()
    proof = json.loads(PROOF_VECTORS.read_text(encoding="utf-8"))["vectors"][0]
    node = json.loads(VECTORS.read_text(encoding="utf-8"))["test_credentials"]["node_id"]
    k = bytes.fromhex(proof["K"])
    keys1 = seal.seal_keys(k, bytes.fromhex(proof["client_nonce"]), bytes.fromhex(proof["device_nonce"]), node)
    cn2, dn2 = seal.det("reproof cn", 16), seal.det("reproof dn", 16)
    keys2 = seal.seal_keys(k, cn2, dn2, node)

    def pair(a: str, r: str) -> bytes:
        return json.dumps({"access_token": a, "refresh_token": r}, separators=(",", ":")).encode()

    up = seal.CLIENT_TO_DEVICE
    return {
        "keys1": keys1, "keys2": keys2, "cn2": cn2, "dn2": dn2,
        "updates": {
            "UPD_0": seal.seal(*keys1, up, 0, pair("A2", "R2")),
            "UPD_1": seal.seal(*keys1, up, 1, pair("A3", "R3")),
            "UPD_2_SKIP": seal.seal(*keys1, up, 2, pair("AX", "RX")),
            "UPD_1_REFLECT": seal.seal(*keys1, seal.DEVICE_TO_CLIENT, 1, pair("AX", "RX")),
            "UPD_2_NOTOBJ": seal.seal(*keys1, up, 2, b'["A9","R9"]'),
            "UPD_3": seal.seal(*keys1, up, 3, pair("A5", "R5")),
            "UPD_K2_0": seal.seal(*keys2, up, 0, pair("A4", "R4")),
            "UPD_K2_1": seal.seal(*keys2, up, 1, pair("A6", "R6")),
            # What the apps send now: access_expires_at (a number) and another unknown member.
            "UPD_K2_1_EXP": seal.seal(*keys2, up, 1, json.dumps(
                {"access_token": "A7", "refresh_token": "R7", "access_expires_at": 1793860000, "note": "x"},
                separators=(",", ":")).encode()),
            "UPD_K2_2_NOREFRESH": seal.seal(*keys2, up, 2, b'{"access_token":"A8","access_expires_at":1793860000}'),
            "UPD_K2_0_OLDKEY": seal.seal(*keys1, up, 0, pair("AX", "RX")),
            # access_expires_at kept with the pair and handed back (test_access_expires_at).
            "UPD_EXP_0": seal.seal(*keys1, up, 0, json.dumps(
                {"access_token": "A7", "refresh_token": "R7", "access_expires_at": 1793860000, "note": "x"},
                separators=(",", ":")).encode()),
            "UPD_EXP_STRING_0": seal.seal(*keys1, up, 0,
                                          b'{"access_token":"A8","refresh_token":"R8","access_expires_at":"1793860000"}'),
            "UPD_EXP_1": seal.seal(*keys1, up, 1,
                                   b'{"access_token":"A7","refresh_token":"R7","access_expires_at":1793860000}'),
            "UPD_EXP_DUP_2": seal.seal(*keys1, up, 2, b'{"access_token":"A9","refresh_token":"R9",'
                                                      b'"access_expires_at":1793860000,"access_expires_at":1793860001}'),
        },
        "one_session_per_proof": json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["one_session_per_proof"],
    }


def seal_header(directory: Path) -> Path:
    """mg_seal_e2e.h: what sealed_case() gives the harness, as hex."""
    case = sealed_case()
    lines = ["#pragma once", "/* Generated by test_mg_session.py from mg_session_seal_ref.py. */",
             f"#define SE_CN2 {c_hex(case['cn2'])}", f"#define SE_DN2 {c_hex(case['dn2'])}"]
    for name, sealed in case["updates"].items():
        lines.append(f"#define SE_{name} {c_hex(sealed)}")
    steps = case["one_session_per_proof"]["steps"]
    # The sealed hellos (flags bit 0); the unsealed one before them (insecure, the nonces untouched) the
    # harness's own rows cover (an unsealed hello on a sealed-only device).
    hellos = [st for st in steps if st["client"] and int(st["client"][-2:], 16) & 1]
    assert len(hellos) == 3, "one_session_per_proof: three sealed hellos"
    for i, st in enumerate(hellos, 1):
        lines.append(f"#define SE_SPP_HELLO{i} {json.dumps(st['client'])}")
        lines.append(f"#define SE_SPP_ANSWER{i} {json.dumps(st['device'])}")
    # The seal vectors' access_expires_at section,
    # replayed byte for byte on both sessions (test_reference_expires_vectors).
    exp = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["access_expires_at"]
    lines.append(f"#define SX_K {c_hex(bytes.fromhex(exp['K']))}")
    lines.append(f"#define SX_NODE {json.dumps(exp['node_id'])}")
    lines.append("static const struct { const char *cn, *dn; } SX_SESSIONS[] = {")
    for se in exp["sessions"]:
        lines.append(f"    {{ {json.dumps(se['client_nonce'])}, {json.dumps(se['device_nonce'])} }},")
    lines.append("};")
    lines.append("static const struct { int session, direction; const char *sealed, *device; "
                 "unsigned long long stored; const char *access, *refresh; } SX_STEPS[] = {")
    for st in exp["steps"]:
        try:
            pt = json.loads(st["plaintext"]) if st["plaintext"].startswith("{") else {}
        except ValueError:      # a duplicate member: the device refuses it
            pt = {}
        access = pt.get("access_token", "") if isinstance(pt, dict) else ""
        refresh = pt.get("refresh_token", "") if isinstance(pt, dict) else ""
        dev = json.dumps(st["device"]) if st["device"] else "NULL"
        lines.append(f"    {{ {st['session']}, {st['direction']}, {json.dumps(st['sealed'])}, {dev}, "
                     f"{st['stored']}ull, {json.dumps(access)}, {json.dumps(refresh)} }},")
    lines.append("};")
    first = exp["steps"][0]
    assert first["direction"] == 1 and first["stored"] == 0
    p0 = json.loads(first["plaintext"])
    lines.append(f"#define SX_ACCESS0 {json.dumps(p0['access_token'])}")
    lines.append(f"#define SX_REFRESH0 {json.dumps(p0['refresh_token'])}")
    # The seal vectors' sealed_data_large section: data
    # both ways in a sealed session with the sealed credentials_saved
    # (device counter 2, the data after it 3), replayed byte for byte
    # (test_sealed_data_vectors). The older sealed_data section is the same
    # exchange before sealed_saved (its device data at counter 2 right after the
    # update, whose answer is the legacy unsealed byte): what a device sends a
    # client whose sealed hello lacks sealed_saved, replayed as SD0_STEPS.
    sd = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_data_large"]
    sd0 = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_data"]
    assert all(sd0[k] == sd[k] for k in ("K", "client_nonce", "device_nonce", "node_id", "max_write"))
    lines.append(f"#define SD_K {c_hex(bytes.fromhex(sd['K']))}")
    lines.append(f"#define SD_CN {c_hex(bytes.fromhex(sd['client_nonce']))}")
    lines.append(f"#define SD_DN {c_hex(bytes.fromhex(sd['device_nonce']))}")
    lines.append(f"#define SD_NODE {json.dumps(sd['node_id'])}")
    lines.append(f"#define SD_MTU {sd['max_write'] + 3}")
    first = json.loads(bytes.fromhex(sd["steps"][0]["plaintext"]))
    lines.append(f"#define SD_ACCESS0 {json.dumps(first['access_token'])}")
    lines.append(f"#define SD_REFRESH0 {json.dumps(first['refresh_token'])}")
    lines.append("typedef struct { const char *name; int direction, sub; const char *outcome, *answer, *plaintext; "
                 "int n_frames; const char *frames[8]; } sd_step_t;")
    for name, section in (("SD_STEPS", sd), ("SD0_STEPS", sd0)):
        lines.append(f"static const sd_step_t {name}[] = {{")
        for st in section["steps"]:
            frames = st["frames"]
            assert len(frames) <= 8
            ans = json.dumps(st["answer"]) if st["answer"] else "NULL"
            sub = bytes.fromhex(frames[0])[1]
            lines.append(f"    {{ {json.dumps(st['step'])}, {st['direction']}, {sub}, {json.dumps(st['outcome'])}, "
                         f"{ans}, {json.dumps(st['plaintext'])}, {len(frames)}, "
                         f"{{ {', '.join(json.dumps(f) for f in frames)} }} }},")
        lines.append("};")
    # sealed_interleaved ("Two in flight"): an update and data in flight together, the update refused at
    # its tag after data took the later counter (the hole), frame by frame
    # (test_sealed_interleaved_vectors).
    si = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_interleaved"]
    lines.append(f"#define SI_K {c_hex(bytes.fromhex(si['K']))}")
    lines.append(f"#define SI_CN {c_hex(bytes.fromhex(si['client_nonce']))}")
    lines.append(f"#define SI_DN {c_hex(bytes.fromhex(si['device_nonce']))}")
    lines.append(f"#define SI_NODE {json.dumps(si['node_id'])}")
    lines.append(f"#define SI_MTU {si['max_write'] + 3}")
    upd = [json.loads(bytes.fromhex(st["plaintext"])) for st in si["steps"]
           if st["outcome"] == "message" and bytes.fromhex(st["frames"][0])[1] == 0x07]
    assert upd and "access_expires_at" in upd[0]
    lines.append(f"#define SI_ACCESS {json.dumps(upd[0]['access_token'])}")
    lines.append(f"#define SI_REFRESH {json.dumps(upd[0]['refresh_token'])}")
    lines.append(f"#define SI_EXPIRES {upd[0]['access_expires_at']}ull")
    lines.append("static const struct { const char *name; int direction, sub; const char *outcome, *answer, *plaintext; "
                 "int n_frames; const char *frames[8]; } SI_STEPS[] = {")
    for st in si["steps"]:
        frames = st["frames"]
        assert len(frames) <= 8 and st["direction"] == 2
        ans = json.dumps(st["answer"]) if st["answer"] else "NULL"
        sub = bytes.fromhex(frames[0])[1]
        lines.append(f"    {{ {json.dumps(st['step'])}, {st['direction']}, {sub}, {json.dumps(st['outcome'])}, {ans}, "
                     f"{json.dumps(st['plaintext'])}, {len(frames)}, {{ {', '.join(json.dumps(f) for f in frames)} }} }},")
    lines.append("};")
    # sealed_credentials_saved: the device's
    # credentials, then each update and the answers the vector shows, with the
    # client's verdicts (test_saved_vectors).
    sc = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_credentials_saved"]
    lines.append(f"#define SC_K {c_hex(bytes.fromhex(sc['K']))}")
    lines.append(f"#define SC_CN {c_hex(bytes.fromhex(sc['client_nonce']))}")
    lines.append(f"#define SC_DN {c_hex(bytes.fromhex(sc['device_nonce']))}")
    lines.append(f"#define SC_NODE {json.dumps(sc['node_id'])}")
    seal = seal_reference()
    e, m = seal.seal_keys(bytes.fromhex(sc["K"]), bytes.fromhex(sc["client_nonce"]),
                          bytes.fromhex(sc["device_nonce"]), sc["node_id"])
    creds = json.loads(seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 0, bytes.fromhex(sc["steps"][0]["device"][0])[7:]))
    lines.append(f"#define SC_ACCESS0 {json.dumps(creds['access_token'])}")
    lines.append(f"#define SC_REFRESH0 {json.dumps(creds['refresh_token'])}")
    lines.append(f"#define SC_CREDS {json.dumps(sc['steps'][0]['device'][0])}")
    lines.append("static const struct { const char *name, *access, *refresh; int n_client; const char *client[4]; "
                 "int n_device; const char *device[4]; int saved[4]; } SC_STEPS[] = {")
    for st in sc["steps"][1:]:
        upd = json.loads(st["update_plaintext"])
        assert len(st["client"]) <= 4 and len(st["device"]) <= 4
        lines.append(f"    {{ {json.dumps(st['step'])}, {json.dumps(upd['access_token'])}, "
                     f"{json.dumps(upd['refresh_token'])}, {len(st['client'])}, "
                     f"{{ {', '.join(json.dumps(c) for c in st['client'])} }}, {len(st['device'])}, "
                     f"{{ {', '.join(json.dumps(d) for d in st['device'])} }}, "
                     f"{{ {', '.join('1' if v == 'saved' else '0' for v in st['verdicts'])} }} }},")
    lines.append("};")
    header = directory / "mg_seal_e2e.h"
    header.write_text("\n".join(lines) + "\n")
    return header


def slice_between(source: str, start: str, end: str, inclusive_end: bool = False) -> str:
    a = source.index(start)
    b = source.index(end, a + len(start))
    return source[a:b + (len(end) if inclusive_end else 0)]


def function(source: str, signature: str) -> str:
    """The definition starting at signature, through its closing brace."""
    start = source.index(signature)
    depth = 0
    for i in range(source.index("{", start), len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1] + "\n"
    raise AssertionError(f"no end for {signature}")


def link_code(directory: Path) -> None:
    """noise_control.cpp's link code, sliced out for the whole-gadget harness: the
    session state, the control sender, the extra daemon requests, the identity
    fetch, result wrapping, invoke handling, inbound dispatch, the reconnect
    policy, and the device session over BLE. The harness stands in for build_register_json and
    send_device_health (the reference device's register and payloads)."""
    src = CONTROL.read_text()
    constants = "\n".join(line for line in src.splitlines() if line.startswith((
        "#define CTRL_STREAM_ID", "#define IDENTITY_STREAM_ID", "#define IDENTITY_BODY_MAX",
        "#define IDLE_POLL_MS", "#define WS_HEADROOM", "#define CTRL_BODY_CHUNK_MAX",
        "#define OUT_SVC_SCRATCH", "#define OUT_ENV_SCRATCH", "#define RECONNECT_MIN_MS",
        "#define RECONNECT_MAX_MS", "#define RECONNECT_AUTH_FAILED_MS", "#define STABLE_SESSION_MS",
        "#define VM_REFRESH_FAILURES")))
    parts = {
        "link_state.inc": constants + "\n"
        + slice_between(src, "static char s_node_id[64];", "static TaskHandle_t s_task = nullptr;")
        + slice_between(src, "// Only the single Noise session task advances", "// Forward declarations.")
        + function(src, "static void notify_status(const char *s)"),
        "link_sender.inc": slice_between(src, "static bool flush_outbound(", "// ---- Tunnel stream multiplexing"),
        "link_requests.inc": slice_between(src, "#if CONFIG_HOMEHUB_DAEMON_REQUESTS\n\n// Stream ids",
                                           "#endif  // CONFIG_HOMEHUB_DAEMON_REQUESTS", True),
        "link_identity.inc": slice_between(src, "// Reply to GET /api/gadget/me", "// ---- JSON message builders"),
        "link_results.inc": slice_between(src, "// Queued result from an asynchronous command.",
                                          "// ---- Inbound message dispatch")
        # device.guide (protocols/DEVICE.md), with a guide of its own.
        + 'extern "C" const char muse_device_guide_md[] = "# BLE gadget\\n";\n'
        + function(src, "static void send_device_guide(")
        + function(src, "static const char *log_name(")
        + function(src, "static void log_control_unhandled(")
        + function(src, "static void handle_invoke_request(")
        + slice_between(src, "static char *wrap_result_json(", "// ---- Reconnect policy"),
        "link_inbound.inc": slice_between(src, "// A control-stream message cut across BodyChunks", "// The session loop's pause")
        # A tunnel stream's Reset or refusal (never over BLE: ble_frame_ours()).
        + "#define TUNNEL_FAILURE_MAX 72\nstatic void noise_tunnel_on_server_drop(const char *why) { (void)why; }\n"
        + slice_between(src, "// Longest server Reset reason kept.", "static constexpr int64_t WS_FRAME_TIMEOUT_US")
        + function(src, "static bool dispatch_inbound_frame("),
        "link_policy.inc": slice_between(src, "enum session_result_t {", "static void reconnect_delay("),
        "link_ble.inc": slice_between(src, "#if CONFIG_MUSE_GADGET_BLE_SESSION\n\n#define BLE_SESSION_STACK",
                                      "#endif  // CONFIG_MUSE_GADGET_BLE_SESSION", True),
        "link_request_api.inc": function(src, 'extern "C" void noise_ctrl_send_command_result(')
        + slice_between(src, '#if CONFIG_HOMEHUB_DAEMON_REQUESTS\nextern "C" int64_t noise_ctrl_req_open(',
                        "#endif  // CONFIG_HOMEHUB_DAEMON_REQUESTS", True),
    }
    for name, text in parts.items():
        (directory / name).write_text(text + "\n")


class DeviceSessionTest(unittest.TestCase):
    def run_session_harness(self, mode: str, extra: tuple = ()) -> str:
        with tempfile.TemporaryDirectory(prefix="mg-session-") as gen:
            vectors_header(Path(gen))
            seal_header(Path(gen))
            srcs, defs, incs, vendor = mg_build(Path(gen))
            return compile_and_run(self, [ROOT / "tests/mg_session_harness.c", *srcs], [*defs, *extra], incs,
                                   vendor=vendor, args=[mode])

    def test_sealed_clear_in_both_session_kinds_and_builds(self) -> None:
        # The cap bit and the unsealed token_proof clear's refusal only with CONFIG_MUSE_GADGET_SEALED_CLEAR (off
        # by default); the sealed clear itself in every build. The BLE-only session's rows (both image
        # kinds) are in the unit mode; the credentials-only session (layer 2) with the build's
        # MGDS_CAP_SEALED_CLEAR on:
        out = self.run_session_harness("creds_clear", ("MGDS_CAP_SEALED_CLEAR=1",))
        self.assertIn("PASS creds sealed clear: declared, the unsealed token_proof clear refused, the answer from the erase", out)
        cmake = (MG / "CMakeLists.txt").read_text()
        self.assertIn("if(CONFIG_MUSE_GADGET_SEALED_CLEAR)\n"
                      "        target_compile_definitions(${COMPONENT_LIB} PRIVATE MGDS_CAP_SEALED_CLEAR=1)", cmake)
        ble = (MG / "mg_ble.c").read_text()
        self.assertIn("#if CONFIG_MUSE_GADGET_SEALED_CLEAR\n"
                      "#define MG_SEALED_CLEAR true\n#else\n#define MG_SEALED_CLEAR false", ble)
        self.assertIn(".sealed_clear = MG_SEALED_CLEAR,", ble)
        proto = (MG / "mg_proto.c").read_text()
        self.assertIn(".clear = creds_clear,", proto)
        self.assertIn(".clear = session_clear,", proto)

    def test_what_the_vectors_dont_reach(self) -> None:
        # A full BLE stack (credit, credentials and fragments wait and go in
        # order), the window, disconnects and new hellos, the token proof's
        # clear, not offered, and the credentials JSON both ways.
        out = self.run_session_harness("unit")
        self.assertIn("PASS mg_session unit", out)
        self.assertIn("PASS window under max_message: a 6 KB message completes through a 4 KB window", out)
        self.assertIn("PASS sealed clear: erased once taken, every refusal erases nothing, credentials not yet out "
                      "dropped (the answer on their counter), on a lost link; token_proof clear refused where declared",
                      out)
        # Forget's [1] holds: an update after the clear is taken is refused, nothing stored, no credentials_saved; a new connection's
        # hello stores the next as ever; a refused clear starts no refusal.
        self.assertIn("PASS sealed clear and updates: none stored once taken, stored after a new hello and after a "
                      "refused clear", out)
        self.assertIn("PASS sealed clear async: answered from the erase's result, nothing before it, nothing after "
                      "the link", out)
        # Set up with no proof key fails closed.
        self.assertIn("PASS owner fails closed: set up with no key is nobody, unset is open, a key needs its proof", out)
        self.assertIn("PASS new hello drops partials: a clear part way in never completes, an old message replayed "
                      "is refused", out)


class SealedSessionTest(unittest.TestCase):
    """Sealed credentials (mgcommands.h, Sealed
    credentials) end to end: the device side (mg_proto.c, mg_session.c, the
    vendored seal on the component's crypto glue) against the seal's Python
    reference as the client. The harness checks every refusal itself; what
    the device sealed is opened here, by the reference."""

    def test_sealed_exchange_against_the_reference(self) -> None:
        out = DeviceSessionTest.run_session_harness(self, "sealed")
        self.assertIn("PASS mg_session sealed", out)
        # The seal vectors' sealed_data section, every step (mgcommands.h, Sealed data).
        steps = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_data_large"]["steps"]
        self.assertIn(f"SEALED_DATA_LARGE {len(steps)} steps byte for byte", out)
        # An older client (sealed hello without sealed_saved: an older iOS app, the bench app):
        # the older sealed_data section byte for byte, the update answered with the
        # legacy unsealed byte and the device's next data under the counter it had before sealed_saved (2, no gap).
        steps0 = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_data"]["steps"]
        self.assertIn(f"SEALED_DATA_LARGE {len(steps0)} steps byte for byte (credentials_saved 890801, no counter)", out)
        # The same client's answers on the BLE-only session: one byte, unsealed, saved / not saved / superseded,
        # no device counter taken (the next data is counter 1); the hello echoes only the flags accepted.
        self.assertIn("SAVED_LEGACY session: [1] [0] superseded [0], next data counter 1", out)
        self.assertIn("HELLO_FLAGS session: echoed sealed|sealed_saved, sealed, none", out)
        # sealed_credentials_saved, byte for byte where a conforming device makes the bytes.
        sc = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_credentials_saved"]["steps"]
        self.assertIn(f"SEALED_SAVED {len(sc) - 1} updates (session)", out)
        # No sealed device counter twice, counters strictly increasing on the wire (data part-way, data sealed but
        # held, stored updates' answers, more data).
        self.assertIn("COUNTER_ORDER 7 sealed messages, counters 0-6 once each, in order", out)
        # sealed_interleaved ("Two in flight"), frame by
        # frame, the update stored at once and on the worker (store_later).
        il = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_interleaved"]["steps"]
        self.assertIn(f"SEALED_INTERLEAVED {len(il)} steps frame by frame (stored at once)", out)
        self.assertIn(f"SEALED_INTERLEAVED {len(il)} steps frame by frame (store_later)", out)
        seal = seal_reference()
        data = json.loads(VECTORS.read_text(encoding="utf-8"))["test_credentials"]
        case = sealed_case()
        got = dict(line.split(" ", 1) for line in out.splitlines() if line.startswith(("SEALED", "EXP_")))
        got.update({"SEALED_" + k: v for k, v in got.items() if k.startswith("EXP_")})

        def creds(access: str, refresh: str) -> bytes:
            c = {"node_id": data["node_id"], "access_token": access, "refresh_token": refresh,
                 "device_key": data["device_key"], "key_id": data["key_id"]}
            return json.dumps(c, separators=(",", ":"), ensure_ascii=False).encode()

        down = seal.DEVICE_TO_CLIENT
        first = bytes.fromhex(got["SEALED1"])
        # The first proof's credentials: counter 0, the stored pair; the
        # reference opens them, and nothing else does (another key, the
        # other direction, the next counter).
        self.assertEqual(seal.unseal(*case["keys1"], down, 0, first), creds(data["access_token"], data["refresh_token"]))
        self.assertIsNone(seal.unseal(*case["keys2"], down, 0, first))
        self.assertIsNone(seal.unseal(*case["keys1"], seal.CLIENT_TO_DEVICE, 0, first))
        self.assertIsNone(seal.unseal(*case["keys1"], down, 1, first))
        # The same proof's nonces at the sealed session's floor, ATT MTU 100, seal the same bytes (23 is refused).
        self.assertEqual(bytes.fromhex(got["SEALED_MTU100"]), first)
        # A re-proof: new keys, the pair the session last stored (A5).
        second = bytes.fromhex(got["SEALED2"])
        self.assertEqual(seal.unseal(*case["keys2"], down, 0, second), creds("A5", "R5"))
        self.assertIsNone(seal.unseal(*case["keys1"], down, 0, second))
        # No token bytes in the clear anywhere the device notified.
        self.assertNotIn(data["access_token"].encode().hex(), out)
        self.assertNotIn(data["refresh_token"].encode().hex(), out)
        # access_expires_at: kept with the pair and handed back last; an update
        # without it leaves none.
        kept = creds("A7", "R7")
        self.assertEqual(seal.unseal(*case["keys2"], down, 0, bytes.fromhex(got["SEALED_EXP_KEPT"])),
                         kept[:-1] + b',"access_expires_at":1793860000}')
        self.assertEqual(seal.unseal(*case["keys1"], down, 0, bytes.fromhex(got["SEALED_EXP_NONE"])), creds("A4", "R4"))


class CredentialsOnlySessionTest(unittest.TestCase):
    """The credentials-only device session a Wi-Fi gadget runs (portable
    mg_device_session layer 2 under mg_proto.c, cfg.creds_session):
    caps sealed only, the refusals, one sealed session
    per proof, sealed updates through the same store, every VM-session part
    refused; what it sealed opened by the seal's reference."""

    def test_credentials_only_against_the_reference(self) -> None:
        out = DeviceSessionTest.run_session_harness(self, "creds")
        self.assertIn("PASS mg_session creds", out)
        # The sealed answers through layer 2: sealed_credentials_saved byte for byte, the 30 s give-up answering the
        # vector's sealed [0], and a plain session's one-byte [result].
        sc = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_credentials_saved"]["steps"]
        self.assertIn(f"SEALED_SAVED {len(sc) - 1} updates (creds)", out)
        self.assertIn("CREDS_GIVE_UP sealed [0] at 30 s, byte for byte", out)
        self.assertIn("PLAIN_REFUSED (creds)", out)
        # An older client (sealed hello without sealed_saved): the
        # one-byte unsealed [1] / [0] that take no counter, the 30 s give-up silent (as before sealed_saved), and the hello
        # echoing only the flags it accepted.
        self.assertIn("SAVED_LEGACY creds: [1] [0], give-up silent", out)
        self.assertIn("HELLO_FLAGS creds: echoed sealed|sealed_saved, sealed, none", out)
        seal = seal_reference()
        data = json.loads(VECTORS.read_text(encoding="utf-8"))["test_credentials"]
        case = sealed_case()
        got = dict(line.split(" ", 1) for line in out.splitlines() if line.startswith("CREDS_SEALED"))

        def creds(access: str, refresh: str) -> bytes:
            # Layer 2 hands out the node id and the pair (no device key).
            c = {"node_id": data["node_id"], "access_token": access, "refresh_token": refresh}
            return json.dumps(c, separators=(",", ":"), ensure_ascii=False).encode()

        down = seal.DEVICE_TO_CLIENT
        first = bytes.fromhex(got["CREDS_SEALED1"])
        self.assertEqual(seal.unseal(*case["keys1"], down, 0, first), creds(data["access_token"], data["refresh_token"]))
        self.assertIsNone(seal.unseal(*case["keys2"], down, 0, first))
        self.assertEqual(bytes.fromhex(got["CREDS_SEALED_MTU100"]), first)
        second = bytes.fromhex(got["CREDS_SEALED2"])
        self.assertEqual(seal.unseal(*case["keys2"], down, 0, second), creds("A3", "R3"))
        self.assertNotIn(data["access_token"].encode().hex(), out)
        # access_expires_at through layer 2's hooks: kept and handed back last
        # (layer 2 puts it there), none after an update without it.
        exp = dict(line.split(" ", 1) for line in out.splitlines() if line.startswith("CREDS_EXP_"))
        self.assertEqual(seal.unseal(*case["keys2"], down, 0, bytes.fromhex(exp["CREDS_EXP_KEPT"])),
                         creds("A7", "R7")[:-1] + b',"access_expires_at":1793860000}')
        self.assertEqual(seal.unseal(*case["keys1"], down, 0, bytes.fromhex(exp["CREDS_EXP_NONE"])), creds("A4", "R4"))


class WholeGadgetTest(unittest.TestCase):
    """mg_session.c under noise_control.cpp's link code, and that code over Wi-Fi too."""

    def run_gadget_harness(self, mode: str) -> str:
        if not (CJSON / "cJSON.c").exists():
            self.skipTest("cJSON comes from managed_components: run one idf.py build first (AGENTS.md, Tests)")
        cc = shlex.split(os.environ.get("CC", "cc"))
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"}
        with tempfile.TemporaryDirectory(prefix="mg-gadget-") as gen:
            g = Path(gen)
            vectors_header(g)
            link_code(g)
            srcs, defs, incs, vendor = mg_build(g)
            common = [*[f"-D{d}" for d in defs], *[a for i in incs for a in ("-I", str(i))]]
            objs = []
            for i, s in enumerate([*vendor, *srcs, ROOT / "tests/mg_session_gadget_ble.c", CJSON / "cJSON.c"]):
                obj = g / f"o{i}_{s.stem}.o"
                quiet = s in vendor or s.parent == CJSON
                flags = ["-w"] if quiet else ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                                              "-Wno-missing-field-initializers"]
                r = subprocess.run([*cc, "-std=c11", "-D_DEFAULT_SOURCE", "-O1", *san, *common, *flags, "-I", str(CJSON),
                                    "-c", str(s), "-o", str(obj)], text=True, capture_output=True, env=env)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                objs.append(str(obj))
            core = ROOT / "components/noise_core"
            core_srcs = [core / "src" / f"{n}.cpp" for n in ("ClientSession", "InitiatorHandshake", "ServiceCodec",
                                                             "Status", "Transport", "TransportFrameCodec")]
            exe = g / "gadget"
            r = subprocess.run([*cxx, "-std=c++17", "-O1", *san, "-Wall", "-Wextra", "-Werror",
                                "-Wno-unused-function", "-Wno-unused-variable", "-Wno-missing-field-initializers", *common,
                                "-I", str(CJSON), "-I", str(ROOT / "main"), "-I", str(ROOT / "tests"),
                                "-I", str(core / "include"), str(ROOT / "tests/mg_session_gadget_harness.cpp"),
                                *map(str, core_srcs), *objs, "-lm", "-o", str(exe)],
                               text=True, capture_output=True, env=env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe), mode], text=True, capture_output=True, env=env, cwd=gen)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r.stdout

    def test_the_vectors_through_the_link_code(self) -> None:
        # link.register from the session's start, link.invoke answered with
        # link.result, credit, the session's end and a new one: the gadget's
        # own code produces the reference device's bytes.
        out = self.run_gadget_harness("vectors")
        exchanges = recorded_exchanges()   # all but the wake exchange (no mg_session_cap_wake here)
        self.assertIn(f"PASS gadget vectors: {len(exchanges)} exchanges", out)

    def test_wifi_frames_the_same_envelopes(self) -> None:
        # The shared senders over ClientSession (Wi-Fi, sealed as identity by
        # the test crypto) and over the device session send the same
        # ServiceRequestEnvelopes, byte for byte.
        self.assertIn("PASS gadget wifi", self.run_gadget_harness("wifi"))

    def test_requests_heartbeat_and_failures_over_ble(self) -> None:
        # A GET /fs/raw request stream and its reply, the heartbeat after the
        # register reply, a small client max_message, too_large, and a failed
        # session asking the client for a new one.
        self.assertIn("PASS gadget extra", self.run_gadget_harness("extra"))

    def test_reask_series_backs_off_across_short_sessions(self) -> None:
        # mgcommands.h's re-ask series, noise_control.cpp itself: a server that opens and ends cleanly
        # (closed) after 5 s, ten times, is asked at once only the first time,
        # then after 1, 2, 4 ... 60 s (no immediate re-ask after each); ten
        # no_network ends run their own series, at once then 1, 2, 4 ... 15 s;
        # neither restarts the other; a session open 59.999 s continues, one
        # open 60 s or 61 s restarts both (at once, then 1 s); an ask of one
        # kind waiting doesn't hold back the other's step; a credentials_update
        # saved restarts both, and
        # saved after an end (auth_failed included, in the same pass too)
        # asks at once (exchange 8 step 12), right after a hello nothing more,
        # not saved nothing; a hello restarts it and drops a waiting ask (with
        # the next state in the same pass, that state asks at once); the
        # link's end resets it.
        out = self.run_gadget_harness("reask")
        self.assertIn("PASS gadget reask", out)
        self.assertIn("REASK closed#1 at once", out)
        for epoch, wait in zip(range(2, 11), (1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000)):
            self.assertIn(f"REASK closed#{epoch} after {wait} ms", out)
        self.assertIn("REASK no_network#1 at once", out)
        for n, wait in zip(range(2, 11), (1000, 2000, 4000, 8000, 15000, 15000, 15000, 15000, 15000)):
            self.assertIn(f"REASK no_network#{n} after {wait} ms", out)
        self.assertIn("REASK neither restarts the other", out)
        self.assertIn("REASK 61s at once", out)
        self.assertIn("REASK kinds overlap", out)
        self.assertIn("REASK credentials_update restarts", out)
        self.assertIn("REASK unsaved update changes nothing", out)
        self.assertIn("REASK saved after an end asks at once", out)
        self.assertIn("REASK hello restarts", out)
        self.assertIn("REASK new link at once", out)
        # A failed open (exchange 14): open_request -> connecting -> closed
        # with no session ever open asks again at the series' next step, every
        # time (never silent): at once, then 1, 2, 4 ... 60 s; no_network's
        # own series to 15 s; a failure while an ask waits leaves it standing.
        self.assertIn("REASK failed open closed#1 at once", out)
        for n, wait in zip(range(2, 11), (1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000)):
            self.assertIn(f"REASK failed open closed#{n} after {wait} ms", out)
        self.assertIn("REASK failed open no_network#1 at once", out)
        for n, wait in zip(range(2, 11), (1000, 2000, 4000, 8000, 15000, 15000, 15000, 15000, 15000)):
            self.assertIn(f"REASK failed open no_network#{n} after {wait} ms", out)
        self.assertIn("REASK failed open waiting ask stands", out)
        self.assertIn("REASK failed open exchange 14 timed", out)

    def test_unhandled_and_malformed_messages_and_session_ping(self) -> None:
        # The placeholder for what the server sends back through the phone:
        # a frame of an unknown kind, a Request from the VM, frames on streams
        # nobody runs (a Reset or refusal there no longer ends the session),
        # control messages of unknown types, each logged on mg.session by type
        # and size (never content), accepted, credited, the session left open;
        # malformed ones (garbage, an empty envelope, frames that don't
        # decode, control messages that aren't JSON objects, an invoke
        # without an id, random bytes) refused without a crash under ASan and
        # UBSan. link.register and its reply are logged; session.ping sends
        # exactly one heartbeat on an open, registered session and refuses
        # without one.
        self.assertIn("PASS gadget relay", self.run_gadget_harness("relay"))

    def test_session_heap_is_bounded_and_keeps_nothing(self) -> None:
        # A 64 KiB / 64 KiB client hello (and 4 KiB) on a sealed session that
        # opens, registers and answers every invoke: the session's
        # allocations peak within the gadget's own limits and nothing stays
        # after each session (12 open -> traffic -> close -> reconnect cycles).
        out = self.run_gadget_harness("heap")
        print(out)
        self.assertIn("PASS gadget heap", out)


class WiringTest(unittest.TestCase):


    def test_production_register_has_the_vms_shape(self) -> None:
        # The whole-gadget replay stands in the reference device's register;
        # the session over BLE sends build_register_json()'s, as Wi-Fi does.
        # Built as a BLE-only build has it (no tunnel, no OTA), it has the
        # keys the VM takes from both its clients (the Linux client's
        # register_params(): commands_v2, not the reference's commands).
        src = CONTROL.read_text()
        functions = "\n".join(function(src, signature) for signature in (
            "static void add_command(",
            "static void add_register_metadata_string(", "static char *build_register_json("))
        with tempfile.TemporaryDirectory(prefix="mg-register-") as name:
            tmp = Path(name)
            source = tmp / "register.c"
            source.write_text('''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#define CONFIG_MUSE_GADGET_BLE_AUDIO 1          /* a BLE-only gadget build */
#define CONFIG_HOMEHUB_BLE_PRODUCT_CODE "SCW1"
#define CONFIG_MUSE_DEVICE_MODEL_ID "muse-gadget-sensecap-watcher"
#define CONFIG_MUSE_DEVICE_MODEL_NAME "Seeed SenseCAP Watcher"   /* every overlay sets one */
#include "register_family.h"
#define nullptr NULL
static esp_app_desc_t app = { .version = "1.2.3" };
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static char s_wifi_ssid[33] = "", s_register_req_id[40];
static const char *s_node_id = "gadget-node", *s_display_name = "BLE gadget";
static void copy_wifi_ssid(char *out, size_t size) { snprintf(out, size, "%s", s_wifi_ssid); }
static void make_uuid(char *out, size_t size) { snprintf(out, size, "req-1"); }
''' + functions + '''
int main(void) {
    char *json = build_register_json();
    if (!json || strcmp(s_register_req_id, "req-1")) return 1;
    puts(json);
    free(json);
    return 0;
}
''')
            exe = tmp / "register"
            cc = shlex.split(os.environ.get("CC", "cc"))
            r = subprocess.run([*cc, "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "tests/link_fakes"),
                                "-I", str(ROOT / "main"), str(source), str(ROOT / "tests/link_fakes/cJSON.c"),
                                "-o", str(exe)], text=True, capture_output=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        register = json.loads(r.stdout)
        self.assertEqual({k: register[k] for k in ("type", "id", "method")},
                         {"type": "req", "id": "req-1", "method": "link.register"})
        params = register["params"]
        self.assertEqual(set(params), {"node_id", "display_name", "platform", "version", "device_family", "model_id",
                                       "is_wakeup_supported", "metadata", "commands_v2"})
        # No short product name (CONFIG_HOMEHUB_MODEL_NAME) here: the overlay's model name.
        self.assertEqual(params["metadata"], {"model_name": "Seeed SenseCAP Watcher"})
        self.assertEqual((params["node_id"], params["display_name"], params["version"]),
                         ("gadget-node", "BLE gadget", "1.2.3"))
        # A Muse Gadget registers as one (the apps' Set up again refuses
        # link-family nodes; register_family.h), its model_id the board's overlay id, never the catalog code.
        self.assertEqual((params["device_family"], params["model_id"]), ("gadget", "muse-gadget-sensecap-watcher"))
        self.assertIn("device.health", params["commands_v2"])
        for name, command in params["commands_v2"].items():
            self.assertTrue(command["description"], name)
            self.assertIsInstance(command["required"], dict, name)
            self.assertIsInstance(command["optional"], dict, name)


class SealedNeedsTheMinimumMtu(unittest.TestCase):
    """A sealed hello below MG_MIN_ATT_MTU gets invalid_value (the vectors' low-MTU
    exchange and mg_session_harness's MTU 23 row), and mg_session_cap_sealed only where the build reaches it."""

    def test_every_build_asks_for_it(self) -> None:
        ble = (MG / "mg_ble.c").read_text()
        self.assertIn("_Static_assert(CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU >= MG_MIN_ATT_MTU,", ble)
        for f in [ROOT / "sdkconfig.defaults", *sorted((ROOT / "devices").glob("sdkconfig.*"))]:
            for line in f.read_text().splitlines():
                if line.startswith("CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU="):
                    self.assertGreaterEqual(int(line.split("=", 1)[1]), 100, f.name)

    def test_the_hello_refuses_below_it(self) -> None:
        src = (MG / "mg_session.c").read_text()
        hello = src[src.index("static void on_hello(mg_session_t *s, const uint8_t *p, size_t n, bool proof)"):]
        hello = hello[:hello.index("\n}\n")]
        self.assertIn("|| s->mtu < MG_MIN_ATT_MTU", hello)
        self.assertLess(hello.index(": !sealed ? mg_error_code_insecure"), hello.index("|| s->mtu < MG_MIN_ATT_MTU"))


if __name__ == "__main__":
    unittest.main()
