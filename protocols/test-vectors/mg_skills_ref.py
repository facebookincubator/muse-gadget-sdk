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

"""Reference skills engine for mgcommands.h (Skills), and its test vectors.

Python standard library only. It plays the device side of mg_command_skill
(list, invoke, result, cancel) the way mgcommands.h describes it, and records
exchanges byte for byte. Check the vectors next to this file:

    python3 protocols/test-vectors/mg_skills_ref.py

and regenerate them (the inputs are fixed, so the output never changes):

    python3 protocols/test-vectors/mg_skills_ref.py --write

Firmware and apps check themselves against mg-skills-v2.json: a device
registers TEST_SKILLS with the fixed handler payloads and TEST_MANIFEST, must
build exactly the vector's skills_document, and must answer every write of
every exchange with exactly the notifications listed.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

VECTORS = Path(__file__).with_name("mg-skills-v2.json")

# From mgcommands.h.
MG_COMMAND_ERROR = 25
MG_COMMAND_SKILL = 0x88
LIST_REQUEST, LIST_CHUNK, INVOKE, RESULT, CANCEL = 0, 1, 2, 3, 4
ERROR_DATA_CODE = 1
UNSUPPORTED, INVALID_LENGTH, INVALID_VALUE = 1, 2, 3
LIST_CHUNK_HEADER = 6     # command, sub-command, offset, total_len
INVOKE_HEADER = 7         # command, sub-command, request_id, offset, total_len
MAX_REQUEST = 1024
MAX_RESULT = 2048
MAX_ATT_VALUE = 512       # MG_MAX_ATT_VALUE: the Core spec's attribute cap, whatever the MTU


def max_value(mtu: int) -> int:
    """The largest write or notification: min(ATT_MTU - 3, MG_MAX_ATT_VALUE)."""
    return min(mtu - 3, MAX_ATT_VALUE)

TYPES = {"string": "a string", "integer": "an integer", "number": "a number",
         "boolean": "a boolean", "object": "an object", "array": "an array"}


def compact(value) -> str:
    """JSON as the devices write it: no spaces, keys in insertion order, UTF-8."""
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def le16(v: int) -> bytes:
    return bytes([v & 0xFF, v >> 8])


def error_frame(code: int) -> bytes:
    return bytes([MG_COMMAND_ERROR, MG_COMMAND_SKILL, ERROR_DATA_CODE]) + le16(code)


def ok(payload: dict) -> str:
    return '{"ok":true,"payload":' + compact(payload) + "}"


def fail(code: str, message: str) -> str:
    return compact({"ok": False, "error": {"code": code, "message": message}})


def skill_entry(s: dict) -> dict:
    return {"name": s["name"], "description": s["description"], "required": s["required"],
            "optional": s["optional"], "timeout_ms": s["timeout_ms"]}


def skills_document(skills: list, manifest: dict | None) -> str:
    doc = {"v": 1}
    if skills:
        doc["skills"] = [skill_entry(s) for s in skills]
    if manifest:
        doc["manifest"] = {"url": manifest["url"], "revision": manifest["revision"]}
    return compact(doc)


def type_ok(kind: str, value) -> bool:
    if kind == "string":
        return isinstance(value, str)
    if kind == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if kind == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if kind == "boolean":
        return isinstance(value, bool)
    if kind == "object":
        return isinstance(value, dict)
    if kind == "array":
        return isinstance(value, list)
    return True  # unknown types are not checked


class Device:
    """The device side of mg_command_skill on one connection."""

    def __init__(self, skills: list, manifest: dict | None, payloads: dict, mtu: int):
        self.skills = {s["name"]: s for s in skills}
        self.document = skills_document(skills, manifest).encode()
        self.payloads = payloads
        self.mtu = mtu
        self.rx = None  # (request_id, total_len, bytearray) of the invoke being received

    def write(self, frame: bytes) -> list:
        if not frame or frame[0] != MG_COMMAND_SKILL:
            raise ValueError("not a skill command")
        if len(frame) < 2:
            return [error_frame(INVALID_LENGTH)]
        sub, p = frame[1], frame[2:]
        if sub == LIST_REQUEST:
            if len(p) < 2:
                return [error_frame(INVALID_LENGTH)]
            off, total = p[0] | p[1] << 8, len(self.document)
            if off > total:
                return [error_frame(INVALID_VALUE)]
            n = min(max_value(self.mtu) - LIST_CHUNK_HEADER, total - off)
            return [bytes([MG_COMMAND_SKILL, LIST_CHUNK]) + le16(off) + le16(total) +
                    self.document[off:off + n]]
        if sub == INVOKE:
            return self._invoke(p)
        if sub == CANCEL:
            if len(p) < 1:
                return [error_frame(INVALID_LENGTH)]
            if self.rx and self.rx[0] == p[0]:
                self.rx = None
            return []
        return [error_frame(UNSUPPORTED)]

    def _invoke(self, p: bytes) -> list:
        if len(p) < 5:
            return [error_frame(INVALID_LENGTH)]
        rid, off, total, data = p[0], p[1] | p[2] << 8, p[3] | p[4] << 8, p[5:]
        if off == 0 and self.rx and self.rx[0] != rid:
            # One invoke at a time: a new request is refused at its first chunk.
            return self._result(rid, fail("busy", "another skill is running"))
        if off != 0 and (not self.rx or self.rx[0] != rid):
            return []  # a request the device isn't receiving (cancelled, refused or discarded)
        if off == 0:
            self.rx = None
            if total == 0 or total > MAX_REQUEST or not data or len(data) > total:
                return [error_frame(INVALID_VALUE)]
            self.rx = (rid, total, bytearray())
        else:
            _, want_total, got = self.rx
            if total != want_total or off != len(got) or not data or off + len(data) > total:
                self.rx = None
                return [error_frame(INVALID_VALUE)]
        self.rx[2].extend(data)
        if len(self.rx[2]) < self.rx[1]:
            return []
        request = bytes(self.rx[2])
        self.rx = None
        return self._result(rid, self._run(request))

    def _run(self, request: bytes) -> str:
        try:
            req = json.loads(request.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            req = None
        if not isinstance(req, dict):
            return fail("invalid_params", "request is not a JSON object")
        name = req.get("name")
        if not isinstance(name, str):
            return fail("invalid_params", "name must be a string")
        skill = self.skills.get(name)
        if skill is None:
            return fail("unknown_skill", "no skill named " + name)
        params = req.get("params", {})
        if not isinstance(params, dict):
            return fail("invalid_params", "params must be an object")
        for group, required in (("required", True), ("optional", False)):
            for key, spec in skill[group].items():
                if key not in params:
                    if required:
                        return fail("invalid_params", "missing parameter " + key)
                    continue
                if not type_ok(spec["type"], params[key]):
                    return fail("invalid_params",
                                f"parameter {key} must be {TYPES.get(spec['type'], spec['type'])}")
        return ok(self.payloads[name])

    def _result(self, rid: int, result: str) -> list:
        data = result.encode()
        if len(data) > MAX_RESULT:
            data = fail("failed", "result too large").encode()
        room = max_value(self.mtu) - INVOKE_HEADER
        out, off = [], 0
        while True:
            chunk = data[off:off + room]
            out.append(bytes([MG_COMMAND_SKILL, RESULT, rid]) + le16(off) + le16(len(data)) + chunk)
            off += len(chunk)
            if off >= len(data):
                return out


def invoke_frames(rid: int, request: bytes, mtu: int) -> list:
    """The client's invoke chunks: each filled to min(ATT_MTU - 3, 512) - 7 bytes but the last."""
    room = max_value(mtu) - INVOKE_HEADER
    return [bytes([MG_COMMAND_SKILL, INVOKE, rid]) + le16(off) + le16(len(request)) +
            request[off:off + room] for off in range(0, len(request), room)]


