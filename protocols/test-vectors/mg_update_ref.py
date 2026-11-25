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

"""Reference update gate for Bluetooth firmware updates over SMP (mgcommands.h, Token proof), and its test vectors.

Python standard library only. On a release build, an update change (image upload, image state write, erase, reset)
from a link whose token proof hasn't matched needs one physical press on the gadget, set up or not (any key, the talk key included; injected input never counts): the first such write
gets rc 10 (EBUSY) while the gadget prompts, the client retries, and the first write after the press gets rc 0. The
press confirms the upload whose write started the prompt and binds to its length and whole-image SHA-256 for 600 s,
across link drops, not to the link: on any link within the window that upload goes on (started again or resumed with no
new prompt, its later chunks, the state write that stages it, an erase or the reset on its link), and the state write
stages the slot only if the slot's bytes hash to the bound SHA-256 (the write's own hash is the image's, not the
whole-file SHA-256); any other upload, or one without its SHA-256 on a new link, asks again. The prompt times out after
30 s and shows at most once per 10 s per connection (a sooner write gets rc 10 with no new prompt); a cancel or a
timeout answers the next write rc 11 once and ends that prompt, binding nothing: the same upload then asks again. A
matched proof, the wired console's arm, a recovery the user's button entered and bench or automation
builds need no press; a gadget that holds no device token (not set up, or after Forget) asks for the press like a set-up
one, and a recovery the gadget entered itself refuses with rc 11. Check the vectors next to this file:

    python3 protocols/test-vectors/mg_update_ref.py

and regenerate them (the inputs are fixed, so the output never changes):

    python3 protocols/test-vectors/mg_update_ref.py --write

A device checks itself against mg-update-v2.json: given each exchange's steps in order, it answers each SMP request with
the rc listed and ends each step with the prompt listed.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
from pathlib import Path

VECTORS = Path(__file__).with_name("mg-update-v2.json")


def cbor_map(b: bytes) -> dict:
    """Decodes the CBOR an SMP request carries (a map of text keys to unsigned integers, byte or text strings and
    booleans), enough for the fields the gate reads."""
    def item(i):
        major, info = b[i] >> 5, b[i] & 31
        i += 1
        if info < 24:
            n = info
        else:
            size = {24: 1, 25: 2, 26: 4, 27: 8}[info]
            n, i = int.from_bytes(b[i:i + size], "big"), i + size
        if major == 0:
            return n, i
        if major in (2, 3):
            v = b[i:i + n]
            return (v if major == 2 else v.decode()), i + n
        if major == 5:
            out = {}
            for _ in range(n):
                k, i = item(i)
                out[k], i = item(i)
            return out, i
        if major == 7:
            return {20: False, 21: True}[info], i
        raise ValueError("unexpected CBOR")
    return item(0)[0] if b else {}


def session_ref():
    """mg_session_ref.py (next to this file), for its SMP request encoding."""
    mod = sys.modules.get("mg_session_ref")
    if mod is None:
        spec = importlib.util.spec_from_file_location("mg_session_ref", Path(__file__).with_name("mg_session_ref.py"))
        mod = importlib.util.module_from_spec(spec)
        keep, sys.dont_write_bytecode = sys.dont_write_bytecode, True
        try:
            spec.loader.exec_module(mod)
        finally:
            sys.dont_write_bytecode = keep
        sys.modules["mg_session_ref"] = mod
    return mod


S = session_ref()
SMP_READ, SMP_WRITE, SMP_GROUP_OS, SMP_GROUP_IMAGE = S.SMP_READ, S.SMP_WRITE, S.SMP_GROUP_OS, S.SMP_GROUP_IMAGE
RC_OK, RC_EBUSY, RC_EACCESSDENIED = 0, 10, 11
UPDATE_CHANGES = {(SMP_GROUP_IMAGE, 1): "image upload", (SMP_GROUP_IMAGE, 0): "image state write",
                  (SMP_GROUP_IMAGE, 5): "erase", (SMP_GROUP_OS, 5): "reset"}
PROMPT_TIMEOUT_MS = 30000
PROMPT_REARM_MS = 10000      # a new prompt at most once per this per connection (release)
CONFIRM_WINDOW_MS = 600000   # how long a press stays bound to its upload, across link drops

IMAGE = bytes(range(64))          # a stand-in image, uploaded in two chunks
IMAGE_SHA = hashlib.sha256(IMAGE).digest()   # its whole-image SHA-256 (the upload's sha)
IMAGE_HASH = hashlib.sha256(IMAGE[:48]).digest()   # its own hash, as the image list shows it and a state write names
#                                                    it (MCUboot's covers the header and body, not the trailing TLVs)
OTHER = bytes(range(64, 160))     # another image
FORGED = bytes(range(160, 224))   # bytes an upload claims are IMAGE (its length and SHA-256)
UPLOAD_0 = S.smp_request(SMP_WRITE, SMP_GROUP_IMAGE, 1, {"image": 0, "len": len(IMAGE), "off": 0, "sha": IMAGE_SHA,
                                                         "data": IMAGE[:32]})
UPLOAD_0_NO_SHA = S.smp_request(SMP_WRITE, SMP_GROUP_IMAGE, 1, {"image": 0, "len": len(IMAGE), "off": 0,
                                                                "data": IMAGE[:32]})
UPLOAD_OTHER_0 = S.smp_request(SMP_WRITE, SMP_GROUP_IMAGE, 1, {"image": 0, "len": len(OTHER), "off": 0,
                                                               "sha": hashlib.sha256(OTHER).digest(),
                                                               "data": OTHER[:32]})
UPLOAD_32 = S.smp_request(SMP_WRITE, SMP_GROUP_IMAGE, 1, {"off": 32, "data": IMAGE[32:]}, seq=1)
FORGED_0 = S.smp_request(SMP_WRITE, SMP_GROUP_IMAGE, 1, {"image": 0, "len": len(IMAGE), "off": 0, "sha": IMAGE_SHA,
                                                         "data": FORGED[:32]})
FORGED_32 = S.smp_request(SMP_WRITE, SMP_GROUP_IMAGE, 1, {"off": 32, "data": FORGED[32:]}, seq=1)
STATE_TEST = S.smp_request(SMP_WRITE, SMP_GROUP_IMAGE, 0, {"hash": IMAGE_HASH, "confirm": False}, seq=2)
RESET = S.smp_request(SMP_WRITE, SMP_GROUP_OS, 5, {}, seq=3)
ERASE = S.smp_request(SMP_WRITE, SMP_GROUP_IMAGE, 5, {"slot": 1})
IMAGE_LIST = S.smp_request(SMP_READ, SMP_GROUP_IMAGE, 0, {})


class Device:
    """The gadget's SMP update gate on one connection. Flags: release (else a bench or automation build),
    holds_token, proof (this link's proof matched), console_arm (the wired console armed it), button_recovery (a
    recovery the user's button entered), self_recovery_unprompted (a recovery it entered itself: it refuses), locked (a
    passcode lock is up). Events: press (a physical press while the prompt shows, any key, the talk key included),
    injected_press (a console or Nordic UART word, or a scripted touch: never a confirm), forget (the device token
    erased: Forget), unlock (the passcode entered), cancel
    (the user cancels the prompt), time (ms pass), reconnect (a new link: its proof and any part it had in an upload
    gone, the press's binding kept). prompt is what the gadget shows: none, keypad or confirm. slot
    is what image uploads wrote: the bytes a state write would stage."""

    def __init__(self, release=True, holds_token=True, proof=False, console_arm=False, button_recovery=False,
                 self_recovery_unprompted=False, locked=False):
        self.release, self.holds_token, self.proof = release, holds_token, proof
        self.console_arm, self.button_recovery = console_arm, button_recovery
        self.self_recovery_unprompted, self.locked = self_recovery_unprompted, locked
        self.now = 0
        self.prompt = "none"
        self.prompt_at = None     # the prompt showing since
        self.shown_at = None      # when this connection last showed a prompt (the re-arm limit)
        self.armed = None         # the change that started the prompt: (len, sha) of an upload, sha None without one
        self.refuse_next = False  # a cancel or timeout: the next update write gets rc 11
        self.bound = None         # (len, sha, when): the upload a press confirmed; with a sha it survives a link drop
        self.here = False         # this link carries the confirmed upload (or the change a press covered)
        self.slot = bytearray()

    def needs_press(self) -> bool:
        """A release build, set up or not, on a link without the proof asks for the press."""
        return self.release and not (self.proof or self.console_arm or self.button_recovery)

    @staticmethod
    def upload_identity(request: bytes):
        """(len, sha) of an image upload's first chunk (sha None if it names none), else None."""
        body = cbor_map(request[8:])
        if body.get("off") == 0 and "len" in body:
            return body["len"], body.get("sha")
        return None

    def bound_live(self) -> bool:
        return self.bound is not None and self.now - self.bound[2] < CONFIRM_WINDOW_MS

    def write_slot(self, request: bytes):
        body = cbor_map(request[8:])
        off, data = body.get("off", 0), body.get("data", b"")
        if off == 0:
            self.slot = bytearray(body.get("len", len(data)))
        self.slot[off:off + len(data)] = data

    def slot_matches(self) -> bool:
        """The slot's first bound-length bytes hash to the bound SHA-256 (a press that bound no SHA checks nothing)."""
        if self.bound is None or self.bound[1] is None:
            return True
        n = self.bound[0]
        return len(self.slot) >= n and hashlib.sha256(bytes(self.slot[:n])).digest() == self.bound[1]

    def fits(self, kind: str, ident) -> bool:
        """Whether an update write on the link that carries the confirmed upload is that upload's."""
        if kind == "upload0":
            return self.bound is not None and ident == self.bound[:2]
        return True   # its later chunks, its state write (the slot is checked), an erase or the reset

    def smp(self, request: bytes) -> int:
        op, group, command = request[0] & 7, int.from_bytes(request[4:6], "big"), request[7]
        if op != SMP_WRITE or (group, command) not in UPDATE_CHANGES:
            return RC_OK   # reads and everything else stay open
        if not self.needs_press():
            return RC_OK
        if self.self_recovery_unprompted:
            return RC_EACCESSDENIED   # no way to prompt: refused, never asked
        ident = self.upload_identity(request) if (group, command) == (SMP_GROUP_IMAGE, 1) else None
        kind = ("upload0" if ident else "chunk") if (group, command) == (SMP_GROUP_IMAGE, 1) else \
            "state" if (group, command) == (SMP_GROUP_IMAGE, 0) else "other"
        if not self.here and ident and ident[1] is not None and self.bound_live() and ident == self.bound[:2]:
            self.here = True   # the bound upload, started again on a new link: no new prompt
        if self.here:
            if self.fits(kind, ident):
                if kind == "state" and not self.slot_matches():
                    self.here, self.bound = False, None   # not the bytes the press was for: the binding ends
                    return RC_EACCESSDENIED
                if kind in ("upload0", "chunk"):
                    self.write_slot(request)
                return RC_OK
            self.here = False   # another upload: asks again
        if self.refuse_next:
            self.refuse_next = False
            return RC_EACCESSDENIED
        if self.prompt_at is not None and self.now - self.prompt_at >= PROMPT_TIMEOUT_MS:
            self.prompt, self.prompt_at = "none", None
            return RC_EACCESSDENIED   # timed out
        if self.prompt_at is None:
            if self.shown_at is not None and self.now - self.shown_at < PROMPT_REARM_MS:
                return RC_EBUSY   # the last prompt on this connection showed under 10 s ago: none yet
            self.prompt_at = self.shown_at = self.now
            self.armed = ident
            self.prompt = "keypad" if self.locked else "confirm"
        return RC_EBUSY

    def event(self, what: str, ms: int = 0):
        if what == "time":
            self.now += ms
            if self.prompt_at is not None and self.now - self.prompt_at >= PROMPT_TIMEOUT_MS:
                self.prompt, self.prompt_at, self.refuse_next = "none", None, True
        elif what == "unlock":
            self.locked = False
            if self.prompt == "keypad":
                self.prompt = "confirm"
        elif what == "press":
            if self.prompt == "confirm":
                self.prompt, self.prompt_at = "none", None
                # Bound to the armed upload (its length and SHA-256); a change that wasn't an upload naming its
                # SHA-256 binds to no image and is covered on this link only.
                self.bound = (self.armed + (self.now,)) if self.armed else None
                self.here = True
        elif what == "injected_press":
            pass   # never a confirm: only a physical press is
        elif what == "forget":
            self.holds_token = False   # the device token erased: an unproven update still asks for the press
            self.prompt, self.prompt_at = "none", None
        elif what == "cancel":
            if self.prompt != "none":
                self.prompt, self.prompt_at, self.refuse_next = "none", None, True   # rc 11 once, binds nothing
        elif what == "reconnect":
            self.proof = self.console_arm = self.here = self.refuse_next = False
            self.prompt, self.prompt_at, self.shown_at = "none", None, None
            if self.bound is not None and self.bound[1] is None:
                self.bound = None   # no SHA-256: nothing to resume by


class Recorder:
    def __init__(self, name: str, note: str, **device):
        self.dev = Device(**device)
        self.ex = {"name": name, "device": dict(device), "note": note, "steps": []}

    def smp(self, request: bytes, note: str = None):
        st = {"smp": request.hex(), "rc": self.dev.smp(request), "prompt": self.dev.prompt}
        if note:
            st["note"] = note
        self.ex["steps"].append(st)

    def event(self, what: str, note: str = None, **args):
        self.dev.event(what, **args)
        st = {"event": what, **args, "prompt": self.dev.prompt}
        if note:
            st["note"] = note
        self.ex["steps"].append(st)


def transfer(r: Recorder, note: str):
    r.smp(UPLOAD_32, note)
    r.smp(STATE_TEST)
    r.smp(RESET)


def press_exchange() -> dict:
    r = Recorder("one confirm press", "A release build, set up, a link without the proof: the first update write gets "
                 "rc 10 while the gadget asks for the press, the client retries about once a second, an injected "
                 "press confirms nothing, and the first write after a physical press (any key, the talk key "
                 "included) gets rc 0; the transfer then goes on.")
    r.smp(IMAGE_LIST, "image list (a read): rc 0, no prompt")
    r.smp(UPLOAD_0, "image upload, offset 0: rc 10, the gadget shows \"Press to confirm update\"")
    r.event("time", "1 s later", ms=1000)
    r.smp(UPLOAD_0, "the client retries: rc 10, still asking")
    r.event("injected_press", "a console or Nordic UART press word, or a scripted touch: not a confirm")
    r.smp(UPLOAD_0, "a retry: rc 10, still asking")
    r.event("press", "the user presses the gadget during the prompt (any key, the talk key included)")
    r.smp(UPLOAD_0, "the retry after the press: rc 0")
    transfer(r, "the rest of the image, the state write (test) and the reset: rc 0, authorized for the connection")
    return r.ex


def forget_exchange() -> dict:
    r = Recorder("after Forget, the press still", "A gadget whose device token is erased (Forget) holds no token: an "
                 "unproven update still needs the press, as on a gadget that was never set up.")
    r.smp(UPLOAD_0, "set up: image upload: rc 10, the prompt shows")
    r.event("forget", "the owner forgets the gadget: its device token is erased (the prompt goes)")
    r.event("time", "10 s later", ms=PROMPT_REARM_MS)
    r.smp(UPLOAD_0, "image upload: rc 10, a new prompt")
    r.event("press", "a press confirms it")
    r.smp(UPLOAD_0, "the retry: rc 0")
    transfer(r, "the rest, the state write and the reset: rc 0")
    return r.ex


def unset_press_exchange() -> dict:
    r = Recorder("a release build that isn't set up", "A release gadget that holds no device token asks for the same "
                 "one physical press; an injected press doesn't confirm.", holds_token=False)
    r.smp(UPLOAD_0, "image upload: rc 10, the prompt shows")
    r.event("injected_press", "a console or Nordic UART press word, or a scripted touch: not a confirm")
    r.smp(UPLOAD_0, "a retry: rc 10")
    r.event("press", "a physical press confirms it")
    r.smp(UPLOAD_0, "the retry: rc 0")
    transfer(r, "the rest, the state write and the reset: rc 0")
    return r.ex


def timeout_exchange() -> dict:
    r = Recorder("the prompt times out", "Nobody presses: after 30 s the prompt ends and the next update write gets "
                 "rc 11.")
    r.smp(UPLOAD_0, "image upload: rc 10, the prompt shows")
    r.event("time", "29 s later", ms=29000)
    r.smp(UPLOAD_0, "a retry: rc 10")
    r.event("time", "30 s after the prompt began: it ends", ms=1000)
    r.smp(UPLOAD_0, "a retry: rc 11")
    r.event("press", "a press now confirms nothing")
    r.smp(UPLOAD_0, "a new attempt: rc 10, a new prompt")
    return r.ex


def cancel_exchange() -> dict:
    r = Recorder("the user cancels", "A cancel on the prompt answers the next update write with rc 11.")
    r.smp(UPLOAD_0, "image upload: rc 10, the prompt shows")
    r.event("cancel", "the user cancels on the gadget")
    r.smp(UPLOAD_0, "a retry: rc 11")
    return r.ex


def no_press_exchange(name: str, note: str, **device) -> dict:
    r = Recorder(name, note, **device)
    r.smp(UPLOAD_0, "image upload: rc 0, no prompt")
    transfer(r, "the rest, the state write and the reset: rc 0")
    return r.ex


def unprompted_recovery_exchange() -> dict:
    r = Recorder("a recovery the gadget entered itself, with no way to prompt", "On a set-up gadget's release build "
                 "such a recovery refuses update writes with rc 11 and never prompts. Reads stay open.",
                 self_recovery_unprompted=True)
    r.smp(UPLOAD_0, "image upload: rc 11")
    r.smp(IMAGE_LIST, "image list: rc 0")
    return r.ex


def unset_recovery_exchange() -> dict:
    r = Recorder("a recovery the gadget entered itself, not set up", "A gadget that holds no device token refuses "
                 "update writes with rc 11 in a recovery it entered itself, as a set-up one does.", holds_token=False,
                 self_recovery_unprompted=True)
    r.smp(UPLOAD_0, "image upload: rc 11")
    r.smp(IMAGE_LIST, "image list: rc 0")
    return r.ex


def locked_exchange() -> dict:
    r = Recorder("a gadget locked with a passcode", "The passcode first, then the one confirm press.", locked=True)
    r.smp(UPLOAD_0, "image upload: rc 10, the gadget shows its keypad")
    r.event("press", "a press on the keypad screen confirms nothing")
    r.smp(UPLOAD_0, "a retry: rc 10")
    r.event("unlock", "the owner enters the passcode: the confirm prompt shows")
    r.event("press", "the press")
    r.smp(UPLOAD_0, "a retry: rc 0")
    return r.ex


def confirmed(r: Recorder):
    r.smp(UPLOAD_0, "image upload (its length and SHA-256): rc 10")
    r.event("press", "the press")
    r.smp(UPLOAD_0, "rc 0: the press is bound to this upload for 600 s")


def different_upload_exchange() -> dict:
    r = Recorder("a new link, a different upload, asks again", "The press is bound to its upload: another one on "
                 "a new link needs a new press.")
    confirmed(r)
    r.event("reconnect", "a new link")
    r.smp(UPLOAD_OTHER_0, "another image's upload: rc 10, the prompt again")
    return r.ex


def no_sha_exchange() -> dict:
    r = Recorder("a new link, an upload without its SHA-256, asks again", "A resume must name the bound image: an "
                 "upload that carries no SHA-256 on a new link needs a new press.")
    confirmed(r)
    r.event("reconnect", "a new link")
    r.smp(UPLOAD_0_NO_SHA, "the same bytes, no SHA-256: rc 10, the prompt again")
    return r.ex


def resume_exchange() -> dict:
    r = Recorder("the same upload resumes across a reconnect without a new prompt", "Within 600 s of the press the "
                 "same upload (same length and SHA-256) resumes on a new link with no prompt, and that image "
                 "completes.")
    confirmed(r)
    r.event("time", "2 min later the link drops", ms=120000)
    r.event("reconnect", "a new link")
    r.smp(UPLOAD_0, "the same upload again (same length and SHA-256): rc 0, no prompt")
    transfer(r, "the rest of the image, its state write and the reset: rc 0")
    return r.ex


def window_ends_exchange() -> dict:
    r = Recorder("the window ends after 600 s", "600 s after the press the binding ends: the same upload on a new "
                 "link asks again.")
    confirmed(r)
    r.event("time", "600 s pass", ms=CONFIRM_WINDOW_MS)
    r.event("reconnect", "a new link")
    r.smp(UPLOAD_0, "the same upload: rc 10, the prompt again")
    return r.ex


def armed_exchange() -> dict:
    r = Recorder("the press is the armed upload's", "The press confirms the upload that started the prompt. Another "
                 "upload on the same link asks again; the confirmed one is still its.")
    r.smp(UPLOAD_0, "image upload (its length and SHA-256): rc 10, the prompt shows")
    r.event("press", "the press: bound to that upload")
    r.event("time", "10 s later", ms=PROMPT_REARM_MS)
    r.smp(UPLOAD_OTHER_0, "another image's upload instead: rc 10, the prompt shows")
    r.event("cancel", "the user cancels that one")
    r.smp(UPLOAD_0, "the upload the press confirmed: rc 0, still its")
    return r.ex


def rearm_exchange() -> dict:
    r = Recorder("a new prompt waits 10 s", "A new prompt shows at most once per 10 s per connection: until then a "
                 "write that would show one gets rc 10 with no prompt; a new connection may show it at once.")
    r.smp(UPLOAD_0, "image upload: rc 10, the prompt shows")
    r.event("press", "the press: bound to that upload")
    r.smp(UPLOAD_OTHER_0, "another image's upload: rc 10, no prompt yet (the last one showed under 10 s ago)")
    r.event("time", "10 s after the last prompt", ms=PROMPT_REARM_MS)
    r.smp(UPLOAD_OTHER_0, "a retry: rc 10, the prompt shows")
    r.event("cancel", "the user cancels it")
    r.smp(UPLOAD_OTHER_0, "a retry: rc 11")
    r.event("reconnect", "a new link")
    r.smp(UPLOAD_0, "the confirmed upload resumes: rc 0")
    return r.ex


def cancel_asks_again_exchange() -> dict:
    r = Recorder("after a cancel, the same upload asks again", "A cancel answers the next update write rc 11 once and "
                 "ends that prompt; it binds nothing, so the same upload is a new request: rc 10 and a new prompt, "
                 "at the prompt rate (once per 10 s per connection).")
    r.smp(UPLOAD_0, "image upload (its length and SHA-256): rc 10, the prompt shows")
    r.event("cancel", "the user cancels on the gadget")
    r.smp(UPLOAD_0, "the retry: rc 11 (once), the prompt has ended")
    r.smp(UPLOAD_0, "the same upload again: rc 10, no prompt yet (the last one showed under 10 s ago)")
    r.event("time", "10 s after the last prompt", ms=PROMPT_REARM_MS)
    r.smp(UPLOAD_0, "the same upload: rc 10, the prompt shows again")
    r.event("press", "the press")
    r.smp(UPLOAD_0, "the retry after the press: rc 0")
    return r.ex


def forged_exchange() -> dict:
    r = Recorder("bytes that aren't the bound image don't stage", "An upload names IMAGE's length and SHA-256 but "
                 "sends other bytes. The state write stages the slot only if its bytes hash to the bound SHA-256: rc "
                 "11, and the binding ends. (A device that checks the whole-image SHA-256 as the last chunk arrives "
                 "refuses there instead.)")
    r.smp(FORGED_0, "image upload, offset 0, naming IMAGE: rc 10, the prompt shows")
    r.event("press", "the press: bound to IMAGE's length and SHA-256")
    r.smp(FORGED_0, "the retry: rc 0")
    r.smp(FORGED_32, "the last chunk: rc 0")
    r.smp(STATE_TEST, "the state write: the slot doesn't hash to the bound SHA-256: rc 11")
    return r.ex


def build() -> dict:
    return {
        "description": "Bluetooth firmware updates over SMP (mgcommands.h, Token proof). Each step is an SMP v1 "
                       "request (smp: 8-byte header, CBOR body) with the rc the device answers (0, 10 = EBUSY: "
                       "confirm on the gadget, 11 = EACCESSDENIED), or an event on the device (press: a physical "
                       "press, any key, the talk key included; injected_press: a console or Nordic UART word or a "
                       "scripted touch, never a confirm; forget: the device token erased; unlock, cancel, time with "
                       "ms, reconnect), each with prompt, what the gadget shows after it (none, "
                       "keypad, confirm). Every exchange starts on a new link without a matched proof to a release "
                       "build that holds a token unless its device flags say otherwise (release false: a bench or "
                       "automation build; holds_token false: not set up, the same press; proof; console_arm: the wired console's arm; "
                       "button_recovery: a recovery the user's button entered; self_recovery_unprompted: a recovery "
                       "the gadget entered itself with no way to prompt; locked: a passcode lock is up). A press "
                       "binds to the upload whose write started the prompt (its len and sha) for confirm_window_ms, "
                       "across reconnects; a state write stages only a slot whose bytes (the bound len of them) hash "
                       "to the bound sha (its own hash field is the image's, not the whole-image sha); a cancel "
                       "answers the next update write rc 11 once and binds nothing. "
                       "On a release build a new prompt shows at most once per prompt_rearm_ms per connection (a "
                       "sooner write that would show one gets rc 10 with no prompt). Generated by mg_update_ref.py.",
        "version": 2,
        "prompt_timeout_ms": PROMPT_TIMEOUT_MS,
        "confirm_window_ms": CONFIRM_WINDOW_MS,
        "prompt_rearm_ms": PROMPT_REARM_MS,
        "exchanges": [
            press_exchange(),
            unset_press_exchange(),
            forget_exchange(), timeout_exchange(), cancel_exchange(),
            no_press_exchange("a proven link", "A link whose proof matched needs no press.", proof=True),
            no_press_exchange("the wired console's arm", "Armed from the wired console: no press.", console_arm=True),
            no_press_exchange("a recovery the user's button entered", "The button route was the press.",
                              button_recovery=True),
            no_press_exchange("a bench or automation build", "No press.", release=False),
            unprompted_recovery_exchange(), unset_recovery_exchange(), locked_exchange(), different_upload_exchange(), no_sha_exchange(),
            resume_exchange(), window_ends_exchange(), armed_exchange(), rearm_exchange(), cancel_asks_again_exchange(),
            forged_exchange(),
        ],
    }


def render() -> str:
    return json.dumps(build(), indent=2, ensure_ascii=False) + "\n"


def replay(data: dict, device_cls=Device) -> list:
    bad = []
    for ex in data["exchanges"]:
        dev = device_cls(**ex["device"])
        for i, st in enumerate(ex["steps"]):
            if "smp" in st:
                ok = dev.smp(bytes.fromhex(st["smp"])) == st["rc"]
            else:
                dev.event(st["event"], **({"ms": st["ms"]} if "ms" in st else {}))
                ok = True
            if not ok or dev.prompt != st["prompt"]:
                bad.append(f"{ex['name']} step {i}")
    return bad


class ProofDemandingDevice(Device):
    """Refuses a link without the proof on a set-up device (rc 11), with no prompt."""

    def smp(self, request: bytes) -> int:
        op, group, command = request[0] & 7, int.from_bytes(request[4:6], "big"), request[7]
        if op == SMP_WRITE and (group, command) in UPDATE_CHANGES and self.release and self.holds_token \
                and not (self.proof or self.console_arm or self.button_recovery):
            return RC_EACCESSDENIED
        return super().smp(request)


class RepromptsSameUpload(Device):
    """Forgets the confirmed upload at a link drop, so the same upload asks again."""

    def event(self, what: str, ms: int = 0):
        super().event(what, ms)
        if what == "reconnect":
            self.bound = None


class UnpromptedDevice(Device):
    """Takes updates from any link with no press at all."""

    def needs_press(self) -> bool:
        return False


class UnsetDeviceNoPress(Device):
    """Takes an unproven update on a release gadget that holds no device token with no press: must fail."""

    def needs_press(self) -> bool:
        return self.release and self.holds_token and not (self.proof or self.console_arm or self.button_recovery)


class TakesInjectedPress(Device):
    """Takes an injected press as the confirm: must fail."""

    def event(self, what: str, ms: int = 0):
        super().event("press" if what == "injected_press" else what, ms)


class PromptsAnyTime(Device):
    """Shows a new prompt as often as writes come: must fail."""

    def smp(self, request: bytes) -> int:
        self.shown_at = None
        return super().smp(request)


class StateHashIsTheFileSha(Device):
    """Takes a state write only if its hash is the bound whole-image SHA-256 (which an MCUboot hash never is)."""

    def smp(self, request: bytes) -> int:
        if request[0] & 7 == SMP_WRITE and (int.from_bytes(request[4:6], "big"), request[7]) == (SMP_GROUP_IMAGE, 0) \
                and self.needs_press() and self.bound and cbor_map(request[8:]).get("hash") != self.bound[1]:
            return RC_EACCESSDENIED
        return super().smp(request)


class StagesAnyBytes(Device):
    """Stages whatever the slot holds."""

    def slot_matches(self) -> bool:
        return True


class CancelRefusesTheUpload(Device):
    """Keeps refusing the cancelled upload instead of asking again: must fail."""

    cancelled = None

    def event(self, what: str, ms: int = 0):
        if what == "cancel" and self.prompt != "none":
            self.cancelled = self.armed
        super().event(what, ms)

    def smp(self, request: bytes) -> int:
        if self.cancelled and (int.from_bytes(request[4:6], "big"), request[7]) == (SMP_GROUP_IMAGE, 1) \
                and self.upload_identity(request) == self.cancelled:
            return RC_EACCESSDENIED
        return super().smp(request)


class BindsTheNextUpload(Device):
    """Binds the press to whichever upload comes next, not the one that started the prompt."""

    def event(self, what: str, ms: int = 0):
        if what == "press":
            self.armed = None
        super().event(what, ms)

    def fits(self, kind: str, ident) -> bool:
        if kind == "upload0" and self.bound is None and ident:
            self.bound = ident + (self.now,)
            return True
        return super().fits(kind, ident)


MUTANTS = (ProofDemandingDevice, UnpromptedDevice, RepromptsSameUpload, StateHashIsTheFileSha, StagesAnyBytes,
           CancelRefusesTheUpload, BindsTheNextUpload, UnsetDeviceNoPress, TakesInjectedPress,
           PromptsAnyTime)


def check() -> list:
    data = json.loads(VECTORS.read_text(encoding="utf-8"))
    bad = replay(data)
    for m in MUTANTS:
        if not replay(data, m):
            bad.append(f"{m.__name__} passes every exchange")
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
        print(f"OK {VECTORS.name}: {len(data['exchanges'])} exchanges, "
              f"{sum(len(e['steps']) for e in data['exchanges'])} steps, {len(MUTANTS)} wrong devices caught")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
