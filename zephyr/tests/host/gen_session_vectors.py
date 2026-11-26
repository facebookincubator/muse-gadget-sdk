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

"""Turns the device session vectors into a C header.

  gen_session_vectors.py MG_SESSION_JSON OUT_H

Every exchange and step, byte for byte; the vector device's hello, node id,
credentials and commands, with a handler per command that writes its
test_payload. link.register's other params (display_name, platform, ...)
are only in the bytes, so they are read back from the first exchange's
register message.
"""

import base64
import json
import struct
import sys

SESSION = 0x89
DATA = 3
FIRST, LAST = 1, 2
TYPES = {"string": "MG_VM_STRING", "integer": "MG_VM_INTEGER", "number": "MG_VM_NUMBER",
         "boolean": "MG_VM_BOOLEAN", "object": "MG_VM_OBJECT", "array": "MG_VM_ARRAY"}


def arr(b):
    return "{" + ", ".join("0x%02x" % x for x in b) + "}" if b else "{0}"


def cstr(s):
    out = []
    for ch in s.encode("utf-8"):
        if ch in (0x22, 0x5C):
            out.append("\\" + chr(ch))
        elif 0x20 <= ch < 0x7F:
            out.append(chr(ch))
        else:
            out.append("\\x%02x\"\"" % ch)
    return '"' + "".join(out) + '"'


def read_varint(data, i):
    v = shift = 0
    while True:
        b = data[i]
        i += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return v, i


def parse(data):
    out, i = {}, 0
    while i < len(data):
        key, i = read_varint(data, i)
        num, wire = key >> 3, key & 7
        if wire == 0:
            v, i = read_varint(data, i)
        else:
            n, i = read_varint(data, i)
            v, i = data[i:i + n], i + n
        out.setdefault(num, []).append(v)
    return out


def device_messages(exchange):
    """Reassembles the device's data messages in an exchange."""
    msgs, cur = [], None
    for step in exchange["steps"]:
        for hx in step["notify"]:
            f = bytes.fromhex(hx)
            if f[0] != SESSION or f[1] != DATA:
                continue
            flags, rest = f[2], f[3:]
            if flags & FIRST:
                cur, rest = bytearray(), rest[4:]
            cur.extend(rest)
            if flags & LAST:
                msgs.append(bytes(cur))
    return msgs


# The reference device's link.register params (mg_session_ref.py), for vectors
# whose exchanges carry no plain VM data to read them from (every unsealed
# hello is refused, and the register goes sealed). It handles wake, so
# is_wakeup_supported; its commands go as commands_v2.
REF_REGISTER = {"display_name": "Reference Gadget", "platform": "mg-ref", "version": "1",
                "device_family": "gadget", "model_id": "mg-ref", "is_wakeup_supported": True}


def register_params(data):
    for ex in data["exchanges"]:
        for msg in device_messages(ex):
            try:
                frame = parse(parse(msg)[2][0])
            except (IndexError, KeyError, ValueError):
                continue
            if 4 in frame:
                body = parse(frame[4][0])[1][0]
                req = json.loads(body[4:4 + struct.unpack("<I", body[:4])[0]])
                if req.get("method") == "link.register":
                    return req["params"]
    return dict(REF_REGISTER, node_id=data["node_id"])


def payload_code(obj, out, indent="\t"):
    for k, v in obj.items():
        if isinstance(v, bool):
            out.append("%smg_jw_bool(w, %s, %s);\n" % (indent, cstr(k), "true" if v else "false"))
        elif isinstance(v, int):
            out.append("%smg_jw_int(w, %s, %d);\n" % (indent, cstr(k), v))
        elif isinstance(v, str):
            out.append("%smg_jw_str(w, %s, %s);\n" % (indent, cstr(k), cstr(v)))
        elif v is None:
            out.append("%smg_jw_raw(w, %s, \"null\", 4);\n" % (indent, cstr(k)))
        elif isinstance(v, dict):
            out.append("%smg_jw_obj(w, %s);\n" % (indent, cstr(k)))
            payload_code(v, out, indent)
            out.append("%smg_jw_close(w);\n" % indent)
        else:
            raise SystemExit("payload value %r not supported" % (v,))