def list_request(offset: int) -> bytes:
    return bytes([MG_COMMAND_SKILL, LIST_REQUEST]) + le16(offset)


def cancel(rid: int) -> bytes:
    return bytes([MG_COMMAND_SKILL, CANCEL, rid])


def reassemble(frames: list, header: int) -> bytes:
    return b"".join(f[header:] for f in frames)


# ---- the fixed inputs -------------------------------------------------------

def param(kind: str, description: str) -> dict:
    return {"type": kind, "description": description}


TEST_SKILLS = [
    {"name": "device.led",
     "description": "Set the gadget's LED colour. Values 0-255; all 0 turns it off.",
     "required": {"r": param("integer", "Red 0-255"), "g": param("integer", "Green 0-255"),
                  "b": param("integer", "Blue 0-255")},
     "optional": {"duration_ms": param("integer", "How long to show it, in ms (default 5000)")},
     "timeout_ms": 5000},
    {"name": "device.identify",
     "description": "Blink so the user can find the gadget (3 s).",
     "required": {}, "optional": {}, "timeout_ms": 5000},
    {"name": "display.message",
     "description": "Show a short caption on the gadget's screen.",
     "required": {"text": param("string", "The caption, at most 120 bytes")},
     "optional": {"seconds": param("integer", "How long to show it (default 5)")},
     "timeout_ms": 5000},
]
TEST_MANIFEST = {
    "url": "https://github.com/facebookincubator/muse-gadget-sdk/blob/0123456789ab/zephyr/skills.json",
    "revision": "1.2.3+0123456789ab",
}
# What each test skill's handler returns as its payload, whatever the params.
TEST_PAYLOADS = {
    "device.led": {"r": 255, "g": 0, "b": 0, "duration_ms": 5000},
    "device.identify": {"duration_ms": 3000},
    "display.message": {"seconds": 5},
}

LED_RED = compact({"name": "device.led", "params": {"r": 255, "g": 0, "b": 0}}).encode()


class Recorder:
    def __init__(self, name: str, mtu: int, note: str):
        self.dev = Device(TEST_SKILLS, TEST_MANIFEST, TEST_PAYLOADS, mtu)
        self.ex = {"name": name, "mtu": mtu, "note": note, "steps": []}

    def write(self, frame: bytes, note: str | None = None) -> list:
        out = self.dev.write(frame)
        step = {"write": frame.hex(), "notify": [f.hex() for f in out]}
        if note:
            step["note"] = note
        self.ex["steps"].append(step)
        return out


def list_exchange(mtu: int) -> dict:
    r = Recorder(f"list at MTU {mtu}", mtu,
                 "The client reads the skills document chunk by chunk from offset 0.")
    off = 0
    while True:
        chunk = r.write(list_request(off))[0]
        total = chunk[4] | chunk[5] << 8
        off += len(chunk) - LIST_CHUNK_HEADER
        if off >= total:
            return r.ex


def invoke_exchange(mtu: int) -> dict:
    r = Recorder(f"invoke device.led at MTU {mtu}", mtu,
                 'request_id 1: {"name":"device.led","params":{"r":255,"g":0,"b":0}}; the '
                 "result arrives as notifications after the last chunk.")
    for f in invoke_frames(1, LED_RED, mtu):
        r.write(f)
    return r.ex


def busy_exchange() -> dict:
    r = Recorder("busy at MTU 23", 23,
                 "request 2 is half sent when request 3 starts: 3 gets busy at its first chunk "
                 "and its other chunks are ignored; 2 then completes normally.")
    two = invoke_frames(2, LED_RED, 23)
    three = invoke_frames(3, compact({"name": "device.identify"}).encode(), 23)
    r.write(two[0])
    r.write(two[1])
    r.write(three[0], "first chunk of request 3: busy")
    r.write(three[1], "the rest of request 3: ignored")
    for f in two[2:]:
        r.write(f)
    return r.ex


def cancel_exchange() -> dict:
    r = Recorder("cancel at MTU 23", 23,
                 "request 4 is cancelled half way: nothing is sent, its later chunks are "
                 "ignored, and request 5 then runs.")
    four = invoke_frames(4, LED_RED, 23)
    r.write(four[0])
    r.write(four[1])
    r.write(cancel(4), "cancel: no reply")
    r.write(four[2], "a chunk of the cancelled request: ignored")
    r.write(cancel(9), "cancel of a request the device doesn't hold: ignored")
    for f in invoke_frames(5, compact({"name": "device.identify", "params": {}}).encode(), 23):
        r.write(f)
    return r.ex


def errors_exchange() -> dict:
    r = Recorder("errors at MTU 247", 247, "Each step stands alone.")
    reqs = [
        (10, '{"name":"device.fly","params":{}}', "unknown skill"),
        (11, '{"name":"device.led",', "not JSON"),
        (12, '["device.led"]', "not an object"),
        (13, '{"name":7}', "name not a string"),
        (14, '{"name":"device.led","params":[1,2,3]}', "params not an object"),
        (15, '{"name":"device.led","params":{"r":1,"g":2}}', "a required parameter missing"),
        (16, '{"name":"device.led","params":{"r":"red","g":0,"b":0}}', "a parameter of the wrong type"),
        (17, '{"name":"device.led","params":{"r":1.5,"g":0,"b":0}}', "a fraction is not an integer"),
        (18, '{"name":"device.led","params":{"r":0,"g":0,"b":0,"duration_ms":true}}',
         "an optional parameter of the wrong type"),
        (19, '{"name":"display.message","params":{"text":5}}', "a string parameter given a number"),
        (20, '{"name":"display.message","params":{"text":"hi","extra":1}}',
         "parameters the skill doesn't declare are ignored"),
        (21, '{"params":{"r":0},"name":"device.identify"}', "member order doesn't matter"),
    ]
    for rid, text, note in reqs:
        for f in invoke_frames(rid, text.encode(), 247):
            r.write(f, note)
    # Framing errors.
    two = bytes([MG_COMMAND_SKILL, INVOKE, 30]) + le16(0) + le16(4) + b"{}"
    r.write(two, "first half of a 4-byte request")
    r.write(bytes([MG_COMMAND_SKILL, INVOKE, 30]) + le16(3) + le16(4) + b"}",
            "a chunk at the wrong offset: error, the request is discarded")
    r.write(bytes([MG_COMMAND_SKILL, INVOKE, 30]) + le16(2) + le16(4) + b"{}",
            "the right chunk, too late: ignored")
    r.write(bytes([MG_COMMAND_SKILL, INVOKE, 31]) + le16(0) + le16(4) + b"{}")
    r.write(bytes([MG_COMMAND_SKILL, INVOKE, 31]) + le16(2) + le16(5) + b"{}",
            "total_len changed between chunks")
    r.write(bytes([MG_COMMAND_SKILL, INVOKE, 32]) + le16(0) + le16(4) + b"{}{}{}",
            "a chunk past total_len")
    r.write(bytes([MG_COMMAND_SKILL, INVOKE, 33]) + le16(0) + le16(MAX_REQUEST + 1) + b"{",
            "total_len over the 1024-byte limit")
    r.write(bytes([MG_COMMAND_SKILL, INVOKE, 34]) + le16(0) + le16(0), "total_len 0")
    r.write(bytes([MG_COMMAND_SKILL, INVOKE, 35]) + le16(0) + le16(2), "an empty first chunk")
    r.write(bytes([MG_COMMAND_SKILL, INVOKE, 36, 0]), "invoke too short")
    r.write(bytes([MG_COMMAND_SKILL, LIST_REQUEST, 0]), "list_request too short")
    r.write(list_request(len(r.dev.document) + 1), "list offset past the document")
    r.write(list_request(len(r.dev.document)), "list offset at the end: an empty chunk")
    r.write(bytes([MG_COMMAND_SKILL, CANCEL]), "cancel too short")
    r.write(bytes([MG_COMMAND_SKILL, RESULT, 1]) + le16(0) + le16(2) + b"{}",
            "a device->client sub-command from the client")
    r.write(bytes([MG_COMMAND_SKILL, 9]), "an unknown sub-command")
    r.write(bytes([MG_COMMAND_SKILL]), "no sub-command")
    return r.ex