def main():
    src, out = sys.argv[1:3]
    data = json.load(open(src))
    # mg_session_wake: a client sends it only to a device that declares mg_session_cap_wake
    # (mgcommands.h), which this one doesn't, so its exchange isn't replayed.
    data["exchanges"] = [ex for ex in data["exchanges"]
                         if not any(st.get("write", "").startswith("890b") for st in ex["steps"])]
    reg = register_params(data)
    creds = data["test_credentials"]
    bound = data["test_credentials_bound"]
    L = ["/* Generated by gen_session_vectors.py. Do not edit. */\n",
         "#include <stdbool.h>\n#include <stdint.h>\n\n#include \"mg_vm_link.h\"\n\n"]
    L.append("#define SV_MAX_MESSAGE %d\n#define SV_WINDOW %d\n" % (
        data["device_hello"]["max_message"], data["device_hello"]["window"]))
    L.append("#define SV_NODE_ID %s\n#define SV_REGISTER_ID %s\n" % (
        cstr(data["node_id"]), cstr(data["register_id"])))
    L.append("#define SV_ACCESS %s\n#define SV_REFRESH %s\n" % (
        cstr(creds["access_token"]), cstr(creds["refresh_token"])))
    seal = data.get("session_seal")
    if seal:
        # The token proof the sealed exchanges were made under (K, nonces).
        L.append("#define SV_HAS_SEAL 1\n")
        L.append("static const uint8_t sv_seal_k[] = %s;\n" % arr(bytes.fromhex(seal["K"])))
        L.append("static const uint8_t sv_seal_client_nonce[] = %s;\n" % arr(bytes.fromhex(seal["client_nonce"])))
        L.append("static const uint8_t sv_seal_device_nonce[] = %s;\n" % arr(bytes.fromhex(seal["device_nonce"])))
    L.append("static const uint8_t sv_device_key[] = %s;\n" % arr(base64.b64decode(creds["device_key"])))
    L.append("static const uint8_t sv_key_id[] = %s;\n" % arr(base64.b64decode(creds["key_id"])))
    L.append("#define SV_VM_ID %s\n#define SV_VM_KIND MG_SETUP_VM_KIND_%s\n" % (
        cstr(bound["vm_id"]), {"svm": "PLAIN", "cvm": "ATTESTED"}[bound["vm_kind"]]))
    L.append("static const struct mg_vm_identity sv_identity = {%s, %s, %s, %s, %s, %s, NULL, %s};\n\n" % (
        tuple(cstr(reg[k]) for k in ("node_id", "display_name", "platform", "version", "device_family",
                                     "model_id")) + ("true" if reg.get("is_wakeup_supported") else "false",)))
    names = list(data["test_commands"])
    for i, name in enumerate(names):
        c = data["test_commands"][name]
        params = [(k, v, True) for k, v in c["required"].items()] + \
                 [(k, v, False) for k, v in c["optional"].items()]
        L.append("static const struct mg_vm_param sv_params_%d[] = {\n" % i)
        for k, v, req in params:
            L.append("\t{%s, %s, %s, %s},\n" % (cstr(k), TYPES[v["type"]],
                                              cstr(v["description"]) if "description" in v else "NULL",
                                              "true" if req else "false"))
        if not params:
            L.append("\t{NULL, MG_VM_STRING, NULL, false},\n")
        L.append("};\n")
        L.append("static int sv_handler_%d(const struct mg_json_obj *params, struct mg_jw *w, "
                 "char *err, size_t err_len)\n{\n\t(void)params;\n\t(void)err;\n\t(void)err_len;\n" % i)
        payload_code(data["test_payloads"][name], L)
        L.append("\treturn 0;\n}\n")
    L.append("static const struct mg_vm_command sv_commands[] = {\n")
    for i, name in enumerate(names):
        c = data["test_commands"][name]
        n = len(c["required"]) + len(c["optional"])
        L.append("\t{%s, %s, sv_params_%d, %d, %d, sv_handler_%d},\n" % (
            cstr(name), cstr(c["description"]), i, n, c.get("timeout_ms", 0), i))
    L.append("};\n#define SV_COMMAND_COUNT %d\n\n" % len(names))

    L.append("struct sv_frame { const uint8_t *b; uint16_t n; };\n")
    # A step writes Control, or (event "proof") runs a new token proof with
    # the given nonces, or (event "task") starts (1) or ends (2) a task.
    L.append("struct sv_step { const uint8_t *w; uint16_t wn; const struct sv_frame *notify; "
             "uint16_t n_notify; const char *note; const uint8_t *proof_cn, *proof_dn; "
             "uint8_t task; int64_t ctr_rx, ctr_tx; };\n")
    # credentials_wait: the board's stack has no room for the sealed credentials when the hello is answered.
    L.append("struct sv_exchange { const char *name; uint16_t mtu; bool holds_token, proof, encrypted, "
             "vm_binding, task, credentials_wait; const struct sv_step *steps; uint16_t n_steps; };\n")
    for e, ex in enumerate(data["exchanges"]):
        for s, step in enumerate(ex["steps"]):
            if step.get("event") == "proof":
                L.append("static const uint8_t sv_pc_%d_%d[] = %s;\n" % (e, s, arr(bytes.fromhex(step["client_nonce"]))))
                L.append("static const uint8_t sv_pd_%d_%d[] = %s;\n" % (e, s, arr(bytes.fromhex(step["device_nonce"]))))
            elif "write" in step:
                L.append("static const uint8_t sv_w_%d_%d[] = %s;\n" % (e, s, arr(bytes.fromhex(step["write"]))))
            for k, hx in enumerate(step["notify"]):
                L.append("static const uint8_t sv_n_%d_%d_%d[] = %s;\n" % (e, s, k, arr(bytes.fromhex(hx))))
            L.append("static const struct sv_frame sv_ns_%d_%d[] = {\n" % (e, s))
            for k, hx in enumerate(step["notify"]):
                L.append("\t{sv_n_%d_%d_%d, %d},\n" % (e, s, k, len(hx) // 2))
            if not step["notify"]:
                L.append("\t{NULL, 0},\n")
            L.append("};\n")
        L.append("static const struct sv_step sv_steps_%d[] = {\n" % e)
        for s, step in enumerate(ex["steps"]):
            note = cstr(step["note"]) if "note" in step else "NULL"
            if step.get("event") == "proof":
                L.append("\t{NULL, 0, sv_ns_%d_%d, %d, %s, sv_pc_%d_%d, sv_pd_%d_%d, 0, -1, -1},\n" % (
                    e, s, len(step["notify"]), note, e, s, e, s))
            elif step.get("event") == "task":
                L.append("\t{NULL, 0, sv_ns_%d_%d, %d, %s, NULL, NULL, %d, -1, -1},\n" % (
                    e, s, len(step["notify"]), note, 1 if step["active"] else 2))
            elif step.get("event") == "counters":
                # A long session's stand-in: the device's next counters (task 3).
                L.append("\t{NULL, 0, sv_ns_%d_%d, %d, %s, NULL, NULL, 3, %dLL, %dLL},\n" % (
                    e, s, len(step["notify"]), note, step.get("rx", -1), step.get("tx", -1)))
            elif step.get("event") == "nudge":
                # The credit keepalive (task 4, mg_vm_session_nudge()).
                L.append("\t{NULL, 0, sv_ns_%d_%d, %d, %s, NULL, NULL, 4, -1, -1},\n" % (
                    e, s, len(step["notify"]), note))
            elif step.get("event") == "buffer":
                # The board's stack has room again (task 5): what waited for it goes.
                L.append("\t{NULL, 0, sv_ns_%d_%d, %d, %s, NULL, NULL, 5, -1, -1},\n" % (
                    e, s, len(step["notify"]), note))
            elif "event" in step and step["event"] != "task":
                raise SystemExit("unknown step event %r" % step["event"])
            else:
                L.append("\t{sv_w_%d_%d, %d, sv_ns_%d_%d, %d, %s, NULL, NULL, 0, -1, -1},\n" % (
                    e, s, len(step["write"]) // 2, e, s, len(step["notify"]), note))
        L.append("};\n")
    L.append("static const struct sv_exchange sv_exchanges[] = {\n")
    for e, ex in enumerate(data["exchanges"]):
        d = ex["device"]
        # A task is active (device flag task); vectors from before the flag
        # describe a gadget that always has one.
        has_flag = any("task" in x.get("device", {}) or
                       any(st.get("event") == "task" for st in x["steps"]) for x in data["exchanges"])
        L.append("\t{%s, %d, %s, %s, %s, %s, %s, %s, sv_steps_%d, %d},\n" % (
            cstr(ex["name"]), ex["mtu"], "true" if d.get("holds_token", True) else "false",
            "true" if d.get("proof", True) else "false",
            "true" if d.get("encrypted", True) else "false",
            "true" if d.get("vm_binding", False) else "false",
            "true" if d.get("task", not has_flag) else "false",
            "true" if d.get("credentials_wait", False) else "false", e, len(ex["steps"])))
    L.append("};\n#define SV_EXCHANGE_COUNT %d\n#define SV_STEP_COUNT %d\n" % (
        len(data["exchanges"]), sum(len(ex["steps"]) for ex in data["exchanges"])))
    open(out, "w").write("".join(L))


if __name__ == "__main__":
    main()