def build() -> dict:
    doc = skills_document(TEST_SKILLS, TEST_MANIFEST)
    return {
        "description": "Skills (protocols/mgcommands.h, Skills). Hex strings are the Control payloads "
                       "(write: client -> device, notify: what the device sends back, in order; an "
                       "empty list means nothing). Register test_skills with test_payloads as each "
                       "handler's payload and test_manifest as the manifest: the device must build "
                       "exactly skills_document, and every exchange starts on a new connection at "
                       "the given ATT MTU. Generated by mg_skills_ref.py.",
        "version": 2,
        "command": MG_COMMAND_SKILL,
        "max_request": MAX_REQUEST,
        "max_result": MAX_RESULT,
        "test_skills": TEST_SKILLS,
        "test_manifest": TEST_MANIFEST,
        "test_payloads": {k: compact(v) for k, v in TEST_PAYLOADS.items()},
        "skills_document": doc,
        "exchanges": [list_exchange(23), list_exchange(247), invoke_exchange(23),
                      invoke_exchange(247), busy_exchange(), cancel_exchange(), errors_exchange()],
    }


def render() -> str:
    return json.dumps(build(), indent=2, ensure_ascii=False) + "\n"


def check() -> list:
    """Replays every exchange in the file through the reference device; returns the problems."""
    data = json.loads(VECTORS.read_text(encoding="utf-8"))
    bad = []
    payloads = {k: json.loads(v) for k, v in data["test_payloads"].items()}
    if skills_document(data["test_skills"], data["test_manifest"]) != data["skills_document"]:
        bad.append("skills_document")
    for ex in data["exchanges"]:
        dev = Device(data["test_skills"], data["test_manifest"], payloads, ex["mtu"])
        for i, step in enumerate(ex["steps"]):
            frame = bytes.fromhex(step["write"])
            if len(frame) > ex["mtu"] - 3:
                bad.append(f"{ex['name']} step {i}: write longer than ATT_MTU - 3")
            got = [f.hex() for f in dev.write(frame)]
            if got != step["notify"]:
                bad.append(f"{ex['name']} step {i}")
            for f in got:
                if len(f) // 2 > ex["mtu"] - 3:
                    bad.append(f"{ex['name']} step {i}: notification longer than ATT_MTU - 3")
    if VECTORS.read_text(encoding="utf-8") != render():
        bad.append("file differs from the generator's output")
    return bad


def main() -> int:
    if sys.argv[1:] == ["--write"]:
        VECTORS.write_text(render(), encoding="utf-8")
        print(f"wrote {VECTORS}")
        return 0
    if sys.argv[1:]:
        print(__doc__)
        return 2
    bad = check()
    for b in bad:
        print(f"FAIL {b}")
    if not bad:
        data = json.loads(VECTORS.read_text(encoding="utf-8"))
        steps = sum(len(e["steps"]) for e in data["exchanges"])
        print(f"OK {VECTORS.name}: {len(data['exchanges'])} exchanges, {steps} steps")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
