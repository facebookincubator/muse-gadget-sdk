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
"""Layer 2 (mg_device_session) end to end: the C gadget (sim_session.c, built
with ASan + UBSan by run.sh) against a client made of the references:
protocols/test-vectors/mg_session_seal_ref.py (the seal) and
protocols/test-vectors/mg_session_ref.py (the exchanges). Byte-exact
where the vectors apply to a credentials-only device; the rest checks the
protocol rules (mgcommands.h). With MGDS_DATA=1 every
exchange is replayed byte for byte through the data path (DataVectors), with
the re-ask series timed on the host clock (ReAsk) and sealed data both ways
(DataPath).

    MGDS_SIM=build/sim_session [MGDS_SEAL_DIR=...] [MGDS_FRAME_MAX=n] [MGDS_DATA=1] python3 tests/test_session.py
"""
import hashlib
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
# protocols/test-vectors (the seal reference and vectors); a vendored copy
# points this at its own copy of them.
SEAL_DIR = Path(os.environ.get("MGDS_SEAL_DIR", str(REPO / "protocols/test-vectors")))
sys.path.insert(0, str(SEAL_DIR))
import mg_session_ref as ref  # noqa: E402
import mg_session_seal_ref as seal  # noqa: E402

SIM = os.environ.get("MGDS_SIM", str(HERE / "build/sim_session"))
FRAME_MAX = int(os.environ.get("MGDS_FRAME_MAX", "244"))   # the build's MGDS_FRAME_MAX
MIN_MTU = 100   # MG_MIN_ATT_MTU: a sealed hello on a smaller link gets invalid_value


def opened_at(s, mtu):
    """Once a sealed session is open (at MG_MIN_ATT_MTU or more): the link's MTU the test runs at from now on. Below
    MG_MIN_ATT_MTU it stands in for a board whose fragment buffer (MGDS_FRAME_MAX) is that small, where the device's
    sealed data and clear answers take more than one write at any MTU (as in the MGDS_FRAME_MAX=20 build); a sealed
    credentials_saved takes more than one only below 39, which no sealed session's MTU is."""
    if mtu < MIN_MTU:
        s.cmd(f"mtu {mtu}")
SEALED_DATA = os.environ.get("MGDS_SEALED_DATA", "1") != "0"   # the build's MGDS_SEALED_DATA
TAG_LEND = os.environ.get("MGDS_TAG_LEND", "0") == "1"          # the build's MGDS_TAG_LEND
KEEP_KEYS = os.environ.get("MGDS_KEEP_KEYS", "1") != "0"        # the build's MGDS_KEEP_KEYS
END_ON_BAD_TAG = os.environ.get("MGDS_END_ON_BAD_TAG", "0") == "1"   # the build's MGDS_END_ON_BAD_TAG
TX_CTR_U8 = os.environ.get("MGDS_TX_CTR_U8", "0") == "1"              # the build's MGDS_TX_CTR_U8
DATA_PATH = os.environ.get("MGDS_DATA", "0") == "1"                   # the build's MGDS_DATA (the data path)
STALL = os.environ.get("MGDS_STALL", "0") == "1"                      # the build's MGDS_STALL (layer 2's stall rules)
GIVE_UP_LATER = os.environ.get("MGDS_GIVE_UP_LATER", "0") == "1"      # the build's MGDS_GIVE_UP_LATER
ASK_QUIET = os.environ.get("MGDS_ASK_QUIET", "0") == "1"              # the build's MGDS_ASK_QUIET
EXCH = json.loads((SEAL_DIR / "mg-session-v1.json").read_text())
SESSION_SEAL = {k: bytes.fromhex(v) for k, v in EXCH["session_seal"].items()}
# Vectors with the device flag / event task start each device with no active task; older ones
# describe a device that always has one.
VEC_TASKS = any("task" in ex["device"] or any(st.get("event") == "task" for st in ex["steps"])
                for ex in EXCH["exchanges"])
SEALV = json.loads((SEAL_DIR / "mg-session-seal-v1.json").read_text())

HELLO, STATE, DATA, CREDIT, ABORT, CREDS, UPDATE, SAVED = 0, 1, 3, 4, 5, 6, 7, 8
FIRST, LAST, SEALED = 1, 2, 4
K = bytes(range(32))
NODE = "homelink-9e2a68"
ACCESS = "eyJhbGciOiJFUzI1NiJ9.access-0001." + "a" * 600
REFRESH = "mgr_refresh_0001_" + "b" * 120
CN1, DN1 = bytes(range(16)), bytes(range(0xF0, 0x100))
CN2, DN2 = bytes(range(0x40, 0x50)), bytes(range(0x80, 0x90))
# max 4096, window 4096; sealed | sealed_saved: a client that counts sealed answers (the sealed
# credentials_saved only for a hello with mg_session_hello_flag_sealed_saved, echoed)
SEALED_HELLO = bytes.fromhex("8900010010000000100000") + b"\x03"
# sealed only: a client that predates sealed answers (the one-byte credentials_saved, no device counter, give-ups silent)
LEGACY_HELLO = bytes.fromhex("8900010010000000100000") + b"\x01"
PLAIN_HELLO = bytes.fromhex("8900010000010000000100")              # the vectors' unsealed hello


def err(code):
    return bytes([25, 0x89, 1, code, 0])


class Sim:
    def __init__(self, *setup):
        self.p = subprocess.Popen([SIM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                  stderr=subprocess.PIPE)
        self.events = []
        for c in (f"k {K.hex()}", f"node {NODE}", f"access {ACCESS}", f"refresh {REFRESH}") + setup:
            self.cmd(c)

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        out = []
        while True:
            s = self.p.stdout.readline()
            if not s:
                raise AssertionError("sim exited: " + self.p.stderr.read())
            s = s.rstrip("\n")
            if s == ".":
                break
            if s.startswith("E "):
                raise AssertionError("sim: " + s)
            out.append(s)
        self.events += out
        return out

    def frames(self, line):
        return [bytes.fromhex(s[2:]) for s in self.cmd(line) if s.startswith("N ")]

    def w(self, frame: bytes):
        return self.frames("w " + frame.hex())

    def close(self):
        self.p.stdin.close()
        rc = self.p.wait(30)
        assert rc == 0, self.p.stderr.read()


def creds_plain(access=ACCESS, refresh=REFRESH, node=NODE) -> bytes:
    return seal.creds_json(access, refresh, node)


def reassemble(frames, mtu, sealed, sub=None):
    """The credentials message (or sub's, framed as credentials) from its fragments, checking the framing."""
    msg, total = b"", None
    for i, f in enumerate(frames):
        assert f[0] == 0x89 and f[1] == (CREDS if sub is None else sub), f.hex()
        assert len(f) <= mtu - 3, (len(f), mtu)
        flags = f[2]
        assert bool(flags & SEALED) == sealed
        assert bool(flags & FIRST) == (i == 0)
        if i == 0:
            total = int.from_bytes(f[3:7], "little")
            msg += f[7:]
        else:
            msg += f[3:]
        assert bool(flags & LAST) == (i == len(frames) - 1)
        assert len(f) > (7 if i == 0 else 3)
    assert len(msg) == total
    return msg


def update_frames(payload: bytes, mtu: int, sealed: bool):
    out = []
    for f in ref.fragments(payload, mtu, UPDATE):
        f = bytearray(f)
        if sealed:
            f[2] |= SEALED
        out.append(bytes(f))
    return out


def sealed_update(cn, dn, counter, access, refresh, node=NODE, extra=""):
    e, m = seal.seal_keys(K, cn, dn, node)
    body = json.dumps({"access_token": access, "refresh_token": refresh}, separators=(",", ":"))
    if extra:
        body = body[:-1] + "," + extra + "}"
    return seal.seal(e, m, seal.CLIENT_TO_DEVICE, counter, body.encode())


def saved(counter, ok, refresh, cn=CN1, dn=DN1, mtu=247, node=NODE, k=K):
    """A sealed credentials_saved as the device frames it: the reference's plaintext [ok ||
    SHA-256(refresh)[:8]] (zeros when not saved) sealed under device -> client counter `counter`, fragmented as
    credentials to the MTU (one write at MTU >= 39 whatever the build's fragment buffer)."""
    e, m = seal.seal_keys(k, cn, dn, node)
    blob = seal.seal(e, m, seal.DEVICE_TO_CLIENT, counter, seal.saved_plaintext(ok, refresh))
    return seal.fragments(SAVED, blob, True, mtu - 3)


def saved_pt(frames, counter, cn, dn, node, k):
    """The plaintext of a sealed credentials_saved's frames (None if they don't open under that counter)."""
    blob = b"".join([frames[0][7:]] + [f[3:] for f in frames[1:]])
    if int.from_bytes(frames[0][3:7], "little") != len(blob):
        return None
    e, m = seal.seal_keys(k, cn, dn, node)
    return seal.unseal(e, m, seal.DEVICE_TO_CLIENT, counter, blob)


CLEAR = 12   # mg_session_clear (mgcommands.h)
CAP_SEALED_CLEAR = os.environ.get("MGDS_CAP_SEALED_CLEAR", "0") == "1"   # the build's MGDS_CAP_SEALED_CLEAR


def clear_frames(counter, mtu=247, plain=b"\x00", cn=CN1, dn=DN1):
    e, m = seal.seal_keys(K, cn, dn, NODE)
    return seal.fragments(CLEAR, seal.seal(e, m, seal.CLIENT_TO_DEVICE, counter, plain), True, mtu - 3)


def clear_answer(frames, counter, cn=CN1, dn=DN1):
    """The plaintext of the device's sealed clear answer (None if it doesn't open under that counter)."""
    assert frames and all(f[:2] == bytes([0x89, CLEAR]) and f[2] & SEALED for f in frames), [f.hex() for f in frames]
    blob = b"".join([frames[0][7:]] + [f[3:] for f in frames[1:]])
    assert int.from_bytes(frames[0][3:7], "little") == len(blob) == 21
    e, m = seal.seal_keys(K, cn, dn, NODE)
    return seal.unseal(e, m, seal.DEVICE_TO_CLIENT, counter, blob)


class SealedClear(unittest.TestCase):
    """mg_session_clear (mgcommands.h): the Forget only sealed. The vectors' 'sealed clear'
    exchange runs in Exchanges/DataVectors; these are the rest: fragments, the erase failing, a full stack, a clear
    that drops the credentials not started or waits behind them part way, a lost link and no port clear."""

    def session(self, mtu=247, *extra):
        s = Sim(f"mtu {max(mtu, MIN_MTU)}", f"proof {CN1.hex()} {DN1.hex()}", *extra)
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
        opened_at(s, mtu)
        return s

    def send(self, s, frames):
        out = []
        for f in frames:
            out += s.w(f)
        return out

    def test_fragmented_at_mtu_23(self):
        s = self.session(23)
        frames = clear_frames(0, 23)
        self.assertGreater(len(frames), 1)
        out = self.send(s, frames)
        self.assertIn("CL 1", s.events)
        self.assertEqual(clear_answer(out, 1), b"\x01")
        self.assertTrue(all(len(f) <= 20 for f in out))
        self.assertIn("busy=0", s.cmd("show")[0])
        self.assertEqual(s.w(SEALED_HELLO), [err(5)], "no token after the clear: not_found")
        s.close()

    def test_erase_fails(self):
        s = self.session(247, "clearfail 1")
        out = self.send(s, clear_frames(0))
        self.assertIn("CL 0", s.events)
        self.assertEqual(clear_answer(out, 1), b"\x00", "the erase failed: sealed [0]")
        self.assertIn(f"access={ACCESS}", s.cmd("show")[0], "nothing erased")
        s.close()

    def test_pending_erase_then_done_erased(self):
        s = self.session(247, "clearpend 1")
        out = self.send(s, clear_frames(0))
        self.assertIn("CL pend", s.events)
        self.assertEqual(out, [], "the erase runs on: no answer yet")
        self.assertIn("busy=1", s.cmd("show")[0])
        self.assertEqual(s.w(bytes([0x89, 4, 1, 0, 0, 0])), [err(3)], "pending: other frames refused")
        self.assertEqual(s.w(SEALED_HELLO), [err(3)], "pending: a hello too")
        self.assertEqual(s.frames("poll 10"), [], "nothing goes out while it runs")
        out = s.frames("cleardone 1")
        self.assertEqual(clear_answer(out, 1), b"\x01", "erased: sealed [1], the next device counter")
        self.assertIn("busy=0", s.cmd("show")[0])
        self.assertEqual(s.w(SEALED_HELLO), [err(5)], "no token after the clear: not_found")
        s.close()

    def test_pending_erase_then_done_failed(self):
        s = self.session(247, "clearpend 1")
        self.assertEqual(self.send(s, clear_frames(0)), [])
        out = s.frames("cleardone 0")
        self.assertEqual(clear_answer(out, 1), b"\x00", "the erase failed: sealed [0]")
        self.assertIn(f"access={ACCESS}", s.cmd("show")[0], "nothing erased")
        self.assertIn("busy=0", s.cmd("show")[0])
        s.close()

    def test_pending_erase_link_drop(self):
        s = self.session(247, "clearpend 1")
        self.assertEqual(self.send(s, clear_frames(0)), [])
        self.assertEqual(s.frames("reset 0"), [], "the link dropped: nothing sent")
        self.assertIn("busy=0", s.cmd("show")[0])
        self.assertEqual(s.frames("cleardone 1"), [], "the erase still completes; no session, no answer")
        self.assertEqual(s.w(SEALED_HELLO), [err(5)], "erased: not_found on the next hello")
        s.close()

    def test_pending_erase_answer_waits_for_a_buffer(self):
        s = self.session(247, "clearpend 1")
        self.assertEqual(self.send(s, clear_frames(0)), [])
        s.cmd("again 1")
        self.assertEqual(s.frames("cleardone 1"), [], "no buffer: the answer waits")
        out = s.frames("poll 10")
        self.assertEqual(clear_answer(out, 1), b"\x01")
        s.close()

    def test_no_stack_buffer_then_poll(self):
        s = self.session()
        s.cmd("again 1")
        out = self.send(s, clear_frames(0))
        self.assertIn("CL 1", s.events)
        self.assertEqual(out, [], "no buffer: the answer waits")
        self.assertIn("busy=1", s.cmd("show")[0])
        self.assertEqual(s.w(bytes([0x89, 4, 1, 0, 0, 0])), [err(3)], "the session is ending: other writes refused")
        out = s.frames("poll 10")
        self.assertEqual(clear_answer(out, 1), b"\x01")
        self.assertIn("busy=0", s.cmd("show")[0])
        s.close()

    def test_drops_the_credentials_not_started(self):
        # mgcommands.h: a device message not yet started never goes out, its counter never used: the credentials'
        # counter (0) goes back and the answer is sealed on it, the client's next device counter.
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "budget 1")
        hello_out = s.w(SEALED_HELLO)                    # only the hello goes; the credentials wait for the stack
        self.assertEqual(len(hello_out), 1)
        out = self.send(s, clear_frames(0))
        self.assertEqual(out, [], "no buffer: the answer waits")
        self.assertIn("CL 1", s.events, "erased at once: no credentials go before it")
        s.cmd("budget -1")
        out = s.frames("pump")
        self.assertEqual([f for f in out if f[1] == CREDS], [], "the credentials never go out")
        self.assertIsNone(clear_answer(out, 1), "not on counter 1: that would leave a hole the client never fills")
        self.assertEqual(clear_answer(out, 0), b"\x01", "sealed on counter 0: the dropped credentials never used it")
        self.assertEqual(s.events.count("CL 1"), 1, "erased once")
        self.assertIn("busy=0", s.cmd("show")[0])
        self.assertEqual(s.w(SEALED_HELLO), [err(5)], "erased: not_found on the next hello")
        s.close()

    def test_waits_behind_the_credentials_part_way(self):
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}", "budget 2")   # hello and one credentials fragment
        hello_out = s.w(SEALED_HELLO)
        self.assertEqual([f[1] for f in hello_out], [HELLO, CREDS])
        out = self.send(s, clear_frames(0, 100))
        self.assertEqual(out, [], "taken, but the credentials part way out go first")
        self.assertNotIn("CL 1", s.events, "they are read from the stored pair: the erase waits behind them")
        s.cmd("budget -1")
        out = s.frames("pump")
        creds = [f for f in out if f[1] == CREDS]
        self.assertTrue(creds and creds[-1][2] & LAST)
        self.assertIn("CL 1", s.events)
        self.assertEqual(clear_answer([f for f in out if f[1] == CLEAR], 1), b"\x01")
        self.assertLess(out.index(creds[-1]), next(i for i, f in enumerate(out) if f[1] == CLEAR))
        s.close()

    def test_link_lost_before_it_runs_still_erases(self):
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}", "budget 2")   # the credentials part way out
        s.w(SEALED_HELLO)
        self.send(s, clear_frames(0, 100))
        self.assertNotIn("CL 1", s.events)
        s.w(bytes([0x89, 7, 0]))                         # refused update writes don't cancel it
        s.w(update_frames(b'{"access_token":"x","refresh_token":"y"}', 100, False)[0])
        s.cmd("reset 0")                                 # a disconnect before the credentials were out
        self.assertIn("CL 1", s.events, "the authenticated Forget still runs")
        s.close()

    def test_no_port_clear(self):
        s = self.session(247, "clearcb 0")
        self.assertEqual(self.send(s, clear_frames(0)), [err(1)], "unsupported, no counter taken")
        self.send(s, update_frames(sealed_update(CN1, DN1, 0, "acc-2", "ref-2"), 247, True))
        self.assertIn("access=acc-2", s.cmd("show")[0], "counter 0 was still free")
        s.close()

    @unittest.skipUnless(DATA_PATH, "an MGDS_DATA build")
    def test_waits_for_a_data_message_part_way(self):
        # Erased when its tag checks; only the sealed answer waits for the data message's last fragment. Without
        # MGDS_KEEP_KEYS the data's fragments are sealed under keys derived from K, so the erase waits with it.
        s = self.session()
        s.w(STATE_OPEN)
        s.frames("pump")
        s.cmd("budget 2")
        lines = s.cmd("send " + bytes(1500).hex())              # two fragments go, the rest waits for the stack
        sent = [bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")]
        self.assertTrue(sent and sent[0][1] == DATA and sent[0][2] & FIRST)
        out = self.send(s, clear_frames(0))
        self.assertEqual([f for f in out if f[1] == CLEAR], [], "never between a data message's fragments")
        self.assertEqual("CL 1" in s.events, KEEP_KEYS, "erased at once (kept keys)")
        s.cmd("budget -1")
        out = s.frames("poll 10")
        kinds = [f[1] for f in out]
        self.assertIn(CLEAR, kinds)
        self.assertTrue(all(k == DATA for k in kinds[:kinds.index(CLEAR)]), "the data message finishes first")
        self.assertTrue(out[kinds.index(CLEAR) - 1][2] & LAST)
        self.assertEqual(s.events.count("CL 1"), 1, "erased once")
        self.assertEqual(clear_answer([f for f in out if f[1] == CLEAR], 2), b"\x01", "counter 2: credentials 0, data 1")
        s.close()

    def test_erased_while_an_answer_goes_out(self):
        """The erase runs the moment the clear's tag checks; only its sealed answer waits for what is already going
        out (a sealed credentials_saved part-way at MTU 23), so a link lost in between leaves the gadget erased, and
        nothing erases twice. A pending erase that ends meanwhile is answered behind it. Without MGDS_KEEP_KEYS the
        answer's keys come from K, so the erase waits with it."""
        for how in ("link lost", "answered", "pending") if KEEP_KEYS else ("link lost", "answered"):
            s = self.session(23, *(["clearpend 1"] if how == "pending" else []))
            s.cmd("budget 1")                                    # the saved answer's first fragment only
            out = self.send(s, update_frames(sealed_update(CN1, DN1, 0, "acc-2", "ref-2"), 23, True))
            self.assertEqual([f[1] for f in out], [SAVED], how)
            self.assertIn("access=acc-2", s.cmd("show")[0])
            self.assertEqual(self.send(s, clear_frames(1, 23)), [], "nothing between the answer's fragments")
            erased = "CL pend" if how == "pending" else "CL 1"
            self.assertEqual(erased in s.events, KEEP_KEYS, how)
            if how == "pending":
                s.cmd("cleardone 1")                             # the erase ends before the answer's turn
            if KEEP_KEYS:
                self.assertIn("access= refresh=", s.cmd("show")[0], "erased before its answer")
                self.assertEqual(self.send(s, update_frames(sealed_update(CN1, DN1, 2, "a3", "r3"), 23, True)[:1]),
                                 [], "an update meanwhile is refused (no buffer for the error now): the answer "
                                     "stays owed")
            s.cmd("budget -1")
            if how == "link lost":
                s.cmd("reset 0")
                self.assertEqual(s.events.count("CL 1"), 1, "erased once")
                self.assertEqual(s.w(SEALED_HELLO), [err(5)], "not set up any more: not_found")
            else:
                out = s.frames("pump")
                kinds = [f[1] for f in out]
                self.assertEqual(kinds[0], SAVED, how)
                self.assertEqual(clear_answer([f for f in out if f[1] == CLEAR], 2), b"\x01", how)
                self.assertEqual(s.events.count(erased), 1, how)
            s.close()

    def test_fragments_without_storage_are_busy(self):
        s = self.session(23, "storage 0")
        frames = clear_frames(0, 23)
        self.assertEqual(s.w(frames[0]), [err(4)], "no lent storage: busy, its counter used")
        s.cmd("storage 1")
        self.assertEqual(self.send(s, frames), [err(3)] * 2, "the same counter again: refused (both fragments)")
        out = self.send(s, clear_frames(1, 23))
        self.assertEqual(clear_answer(out, 1), b"\x01", "the retry on the next counter")
        self.assertIn("borrowed=0", s.cmd("show")[0], "the storage went back")
        s.close()

    def test_busy_while_an_update_arrives(self):
        s = self.session(23)
        upd = update_frames(sealed_update(CN1, DN1, 0, "acc-2", "ref-2"), 23, True)
        s.w(upd[0])                                           # an update part-way holds the storage
        self.assertEqual(s.w(clear_frames(1, 247)[0]), [err(4)], "busy, its counter used")
        for f in upd[1:]:
            s.w(f)
        s.frames("pump")
        self.assertIn("access=acc-2", s.cmd("show")[0])
        out = self.send(s, clear_frames(2, 247))
        self.assertEqual(clear_answer([f for f in out if f[1] == CLEAR], 2), b"\x01")
        s.close()

    def test_second_clear_and_junk(self):
        s = self.session()
        frames = clear_frames(0, 23)        # two fragments
        self.assertEqual(s.w(frames[1]), [err(3)], "a continuation with no clear arriving")
        self.assertEqual(s.w(bytes([0x89, CLEAR])), [err(2)], "no flags")
        self.assertEqual(s.w(bytes([0x89, CLEAR, 7, 21, 0, 0, 0, 0, 0])), [err(2)], "under the 4-byte counter")
        self.assertEqual(s.w(bytes([0x89, CLEAR, 7, 22, 0, 0, 0, 0, 0, 0, 0])), [err(3)], "total not 21")
        out = self.send(s, clear_frames(0))
        self.assertEqual(clear_answer(out, 1), b"\x01")
        s.close()


class Gating(unittest.TestCase):
    """An unsealed hello on any link, and recovery mode (the vectors' no token, no proof and not encrypted
    exchanges run in Exchanges / DataVectors)."""

    def test_unsealed_hello_refused_on_any_link(self):
        # An unsealed hello is refused insecure on an encrypted link too (a relayed proof
        # plus the relay's own bond); the proof's nonces stay untaken, so the sealed hello after it opens.
        for enc in (1, 0):
            s = Sim("mtu 247", f"enc {enc}", f"proof {CN1.hex()} {DN1.hex()}")
            self.assertEqual(s.w(PLAIN_HELLO), [err(7)])
            self.assertEqual(s.frames("pump") + s.frames("poll 1000"), [])
            self.assertIn("takes=0", s.cmd("show")[0])
            body = b'{"access_token":"evil-access","refresh_token":"evil-refresh"}'
            self.assertEqual(sum((s.w(f) for f in update_frames(body, 247, False)), []), [err(3)])
            self.assertIn(f"access={ACCESS} refresh={REFRESH}", s.cmd("show")[0])
            out = s.w(SEALED_HELLO) + s.frames("pump")
            self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
            self.assertTrue(all(f[2] & SEALED for f in out[1:]), "the credentials go only sealed")
            self.assertFalse(any(ACCESS.encode()[:24].hex() in e for e in s.events), "no token byte in the clear")
            s.close()

    def test_recovery(self):
        s = Sim("enc 1", f"proof {CN1.hex()} {DN1.hex()}", "recovery 1")
        self.assertEqual(s.w(PLAIN_HELLO), [err(8)])
        self.assertEqual(s.w(SEALED_HELLO), [err(8)])
        s.close()


def vector_sim(ex, *extra):
    """A sim set up as the vectors' device for this exchange: their test credentials (bound ones are this layer's
    pair; it hands over no device key), node_id, session_seal's K and proof nonces, and the exchange's device flags."""
    dev = ex["device"]
    tc = EXCH["test_credentials"]
    setup = [f"mtu {ex['mtu']}", "hello 16384 4096", "expiry 1", "exp 0", f"node {EXCH['node_id']}",
             f"k {SESSION_SEAL['K'].hex()}", f"access {tc['access_token']}", f"refresh {tc['refresh_token']}",
             f"proof {SESSION_SEAL['client_nonce'].hex()} {SESSION_SEAL['device_nonce'].hex()}",
             "enc 0" if dev.get("encrypted") is False else "enc 1"]
    if dev.get("holds_token") is False:
        setup.append("nopair")
    if dev.get("proof") is False:
        setup.append("unproof")
    if DATA_PATH and VEC_TASKS and not dev.get("task"):
        setup.append("task 0")      # the vectors' device starts with no active task
    if dev.get("credentials_wait"):
        setup.append("credswait 1")  # no room for the credentials until the buffer event
    return Sim(*(setup + list(extra)))


def our_sealed_creds_frames(vec_frames, mtu, nonces):
    """The reference's sealed credentials rewritten for this device: the same JSON without the device key fields
    (this layer hands over node_id, the pair and access_expires_at; mgcommands.h lets a device set up without a device
    key leave them out), sealed on counter 0 under the latest proof, fragmented at the build's fragment buffer."""
    e, m = seal.seal_keys(SESSION_SEAL["K"], *nonces, EXCH["node_id"])
    obj = json.loads(seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 0, reassemble(vec_frames, mtu, True)))
    for k in ("device_key", "key_id", "vm_id", "vm_kind"):
        obj.pop(k, None)
    blob = seal.seal(e, m, seal.DEVICE_TO_CLIENT, 0, ref.compact(obj).encode())
    return seal.fragments(CREDS, blob, True, min(mtu - 3, FRAME_MAX))


def vector_want(st, mtu, nonces):
    want = [bytes.fromhex(x) for x in st["notify"]]
    if want and want[0][:2] == bytes([0x89, HELLO]) and len(want) > 1:
        return [want[0]] + our_sealed_creds_frames(want[1:], mtu, nonces)
    if want and want[0][:2] == bytes([0x89, CREDS]):     # credentials that waited (the buffer event)
        return our_sealed_creds_frames(want, mtu, nonces)
    out, msg = [], None
    for f in want:      # a sealed credentials_saved (one write) or clear answer (at the build's fragment buffer)
        if f[:2] in (bytes([0x89, SAVED]), bytes([0x89, CLEAR])) and len(f) > 3 and f[2] & SEALED:
            msg = bytearray(f[7:]) if f[2] & FIRST else msg + f[3:]
            if f[2] & LAST:
                out += seal.fragments(f[1], bytes(msg), True, mtu - 3 if f[1] == SAVED else min(mtu - 3, FRAME_MAX))
        else:
            out.append(f)
    return out


KEY_FIELDS = ("device_key", "key_id", "vm_id", "vm_kind")


def as_this_device(ex):
    """The exchange as this layer's stored document meets it: a sealed credentials_update the reference refuses
    because its whole stored document would pass MG_SESSION_MAX_CREDENTIALS, where this layer's (no device key fields)
    would fit, gets its access_token grown by those fields' bytes, so this document is over by as many (the same rule,
    the same refusal): re-sealed on its counter, fragmented at the exchange's MTU, the last fragment keeping the
    answer."""
    tc = EXCH["test_credentials_bound" if ex["device"].get("vm_binding") else "test_credentials"]
    keyed = {k: v for k, v in tc.items() if k in KEY_FIELDS}
    nonces = (SESSION_SEAL["client_nonce"], SESSION_SEAL["device_nonce"])
    steps, i = [], 0
    while i < len(ex["steps"]):
        st = ex["steps"][i]
        if st.get("event") == "proof":
            nonces = (bytes.fromhex(st["client_nonce"]), bytes.fromhex(st["device_nonce"]))
        f = bytes.fromhex(st.get("write", ""))
        if f[:2] != bytes([0x89, UPDATE]) or len(f) < 11 or (f[2] & (FIRST | SEALED)) != FIRST | SEALED:
            steps.append(st)
            i += 1
            continue
        run = [st]
        while not bytes.fromhex(run[-1]["write"])[2] & LAST and i + len(run) < len(ex["steps"]) and \
                ex["steps"][i + len(run)].get("write", "")[:4] == "8907":
            run.append(ex["steps"][i + len(run)])
        i += len(run)
        frames = [bytes.fromhex(x["write"]) for x in run]
        if not frames[-1][2] & LAST:
            steps += run
            continue
        blob = frames[0][7:] + b"".join(x[3:] for x in frames[1:])
        e, m = seal.seal_keys(SESSION_SEAL["K"], *nonces, EXCH["node_id"])
        ctr = int.from_bytes(blob[:4], "big")
        pt = seal.unseal(e, m, seal.CLIENT_TO_DEVICE, ctr, blob)
        try:
            upd = json.loads(pt)
            doc = {"node_id": EXCH["node_id"], "access_token": upd["access_token"],
                   "refresh_token": upd["refresh_token"]}
            exp = upd.get("access_expires_at")
            tail = {"access_expires_at": exp} if type(exp) is int and 0 < exp < 1 << 64 else {}
            ours, theirs = len(ref.compact({**doc, **tail})), len(ref.compact({**doc, **keyed, **tail}))
        except (TypeError, ValueError, KeyError):
            ours = theirs = 0
        if not (theirs > ref.MAX_CREDENTIALS >= ours) or any(x["notify"] for x in run[:-1]) or \
                run[-1]["notify"] != [err(3).hex()]:
            steps += run
            continue
        upd["access_token"] += "x" * (theirs - ours)
        grown = update_frames(seal.seal(e, m, seal.CLIENT_TO_DEVICE, ctr, ref.compact(upd).encode()), ex["mtu"], True)
        for k, g in enumerate(grown):
            last = k == len(grown) - 1
            steps.append({"write": g.hex(), "notify": run[-1]["notify"] if last else [],
                          "note": run[0].get("note", "") if k == 0 else ""})
    return dict(ex, steps=steps)


def replay(test, ex, w, cmd, compare):
    """Every step of one exchange: a proof event through the sim's proof command, a task event through its task
    command, a write through w(), each answer checked by compare(got, want, what)."""
    ex = as_this_device(ex)
    nonces = (SESSION_SEAL["client_nonce"], SESSION_SEAL["device_nonce"])
    for i, st in enumerate(ex["steps"]):
        what = f"{ex['name']} step {i}: {st.get('note', '')}"
        if st.get("event") == "task":
            compare(cmd("task 1" if st["active"] else "task 0"), [bytes.fromhex(x) for x in st["notify"]], what)
            continue
        if st.get("event") == "counters":     # a long session's stand-in: the counters jump there
            compare(cmd(f"counters {st.get('rx', '-')} {st.get('tx', '-')}"), [], what)
            continue
        if st.get("event") == "nudge":        # the keepalive (mgds_nudge)
            compare(cmd("nudge"), [bytes.fromhex(x) for x in st["notify"]], what)
            continue
        if st.get("event") == "buffer":       # the board's stack has room: what waited goes from the next poll
            cmd("credswait 0")
            compare(cmd("pump"), vector_want(st, ex["mtu"], nonces), what)
            continue
        if st.get("event") == "proof":
            if any(x[:4] == "1922" for x in st["notify"]):
                continue        # the token proof core answers it (no token): nothing reaches layer 2
            nonces = (bytes.fromhex(st["client_nonce"]), bytes.fromhex(st["device_nonce"]))
            compare(cmd(f"proof {nonces[0].hex()} {nonces[1].hex()}"), [bytes.fromhex(x) for x in st["notify"]],
                    what)
            continue
        compare(w(bytes.fromhex(st["write"])), vector_want(st, ex["mtu"], nonces), what)


def credentials_only(ex):
    """The exchange as a device without the data path answers it, or None: state open opens nothing here (the
    reference's /link-control request is the VM's), and an exchange that needs the VM session (its data or
    open_request) isn't one this device answers."""
    steps, vm_data = [], False
    for st in ex["steps"]:
        if not SEALED_DATA and "bad tag" in st.get("note", ""):
            break               # a data tag this build doesn't check: the rest depends on it
        if st.get("write", "")[:6] == "890102" and len(st["write"]) == 10:   # state open, any epoch
            vm_data = vm_data or any(x[:4] == "8903" for x in st["notify"])
            st = dict(st, notify=[])
        if any(x[:4] in ("8902", "8903") for x in st["notify"]):
            return None
        if vm_data and any(x[:4] in ("8908", "890c") and x[4:6] == "07" for x in st["notify"]):
            return None         # a sealed answer's counter after the VM session's data, which this device never sends
        steps.append(st)
    return dict(ex, steps=steps)


@unittest.skipIf(DATA_PATH, "DataVectors replays every exchange through the data path")
class Exchanges(unittest.TestCase):
    """mg-session-v1.json's exchanges a credentials-only device answers, byte for byte, the credentials rewritten
    as this device's: the sealed credentials and refresh exchanges, the gating ones (an unsealed hello refused
    insecure on any link) and the errors (state open opens nothing here)."""

    def test_exchanges(self):
        ran = []
        for ex in EXCH["exchanges"]:
            ex = credentials_only(ex)
            if ex is None:
                continue
            with self.subTest(ex["name"]):
                s = vector_sim(ex)
                replay(self, ex, s.w, s.frames,
                       lambda got, want, what: self.assertEqual([f.hex() for f in got], [f.hex() for f in want], what))
                s.close()
            ran.append(ex["name"])
        self.assertIn("errors at MTU 247", ran)
        self.assertIn("credentials refresh with access_expires_at at MTU 100", ran)


class Sealed(unittest.TestCase):
    def hello(self, s, mtu, cn=CN1, dn=DN1, access=ACCESS, refresh=REFRESH):
        out = s.w(SEALED_HELLO)
        out += s.frames("pump")
        self.assertEqual(out[0], bytes([0x89, HELLO, 1]) + (16384).to_bytes(4, "little") +
                         (4096).to_bytes(4, "little") + b"\x03")       # sealed | sealed_saved echoed
        msg = reassemble(out[1:], mtu, True)
        e, m = seal.seal_keys(K, cn, dn, NODE)
        pt = seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 0, msg)
        self.assertEqual(pt, creds_plain(access, refresh))
        return msg

    def test_handoff_every_mtu(self):
        for mtu in (23, 27, 64, 99, 100, 185, 247, 517):
            s = Sim(f"mtu {mtu}", f"proof {CN1.hex()} {DN1.hex()}")
            if mtu < MIN_MTU:   # the sealed session needs MG_MIN_ATT_MTU: invalid_value, the proof not spent
                self.assertEqual(s.w(SEALED_HELLO), [err(3)], mtu)
                s.cmd(f"mtu {MIN_MTU}")
            self.hello(s, max(mtu, MIN_MTU))
            s.close()

    def test_one_session_per_proof_vector(self):
        v = SEALV["one_session_per_proof"]
        s = Sim("mtu 247", "hello 4096 4096")
        nonces = [(CN1, DN1), (seal.det("reproof cn", 16), seal.det("reproof dn", 16))]
        for st in v["steps"]:
            if st["client"] is None:
                cn, dn = nonces.pop(0)
                s.cmd(f"proof {cn.hex()} {dn.hex()}")
                continue
            out = s.w(bytes.fromhex(st["client"]))
            self.assertEqual(out[0].hex(), st["device"], st["step"])
            if st["device"].startswith("19"):
                self.assertEqual(len(out), 1, "a refusal and nothing else")
            s.cmd("pump")
        s.close()

    def test_second_hello_same_proof_no_second_keystream(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        c1 = self.hello(s, 247)
        before = len([e for e in s.events if e.startswith("N ")])
        self.assertEqual(s.w(SEALED_HELLO), [err(6)])
        self.assertEqual(s.frames("pump"), [])
        self.assertEqual(len([e for e in s.events if e.startswith("N ")]), before + 1)
        self.assertIn("takes=1", s.cmd("show")[0])     # the proof was never asked again
        # The first session stays: its update (counter 0) is still taken.
        for f in update_frames(sealed_update(CN1, DN1, 0, "acc-2", "ref-2"), 247, True)[:-1]:
            self.assertEqual(s.w(f), [])
        last = update_frames(sealed_update(CN1, DN1, 0, "acc-2", "ref-2"), 247, True)[-1]
        self.assertEqual(s.w(last), saved(1, True, "ref-2"))
        # A new proof: one more session, a different keystream.
        s.cmd(f"proof {CN2.hex()} {DN2.hex()}")
        c2 = self.hello(s, 247, CN2, DN2, "acc-2", "ref-2")
        self.assertNotEqual(c1[4:40], c2[4:40])
        show = s.cmd("show")
        self.assertIn("takes=2", show[0])
        s.close()

    def test_updates_counters_and_refusals(self):
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}")
        self.hello(s, 100)

        def send_update(sealed_bytes, sealed=True):
            out = []
            for f in update_frames(sealed_bytes, 100, sealed):
                out += s.w(f)
            return out

        # Sealed answers: device -> client counters 1, 2, ... after the credentials' 0.
        self.assertEqual(send_update(sealed_update(CN1, DN1, 0, "a" * 700, "r1")), saved(1, True, "r1", mtu=100))
        self.assertEqual(send_update(sealed_update(CN1, DN1, 1, "a2", "r2", extra='"access_expires_at":1793900000')),
                         saved(2, True, "r2", mtu=100))
        self.assertIn("access=a2 refresh=r2", s.cmd("show")[0])
        # The counter is read from the first fragment: a replayed or a skipped one is refused and leaves it.
        self.assertEqual(send_update(sealed_update(CN1, DN1, 1, "a3", "r3")), [err(3)])
        self.assertEqual(send_update(sealed_update(CN1, DN1, 3, "a3", "r3")), [err(3)])
        # Refused at its tag (the wrong direction, a flipped tag, wrong keys): the counter isn't used, so a forged
        # message can't move it.
        e, m = seal.seal_keys(K, CN1, DN1, NODE)
        refl = seal.seal(e, m, seal.DEVICE_TO_CLIENT, 2, b'{"access_token":"a3","refresh_token":"r3"}')
        self.assertEqual(send_update(refl), [err(3)])
        bad = bytearray(sealed_update(CN1, DN1, 2, "a3", "r3"))
        bad[-1] ^= 1
        self.assertEqual(send_update(bytes(bad)), [err(3)])
        self.assertEqual(send_update(sealed_update(CN2, DN2, 2, "a3", "r3")), [err(3)])
        # Unsealed in a sealed session: insecure (no downgrade), nothing changes. Refusals stay unsealed error frames
        # and take no device counter.
        self.assertEqual(send_update(b'{"access_token":"a3","refresh_token":"r3"}', sealed=False), [err(7)])
        self.assertIn("access=a2 refresh=r2", s.cmd("show")[0])
        self.assertEqual(send_update(sealed_update(CN1, DN1, 2, "a3", "r3")), saved(3, True, "r3", mtu=100))
        self.assertIn("access=a3 refresh=r3", s.cmd("show")[0])
        s.close()

    def test_update_whose_stored_document_exceeds_the_max(self):
        """The next credentials hand the whole stored document back, so it (here node_id and the pair: this layer
        stores no device key; access_expires_at where the board keeps it) must fit MG_SESSION_MAX_CREDENTIALS too.
        One byte over: invalid_value, nothing stored, its counter stays used (a refusal takes no device counter).
        Exactly the max: saved, and it goes out whole in the next credentials."""
        max_creds = 2048                                    # MG_SESSION_MAX_CREDENTIALS
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        self.hello(s, 247)

        def send_update(b):
            out = []
            for f in update_frames(b, 247, True):
                out += s.w(f)
            return out

        fixed = len(creds_plain("", "r"))                   # node_id, the keys and a one-byte refresh_token
        over = "a" * (max_creds + 1 - fixed)
        self.assertEqual(len(creds_plain(over, "r")), max_creds + 1)
        self.assertEqual(send_update(sealed_update(CN1, DN1, 0, over, "r")), [err(3)])
        self.assertIn(f"access={ACCESS} refresh={REFRESH}", s.cmd("show")[0])     # nothing stored
        exact = "a" * (max_creds - fixed)
        self.assertEqual(send_update(sealed_update(CN1, DN1, 1, exact, "r")), saved(1, True, "r"))
        s.cmd(f"proof {CN2.hex()} {DN2.hex()}")
        self.hello(s, 247, CN2, DN2, exact, "r")            # the whole stored document, 2048 bytes, goes out
        s.close()
        # access_expires_at counts where the board keeps it; a board without it drops the update's, so the pair alone
        # is measured.
        exp = '"access_expires_at":1893456000'
        room = max_creds - fixed - len(exp) - 1             # ,"access_expires_at":N before the brace
        for kept in (True, False):
            s = Sim("mtu 247", f"expiry {int(kept)}", f"proof {CN1.hex()} {DN1.hex()}")
            s.w(SEALED_HELLO)
            s.frames("pump")
            out = send_update(sealed_update(CN1, DN1, 0, "a" * (room + 1), "r", extra=exp))
            self.assertEqual(out, [err(3)] if kept else saved(1, True, "r"))
            if kept:
                self.assertEqual(send_update(sealed_update(CN1, DN1, 1, "a" * room, "r", extra=exp)),
                                 saved(1, True, "r"))
            s.close()
        # The node_id read fails at the update: no hello could hand any pair back, so none is stored.
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        self.hello(s, 247)
        s.cmd("node " + "n" * 70)                           # over MGDS_NODE_MAX: the port's read fails
        self.assertEqual(send_update(sealed_update(CN1, DN1, 0, "a" * 990, "r" * 1000)), [err(3)])
        self.assertIn(f"access={ACCESS} refresh={REFRESH}", s.cmd("show")[0])
        s.close()

    def test_update_abort_and_no_storage_use_the_counter(self):
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}")
        self.hello(s, 100)
        opened_at(s, 64)
        fr = update_frames(sealed_update(CN1, DN1, 0, "a" * 200, "r1"), 64, True)
        self.assertEqual(s.w(fr[0]), [])
        self.assertEqual(s.w(bytes([0x89, ABORT])), [])                # the client dropped it: no answer, counter 0 used
        self.assertEqual(s.cmd("w " + fr[1].hex())[-1:], ["N " + err(3).hex()])   # nothing open any more
        s.cmd("storage 0")
        uf = update_frames(sealed_update(CN1, DN1, 1, "a4", "r4"), 64, True)
        for f in uf[:-1]:
            self.assertEqual(s.w(f), [])                                # no storage: discarded, counter 1 used
        self.assertEqual(s.w(uf[-1]), saved(1, False, "", mtu=64))      # given up: sealed [0] at its last write
        s.cmd("storage 1")
        out = []
        for f in update_frames(sealed_update(CN1, DN1, 2, "a5", "r5"), 64, True):
            out += s.w(f)
        self.assertEqual(out, saved(2, True, "r5", mtu=64))
        self.assertIn("access=a5 refresh=r5", s.cmd("show")[0])
        s.close()

    def test_json_rules(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        self.hello(s, 247)
        e, m = seal.seal_keys(K, CN1, DN1, NODE)
        cases = [
            (b'{"access_token":"x1","refresh_token":"y1"}', True),
            (b'{ "refresh_token" : "y2" , "access_token":"x2", "n":-1.5e3, "b":true, "z":null } ', True),
            (b' {"access_token":"x2","refresh_token":"y2"}', False),      # '{' first: a relabelled message never parses
            (b'{"access_token":"x\\"3","refresh_token":"y3"}', False),     # an escape in a token
            (b'{"access_token":"","refresh_token":"y4"}', False),
            (b'{"access_token":"x5"}', False),
            (b'{"access_token":"x6","refresh_token":"y6","access_token":"x6"}', False),
            (b'{"access_token":"x7","refresh_token":"y7","o":{}}', False),
            (b'{"access_token":"x8","refresh_token":"y8"} x', False),
            (b'["access_token","x9"]', False),
            (b'{"access_token":7,"refresh_token":"y10"}', False),
            (b'{"access_token":"x\x01","refresh_token":"y11"}', False),
            (b'{"access_token":"x12","refresh_token":"y12","s":"esc\\u00e9"}', True),
        ]
        ctr, dev = 0, 1
        for body, good in cases:
            out = []
            for f in update_frames(seal.seal(e, m, seal.CLIENT_TO_DEVICE, ctr, body), 247, True):
                out += s.w(f)
            if good:        # the sealed answer, under the next device counter (a refusal is unsealed and takes none)
                self.assertEqual(out, saved(dev, True, seal.parse_update(body)[0][1]), body)
                dev += 1
            else:
                self.assertEqual(out, [err(3)], body)
            ctr += 1        # a well-sealed message advances the counter even when its JSON is refused
        s.close()

    def test_access_expires_at(self):
        # A board that keeps it: handed back in the credentials, replaced (or cleared) by every update; any form but an
        # unsigned integer stored as none; a duplicate refused. A board without it ignores it (test_updates_...).
        s = Sim("mtu 247", "expiry 1", "exp 1793860000", f"proof {CN1.hex()} {DN1.hex()}")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        e, m = seal.seal_keys(K, CN1, DN1, NODE)
        pt = seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 0, reassemble(out[1:], 247, True))
        self.assertEqual(json.loads(pt), {"node_id": NODE, "access_token": ACCESS, "refresh_token": REFRESH,
                                          "access_expires_at": 1793860000})
        self.assertEqual(pt, creds_plain()[:-1] + b',"access_expires_at":1793860000}')
        cases = [('"access_expires_at":1893456000', 1893456000), ('', 0), ('"access_expires_at":"1893456000"', 0),
                 ('"access_expires_at":1.5', 0), ('"access_expires_at":-5', 0),
                 ('"access_expires_at":99999999999999999999', 0), ('"access_expires_at":18446744073709551615',
                                                                   18446744073709551615),
                 ('"access_expires_at":null', 0), ('"access_expires_at":42', 42)]
        for ctr, (extra, want) in enumerate(cases):
            lines = []
            for f in update_frames(sealed_update(CN1, DN1, ctr, f"a{ctr}", f"r{ctr}", extra=extra), 247, True):
                lines += s.cmd("w " + f.hex())
            self.assertIn(f"X {want}", lines, extra)
            self.assertIn("N " + saved(ctr + 1, True, f"r{ctr}")[0].hex(), lines, extra)
        bad = sealed_update(CN1, DN1, len(cases), "a", "r", extra='"access_expires_at":1,"access_expires_at":2')
        out = []
        for f in update_frames(bad, 247, True):
            out += s.w(f)
        self.assertEqual(out, [err(3)])
        s.close()
        # The longest form in the credentials.
        s = Sim("mtu 100", "expiry 1", "exp 18446744073709551615", f"proof {CN2.hex()} {DN2.hex()}")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        e, m = seal.seal_keys(K, CN2, DN2, NODE)
        pt = seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 0, reassemble(out[1:], 100, True))
        self.assertEqual(json.loads(pt)["access_expires_at"], 18446744073709551615)
        s.close()
        # exp 0 with expiry on: no field (as a board without it).
        s = Sim("mtu 247", "expiry 1", "exp 0", f"proof {CN1.hex()} {DN1.hex()}")
        self.hello(s, 247)
        s.close()

    def test_access_expires_at_reference_vectors(self):
        # mg-session-seal-v1.json's access_expires_at section, byte for byte: every sealed credentials
        # message the gadget sends, its answer to every sealed update, what it keeps.
        v = SEALV["access_expires_at"]
        k = bytes.fromhex(v["K"])
        first = v["steps"][0]
        pair = json.loads(first["plaintext"])
        s = Sim("mtu 247", f"k {k.hex()}", f"node {v['node_id']}", f"access {pair['access_token']}",
                f"refresh {pair['refresh_token']}", "expiry 1", "exp 0")
        # The section keeps the legacy one-byte answer (it tests the storage rule only); a device with sealed answers
        # answers the same verdict sealed (sealed_credentials_saved has those bytes), so a "saved" step is checked as
        # the sealed [1 || hash of its refresh_token] under the session's next device counter.
        sess, dev = None, 1
        for i, st in enumerate(v["steps"]):
            if st["session"] != sess:
                sess, dev = st["session"], 1
                ses = v["sessions"][sess]
                cn, dn = bytes.fromhex(ses["client_nonce"]), bytes.fromhex(ses["device_nonce"])
                s.cmd(f"proof {cn.hex()} {dn.hex()}")
            if st["direction"] == seal.DEVICE_TO_CLIENT:
                out = s.w(SEALED_HELLO) + s.frames("pump")
                self.assertEqual(reassemble(out[1:], 247, True).hex(), st["sealed"], st["step"])
            else:
                lines = []
                for f in update_frames(bytes.fromhex(st["sealed"]), 247, True):
                    lines += s.cmd("w " + f.hex())
                want = st["device"]
                if want == seal.saved_frame(True).hex():
                    refresh = json.loads(st["plaintext"])["refresh_token"]
                    want = saved(dev, True, refresh, cn, dn, 247, v["node_id"], k)[0].hex()
                    dev += 1
                self.assertIn("N " + want, lines, st["step"])
                if st["device"].startswith("8908"):
                    self.assertIn(f"X {st['stored']}", lines, st["step"])
        s.close()

    def test_deferral(self):
        # A hello while busy waits up to 1 s (MGDS_HELLO_DEFER_MS); answered when media ends within it.
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "busy 1")
        self.assertEqual(s.w(SEALED_HELLO), [])
        self.assertEqual(s.frames("poll 999"), [])
        s.cmd("busy 0")
        out = s.frames("poll 10")
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
        reassemble(out[1:], 247, True)
        s.close()
        # At 1 s still busy: [session, busy], once; the proof is not spent, and
        # the client's busy retry is answered normally once the port is idle.
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "busy 1")
        self.assertEqual(s.w(SEALED_HELLO), [])
        self.assertEqual(s.frames("poll 999"), [])
        self.assertEqual(s.frames("poll 1"), [err(4)])
        self.assertEqual(s.frames("poll 1000"), [], "answered once")
        s.cmd("busy 0")
        self.assertEqual(s.frames("poll 10"), [])
        self.assertIn("takes=0", s.cmd("show")[0])
        self.hello(s, 247)
        # An update's commit held while busy, then stored; held past 30 s: dropped, no answer.
        s.cmd("busy 1")
        for f in update_frames(sealed_update(CN1, DN1, 0, "h1", "h2"), 247, True):
            self.assertEqual(s.w(f), [])
        self.assertEqual(s.frames("poll 1000"), [])
        s.cmd("busy 0")
        self.assertEqual(s.frames("poll 10"), saved(1, True, "h2"))
        s.cmd("busy 1")
        for f in update_frames(sealed_update(CN1, DN1, 1, "h3", "h4"), 247, True):
            self.assertEqual(s.w(f), [])
        self.assertEqual(s.frames("poll 29999"), [])
        # Given up at 30 s with the link up: sealed [0] (never silence), the pair not stored.
        self.assertEqual(s.frames("poll 1"), saved(2, False, ""))
        s.cmd("busy 0")
        self.assertEqual(s.frames("poll 10"), [])
        self.assertIn("access=h1 refresh=h2", s.cmd("show")[0])
        # A held update the board takes the storage back from (a talk hold): sealed [0] at once.
        s.cmd("busy 1")
        for f in update_frames(sealed_update(CN1, DN1, 2, "h5", "h6"), 247, True):
            self.assertEqual(s.w(f), [])
        if GIVE_UP_LATER:                        # owed, not sealed inside the release: from the next poll
            self.assertEqual(s.frames("release"), [])
            self.assertIn("busy=1", s.cmd("show")[0])
            self.assertEqual(s.frames("poll 1"), saved(3, False, ""))
        else:
            self.assertEqual(s.frames("release"), saved(3, False, ""))
        # A held update stays through refused first writes (another update, an unsealed one): only refusals; it
        # commits once media stops.
        for f in update_frames(sealed_update(CN1, DN1, 3, "h7", "h8"), 247, True):
            self.assertEqual(s.w(f), [])
        self.assertEqual(s.w(update_frames(sealed_update(CN1, DN1, 4, "h9", "h10"), 247, True)[0]), [err(3)])
        self.assertEqual(s.w(update_frames(b'{"access_token":"x","refresh_token":"y"}', 247, False)[0]), [err(7)])
        s.cmd("busy 0")
        self.assertEqual(s.frames("poll 10"), saved(4, True, "h8"))
        self.assertIn("access=h7 refresh=h8", s.cmd("show")[0])
        self.assertIn("busy=0", s.cmd("show")[0])
        # The link drops while one is held: nothing, ever. (The refused update didn't take its counter: 4 is next.)
        s.cmd("busy 1")
        for f in update_frames(sealed_update(CN1, DN1, 4, "hb", "hc"), 247, True):
            self.assertEqual(s.w(f), [])
        self.assertEqual(s.frames("reset 0"), [])
        self.assertEqual(s.frames("poll 30000"), [])
        s.close()

    def test_storage(self):
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}")
        self.hello(s, 100)
        frames = update_frames(sealed_update(CN1, DN1, 0, "s1" * 90, "s2"), 100, True)
        self.assertGreater(len(frames), 1)
        s.cmd("storage 0")                       # no storage: discarded (its counter, 0, used), sealed [0] at the end
        for f in frames[:-1]:
            self.assertEqual(s.w(f), [])
        self.assertEqual(s.w(frames[-1]), saved(1, False, "", mtu=100))
        s.cmd("storage 1")
        frames = update_frames(sealed_update(CN1, DN1, 1, "s1" * 90, "s2"), 100, True)
        s.w(frames[0])                           # released mid-message: the rest discarded (1 used), [0] at the end
        self.assertEqual(s.frames("release"), [])
        for f in frames[1:-1]:
            self.assertEqual(s.w(f), [])
        self.assertEqual(s.w(frames[-1]), saved(2, False, "", mtu=100))
        out = []                                 # a client never seals twice under a counter it sent: 2 is next
        for f in update_frames(sealed_update(CN1, DN1, 2, "s1" * 90, "s2"), 100, True):
            out += s.w(f)
        self.assertEqual(out, saved(3, True, "s2", mtu=100))
        s.cmd("commitfail 1")
        out = []
        for f in update_frames(sealed_update(CN1, DN1, 3, "s3", "s4"), 100, True):
            out += s.w(f)
        self.assertEqual(out, saved(4, False, "", mtu=100))     # the store refused: sealed [0], zeros
        self.assertIn("access=" + "s1" * 90 + " refresh=s2", s.cmd("show")[0])
        self.assertIn("borrowed=0", s.cmd("show")[0])
        s.close()

    def test_backpressure_abort_and_link_loss(self):
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}", "budget 4")
        out = s.w(SEALED_HELLO)
        self.assertEqual(len(out), 4)            # hello + 3 fragments, then no buffer
        s.cmd("budget -1")
        s.cmd("again 2")
        out += s.frames("pump")
        msg = reassemble(out[1:], 100, True)
        e, m = seal.seal_keys(K, CN1, DN1, NODE)
        self.assertEqual(seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 0, msg), creds_plain())
        s.close()
        # A reset part-way: abort on a live link; nothing on a lost one.
        for live, want in ((1, [bytes([0x89, ABORT])]), (0, [])):
            s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}", "budget 3")
            s.w(SEALED_HELLO)
            s.cmd("budget -1")
            self.assertEqual(s.frames(f"reset {live}"), want)
            self.assertEqual(s.frames("pump"), [])
            self.assertIn("busy=0", s.cmd("show")[0])
            s.close()
        # The link goes: the session ends quietly.
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}", "budget 2")
        s.w(SEALED_HELLO)
        s.cmd("linkdown")
        s.cmd("budget -1")
        s.cmd("pump")
        self.assertIn("busy=0", s.cmd("show")[0])
        s.close()

    def test_update_waits_for_credentials(self):
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}", "budget 3")
        out = s.w(SEALED_HELLO)
        for f in update_frames(sealed_update(CN1, DN1, 0, "w1", "w2"), 64, True):
            self.assertEqual(s.w(f), [])         # the credentials still go out: the commit waits
        s.cmd("budget -1")
        out += s.frames("pump")
        sv = saved(1, True, "w2", mtu=100)
        self.assertEqual(out[-len(sv):], sv)         # after the credentials, under counter 1
        msg = reassemble(out[1:-len(sv)], 100, True)
        e, m = seal.seal_keys(K, CN1, DN1, NODE)
        self.assertEqual(seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 0, msg), creds_plain())
        s.close()

    def test_unsafe_stored_token_refused_before_any_byte(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", 'access bad"quote')
        self.assertEqual(s.w(SEALED_HELLO), [err(5)])
        s.close()
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "access " + "x" * 2000)
        self.assertEqual(s.w(SEALED_HELLO), [err(5)])     # over MG_SESSION_MAX_CREDENTIALS
        s.close()


class SealedSaved(unittest.TestCase):
    """The sealed credentials_saved: the vectors' sealed_credentials_saved section byte for byte where a
    conforming device produces it, and its edge cases: every
    give-up answers sealed [0] while the link is up, a [0] waits for the credentials and for an answer still going out,
    an answer part-way out at a reset gets an abort, a small fragment buffer fragments it and a refused write is offered
    again, refusals stay unsealed error frames, the counter never wraps."""

    def update(self, s, sealed_bytes, mtu=247):
        out = []
        for f in update_frames(sealed_bytes, mtu, True):
            out += s.w(f)
        return out

    def test_reference_vector(self):
        v = SEALV["sealed_credentials_saved"]
        k, cn, dn, node = bytes.fromhex(v["K"]), bytes.fromhex(v["client_nonce"]), bytes.fromhex(v["device_nonce"]), \
            v["node_id"]
        e, m = seal.seal_keys(k, cn, dn, node)
        creds = seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 0, bytes.fromhex(v["steps"][0]["device"][0])[7:])
        pair = json.loads(creds)
        s = Sim("mtu 247", f"k {k.hex()}", f"node {node}", f"access {pair['access_token']}",
                f"refresh {pair['refresh_token']}", f"proof {cn.hex()} {dn.hex()}")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        want = [bytes.fromhex(x) for x in v["steps"][0]["device"]]
        if FRAME_MAX >= len(want[0]):
            self.assertEqual(out[1:], want, "the credentials, counter 0")
        else:
            self.assertEqual(reassemble(out[1:], 247, True), want[0][7:], "the credentials, counter 0")
        expected = 1    # the client's next device -> client counter
        for i, st in enumerate(v["steps"][1:], 1):
            client = [bytes.fromhex(x) for x in st["client"]]
            refresh = json.loads(st["update_plaintext"])["refresh_token"]
            if i == 2:
                s.cmd("commitfail 1")       # the store refuses it: the vector's sealed [0] (also a give-up's)
            got = []
            for f in client:
                got += s.w(f)
            s.cmd("commitfail 0")
            vec = [bytes.fromhex(x) for x in st["device"] if len(x) > 6]   # its sealed answer(s)
            if i <= 2:
                self.assertEqual(got, vec, st["step"])     # saved, and not saved: exactly the vector's frames
            else:
                # the vector's other answers come from a broken or old device (another pair's hash, an unsealed
                # [1]); this one answers its own: sealed [1 || hash of this update's refresh_token]
                self.assertEqual(got, saved(expected, True, refresh, cn, dn, 247, node, k), st["step"])
            # how the client reads it: the reference's verdict on the one write mgcommands.h asks for, in every build
            self.assertEqual(len(got), 1, st["step"])
            verdict, expected = seal.app_saved_verdict(e, m, expected, got[0], refresh)
            self.assertEqual(verdict, "not saved" if i == 2 else "saved", st["step"])
        s.close()

    def test_zero_waits_for_the_credentials(self):
        # No storage for an update while the credentials still go out: its [0] goes after them, counter 1.
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}", "budget 3", "storage 0")
        out = s.w(SEALED_HELLO)
        for f in update_frames(sealed_update(CN1, DN1, 0, "z1", "z2"), 64, True):
            self.assertEqual(s.w(f), [])
        s.cmd("budget -1")
        out += s.frames("pump")
        sv = saved(1, False, "", mtu=100)
        self.assertEqual(out[-len(sv):], sv)
        e, m = seal.seal_keys(K, CN1, DN1, NODE)
        self.assertEqual(seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 0, reassemble(out[1:-len(sv)], 100, True)),
                         creds_plain())
        s.close()

    def test_zero_waits_for_an_answer_going_out(self):
        # MTU 23: an answer takes two writes. Its first goes, the stack refuses the second; meanwhile an update finds
        # no storage. The first answer finishes, then the [0] (counter 2); a commit waits for both.
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}")
        Sealed.hello(self, s, 100)
        opened_at(s, 23)
        s.cmd("budget 1")
        out = self.update(s, sealed_update(CN1, DN1, 0, "y1", "y2"), 23)
        first = saved(1, True, "y2", mtu=23)
        self.assertGreater(len(first), 1)
        self.assertEqual(out, first[:1])
        s.cmd("storage 0")
        self.assertEqual(self.update(s, sealed_update(CN1, DN1, 1, "y3", "y4"), 23), [])
        s.cmd("storage 1")
        self.assertEqual(self.update(s, sealed_update(CN1, DN1, 2, "y5", "y6"), 23), [])   # held: answers owed
        s.cmd("budget -1")
        out = s.frames("pump")
        self.assertEqual(out, first[1:] + saved(2, False, "", mtu=23) + saved(3, True, "y6", mtu=23))
        self.assertIn("access=y5 refresh=y6", s.cmd("show")[0])
        self.assertIn("busy=0", s.cmd("show")[0])
        s.close()

    def test_a_refusal_without_a_buffer_is_answered_zero(self):
        # An update refused when the stack has no buffer for the refusal: never silence (it reads as "assumed
        # saved"), its sealed [0] under the next device counter once the stack takes a frame.
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
        s.cmd("again 1")                                             # the refusal finds no buffer
        self.assertEqual(s.w(update_frames(sealed_update(CN1, DN1, 5, "a", "r"), 247, True)[0]), saved(1, False, ""))
        s.close()

    def test_again_offers_the_same_fragment(self):
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}")
        Sealed.hello(self, s, 100)
        opened_at(s, 23)
        s.cmd("again 1")
        lines = []
        for f in update_frames(sealed_update(CN1, DN1, 0, "g1", "g2"), 23, True):
            lines += s.cmd("w " + f.hex())
        self.assertIn("A", lines)
        out = [bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")] + s.frames("pump")
        self.assertEqual(out, saved(1, True, "g2", mtu=23))
        s.close()

    def test_reset_part_way_aborts(self):
        for live, want in ((1, [bytes([0x89, ABORT])]), (0, [])):
            s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}")
            Sealed.hello(self, s, 100)
            opened_at(s, 23)
            s.cmd("budget 1")
            self.assertEqual(self.update(s, sealed_update(CN1, DN1, 0, "r1", "r2"), 23), saved(1, True, "r2", mtu=23)[:1])
            s.cmd("budget -1")
            self.assertEqual(s.frames(f"reset {live}"), want)
            self.assertEqual(s.frames("pump"), [])
            self.assertIn("busy=0", s.cmd("show")[0])
            s.close()
        # Not part-way (nothing out yet): a reset on a live link sends nothing.
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}")
        Sealed.hello(self, s, 100)
        opened_at(s, 23)
        s.cmd("budget 0")
        self.assertEqual(self.update(s, sealed_update(CN1, DN1, 0, "r1", "r2"), 23), [])
        s.cmd("budget -1")
        self.assertEqual(s.frames("reset 1"), [])
        s.close()

    def test_link_loss_part_way_ends_quietly(self):
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}")
        Sealed.hello(self, s, 100)
        opened_at(s, 23)
        s.cmd("budget 1")
        self.update(s, sealed_update(CN1, DN1, 0, "l1", "l2"), 23)
        s.cmd("linkdown")
        s.cmd("budget -1")
        s.cmd("pump")
        self.assertIn("busy=0", s.cmd("show")[0])
        s.close()

    def test_counter_past_256(self):
        """The device -> client counter goes on past a byte (the default uint32_t; MGDS_TX_CTR_U8 builds end the
        session at 255 instead: test_counter_end_and_a_new_session)."""
        if TX_CTR_U8:
            self.skipTest("an MGDS_TX_CTR_U8 build ends the session at 255")
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        Sealed.hello(self, s, 247)
        e, m = seal.seal_keys(K, CN1, DN1, NODE)
        for ctr in range(258):
            body = b'{"access_token":"w","refresh_token":"x%d"}' % ctr
            out = self.update(s, seal.seal(e, m, seal.CLIENT_TO_DEVICE, ctr, body))
            if ctr in (0, 254, 255, 256, 257):
                self.assertEqual(out, saved(ctr + 1, True, f"x{ctr}"), ctr)   # counters 255, 256, 257, 258
        s.close()

    def test_counter_end_and_a_new_session(self):
        """MGDS_TX_CTR_U8 (a RAM-tight board): after the answer with counter 255 the session is spent: the next update
        gets invalid_value (unsealed), the client re-proves and its new sealed session starts again at 0 (credentials)
        and 1 (the next answer)."""
        if not TX_CTR_U8:
            self.skipTest("an MGDS_TX_CTR_U8 build")
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        Sealed.hello(self, s, 247)
        e, m = seal.seal_keys(K, CN1, DN1, NODE)
        for ctr in range(255):
            body = b'{"access_token":"w","refresh_token":"x%d"}' % ctr
            out = self.update(s, seal.seal(e, m, seal.CLIENT_TO_DEVICE, ctr, body))
            if ctr in (0, 253, 254):
                self.assertEqual(out, saved(ctr + 1, True, f"x{ctr}"), ctr)   # 254: the answer with counter 255
            else:
                self.assertEqual(len(out), len(saved(1, True, "")), ctr)
        self.assertIn("busy=0", s.cmd("show")[0])
        body = b'{"access_token":"w","refresh_token":"y"}'
        self.assertEqual(self.update(s, seal.seal(e, m, seal.CLIENT_TO_DEVICE, 255, body)), [err(3)])
        self.assertIn("access=w refresh=x254", s.cmd("show")[0])
        s.cmd(f"proof {CN2.hex()} {DN2.hex()}")
        Sealed.hello(self, s, 247, CN2, DN2, "w", "x254")
        self.assertEqual(self.update(s, sealed_update(CN2, DN2, 0, "n1", "n2")), saved(1, True, "n2", CN2, DN2))
        s.close()

    def test_counters_never_wrap(self):
        """mgcommands.h, Sealed credentials: once the client's 0xFFFFFFFF is taken no other counter is (the session's
        first, 0, replayed: refused); once the device has sealed its last counter (0xFFFFFFFF, 255 in an
        MGDS_TX_CTR_U8 build) it seals nothing more and refuses every sealed message (updates, a clear) with
        invalid_value. Either way a new proof and sealed hello start over at 0. The vectors' "counters at the top"
        exchanges run through the data path (DataVectors)."""
        last = 0xFF if TX_CTR_U8 else 0xFFFFFFFF
        e, m = seal.seal_keys(K, CN1, DN1, NODE)
        # the client's counter
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        Sealed.hello(self, s, 247)
        first = sealed_update(CN1, DN1, 0, "f1", "f2")
        s.cmd("counters 0xFFFFFFFF -")                               # a long session's stand-in
        self.assertEqual(self.update(s, sealed_update(CN1, DN1, 0xFFFFFFFF, "t1", "t2")), saved(1, True, "t2"))
        self.assertEqual(self.update(s, first), [err(3)], "counter 0 after 0xFFFFFFFF: refused, nothing stored")
        self.assertIn("access=t1 refresh=t2", s.cmd("show")[0])
        s.cmd(f"proof {CN2.hex()} {DN2.hex()}")
        Sealed.hello(self, s, 247, CN2, DN2, "t1", "t2")
        self.assertEqual(self.update(s, sealed_update(CN2, DN2, 0, "n1", "n2")), saved(1, True, "n2", CN2, DN2))
        s.close()
        # the device's counter
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        Sealed.hello(self, s, 247)
        s.cmd(f"counters - {last}")
        self.assertEqual(self.update(s, sealed_update(CN1, DN1, 0, "d1", "d2")), saved(last, True, "d2"))
        self.assertEqual(self.update(s, sealed_update(CN1, DN1, 1, "d3", "d4")), [err(3)], "spent: refused")
        self.assertEqual(self.update(s, sealed_update(CN1, DN1, 1, "d3", "d4")), [err(3)], "and again")
        self.assertIn("access=d1 refresh=d2", s.cmd("show")[0])
        self.assertEqual(s.w(clear_frames(1)[0]), [err(3)], "a clear too: nothing erased")
        self.assertNotIn("CL 1", s.events)
        self.assertIn("busy=0", s.cmd("show")[0])
        s.cmd(f"proof {CN2.hex()} {DN2.hex()}")
        Sealed.hello(self, s, 247, CN2, DN2, "d1", "d2")              # the credentials on device counter 0
        self.assertEqual(self.update(s, sealed_update(CN2, DN2, 0, "n1", "n2")), saved(1, True, "n2", CN2, DN2))
        s.close()

    def test_an_update_held_behind_the_last_answer_is_refused(self):
        """The answer under the device's last counter waits for a buffer while a second update arrives and is held
        behind it. Once that answer is out the session is spent: the held update is refused with invalid_value,
        nothing committed, its storage given back and nothing left owed."""
        last = 0xFF if TX_CTR_U8 else 0xFFFFFFFF
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        Sealed.hello(self, s, 247)
        s.cmd(f"counters - {last}")
        s.cmd("budget 0")
        self.assertEqual(self.update(s, sealed_update(CN1, DN1, 0, "h1", "h2")), [], "committed, its answer waits")
        self.assertEqual(self.update(s, sealed_update(CN1, DN1, 1, "h3", "h4")), [], "held behind that answer")
        s.cmd("budget -1")
        self.assertEqual(s.frames("pump"), saved(last, True, "h2") + [err(3)])
        show = s.cmd("show")[0]
        self.assertIn("access=h1 refresh=h2", show)
        self.assertIn("borrowed=0 busy=0", show)
        s.close()


envelope_payload = ref.envelope_payload     # bytes -> a ServiceResponseEnvelope's payload, or None (not one)


def env(payload: bytes) -> bytes:
    """payload as a ServiceResponseEnvelope's field 1: the only client data the data path hands its port (mgcommands.h,
    Sealed data; mg_session_ref.envelope_payload); anything else gets invalid_value."""
    return bytes([0x0A]) + ref.varint(len(payload)) + payload


def data_frames(payload: bytes, mtu: int, sealed: bool):
    out = []
    for f in ref.fragments(payload, mtu, DATA):
        f = bytearray(f)
        if sealed:
            f[2] |= SEALED
        out.append(bytes(f))
    return out


def credit(n):
    return bytes([0x89, CREDIT]) + n.to_bytes(4, "little")


STATE_OPEN = bytes([0x89, STATE, 2, 1, 0])
OPEN_REQ1, OPEN_REQ0 = bytes([0x89, 2, 1]), bytes([0x89, 2, 0])


class LegacyApp(unittest.TestCase):
    """A sealed hello without mg_session_hello_flag_sealed_saved is a client that predates sealed answers.
    It gets exactly the old behavior: the device hello echoes sealed only, credentials_saved
    is the one-byte unsealed [1] / [0] and takes no device counter (the client's relay never counts one), a given-up
    write-back stays silent; the next sealed message is the one the client expects."""

    def open(self, s, hello=LEGACY_HELLO, cn=CN1, dn=DN1, pair=(ACCESS, REFRESH)):
        out = s.w(hello) + s.frames("pump")
        self.assertEqual(out[0], bytes([0x89, HELLO, 1]) + (16384).to_bytes(4, "little") +
                         (4096).to_bytes(4, "little") + bytes([hello[-1]]))     # echoes what it does
        msg = reassemble(out[1:], 247, True)
        e, m = seal.seal_keys(K, cn, dn, NODE)
        self.assertEqual(seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 0, msg), creds_plain(*pair))
        return e, m

    def update(self, s, e, m, ctr, access="a2", refresh="r2"):
        pt = ('{"access_token":"%s","refresh_token":"%s"}' % (access, refresh)).encode()
        out = []
        for f in update_frames(seal.seal(e, m, seal.CLIENT_TO_DEVICE, ctr, pt), 247, True):
            out += s.w(f)
        return out

    def test_unsealed_answers_and_no_counter(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        e, m = self.open(s)
        self.assertEqual(self.update(s, e, m, 0), [bytes([0x89, SAVED, 1])])
        s.cmd("commitfail 1")
        self.assertEqual(self.update(s, e, m, 1, "a3", "r3"), [bytes([0x89, SAVED, 0])])
        s.cmd("commitfail 0")
        self.assertEqual(self.update(s, e, m, 2, "a4", "r4"), [bytes([0x89, SAVED, 1])])
        s.close()

    def test_a_give_up_stays_silent(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        e, m = self.open(s)
        s.cmd("busy 1")
        self.assertEqual(self.update(s, e, m, 0), [])                 # held
        self.assertEqual(s.frames("poll 31000"), [])                  # given up: nothing (as before)
        s.cmd("busy 0")
        self.assertEqual(s.frames("poll 31010"), [])
        s.close()

    def test_a_reconnect_cycle_stays_in_step(self):
        """The old app's resend at every connection: each session's answer is the one-byte form, and a fresh proof's
        sealed hello opens with counter 0 every time (nothing for the relay to fall behind on)."""
        s = Sim("mtu 247")
        for i in range(4):
            cn, dn = bytes([i]) * 16, bytes([0x80 + i]) * 16
            s.cmd("reset 0")
            s.cmd(f"proof {cn.hex()} {dn.hex()}")
            e, m = self.open(s, cn=cn, dn=dn, pair=(ACCESS, REFRESH) if i == 0 else ("a2", "r2"))
            self.assertEqual(self.update(s, e, m, 0), [bytes([0x89, SAVED, 1])], i)   # the same pair, resent
        s.close()

    def test_the_flag_needs_a_sealed_hello(self):
        """sealed_saved alone (no sealed flag) is not a sealed hello: an unencrypted link refuses it insecure."""
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        self.assertEqual(s.w(bytes.fromhex("8900010010000000100000") + b"\x02"), [err(7)])
        s.close()

    def test_a_flagged_client_gets_the_sealed_form(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        e, m = self.open(s, SEALED_HELLO)
        out = self.update(s, e, m, 0)
        self.assertEqual(len(out), 1)       # one write, whatever the build's fragment buffer
        self.assertEqual(out[0][:7], bytes([0x89, SAVED, 7]) + (29).to_bytes(4, "little"))
        pt = seal.unseal(e, m, seal.DEVICE_TO_CLIENT, 1, out[0][7:])
        self.assertEqual(pt, b"\x01" + hashlib.sha256(b"r2").digest()[:8])
        s.close()


class SealedData(unittest.TestCase):
    """Sealed data (mgcommands.h, Sealed data): the sealed_data vector frame by frame, and the rules around
    it."""

    def session(self, mtu=100, cn=CN1, dn=DN1):
        s = Sim(f"mtu {max(mtu, MIN_MTU)}", f"proof {cn.hex()} {dn.hex()}", "hello 4096 4096")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:3], bytes([0x89, HELLO, 1]))
        self.assertEqual(s.w(STATE_OPEN), [])
        opened_at(s, mtu)
        return s

    def sealed_data(self, counter, pt, cn=CN1, dn=DN1, d=seal.CLIENT_TO_DEVICE):   # self unused (a helper)
        """A sealed data message carrying pt in a ServiceResponseEnvelope (env())."""
        e, m = seal.seal_keys(K, cn, dn, NODE)
        return seal.seal(e, m, d, counter, env(pt))

    def send(self, s, frames):
        """Every frame's notifications, per frame."""
        return [s.w(f) for f in frames]

    def test_reference_vectors(self):
        self.replay_reference("sealed_data")

    def test_reference_vectors_large(self):
        """sealed_data_large: the same exchange with the device's sealed credentials_saved. The vector's device sent data (counter 1) before its answer (2); this device sends no
        data, so its answer is counter 1: the same plaintext, checked against the vector's frame."""
        self.replay_reference("sealed_data_large")

    def replay_reference(self, section):
        v = SEALV[section]
        k, mtu = bytes.fromhex(v["K"]), v["max_write"] + 3
        cn, dn = bytes.fromhex(v["client_nonce"]), bytes.fromhex(v["device_nonce"])
        answer = None   # this device's sealed credentials_saved, from the update's step
        first = json.loads(bytes.fromhex(v["steps"][0]["plaintext"]))
        s = Sim(f"mtu {mtu}", "hello 4096 4096", "expiry 1", f"k {k.hex()}", f"node {v['node_id']}",
                f"access {first['access_token']}", f"refresh {first['refresh_token']}",
                f"proof {v['client_nonce']} {v['device_nonce']}")
        checked = 0
        nxt, taken_by_bad = 0, None     # the client's next counter; a bad-tag message a no-tag build took
        for i, st in enumerate(v["steps"]):
            frames = [bytes.fromhex(f) for f in st["frames"]]
            if st["direction"] == seal.DEVICE_TO_CLIENT:
                if i == 0:      # the device's credentials, counter 0: byte for byte
                    out = s.w(SEALED_HELLO) + s.frames("pump")
                    self.assertEqual(out[0][:3], bytes([0x89, HELLO, 1]))
                    if FRAME_MAX >= v["max_write"]:
                        self.assertEqual(out[1:], frames, st["step"])
                    else:       # a smaller fragment buffer: the same sealed bytes in more writes
                        self.assertEqual(reassemble(out[1:], mtu, True).hex(), st["sealed"], st["step"])
                    self.assertEqual(s.w(STATE_OPEN), [])          # the client opens its VM session
                    checked += 1
                elif frames[0][1] == SAVED:     # the vector's answer: the plaintext this device sent at counter 1
                    self.assertIsNotNone(answer, st["step"])
                    self.assertEqual(saved_pt(answer, 1, cn, dn, v["node_id"], k),
                                     saved_pt(frames, st["counter"], cn, dn, v["node_id"], k), st["step"])
                    checked += 1
                continue        # the device's own data: a credentials-only device sends none (layer 1 seals it)
            outs = self.send(s, frames)
            sub = frames[0][1]
            ctr = st["counter"]
            if st["sealed"] and st["outcome"] == "refused" and ctr == nxt and not SEALED_DATA:
                # refused at its tag by a checking receiver; a build without the tag takes it (its counter used)
                self.assertEqual(outs, [[] for _ in outs[:-1]] + [[credit(len(bytes.fromhex(st["sealed"])))]], st["step"])
                taken_by_bad, nxt = ctr, ctr + 1
                checked += 1
                continue
            if taken_by_bad is not None and ctr == taken_by_bad:
                # the real message with that counter: refused at its first write in a build without the tag
                self.assertEqual(outs[0], [err(3), credit(len(frames[0]) - 7)], st["step"])
                taken_by_bad = None
                checked += 1
                continue
            if st["sealed"] and st["outcome"] in ("message", "aborted"):
                nxt = ctr + 1
            if st["outcome"] == "message":
                for o in outs[:-1]:
                    self.assertEqual(o, [], st["step"])
                if sub == UPDATE:   # sealed, counter 1 (this device sends no data: credentials 0, then this)
                    refresh = json.loads(bytes.fromhex(st["plaintext"]))["refresh_token"]
                    answer = saved(1, True, refresh, cn, dn, mtu, v["node_id"], k)
                    self.assertEqual(outs[-1], answer, st["step"])
                else:           # dropped whole: every sealed byte back as credit
                    self.assertEqual(outs[-1], [credit(len(bytes.fromhex(st["sealed"])))], st["step"])
            elif st["outcome"] == "refused":
                self.assertEqual(len(frames), 1)
                self.assertEqual(outs[-1], [bytes.fromhex(st["answer"]), credit(len(frames[0]) - 7)], st["step"])
            else:               # aborted after its first write: its bytes back, its counter used
                self.assertEqual(st["outcome"], "aborted")
                self.assertEqual(outs, [[], [credit(len(frames[0]) - 7)]], st["step"])
            checked += 1
        self.assertGreaterEqual(checked, 8)
        show = s.cmd("show")[0]
        self.assertIn("access=mga_data_access_2 refresh=mgr_data_refresh_2", show)
        s.close()

    def test_interleaved_vectors(self):
        """sealed_interleaved (two messages in flight) frame by frame: a credentials_update at 0
        refused at its tag after data 1 went through; the real update 0 accepted (the hole), its replay refused, data 2
        accepted. Every write answered as the reference receiver does."""
        v = SEALV["sealed_interleaved"]
        mtu = v["max_write"] + 3
        s = Sim(f"mtu {mtu}", "hello 4096 4096", f"k {v['K']}", f"node {v['node_id']}", "access mga_il_0",
                "refresh mgr_il_0", f"proof {v['client_nonce']} {v['device_nonce']}")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:3], bytes([0x89, HELLO, 1]))
        self.assertEqual(s.w(STATE_OPEN), [])
        checked = 0
        for st in v["steps"]:
            frames = [bytes.fromhex(f) for f in st["frames"]]
            outs = self.send(s, frames)
            sub = frames[0][1]
            if st["outcome"] == "partial":
                self.assertEqual(outs, [[] for _ in frames], st["step"])
            elif st["outcome"] == "message":
                self.assertEqual(outs[:-1], [[] for _ in frames[:-1]], st["step"])
                if sub == UPDATE:   # sealed under device counter 1 (the credentials took 0)
                    refresh = json.loads(bytes.fromhex(st["plaintext"]))["refresh_token"]
                    self.assertEqual(outs[-1], saved(1, True, refresh, bytes.fromhex(v["client_nonce"]),
                                                     bytes.fromhex(v["device_nonce"]), mtu, v["node_id"],
                                                     bytes.fromhex(v["K"])), st["step"])
                else:   # dropped whole: its sealed bytes back
                    self.assertEqual(outs[-1], [credit(int.from_bytes(frames[0][3:7], "little"))], st["step"])
            else:
                self.assertEqual(st["outcome"], "refused")
                ans = bytes.fromhex(st["answer"])
                if sub == UPDATE:   # every write of it refused (nothing open after its first)
                    self.assertEqual(outs, [[ans] for _ in frames], st["step"])
                else:
                    self.assertEqual(outs[-1][0], ans, st["step"])
            checked += 1
        self.assertEqual(checked, len(v["steps"]))
        upd = json.loads(bytes.fromhex([x for x in v["steps"] if x["outcome"] == "message" and
                                        bytes.fromhex(x["frames"][0])[1] == UPDATE][0]["plaintext"]))
        self.assertIn(f"access={upd['access_token']} refresh={upd['refresh_token']}", s.cmd("show")[0])
        s.close()

    def test_receiver_limits(self):
        """The vectors' receiver_limits: the device's receiving rules, each case in a session of its own:
        a first write without the whole counter, total_len past max_message or with no plaintext, a last write short
        of total_len, bytes past it, a first write while one is open, a continuation without the sealed flag, a hole
        more than 255 counters back. Every refused write's first answer is the vector's; every message it takes
        gives its sealed bytes back."""
        v = SEALV["receiver_limits"]
        mtu = v["max_write"] + 3
        for case in v["cases"]:
            with self.subTest(case["name"]):
                s = Sim(f"mtu {mtu}", f"hello {v['max_message']} {v['max_message']}", f"k {v['K']}",
                        f"node {v['node_id']}", f"proof {case['client_nonce']} {case['device_nonce']}")
                out = s.w(SEALED_HELLO) + s.frames("pump")
                self.assertEqual(out[0][:3], bytes([0x89, HELLO, 1]))
                self.assertEqual(s.w(STATE_OPEN), [])
                for st in case["steps"]:
                    frames = [bytes.fromhex(f) for f in st["frames"]]
                    outs = self.send(s, frames)
                    if st["outcome"] == "partial":
                        self.assertEqual(outs, [[] for _ in frames], st["step"])
                    elif st["outcome"] == "message":            # data, one or more whole messages
                        total = 0
                        for f, o in zip(frames, outs):
                            self.assertEqual(f[1], DATA, st["step"])
                            total = int.from_bytes(f[3:7], "little") if f[2] & FIRST else total
                            self.assertEqual(o, [credit(total)] if f[2] & LAST else [], st["step"])
                    else:
                        self.assertEqual(st["outcome"], "refused")
                        self.assertEqual(outs[-1][:1], [bytes.fromhex(st["answer"])], st["step"])
                s.close()

    @unittest.skipUnless(SEALED_DATA, "data's tag is checked (MGDS_SEALED_DATA)")
    def test_a_hole_at_the_top(self):
        """Counters never wrap, but the one hole still may come: data at 0xFFFFFFFE refused at its tag after the update
        at 0xFFFFFFFF went through comes again once; then nothing is next (0 refused)."""
        s = self.session()
        s.cmd("counters 0xFFFFFFFE -")
        bd = bytearray(self.sealed_data(0xFFFFFFFE, b"y" * 200))
        bd[-1] ^= 1
        df = data_frames(bytes(bd), 100, True)
        self.assertEqual(s.w(df[0]), [])                                  # the data takes 0xFFFFFFFE
        out = []
        for f in update_frames(sealed_update(CN1, DN1, 0xFFFFFFFF, "a1", "r1"), 100, True):
            out += s.w(f)
        self.assertEqual(out, saved(1, True, "r1", mtu=100))              # the update takes 0xFFFFFFFF, the last
        self.assertEqual(self.send(s, df[1:])[-1], [err(3), credit(len(bd))])   # the data's tag: refused, a hole
        real = self.sealed_data(0xFFFFFFFE, b"z" * 20)
        self.assertEqual(self.send(s, data_frames(real, 100, True)), [[credit(len(real))]])   # the hole, once
        self.assertEqual(self.send(s, data_frames(real, 100, True))[0][0], err(3))           # not twice
        zero = self.sealed_data(0, b"w" * 20)
        self.assertEqual(self.send(s, data_frames(zero, 100, True)), [[err(3), credit(len(zero))]])   # no wrap
        s.close()

    @unittest.skipUnless(SEALED_DATA, "data's tag is checked (MGDS_SEALED_DATA)")
    def test_tag_refused_with_another_message_in_flight(self):
        """A message refused at its tag doesn't use its counter even when a later counter was taken meanwhile (a
        credentials_update and a data message in flight together): the refused counter may
        still come, once; the counters after it go on."""
        s = self.session(64)

        def update(m):
            out = []
            for f in update_frames(m, 64, True):
                out += s.w(f)
            return out

        bad = bytearray(sealed_update(CN1, DN1, 0, "a" * 80, "r1"))
        bad[-1] ^= 1
        uf = update_frames(bytes(bad), 64, True)
        self.assertEqual(s.w(uf[0]), [])                                  # the update takes 0
        d1 = self.sealed_data(1, b"x" * 30)
        self.assertEqual(self.send(s, data_frames(d1, 64, True)), [[credit(len(d1))]])   # data takes 1 meanwhile
        outs = self.send(s, uf[1:])
        self.assertEqual(outs[-1], [err(3)])                             # the update's tag: refused
        self.assertEqual(update(sealed_update(CN1, DN1, 0, "a2", "r2")), saved(1, True, "r2", mtu=64))   # the real 0
        self.assertIn("access=a2 refresh=r2", s.cmd("show")[0])
        self.assertEqual(update(sealed_update(CN1, DN1, 0, "a3", "r3"))[0], err(3))   # 0 again: a replay
        self.assertEqual(update(sealed_update(CN1, DN1, 2, "a4", "r4")), saved(2, True, "r4", mtu=64))   # on from 2
        # the other way round: data at 3 refused at its tag while the update at 4 went through
        bd = bytearray(self.sealed_data(3, b"y" * 80))
        bd[-1] ^= 1
        df = data_frames(bytes(bd), 64, True)
        self.assertEqual(s.w(df[0]), [])
        self.assertEqual(update(sealed_update(CN1, DN1, 4, "a5", "r5")), saved(3, True, "r5", mtu=64))
        self.assertEqual(self.send(s, df[1:])[-1], [err(3), credit(len(bd))])
        d3 = self.sealed_data(3, b"z" * 20)
        self.assertEqual(self.send(s, data_frames(d3, 64, True)), [[credit(len(d3))]])    # the real 3
        d5 = self.sealed_data(5, b"w" * 20)
        self.assertEqual(self.send(s, data_frames(d5, 64, True)), [[credit(len(d5))]])
        # with nothing in flight, the plain rule: a bad-tag update at 6 (MTU 23's fragments), then the real 6
        s.cmd("mtu 23")
        bad = bytearray(sealed_update(CN1, DN1, 6, "a" * 50, "r6"))
        bad[-1] ^= 1
        out = []
        for f in update_frames(bytes(bad), 23, True):
            out += s.w(f)
        self.assertEqual(out, [err(3)])
        out = []
        for f in update_frames(sealed_update(CN1, DN1, 6, "a6", "r6"), 23, True):
            out += s.w(f)
        self.assertEqual(out, saved(4, True, "r6", mtu=23))       # MTU 23: the answer in two writes
        s.close()

    def test_bad_tag_and_the_counter(self):
        s = self.session()
        pt = bytes(range(200))
        bad = bytearray(self.sealed_data(0, pt))
        bad[100] ^= 1
        outs = self.send(s, data_frames(bytes(bad), 100, True))
        want = [err(3), credit(len(bad))] if SEALED_DATA else [credit(len(bad))]
        self.assertEqual(outs[-1], want)                  # refused at the last write (or, without the tag, dropped)
        self.assertTrue(all(o == [] for o in outs[:-1]))
        good0 = self.sealed_data(0, pt)
        if SEALED_DATA:                                   # refused at its tag: counter 0 unused, the real one goes in
            self.assertEqual(self.send(s, data_frames(good0, 100, True))[-1], [credit(len(good0))])
        else:                                             # no tag check: the bad one was taken, so 0 is used
            outs = self.send(s, data_frames(good0, 100, True))
            self.assertEqual(outs[0], [err(3), credit(len(data_frames(good0, 100, True)[0]) - 7)])
            for w in outs[1:]:
                self.assertEqual(w[0], err(3))            # nothing open: every later write refused
        good1 = self.sealed_data(1, pt)
        self.assertEqual(self.send(s, data_frames(good1, 100, True))[-1], [credit(len(good1))])
        # the shared counter: the next credentials_update is counter 2
        out = []
        for f in update_frames(sealed_update(CN1, DN1, 2, "acc-d", "ref-d"), 100, True):
            out += s.w(f)
        self.assertEqual(out, saved(1, True, "ref-d", mtu=100))
        s.close()

    def test_the_tag_bytes_split_across_writes(self):
        # Every split of the last 16 bytes (the tag) and of the counter's write: the tag still checks.
        for size in range(1, 60):
            s = self.session(mtu=23 + (size % 7))
            sealed = self.sealed_data(0, bytes((i * 7) & 0xFF for i in range(size)))
            outs = self.send(s, data_frames(sealed, 23 + (size % 7), True))
            self.assertEqual(outs[-1], [credit(len(sealed))], size)
            s.close()

    def test_unsealed_and_malformed(self):
        s = self.session()
        # Unsealed data in a sealed session: insecure, nothing changes (its bytes back); counter 0 still next.
        self.assertEqual(self.send(s, data_frames(b"plain", 100, False)), [[err(7), credit(5)]])
        # A sealed message too short to hold a counter and a tag.
        short = bytes([0x89, DATA, FIRST | LAST | SEALED]) + (20).to_bytes(4, "little") + bytes(20)
        self.assertEqual(s.w(short), [err(3), credit(20)])
        # A sealed message's continuation without the sealed flag: framing error, the message dropped.
        fr = data_frames(self.sealed_data(0, bytes(150)), 100, True)
        self.assertEqual(s.w(fr[0]), [])
        plain = bytearray(fr[1])
        plain[2] &= ~SEALED & 0xFF
        self.assertEqual(s.w(bytes(plain)), [err(3), credit(len(fr[0]) - 7 + len(fr[1]) - 3)])
        # Counter 0 was used by that message; counter 1 goes through.
        ok = self.sealed_data(1, b"after")
        self.assertEqual(self.send(s, data_frames(ok, 100, True)), [[credit(len(ok))]])
        s.close()

    def test_credit_without_a_buffer_goes_from_the_poll(self):
        # The stack has no buffer for a message's credit: owed (busy), sent from the next poll, never lost.
        s = self.session()
        m0 = self.sealed_data(0, bytes(50))
        s.cmd("again 1")
        out = sum(self.send(s, data_frames(m0, 100, True)), []) + s.frames("poll 10")
        self.assertIn("A", s.events)
        self.assertEqual(out, [credit(len(m0))], "sent once, from the write's flush or the next poll")
        m1 = self.sealed_data(1, bytes(50))
        self.assertEqual(self.send(s, data_frames(m1, 100, True)), [[credit(len(m1))]])
        s.close()

    def test_state_and_reset(self):
        s = self.session()
        fr = data_frames(self.sealed_data(0, bytes(150)), 100, True)
        self.assertEqual(s.w(fr[0]), [])
        # A state change ends the client's VM session: the message is gone, its counter stays used, and the
        # counters don't restart (only a sealed hello does).
        # (MGDS_DATA: a closed the device didn't ask for is asked again at once)
        self.assertEqual(s.w(bytes([0x89, STATE, 0, 2, 0])), [OPEN_REQ1] if DATA_PATH else [])
        self.assertEqual(s.w(fr[1])[0], err(3))           # no VM session: refused
        self.assertEqual(s.w(STATE_OPEN), [])
        replay = self.sealed_data(0, b"x")
        self.assertEqual(self.send(s, data_frames(replay, 100, True)), [[err(3), credit(len(replay))]])
        ok = self.sealed_data(1, b"x")
        self.assertEqual(self.send(s, data_frames(ok, 100, True)), [[credit(len(ok))]])
        # A new proof and sealed hello: new keys, counters from 0.
        s.cmd(f"proof {CN2.hex()} {DN2.hex()}")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:3], bytes([0x89, HELLO, 1]))
        self.assertEqual(s.w(STATE_OPEN), [])
        new0 = self.sealed_data(0, b"y", CN2, DN2)
        self.assertEqual(self.send(s, data_frames(new0, 100, True)), [[credit(len(new0))]])
        s.close()

    def test_no_plain_session(self):
        # An unsealed hello on an encrypted link is refused: no session, so the data
        # after it is refused as data before a hello.
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}", "enc 1", "hello 4096 4096")
        self.assertEqual(s.w(bytes.fromhex("8900010010000000100000")), [err(7)])
        self.assertEqual(s.w(STATE_OPEN), [err(3)])
        self.assertEqual(self.send(s, data_frames(b"plain data", 100, False)), [[err(3)]])
        s.close()


@unittest.skipUnless(TAG_LEND, "an MGDS_TAG_LEND build")
class TagLend(SealedData):
    """MGDS_TAG_LEND: the tag state lent per sealed data message by the board, returned (wiped) whenever the message
    ends: accepted, refused, aborted, a state change, a reset."""

    def lent(self, s):
        line = [x for x in s.cmd("show") if x.startswith("S lent=")][0]
        return dict(kv.split("=") for kv in line[2:].split())

    def test_lend_and_return(self):
        s = self.session()
        ok = self.sealed_data(0, bytes(300))
        outs = self.send(s, data_frames(ok, 100, True))
        self.assertEqual(outs[-1], [credit(len(ok))])
        st = self.lent(s)
        self.assertEqual((st["lent"], st["lends"], st["returns"]), ("0", "1", "1"))
        bad = bytearray(self.sealed_data(1, b"forged"))
        bad[-1] ^= 1
        self.assertEqual(self.send(s, data_frames(bytes(bad), 100, True))[-1], [err(3), credit(len(bad))])
        fr = data_frames(self.sealed_data(1, bytes(150)), 100, True)   # aborted: returned
        self.assertEqual(s.w(fr[0]), [])
        self.assertEqual(self.lent(s)["lent"], "1")
        self.assertEqual(s.w(bytes([0x89, ABORT])), [credit(len(fr[0]) - 7)])
        fr = data_frames(self.sealed_data(2, bytes(150)), 100, True)   # a state change: returned
        s.w(fr[0])
        s.w(bytes([0x89, STATE, 0, 2, 0]))
        self.assertEqual(s.w(STATE_OPEN), [])
        fr = data_frames(self.sealed_data(3, bytes(150)), 100, True)   # a reset: returned
        s.w(fr[0])
        s.cmd("reset 1")
        st = self.lent(s)
        self.assertEqual((st["lent"], st["lends"], st["returns"]), ("0", "5", "5"))
        s.close()

    def test_key_read_failure_leaves_the_lent_state_alone(self):
        # K gone mid-session (MGDS_KEEP_KEYS=0 derives the keys per message): the lent tag state is never begun, so
        # nothing hashes into its leftovers; the message is refused at its end, its bytes back, the state returned.
        s = self.session()
        s.cmd("nopair")
        m0 = self.sealed_data(0, bytes(250))
        outs = self.send(s, data_frames(m0, 100, True))
        self.assertTrue(all(o == [] for o in outs[:-1]))
        self.assertEqual(outs[-1], [err(3), credit(len(m0))])
        self.assertEqual(self.lent(s)["lent"], "0")
        s.close()

    def test_no_lend_is_busy_and_keeps_the_counter(self):
        s = self.session()
        s.cmd("taglend 0")
        m0 = self.sealed_data(0, bytes(250))
        outs = self.send(s, data_frames(m0, 100, True))
        self.assertTrue(all(o == [] for o in outs[:-1]))
        self.assertEqual(outs[-1], [err(4), credit(len(m0))])          # busy at its end, its bytes back
        s.cmd("taglend 1")
        self.assertEqual(self.send(s, data_frames(m0, 100, True))[-1], [credit(len(m0))])   # counter 0 unused
        s.close()


@unittest.skipUnless(END_ON_BAD_TAG, "an MGDS_END_ON_BAD_TAG build")
class EndOnBadTag(unittest.TestCase):
    """MGDS_END_ON_BAD_TAG: a sealed message refused at its tag (data or credentials_update) also ends the sealed
    session: the next message is refused until a new proof and one sealed hello."""

    def check_ended(self, s):
        later = data_frames(SealedData.sealed_data(None, 0, b"later"), 100, True)
        self.assertEqual(s.w(later[0])[0], err(3))                   # no session: data refused
        self.assertEqual(s.w(SEALED_HELLO), [err(6)])                # the proof's nonces were taken
        s.cmd(f"proof {CN2.hex()} {DN2.hex()}")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:3], bytes([0x89, HELLO, 1]))       # a new proof: a new session
        self.assertEqual(s.w(STATE_OPEN), [])
        m = SealedData.sealed_data(None, 0, b"new", CN2, DN2)
        self.assertEqual([s.w(f) for f in data_frames(m, 100, True)][-1], [credit(len(m))])

    def test_bad_data_tag_ends_the_session(self):
        s = SealedData.session(self)
        bad = bytearray(SealedData.sealed_data(None, 0, bytes(120)))
        bad[50] ^= 1
        outs = [s.w(f) for f in data_frames(bytes(bad), 100, True)]
        self.assertEqual(outs[-1], [err(3), credit(len(bad))])
        self.check_ended(s)
        s.close()

    def test_bad_update_tag_ends_the_session(self):
        s = SealedData.session(self)
        bad = bytearray(sealed_update(CN1, DN1, 0, "a2", "r2"))
        bad[-1] ^= 1
        out = []
        for f in update_frames(bytes(bad), 100, True):
            out += s.w(f)
        self.assertEqual(out, [err(3)])
        self.check_ended(s)
        s.close()

    def test_counter_and_insecure_refusals_dont_end_it(self):
        s = SealedData.session(self)
        replay = SealedData.sealed_data(None, 1, b"x")              # counter 1 while 0 is next: no crypto spent
        self.assertEqual([s.w(f) for f in data_frames(replay, 100, True)], [[err(3), credit(len(replay))]])
        self.assertEqual([s.w(f) for f in data_frames(b"plain", 100, False)], [[err(7), credit(5)]])
        ok = SealedData.sealed_data(None, 0, b"ok")
        self.assertEqual([s.w(f) for f in data_frames(ok, 100, True)], [[credit(len(ok))]])
        s.close()


# ---- MGDS_DATA: the data path ------------------------------------------------------------------------------------

def state(st, epoch=1):
    return bytes([0x89, STATE, st]) + epoch.to_bytes(2, "little")


CLOSED, CONNECTING, OPEN_ST, NO_NETWORK, AUTH_FAILED, NOT_ENROLLED, TOO_LARGE = range(7)


@unittest.skipUnless(SEALED_DATA, "MGDS_CTR_END needs MGDS_SEALED_DATA (default off without it)")
class CounterRefusalsEnd(unittest.TestCase):
    """MGDS_CTR_END 3: the cut-fragment epoch. The relay counted a sealed message whose first
    fragment never reached the gadget (a second epoch cut it), so every later message carries a counter one ahead and
    fails the exact-next counter rule. The third counter refusal in a row ends the session (no new message); the client's next
    open runs a fresh hello and the counters start over. Tag refusals never count, and an acceptable counter resets the
    run."""

    def session(self):
        return SealedData.session(self)

    def stats(self, s):
        """mgds_t.st.ctr_ends, or the expected value in an MGDS_STATS=0 build (no stats line)"""
        line = next((x for x in s.cmd("show") if x.startswith("S stats")), None)
        return None if line is None else int(line.split("ctr_ends=")[1].split()[0])

    def assertEnds(self, s, n):
        got = self.stats(s)
        if got is not None:
            self.assertEqual(got, n)

    def test_the_cut_fragment_epoch_ends_at_the_third(self):
        s = self.session()
        ok0 = SealedData.sealed_data(None, 0, b"m0")              # in sync: counter 0 taken
        self.assertEqual([s.w(f) for f in data_frames(ok0, 100, True)], [[credit(len(ok0))]])
        # The relay counted 1 (its first fragment cut by the next epoch, never received): its next ones are 2, 3, 4.
        for n, c in enumerate((2, 3), 1):
            m = SealedData.sealed_data(None, c, b"m")
            self.assertEqual([s.w(f) for f in data_frames(m, 100, True)], [[err(3), credit(len(m))]], f"refusal {n}")
            self.assertEnds(s, 0)
        m = SealedData.sealed_data(None, 4, b"m")
        self.assertEqual([s.w(f) for f in data_frames(m, 100, True)], [[err(3), credit(len(m))]], "refusal 3")
        self.assertEnds(s, 1)
        EndOnBadTag.check_ended(self, s)                             # ended: a fresh hello, counters from 0
        s.close()

    def test_tag_refusals_and_an_accepted_counter_reset_the_run(self):
        s = self.session()
        for c in (1, 2):                                            # two counter refusals (0 is next)
            m = SealedData.sealed_data(None, c, b"x")
            self.assertEqual([s.w(f) for f in data_frames(m, 100, True)], [[err(3), credit(len(m))]])
        bad = bytearray(SealedData.sealed_data(None, 0, bytes(60)))  # the next counter, a forged tag
        bad[30] ^= 1
        out = [s.w(f) for f in data_frames(bytes(bad), 100, True)]
        if SEALED_DATA:
            self.assertEqual(out[-1], [err(3), credit(len(bad))])   # refused at its tag: the run reset, not counted
        for c in (1, 2):                                            # two more counter refusals: still not three in a row
            m = SealedData.sealed_data(None, c if SEALED_DATA else c + 1, b"y")
            self.assertEqual([s.w(f) for f in data_frames(m, 100, True)], [[err(3), credit(len(m))]])
        self.assertEnds(s, 0)
        good = SealedData.sealed_data(None, 0 if SEALED_DATA else 1, b"ok")   # the session goes on
        self.assertEqual([s.w(f) for f in data_frames(good, 100, True)], [[credit(len(good))]])
        s.close()

    def test_update_counter_refusals_count_too(self):
        s = self.session()
        for c in (2, 3, 4):                                         # credentials_update with a counter ahead, three times
            out = []
            for f in update_frames(sealed_update(CN1, DN1, c, "a", "r"), 100, True):
                out += s.w(f)
            self.assertEqual(out, [err(3)])
        self.assertEnds(s, 1)
        EndOnBadTag.check_ended(self, s)
        s.close()


class Nudge(unittest.TestCase):
    """The keepalive (mgcommands.h, Credit): on a session the client
    reported open, one [session, credit, 0], or the credit owed with it; none before the hello or state open, or after
    another state; the stack without a buffer: from the next poll, dropped if the session ends first."""

    def test_nudge(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        self.assertEqual(s.frames("nudge"), [], "no session")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
        self.assertEqual(s.frames("nudge"), [], "the client hasn't reported the session open")
        self.assertEqual(s.w(STATE_OPEN), [])
        self.assertEqual(s.frames("nudge"), [credit(0)])
        self.assertEqual(s.frames("nudge"), [credit(0)], "again: one more, nothing held")
        self.assertIn("busy=0", s.cmd("show")[0])
        # the stack without a buffer: owed (busy), from the next poll
        s.cmd("again 1")
        self.assertEqual(s.frames("nudge"), [])
        self.assertIn("busy=1", s.cmd("show")[0])
        self.assertEqual(s.frames("poll 10"), [credit(0)])
        self.assertIn("busy=0", s.cmd("show")[0])
        # credit owed goes with it, in one frame
        m = SealedData.sealed_data(None, 0, b"x" * 30)
        # the message's credit finds no buffer (a data path build offers it again at the write's end)
        s.cmd(f"again {2 if DATA_PATH else 1}")
        self.assertEqual([s.w(f) for f in data_frames(m, 247, True)], [[]])
        self.assertEqual(s.frames("nudge"), [credit(len(m))])
        self.assertEqual(s.frames("poll 10"), [], "nothing more owed")
        # a message still arriving: nothing for it until it ends (its credit then, whole, once), as the ESP32's nudge
        m = SealedData.sealed_data(None, 1, b"y" * 300)
        frames = data_frames(m, 100, True)
        self.assertEqual(s.w(frames[0]), [])
        self.assertEqual(s.frames("nudge"), [credit(0)], "a message part-way in owes nothing yet")
        back = [f for fr in frames[1:] for f in s.w(fr) if f[:2] == bytes([0x89, CREDIT])]
        self.assertEqual(back, [credit(len(m))], "its whole credit at its end")
        # an owed keepalive is dropped when the session moves on; none after another state
        # (a data path build may ask to reopen after the closed: open_request, never a credit)
        credits = lambda frames: [f for f in frames if f[:2] == bytes([0x89, CREDIT])]   # noqa: E731
        s.cmd("again 1")
        self.assertEqual(s.frames("nudge"), [])
        self.assertEqual(credits(s.w(bytes([0x89, STATE, 0, 1, 0]))), [])     # closed
        self.assertEqual(credits(s.frames("poll 10")), [])
        self.assertEqual(credits(s.frames("nudge")), [], "closed")
        self.assertEqual(credits(s.frames("poll 10")), [])
        s.close()


class Port:
    """The device's VM client on the C gadget's data path, as a board would write it: the reference device's VM end
    (mg_session_ref.Device: POST /link-control, link.register, link.invoke -> link.result, from its handle() and
    start_session()) driven by the sim's data_state / data_message (or data_piece / data_end) / data_sent, its
    messages handed to mgds_send() one at a time. Every sim command's frames come back in order, with whatever the
    port sent in answer to that command's events."""

    def __init__(self, sim, mtu=247):
        self.s = sim
        self.dev = ref.Device(mtu)
        self.q, self.lent = [], False
        self.got, self.states, self.sent = [], [], []    # messages the port took, states it saw, data_sent results
        self.pieces = bytearray()

    def take(self, lines):
        frames = []
        for x in lines:
            if x.startswith("N "):
                frames.append(bytes.fromhex(x[2:]))
            elif x.startswith("ST "):
                st, ep = map(int, x[3:].split())
                self.states.append((st, ep))
                self.dev.reset_session()
                self.q = []                     # the port drops its streams; a lent message comes back SENT 0
                self.pieces = bytearray()
                self.dev.open = st == OPEN_ST
                if st == OPEN_ST:
                    self.dev.start_session()
            elif x.startswith("D "):
                self.message(bytes.fromhex(x[2:]))
            elif x.startswith("P "):
                off, h, n = x[2:].split()
                assert int(off) == len(self.pieces), x
                self.pieces += bytes.fromhex(h)
                assert len(self.pieces) <= int(n)
            elif x.startswith("PE "):
                if x == "PE 1":
                    self.message(bytes(self.pieces))
                self.pieces = bytearray()
            elif x.startswith("SENT "):
                self.sent.append(x == "SENT 1")
                self.lent = False
                if x == "SENT 1":
                    self.q.pop(0)
        self.q += self.dev.outq
        self.dev.outq = []
        return frames

    def message(self, m):
        self.got.append(m)
        payload = envelope_payload(m)               # a ServiceResponseEnvelope's ServiceFrame (the core checked it)
        if self.dev.open and payload is not None:
            try:
                self.dev.handle(payload)
            except (KeyError, IndexError, ValueError):
                pass                             # not a reference VM message (a test's own bytes)

    def drain(self):
        frames = []
        while self.q and not self.lent:
            out = self.s.cmd("send " + self.q[0].hex())
            if "Q 0" not in out:
                break
            self.lent = True
            frames += self.take(out)
        return frames

    def cmd(self, line):
        frames = self.take(self.s.cmd(line))
        return frames + self.drain()

    def w(self, frame):
        return self.cmd("w " + frame.hex())


def merged(frames):
    """For a build whose fragment buffer is under the MTU: the frames with each run of data fragments merged into its
    payload bytes (the same messages, in more writes)."""
    out = []
    for f in frames:
        if f[:2] == bytes([0x89, DATA]):
            pay = f[7:] if f[2] & FIRST else f[3:]
            if out and isinstance(out[-1], bytearray):
                out[-1] += pay
            else:
                out.append(bytearray(pay))
        else:
            out.append(f.hex())
    return [bytes(x).hex() if isinstance(x, bytearray) else x for x in out]


@unittest.skipUnless(DATA_PATH, "an MGDS_DATA build")
class DataVectors(unittest.TestCase):
    """protocols/test-vectors/mg-session-v1.json: every write of every exchange through the C gadget with its data
    path on, a board's VM client on top (the reference device's VM end), every notification byte for byte; the
    credentials after each hello are this device's (no device key fields), sealed as the vector's; every session
    sealed, data both ways."""

    def run_exchange(self, ex):
        s = vector_sim(ex)
        port = Port(s, ex["mtu"])

        def compare(got, want, what):
            if FRAME_MAX + 3 < ex["mtu"]:   # a smaller fragment buffer: the same bytes in more writes
                self.assertEqual(merged(got), merged(want), what)
            else:
                self.assertEqual([f.hex() for f in got], [f.hex() for f in want], what)
        replay(self, ex, port.w, port.cmd, compare)
        s.close()
        return port

    def test_all_exchanges(self):
        for ex in EXCH["exchanges"]:
            with self.subTest(ex["name"]):
                self.run_exchange(ex)

    def test_all_exchanges_in_pieces(self):
        # The same, the port taking client messages in pieces (data_piece / data_end) instead of reassembled.
        orig = Sim.__init__

        def init(sim, *setup):
            orig(sim, *(setup + ("dmode piece",)))
        Sim.__init__ = init
        try:
            for ex in EXCH["exchanges"]:
                with self.subTest(ex["name"]):
                    self.run_exchange(ex)
        finally:
            Sim.__init__ = orig


def ask_frames(frames):
    return [f for f in frames if f[:2] == bytes([0x89, 2])]


@unittest.skipUnless(DATA_PATH, "an MGDS_DATA build")
class ReAsk(unittest.TestCase):
    """open_request's re-ask series, timed on the host clock (mgcommands.h, Session state;
    mg_session_ref.py): after closed 1, 2, 4 ... capped at 60 s, after no_network capped at 15 s, the first at once;
    neither series restarts the other; a series restarts only after a session open >= 60 s; a hello and a saved
    credentials_update restart both, a save while ended asking at once as the ended kind's first; none after the
    device's own [0], not_enrolled, auth_failed or too_large until a hello, a saved update or an open; a failed open
    (closed or no_network after connecting, no session open) counts as an unasked end."""

    def session(self, sealed=True):
        s = Sim("mtu 247", "enc 1", f"proof {CN1.hex()} {DN1.hex()}")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
        return s

    def expect_ask_after(self, s, ms, what):
        """No ask until ms after now, then exactly one [1]."""
        if ms:
            self.assertEqual(ask_frames(s.frames(f"poll {ms - 1}")), [], f"{what}: early")
        self.assertEqual(ask_frames(s.frames("poll 1")), [OPEN_REQ1], f"{what}: at {ms} ms")
        self.assertEqual(ask_frames(s.frames("poll 120000")), [], f"{what}: one ask only")

    def unasked_end(self, s, st, ms, what):
        out = s.w(state(st))
        if ms == 0:
            self.assertEqual(out, [OPEN_REQ1], f"{what}: at once")
            self.assertEqual(ask_frames(s.frames("poll 120000")), [], f"{what}: one ask only")
        else:
            self.assertEqual(out, [], what)
            self.expect_ask_after(s, ms, what)
        self.assertEqual(s.w(state(CONNECTING)), [])     # the client opens, as asked

    def series(self, n, cap):
        out, d = [0], 1000
        while len(out) < n:
            out.append(min(d, cap))
            d *= 2
        return out

    def test_closed_series_capped_60s(self):
        s = self.session()
        steps = self.series(12, 60000)
        self.assertEqual(steps, [0, 1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000, 60000, 60000])
        for i, ms in enumerate(steps):
            self.unasked_end(s, CLOSED, ms, f"closed #{i}")
        s.close()

    def test_no_network_series_capped_15s(self):
        s = self.session()
        steps = self.series(9, 15000)
        self.assertEqual(steps, [0, 1000, 2000, 4000, 8000, 15000, 15000, 15000, 15000])
        for i, ms in enumerate(steps):
            self.unasked_end(s, NO_NETWORK, ms, f"no_network #{i}")
        s.close()

    def test_series_are_separate(self):
        s = self.session()
        for i, ms in enumerate([0, 1000, 2000, 4000]):
            self.unasked_end(s, CLOSED, ms, f"closed #{i}")
        for i, ms in enumerate([0, 1000, 2000]):       # no_network's own series: at once, not 8 s
            self.unasked_end(s, NO_NETWORK, ms, f"no_network #{i}")
        self.unasked_end(s, CLOSED, 8000, "closed goes on where it was")
        self.unasked_end(s, NO_NETWORK, 4000, "no_network goes on where it was")
        s.close()

    def test_restart_only_after_a_session_open_60s(self):
        s = self.session()
        for i, ms in enumerate([0, 1000, 2000]):
            self.unasked_end(s, CLOSED, ms, f"closed #{i}")
        self.assertEqual(s.w(state(OPEN_ST, 2)), [])
        s.frames("poll 59999")                         # open just under 60 s
        self.unasked_end(s, CLOSED, 4000, "after a short session the series goes on")
        self.assertEqual(s.w(state(OPEN_ST, 3)), [])
        s.frames("poll 60000")                         # open 60 s
        self.unasked_end(s, CLOSED, 0, "after a 60 s session: at once")
        self.unasked_end(s, CLOSED, 1000, "then 1 s")
        # The 60 s session restarted no_network's too
        self.assertEqual(s.w(state(OPEN_ST, 4)), [])
        self.unasked_end(s, NO_NETWORK, 0, "no_network first")
        self.unasked_end(s, NO_NETWORK, 1000, "no_network then 1 s")
        self.assertEqual(s.w(state(OPEN_ST, 5)), [])
        s.frames("poll 60000")
        self.unasked_end(s, NO_NETWORK, 0, "no_network restarted by a 60 s session")
        s.close()

    def test_hello_restarts_both(self):
        s = self.session()
        for i, ms in enumerate([0, 1000, 2000]):
            self.unasked_end(s, CLOSED, ms, f"closed #{i}")
            self.unasked_end(s, NO_NETWORK, ms, f"no_network #{i}")
        s.cmd(f"proof {CN2.hex()} {DN2.hex()}")             # a new sealed hello needs a new proof
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
        self.unasked_end(s, CLOSED, 0, "closed after a hello")
        self.unasked_end(s, NO_NETWORK, 0, "no_network after a hello")
        s.close()

    def update(self, s, n):
        """A credentials_update in this sealed session (credentials are taken only sealed), its sealed
        credentials_saved shown in the one-byte form [0x89, SAVED, ok] for the comparisons below."""
        c, d = getattr(s, "uctr", 0), getattr(s, "dctr", 1)
        out = []
        for f in update_frames(sealed_update(CN1, DN1, c, f"acc-{n}", f"ref-{n}"), 247, True):
            out += s.w(f)
        s.uctr = c + 1
        sv = [f for f in out if f[:2] == bytes([0x89, SAVED])]
        rest = [f for f in out if f[:2] != bytes([0x89, SAVED])]
        if sv:
            pt = saved_pt(sv, d, CN1, DN1, NODE, K)
            s.dctr = d + 1
            rest = [bytes([0x89, SAVED, pt[0]])] + rest
        return rest

    def test_save_while_ended_asks_as_the_ended_kinds_first(self):
        s = self.session()
        for i, ms in enumerate([0, 1000, 2000]):
            self.unasked_end(s, NO_NETWORK, ms, f"no_network #{i}")
            self.unasked_end(s, CLOSED, ms, f"closed #{i}")
        # Ended with no_network (the client is still at connecting: ended is the last state's)
        self.assertEqual(s.w(state(NO_NETWORK)), [])   # its step: 4 s, pending
        self.assertEqual(self.update(s, 2), [bytes.fromhex("890801"), OPEN_REQ1])   # saved, then at once
        self.assertEqual(ask_frames(s.frames("poll 120000")), [], "the pending ask was dropped: one ask")
        self.assertEqual(s.w(state(CONNECTING)), [])
        self.unasked_end(s, NO_NETWORK, 1000, "that ask was no_network's first: next 1 s")
        self.unasked_end(s, CLOSED, 0, "closed's series restarted too")
        # Saved while open or connecting: restarts both, asks nothing
        self.assertEqual(self.update(s, 3), [bytes.fromhex("890801")])
        self.unasked_end(s, NO_NETWORK, 0, "no_network restarted by the save")
        # Saved after closed: closed's first
        self.assertEqual(s.w(state(CLOSED)), [OPEN_REQ1])
        self.assertEqual(self.update(s, 4), [bytes.fromhex("890801"), OPEN_REQ1])
        self.assertEqual(s.w(state(CONNECTING)), [])
        self.unasked_end(s, CLOSED, 1000, "closed's next step 1 s")
        # Each save asks once: two saves, two asks, no loop
        self.assertEqual(s.w(state(AUTH_FAILED)), [])
        self.assertEqual(self.update(s, 5), [bytes.fromhex("890801"), OPEN_REQ1])
        self.assertEqual(self.update(s, 6), [bytes.fromhex("890801"), OPEN_REQ1])
        self.assertEqual(ask_frames(s.frames("poll 120000")), [])
        # A failed commit isn't a save: nothing
        s.cmd("commitfail 1")
        self.assertEqual(self.update(s, 7), [bytes.fromhex("890800")])
        s.close()

    def test_save_while_ended_asks_behind_the_sealed_answer(self):
        s = self.session(sealed=True)
        self.assertEqual(s.w(state(CLOSED)), [OPEN_REQ1])
        s.cmd("again 2")                               # the stack has no buffer for the answer, twice
        out = []
        for f in update_frames(sealed_update(CN1, DN1, 0, "acc-2", "ref-2"), 247, True):
            out += s.w(f)
        self.assertEqual(out, [])                      # neither the answer nor the ask jumps the queue
        out = s.frames("poll 1")
        self.assertEqual(out, saved(1, True, "ref-2") + [OPEN_REQ1])
        s.close()

    def test_blocks(self):
        s = self.session()
        self.unasked_end(s, CLOSED, 0, "closed")
        for st, nxt in ((NOT_ENROLLED, 0), (AUTH_FAILED, 1000), (TOO_LARGE, 2000)):
            with self.subTest(st=st):
                self.assertEqual(s.w(state(st)), [])
                for later in (CLOSED, NO_NETWORK, CLOSED):
                    self.assertEqual(s.w(state(later)), [])
                    self.assertEqual(ask_frames(s.frames("poll 120000")), [])
                self.assertEqual(s.w(state(OPEN_ST, 2)), [])    # an open lifts it
                self.unasked_end(s, NO_NETWORK, nxt, "asks again after the open (short: the series goes on)")
        # The device's own [0]: none until [1], a hello, a saved update or an open
        s.cmd("openreq 0")
        self.assertEqual(s.frames("poll 1"), [OPEN_REQ0])
        self.assertEqual(s.w(state(CLOSED)), [])
        self.assertEqual(ask_frames(s.frames("poll 120000")), [])
        s.cmd("openreq 1")
        self.assertEqual(s.frames("poll 1"), [OPEN_REQ1])
        self.assertEqual(s.w(state(CONNECTING)), [])
        self.unasked_end(s, CLOSED, 1000, "after [1] the closed series goes on")
        s.cmd("openreq 0")
        s.frames("poll 1")
        self.assertEqual(s.w(state(NO_NETWORK)), [])
        self.assertEqual(self.update(s, 2), [bytes.fromhex("890801"), OPEN_REQ1])   # a save lifts it
        s.close()

    def test_connecting_drops_a_waiting_ask(self):
        s = self.session()
        self.unasked_end(s, CLOSED, 0, "first")
        self.assertEqual(s.w(state(CLOSED)), [])       # 1 s
        self.assertEqual(s.w(state(CONNECTING)), [])   # the client opens anyway: dropped
        self.assertEqual(ask_frames(s.frames("poll 120000")), [])
        self.unasked_end(s, CLOSED, 2000, "the series went on")
        s.close()

    def test_same_kind_waiting_stands(self):
        s = self.session()
        self.unasked_end(s, CLOSED, 0, "first")
        self.unasked_end(s, CLOSED, 1000, "second")
        self.assertEqual(s.w(state(CLOSED)), [])       # 2 s
        s.frames("poll 500")
        self.assertEqual(s.w(state(CLOSED)), [])       # one already waits: it stands, the series doesn't move
        self.expect_ask_after(s, 1500, "the first ask's time")
        self.assertEqual(s.w(state(CONNECTING)), [])
        self.unasked_end(s, CLOSED, 4000, "next step")
        s.close()

    def ask_pending(self, s):
        lines = [l for l in s.cmd("askpending") if l.startswith("AP ")]
        return lines[-1] == "AP 1"

    def test_ask_hold_drops_the_waiting_ask_silently(self):
        """A port with no active task drops layer 2's waiting re-ask with mgds_ask_hold(): nothing
        is sent (not even [0]), the series keeps its step, and the port's next mgds_open_request(true) asks."""
        s = self.session()
        self.unasked_end(s, CLOSED, 0, "first")
        self.assertEqual(s.w(state(CLOSED)), [])       # the 1 s ask waits
        self.assertTrue(self.ask_pending(s))
        s.cmd("askhold")
        self.assertFalse(self.ask_pending(s))
        self.assertEqual(s.frames("poll 600000"), [])  # nothing at all for 10 min
        s.cmd("openreq 1")                             # a task: the port asks
        self.assertEqual(ask_frames(s.frames("poll 0")), [OPEN_REQ1])
        self.assertEqual(s.w(state(CONNECTING)), [])
        self.unasked_end(s, CLOSED, 2000, "the series kept its step (the held 1 s ask counted)")
        # no_network the same way.
        self.unasked_end(s, NO_NETWORK, 0, "no_network first")
        self.assertEqual(s.w(state(NO_NETWORK)), [])
        s.cmd("askhold")
        self.assertEqual(s.frames("poll 600000"), [])
        s.close()

    def test_no_buffer_for_the_ask(self):
        s = self.session()
        s.cmd("again 1")
        self.assertEqual(s.w(state(CLOSED)), [])
        self.assertEqual(s.frames("poll 1"), [OPEN_REQ1])
        s.close()

    def test_link_loss_and_reset_end_everything(self):
        s = self.session()
        self.unasked_end(s, CLOSED, 0, "first")
        self.assertEqual(s.w(state(CLOSED)), [])
        s.cmd("reset 0")
        self.assertEqual(s.frames("poll 120000"), [])
        self.assertIn("busy=0", s.cmd("show")[0])
        s.close()


@unittest.skipUnless(DATA_PATH, "an MGDS_DATA build")
class DataPath(unittest.TestCase):
    """The data path itself: sealed both ways on the shared counters, credit and window, abort, the port's calls."""

    def session(self, mtu=247, hello=SEALED_HELLO, mode="buf", cn=CN1, dn=DN1, extra=()):
        s = Sim(f"mtu {max(mtu, MIN_MTU)}", f"proof {cn.hex()} {dn.hex()}", "hello 4096 4096", f"dmode {mode}", *extra)
        out = s.w(hello) + s.frames("pump")
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
        self.assertEqual(s.cmd("w " + STATE_OPEN.hex()), ["ST 2 1"])
        opened_at(s, mtu)
        return s

    def keys(self, cn=CN1, dn=DN1):
        return seal.seal_keys(K, cn, dn, NODE)

    def device_msg(self, frames, counter, mtu=247, cn=CN1, dn=DN1):
        """A device data message from its frames: framing checked, opened under counter (plaintext)."""
        blob, total = b"", None
        for i, f in enumerate(frames):
            self.assertEqual(f[:2], bytes([0x89, DATA]))
            self.assertLessEqual(len(f), min(mtu - 3, FRAME_MAX))
            self.assertTrue(f[2] & SEALED)
            self.assertEqual(bool(f[2] & FIRST), i == 0)
            self.assertEqual(bool(f[2] & LAST), i == len(frames) - 1)
            if i == 0:
                total = int.from_bytes(f[3:7], "little")
                blob += f[7:]
            else:
                blob += f[3:]
        self.assertEqual(len(blob), total)
        e, m = self.keys(cn, dn)
        return seal.unseal(e, m, seal.DEVICE_TO_CLIENT, counter, blob)

    def send(self, s, msg):
        lines = s.cmd("send " + msg.hex())
        self.assertIn("Q 0", lines)
        return [bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")], lines

    def test_caps_and_limits(self):
        s = self.session()
        self.assertEqual(s.cmd("caps"), [f"caps {0x23 if CAP_SEALED_CLEAR else 0x03:02x}"])
        self.assertEqual(s.cmd("sendmax"), [f"SM {4096 - 20}"])       # the client's max_message less the seal
        self.assertEqual(s.cmd("cansend 4076"), ["CS 1"])
        self.assertEqual(s.cmd("cansend 4077"), ["CS 0"])
        self.assertEqual(s.cmd("cansend 0"), ["CS 0"])
        self.assertIn("Q -2", s.cmd("send " + bytes(4077).hex()))
        self.assertEqual(s.cmd("vmstate"), ["VS 2 1"])
        s.close()
        s = Sim("mtu 247", "enc 1", f"proof {CN1.hex()} {DN1.hex()}")
        self.assertEqual(s.cmd("cansend 10"), ["CS 0"])                # no session
        self.assertEqual(s.w(PLAIN_HELLO), [err(7)])                   # no plain session
        self.assertEqual(s.w(STATE_OPEN), [err(3)])
        self.assertEqual(s.cmd("cansend 10"), ["CS 0"])
        self.assertEqual(s.cmd("sendmax"), ["SM 0"])
        s.close()

    def test_sealed_both_ways_on_the_shared_counters(self):
        s = self.session()
        frames, lines = self.send(s, b"device message one")
        self.assertEqual(self.device_msg(frames, 1), b"device message one")   # the credentials took 0
        self.assertIn("SENT 1", lines)
        e, m = self.keys()
        cm = seal.seal(e, m, seal.CLIENT_TO_DEVICE, 0, env(b"client message zero"))
        lines = []
        for f in data_frames(cm, 247, True):
            lines += s.cmd("w " + f.hex())
        self.assertEqual(lines, ["N " + credit(len(cm)).hex(), "D " + env(b"client message zero").hex()])
        upd = []
        for f in update_frames(sealed_update(CN1, DN1, 1, "acc-2", "ref-2"), 247, True):
            upd += s.w(f)
        self.assertEqual(upd, saved(2, True, "ref-2"))               # the answer: 2
        frames, _ = self.send(s, bytes(range(256)) * 3)
        self.assertEqual(self.device_msg(frames, 3), bytes(range(256)) * 3)    # then data: 3
        s.close()

    def test_answer_waits_for_the_data_and_counter_order(self):
        # The client's window is 100: a 300-byte message waits for credit part-way; an update's sealed answer never
        # goes between its fragments: it follows the data's last fragment, its counter after
        # the data's (the data took 1 at its first fragment).
        hello = bytes([0x89, HELLO, 1]) + (4096).to_bytes(4, "little") + (100).to_bytes(4, "little") + b"\x03"
        s = self.session(hello=hello)
        msg = bytes(i & 0xFF for i in range(300))
        frames, lines = self.send(s, msg)
        self.assertNotIn("SENT 1", lines)
        self.assertEqual(sum(len(f) - (7 if f[2] & FIRST else 3) for f in frames), 100)
        upd = []
        for f in update_frames(sealed_update(CN1, DN1, 0, "acc-2", "ref-2"), 247, True):
            upd += s.w(f)
        self.assertEqual(upd, [])                               # held: the data is part-way
        rest, lines = [], []
        for _ in range(3):
            out = s.cmd("w " + credit(100).hex())
            lines += out
            rest += [bytes.fromhex(x[2:]) for x in out if x.startswith("N ")]
        self.assertIn("SENT 1", lines)
        self.assertEqual(rest[-len(saved(2, True, "ref-2")):], saved(2, True, "ref-2"))   # right after the last fragment
        rest = rest[:-len(saved(2, True, "ref-2"))]
        self.assertEqual(self.device_msg(frames + rest, 1), msg)
        frames, _ = self.send(s, b"next")
        self.assertEqual(self.device_msg(frames, 3), b"next")
        s.close()

    def test_answer_waits_for_a_reserved_first_fragment(self):
        s = self.session()
        s.cmd("again 1")                         # the data's first fragment: no buffer (its counter reserved)
        frames, _ = self.send(s, b"held")
        self.assertEqual(frames, [])
        upd = []
        for f in update_frames(sealed_update(CN1, DN1, 0, "acc-2", "ref-2"), 247, True):
            upd += s.w(f)
        # the data first (counter 1), then the answer (2), both from the write's end
        data = [f for f in upd if f[1] == DATA]
        self.assertEqual(upd, data + saved(2, True, "ref-2"))
        self.assertEqual(self.device_msg(data, 1), b"held")
        s.close()

    def test_a_taken_clear_answers_before_the_ports_next_message(self):
        # The port gives its next message from data_sent() (back to back). A sealed clear taken while a device
        # message is part way out answers right after that message's last fragment; the next message never starts
        # (mgcommands.h: only a message already part way out delays the answer), and the session ends with the answer.
        hello = bytes([0x89, HELLO, 1]) + (4096).to_bytes(4, "little") + (64).to_bytes(4, "little") + b"\x03"
        s = self.session(hello=hello)
        frames, lines = self.send(s, bytes(100))                   # 120 sealed bytes, 64 out: part way
        self.assertNotIn("SENT 1", lines)
        s.cmd("sendnext " + bytes(30).hex())
        out = s.frames("w " + clear_frames(0)[0].hex())
        self.assertEqual([f[1] for f in out], [], "taken: its answer waits for the message part way out")
        out = s.frames("w " + credit(64).hex())                    # the rest of it, and room for more
        subs = [f[1] for f in out]
        self.assertIn(CLEAR, subs, "the clear's answer right after the message's last fragment")
        self.assertEqual(subs, [DATA] * subs.index(CLEAR) + [CLEAR] * (len(subs) - subs.index(CLEAR)))
        self.assertEqual(clear_answer([f for f in out if f[1] == CLEAR], 2), b"\x01")   # erased; credentials 0, data 1
        s.close()

    def test_abort_part_way_and_before_any_byte(self):
        hello = bytes([0x89, HELLO, 1]) + (4096).to_bytes(4, "little") + (50).to_bytes(4, "little") + b"\x03"
        s = self.session(hello=hello)
        frames, _ = self.send(s, bytes(200))
        self.assertTrue(frames)
        s.cmd("sendabort")
        self.assertEqual(s.frames("poll 1"), [bytes([0x89, ABORT])])    # its counter (1) used on both sides
        s.w(credit(50))
        frames, _ = self.send(s, b"after the abort")
        self.assertEqual(self.device_msg(frames, 2), b"after the abort")
        s.w(credit(100))
        s.cmd("again 1")
        frames, _ = self.send(s, b"never sent")                  # counter 3 reserved, no byte out
        self.assertEqual(frames, [])
        s.cmd("sendabort")
        self.assertEqual(s.frames("poll 1"), [])                 # nothing to abort, the counter given back
        frames, _ = self.send(s, b"takes 3")
        self.assertEqual(self.device_msg(frames, 3), b"takes 3")
        s.close()

    def test_state_change_reset_and_link_loss(self):
        hello = bytes([0x89, HELLO, 1]) + (4096).to_bytes(4, "little") + (50).to_bytes(4, "little") + b"\x03"
        s = self.session(hello=hello)
        frames, _ = self.send(s, bytes(200))
        self.assertEqual(s.cmd("w " + state(CLOSED).hex()),
                         ["SENT 0", "ST 0 1", "N " + OPEN_REQ1.hex()])       # dropped, no abort; asked again
        self.assertEqual(s.cmd("cansend 1"), ["CS 0"])
        self.assertEqual(s.cmd("w " + state(OPEN_ST, 2).hex()), ["ST 2 2"])
        frames, _ = self.send(s, b"x")                          # credit is the window again; counter 2
        self.assertEqual(self.device_msg(frames, 2), b"x")
        frames, _ = self.send(s, bytes(200))
        self.assertEqual(s.cmd("reset 1"), ["N " + bytes([0x89, ABORT]).hex(), "SENT 0", "ST 0 2"])
        self.assertEqual(s.cmd("vmstate"), ["VS 0 2"])
        s.close()
        s = self.session(hello=hello)
        self.send(s, bytes(200))
        s.cmd("linkdown")
        self.assertEqual(s.cmd("w " + credit(50).hex()), ["SENT 0", "ST 0 1"])      # the link went: quietly
        s.close()

    def send_within_credit(self, s, cm, mtu=247, window=4096):
        """A client that keeps to the device's credit: each fragment at most MTU - 3 less its header and the credit
        it has left. Returns the credit given back, each grant, and the sim's lines."""
        left, off, grants, lines = window, 0, [], []
        while off < len(cm):
            n = min(mtu - 3 - (7 if off == 0 else 3), left, len(cm) - off)
            self.assertGreater(n, 0, f"no credit at {off} of {len(cm)}: stalled")
            flags = SEALED | (FIRST if off == 0 else 0) | (LAST if off + n == len(cm) else 0)
            f = bytes([0x89, DATA, flags]) + (len(cm).to_bytes(4, "little") if off == 0 else b"") + cm[off:off + n]
            out = s.cmd("w " + f.hex())
            lines += out
            got = [int.from_bytes(bytes.fromhex(x[6:]), "little") for x in out if x.startswith("N 8904")]
            grants += got
            left += sum(got) - n
            off += n
        return sum(grants), grants, lines

    def test_client_message_larger_than_the_window(self):
        # The device's window is 4096, its max 16384: a 10,000-byte message the port keeps gives back what it holds
        # each time that fills the window, the rest at its end (mgcommands.h, Credit; the reference device).
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 16384 4096")
        s.w(SEALED_HELLO)
        s.cmd("pump")
        s.w(STATE_OPEN)
        e, m = self.keys()
        pt = env(bytes((i * 7) & 0xFF for i in range(10000)))
        cm = seal.seal(e, m, seal.CLIENT_TO_DEVICE, 0, pt)
        back, grants, lines = self.send_within_credit(s, cm)
        self.assertEqual(back, len(cm))
        self.assertEqual(grants, [4096, 4096, len(cm) - 8192])
        self.assertIn("D " + pt.hex(), lines)
        s.close()

    def test_a_fragment_past_the_credit_is_dropped(self):
        # A client fragment past the credit the device granted drops its message (invalid_value, every byte
        # back, counted), nothing buffered; the next message goes through (the vectors' "credit overrun").
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 16384 4096")
        s.w(SEALED_HELLO)
        s.cmd("pump")
        s.w(STATE_OPEN)
        e, m = self.keys()
        cm = seal.seal(e, m, seal.CLIENT_TO_DEVICE, 0, env(bytes(6000)))
        frs = data_frames(cm, 247, True)
        sent, outs = 0, []
        for f in frs:
            outs.append(s.w(f))
            sent += len(f) - (7 if f[2] & FIRST else 3)
            if outs[-1]:
                break
        self.assertGreater(sent, 4096)
        self.assertEqual(outs[-1], [err(3), credit(sent)], "dropped: every byte back")
        self.assertTrue(all(o == [] for o in outs[:-1]), "nothing back before the window filled")
        self.assertNotIn("D ", " ".join(s.cmd("show")))
        ok = seal.seal(e, m, seal.CLIENT_TO_DEVICE, 1, env(b"next"))
        self.assertEqual(s.w(data_frames(ok, 247, True)[0]), [credit(len(ok))])
        s.close()

    def test_only_credit_sent_counts(self):
        # A grant the stack had no buffer for hasn't reached the client: a fragment past the credit actually sent is
        # an overrun too, and its refusal gives back every byte, that grant included.
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 16384 4096")
        s.w(SEALED_HELLO)
        s.cmd("pump")
        s.w(STATE_OPEN)
        e, m = self.keys()
        cm = seal.seal(e, m, seal.CLIENT_TO_DEVICE, 0, env(bytes(6000)))
        frs, off = [], 0
        while off < 4096:                                      # exactly the window
            n = min(247 - 3 - (7 if off == 0 else 3), 4096 - off)
            frs.append(bytes([0x89, DATA, SEALED | (FIRST if off == 0 else 0)]) +
                       (len(cm).to_bytes(4, "little") if off == 0 else b"") + cm[off:off + n])
            off += n
        for f in frs[:-1]:
            self.assertEqual(s.w(f), [])
        s.cmd("again 2")                                       # the window's grant finds no buffer, twice
        self.assertEqual(s.w(frs[-1]), [])
        more = bytes([0x89, DATA, SEALED]) + cm[4096:4096 + 241]
        self.assertEqual(s.w(more), [err(3), credit(4096 + 241)])
        s.close()

    def test_6kb_message_not_kept_still_gives_credit_as_it_lands(self):
        # the same message while the port keeps nothing (no data path): dropped, but its bytes go back as they land
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 16384 4096", "dnull")
        s.w(SEALED_HELLO)
        s.cmd("pump")
        s.w(STATE_OPEN)
        e, m = self.keys()
        cm = seal.seal(e, m, seal.CLIENT_TO_DEVICE, 0, bytes(6144))
        back, sent = 0, 0
        for f in data_frames(cm, 247, True):
            sent += len(f) - (7 if f[2] & FIRST else 3)
            self.assertLessEqual(sent - back, 4096, "the client never needs more than the window")
            out = s.cmd("w " + f.hex())
            back += sum(int.from_bytes(bytes.fromhex(x[6:]), "little") for x in out if x.startswith("N 8904"))
        self.assertEqual(back, len(cm), "every byte back exactly once")
        s.close()

    def test_pieces_and_a_bad_tag(self):
        s = self.session(mode="piece")
        e, m = self.keys()
        pt = env(bytes(range(200)))
        cm = seal.seal(e, m, seal.CLIENT_TO_DEVICE, 0, pt)
        port = Port(s)
        for f in data_frames(cm, 100, True):
            port.w(f)
        self.assertEqual(port.got, [pt])
        bad = bytearray(seal.seal(e, m, seal.CLIENT_TO_DEVICE, 1, pt))
        bad[-1] ^= 1
        outs = [port.w(f) for f in data_frames(bytes(bad), 100, True)]
        self.assertEqual(outs[-1], [err(3), credit(len(bad))])
        self.assertEqual(port.got, [pt])                          # the pieces were dropped (PE 0)
        ok = seal.seal(e, m, seal.CLIENT_TO_DEVICE, 1, env(b"real"))      # its counter unused
        for f in data_frames(ok, 100, True):
            port.w(f)
        self.assertEqual(port.got, [pt, env(b"real")])
        s.close()

    def test_data_that_isnt_a_service_envelope(self):
        """mg-session-v1.json "data that isn't a service envelope": a clear's and a credentials_update's plaintext
        as data, as a relabel would bring them, and six bytes that aren't an envelope get invalid_value, their credit back and their counters used, and the port never has them; then a real envelope
        on the next counter goes through. The same for the decoder's edges, against the reference's rules, reassembled
        and in pieces."""
        relabels = [b"\x00", ref.compact({"access_token": "a", "refresh_token": "r"}).encode(),
                    bytes.fromhex("010100000807")]                # six bytes that aren't an envelope
        v = ref.varint
        edges = [env(b""), env(b"x") + env(b"y"),                    # field 1 twice: the last one is the payload
                 b"\x08\x96\x01" + env(b"x"),                       # field 1 as a varint: refused
                 bytes([0x10]) + b"\xff" * 9 + b"\x01",              # field 2, a 10-byte varint: fine
                 bytes([0x10]) + b"\xff" * 9 + b"\x02",              # past 64 bits
                 bytes([0x10]) + b"\xff" * 10 + b"\x00",             # 11 bytes
                 bytes([0x19]) + bytes(8), bytes([0x19]) + bytes(7),  # field 3 fixed64, whole and cut short
                 bytes([0x25]) + bytes(4), bytes([0x25]) + bytes(3),  # field 4 fixed32, whole and cut short
                 bytes([0x1b]), bytes([0x1c]),                        # wire types 3 and 4
                 v(19000 << 3) + b"\x00", v(19999 << 3 | 2) + b"\x00", v(18999 << 3) + b"\x00" + env(b"z"),
                 v(((1 << 29) - 1) << 3) + b"\x00", v(1 << 32) + b"\x00",   # the largest field number; past 32 bits
                 b"\x12\x05abc", b"\x12\x80", env(b"x")[:-1],      # a length past the end; a cut varint
                 b"\x12" + v(1 << 33) + b"x"]                         # a length past 32 bits
        for mode in ("buf", "piece"):
            with self.subTest(mode):
                s = self.session(mode=mode)
                port = Port(s)
                e, m = self.keys()
                ctr = 0
                for pt in relabels + edges:
                    ok = envelope_payload(pt) is not None
                    self.assertEqual(ok, pt in edges and edges.index(pt) in (0, 1, 3, 6, 8, 14, 15), pt.hex())
                    cm = seal.seal(e, m, seal.CLIENT_TO_DEVICE, ctr, pt)
                    got = []
                    for f in data_frames(cm, 247, True):
                        got += port.w(f)
                    self.assertEqual(got, ([] if ok else [err(3)]) + [credit(len(cm))], pt.hex())
                    self.assertEqual(port.got[-1:] == [pt], ok, pt.hex())
                    ctr += 1                                          # used either way
                real = env(b"a real envelope")
                cm = seal.seal(e, m, seal.CLIENT_TO_DEVICE, ctr, real)
                got = []
                for f in data_frames(cm, 247, True):
                    got += port.w(f)
                self.assertEqual(got, [credit(len(cm))])
                self.assertEqual(port.got[-1], real)
                s.close()

    def test_every_mtu(self):
        for mtu in (100, 185, 247, 517):   # the sealed session needs MG_MIN_ATT_MTU (Sealed.test_handoff_every_mtu)
            with self.subTest(mtu=mtu):
                s = self.session(mtu=mtu)
                msg = bytes((i * 13) & 0xFF for i in range(1500))
                frames, lines = self.send(s, msg)
                self.assertIn("SENT 1", lines)
                self.assertEqual(self.device_msg(frames, 1, mtu), msg)
                e, mk = self.keys()
                cm = seal.seal(e, mk, seal.CLIENT_TO_DEVICE, 0, env(msg))
                got = []
                for f in data_frames(cm, mtu, True):
                    got += s.cmd("w " + f.hex())
                self.assertEqual(got, ["N " + credit(len(cm)).hex(), "D " + env(msg).hex()])
                s.close()

    def replay_reference(self, section, hello):
        """The seal vector's whole exchange byte for byte, the device's own data included (the port sends the
        vector's plaintext; layer 1 alone sealed it before), every client message handed to the port (its plaintexts
        are service envelopes; one that weren't would be refused invalid_value)."""
        v = SEALV[section]
        k, mtu = bytes.fromhex(v["K"]), v["max_write"] + 3
        first = json.loads(bytes.fromhex(v["steps"][0]["plaintext"]))
        s = Sim(f"mtu {mtu}", "hello 4096 4096", "expiry 1", f"k {k.hex()}", f"node {v['node_id']}",
                f"access {first['access_token']}", f"refresh {first['refresh_token']}",
                f"proof {v['client_nonce']} {v['device_nonce']}")
        pending = []    # frames the device sent ahead of the vector's own step for them (its sealed answer)
        delivered = []
        steps = v["steps"]
        for i, st in enumerate(steps):
            frames = [bytes.fromhex(f) for f in st["frames"]]
            same = FRAME_MAX >= v["max_write"]
            if st["direction"] == seal.DEVICE_TO_CLIENT:
                if i == 0:
                    out = s.w(hello) + s.frames("pump")
                    got = out[1:]
                    self.assertEqual(s.cmd("w " + STATE_OPEN.hex()), ["ST 2 1"])
                elif frames[0][1] == SAVED:
                    got, pending = pending, []
                else:
                    lines = s.cmd("send " + st["plaintext"])
                    self.assertIn("Q 0", lines)
                    self.assertIn("SENT 1", lines)
                    got = [bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")]
                if same or frames[0][1] == SAVED:   # credentials_saved: one write in every build
                    self.assertEqual([f.hex() for f in got], [f.hex() for f in frames], st["step"])
                else:
                    self.assertEqual(b"".join([got[0][7:]] + [f[3:] for f in got[1:]]).hex(), st["sealed"], st["step"])
                continue
            lines = []
            for f in frames:
                lines += s.cmd("w " + f.hex())
            n = [bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")]
            d = [bytes.fromhex(x[2:]) for x in lines if x.startswith("D ")]
            if st["outcome"] == "message" and frames[0][1] == DATA:
                pt = bytes.fromhex(st["plaintext"])
                ok = envelope_payload(pt) is not None
                self.assertEqual(n, ([] if ok else [err(3)]) + [credit(len(bytes.fromhex(st["sealed"])))], st["step"])
                self.assertEqual(d, [pt] if ok else [], st["step"])
                delivered.append(pt)
            elif st["outcome"] == "message":            # credentials_update
                nxt = steps[i + 1] if i + 1 < len(steps) else None
                if nxt and nxt["direction"] == seal.DEVICE_TO_CLIENT and bytes.fromhex(nxt["frames"][0])[1] == SAVED:
                    pending = n                         # checked at the vector's answer step
                else:
                    self.assertEqual(n, [bytes.fromhex("890801")], st["step"])   # a client without sealed answers: one byte
            elif st["outcome"] == "refused":
                self.assertEqual(n, [bytes.fromhex(st["answer"]), credit(len(frames[0]) - 7)], st["step"])
                self.assertEqual(d, [], st["step"])
            else:
                self.assertEqual(n, [credit(len(frames[0]) - 7)], st["step"])
                self.assertEqual(d, [], st["step"])
        self.assertGreaterEqual(len(delivered), 4)
        s.close()

    def test_reference_sealed_data_large(self):
        self.replay_reference("sealed_data_large", SEALED_HELLO)

    def test_reference_sealed_data_legacy_client(self):
        # sealed_data predates sealed answers: its client didn't count a sealed answer, so the device's data after the update
        # is counter 2; a hello without sealed_saved gets the one-byte answer and the same counters.
        self.replay_reference("sealed_data", LEGACY_HELLO)


class AppRx:
    """The Muse app's device -> client side: one counter for every sealed
    device message (credentials, credentials_saved, data), counted when the message completes or when a state the app
    sends cuts it off part-way; the app never interleaves, so a sealed message starting while another is part-way is
    an error. count_cut=False is a client that counts only completed messages (a bench client's data inbox): it
    is left one counter behind by a message a state cut off."""

    def __init__(self, count_cut=True, cn=CN1, dn=DN1):
        self.e, self.m = seal.seal_keys(K, cn, dn, NODE)
        self.count_cut = count_cut
        self.next = 0
        self.part = None                # [sub, total, bytes] of the message part-way
        self.got = []                   # (sub, counter, plaintext)

    def feed(self, frames):
        for f in frames:
            if len(f) < 3 or f[0] != 0x89 or f[1] not in (DATA, CREDS, SAVED) or not f[2] & SEALED:
                continue
            if f[2] & FIRST:
                if self.part is not None:
                    raise AssertionError(f"device sub {f[1]} starts while sub {self.part[0]} is part-way (interleaved)")
                self.part = [f[1], int.from_bytes(f[3:7], "little"), bytearray(f[7:])]
            else:
                if self.part is None or self.part[0] != f[1]:
                    raise AssertionError(f"device sub {f[1]}: a continuation with no message open")
                self.part[2] += f[3:]
            if f[2] & LAST:
                sub, total, blob = self.part
                self.part = None
                if len(blob) != total:
                    raise AssertionError(f"device sub {sub}: {len(blob)} of {total} bytes")
                pt = seal.unseal(self.e, self.m, seal.DEVICE_TO_CLIENT, self.next, bytes(blob))
                if pt is None:
                    at = [c for c in range(max(0, self.next - 4), self.next + 8)
                          if seal.unseal(self.e, self.m, seal.DEVICE_TO_CLIENT, c, bytes(blob)) is not None]
                    raise AssertionError(f"device data does not open at counter {self.next} (sub {sub}, "
                                         f"{len(blob)} B, opens at {at})")
                self.got.append((sub, self.next, pt))
                self.next += 1
        return self

    def state(self):
        """The app sends a state: a message part-way is dropped, its counter (taken at its first fragment) used."""
        if self.part is not None:
            self.part = None
            if self.count_cut:
                self.next += 1


def nframes(lines):
    return [bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")]


@unittest.skipUnless(DATA_PATH, "an MGDS_DATA build")
class CounterAfterUpdates(unittest.TestCase):
    """A watch's case: proof, sealed hello, two credentials_updates (both saved),
    relay state open, the watch's POST /link-control, then link.register: "device data does not open at counter 4".
    The device -> client counter checked by the app's own rules (AppRx) through every order the pumps can take: a
    credentials_saved never goes between a device message's fragments; a counter reserved
    for a message whose first fragment never went out is not used when a state drops it; a message a state cuts off
    part-way has used its counter."""

    LC = b"POST /link-control " + bytes(41)                       # a small message: one write
    REG = bytes((i * 7 + 3) & 0xFF for i in range(2500))           # link.register's size: many writes

    def setup(self, window=4096, updates=2):
        hello = bytes([0x89, HELLO, 1]) + (4096).to_bytes(4, "little") + window.to_bytes(4, "little") + b"\x03"
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 4096 4096", "dmode buf")
        rx = AppRx()
        rx.feed(s.w(hello) + s.frames("pump"))
        self.assertEqual(rx.got[0][:2], (CREDS, 0))
        self.cctr = 0
        for i in range(updates):
            rx.feed(self.update(s, f"u{i}"))
            self.assertEqual(rx.got[-1], (SAVED, 1 + i, seal.saved_plaintext(True, f"ref-u{i}")))
        return s, rx

    def update(self, s, tag):
        out = []
        for f in update_frames(sealed_update(CN1, DN1, self.cctr, f"acc-{tag}", f"ref-{tag}"), 247, True):
            out += s.w(f)
        self.cctr += 1
        return out

    def open(self, s, rx, epoch):
        rx.state()
        lines = s.cmd("w " + state(OPEN_ST, epoch).hex())
        self.assertIn(f"ST 2 {epoch}", lines)
        rx.feed(nframes(lines))
        return lines

    def send(self, s, rx, msg, cmd="send"):
        lines = s.cmd(f"{cmd} " + msg.hex())
        self.assertIn("Q 0", lines)
        rx.feed(nframes(lines))
        return lines

    def credit_until_sent(self, s, rx, n):
        for _ in range(100):
            lines = s.cmd("w " + credit(n).hex())
            rx.feed(nframes(lines))
            if "SENT 1" in lines:
                return lines
        self.fail("the message never finished")

    def saved_stat(self, s):
        """mgds_t.st.saved, or None in an MGDS_STATS=0 build (no stats)."""
        line = next((x for x in s.cmd("show") if x.startswith("S stats")), None)
        return None if line is None else dict(kv.split("=") for kv in line.split()[2:])["saved"]

    def assertSaved(self, s, n):
        got = self.saved_stat(s)
        if got is not None:
            self.assertEqual(got, str(n))

    def test_register_after_two_updates(self):
        s, rx = self.setup()
        self.assertSaved(s, 2)                                     # two updates: two saves, not four
        self.open(s, rx, 1)
        self.assertIn("SENT 1", self.send(s, rx, self.LC))
        self.assertEqual(rx.got[-1], (DATA, 3, self.LC))
        lines = self.send(s, rx, self.REG)
        self.assertIn("SENT 1", lines)
        self.assertGreater(len(nframes(lines)), 1)
        self.assertEqual(rx.got[-1], (DATA, 4, self.REG))
        self.assertSaved(s, 2)
        s.close()

    def test_saved_owed_when_a_long_message_starts(self):
        # A third update's answer is owed (the stack had no buffer) when link.register is handed over, and the client's
        # window stops register part-way: the answer goes whole before register or after its last fragment.
        s, rx = self.setup(window=600)
        self.open(s, rx, 1)
        self.send(s, rx, self.LC)
        self.assertEqual(rx.got[-1], (DATA, 3, self.LC))
        s.cmd("again 99")
        rx.feed(self.update(s, "u2"))                             # saved, its answer owed
        s.cmd("again 0")
        self.send(s, rx, self.REG, "sendnp")                       # handed over; goes from the next write's flush
        self.credit_until_sent(s, rx, 600)
        s.cmd("pump")
        subs = [(g[0], g[1]) for g in rx.got[-2:]]
        self.assertEqual(sorted(subs, key=lambda x: x[1]), subs)
        self.assertEqual({g[0] for g in rx.got[-2:]}, {SAVED, DATA})
        self.assertEqual([g[1] for g in rx.got[-2:]], [4, 5])
        self.assertIn((DATA, self.REG), [(g[0], g[2]) for g in rx.got[-2:]])
        self.assertIn((SAVED, seal.saved_plaintext(True, "ref-u2")), [(g[0], g[2]) for g in rx.got[-2:]])
        self.send(s, rx, b"next")
        self.assertEqual(rx.got[-1], (DATA, 6, b"next"))
        s.close()

    def test_saved_due_mid_message_waits_for_its_last_fragment(self):
        # link.register part-way (the client's window), then an update: its sealed answer must not go between
        # register's fragments; it follows register's last fragment, under the next counter.
        s, rx = self.setup(window=300)
        self.open(s, rx, 1)
        self.send(s, rx, self.LC)                                  # 80 B of the 300
        lines = self.send(s, rx, self.REG)
        self.assertNotIn("SENT 1", lines)
        self.assertIsNotNone(rx.part)                               # register part-way, counter 4 taken
        rx.feed(self.update(s, "u2"))                             # saved now: held
        self.assertEqual(rx.got[-1][:2], (DATA, 3))
        self.credit_until_sent(s, rx, 300)
        s.cmd("pump")
        self.assertEqual(rx.got[-2], (DATA, 4, self.REG))
        self.assertEqual(rx.got[-1], (SAVED, 5, seal.saved_plaintext(True, "ref-u2")))
        self.send(s, rx, b"next")
        self.assertEqual(rx.got[-1], (DATA, 6, b"next"))
        s.close()

    def test_reserved_counter_dropped_by_a_state_is_not_used(self):
        # The watch's message took counter 3 as its first fragment was made, but the stack had no buffer for it; the
        # relay's state open (a new epoch, answering the watch's own open_request) drops it before any byte went out.
        # The client never saw counter 3: the watch's next message must carry 3, not 4.
        s, rx = self.setup()
        self.open(s, rx, 1)
        s.cmd("again 1")
        lines = self.send(s, rx, self.LC)
        self.assertEqual(nframes(lines), [])
        self.assertIn("SENT 0", self.open(s, rx, 2))
        self.send(s, rx, self.LC)
        self.assertEqual(rx.got[-1], (DATA, 3, self.LC))
        self.send(s, rx, self.REG)
        self.assertEqual(rx.got[-1], (DATA, 4, self.REG))
        s.close()

    def test_message_cut_part_way_by_a_state_used_its_counter(self):
        # link.register part-way (counter 4) when the relay's state open epoch 2 arrives: dropped on both sides, its
        # counter used (sealed_stream_restart). The watch's next message is 5. A client that counts only
        # completed messages (a bench client's data inbox) expects 4 there and fails.
        s, rx = self.setup(window=300)
        lazy = AppRx(count_cut=False)
        lazy.next = rx.next
        self.open(s, rx, 1)
        lazy.state()
        frames = nframes(s.cmd("send " + self.LC.hex())) + nframes(s.cmd("send " + self.REG.hex()))
        rx.feed(frames)
        lazy.feed(frames)
        self.assertEqual(rx.got[-1], (DATA, 3, self.LC))
        self.assertIsNotNone(rx.part)
        lines = s.cmd("w " + state(OPEN_ST, 2).hex())
        self.assertIn("SENT 0", lines)                              # dropped, no abort (a state ends it)
        rx.state()
        lazy.state()
        frames = nframes(s.cmd("send " + self.LC.hex()))
        rx.feed(frames)
        self.assertEqual(rx.got[-1], (DATA, 5, self.LC))
        with self.assertRaisesRegex(AssertionError, "does not open at counter 4"):
            lazy.feed(frames)
        s.close()

    def test_saved_stat_counts_the_boot_not_the_session(self):
        # The board's "saved=" (mgds_t.st, its session console line) counts every save since mgds_init: two sessions of two
        # updates each read 4. Each session's own answers: 1 and 2.
        s, rx = self.setup()
        self.assertSaved(s, 2)
        s.cmd(f"proof {CN2.hex()} {DN2.hex()}")
        rx = AppRx(cn=CN2, dn=DN2)
        rx.feed(s.w(SEALED_HELLO) + s.frames("pump"))
        self.assertEqual(rx.got[0][:2], (CREDS, 0))
        e, m = seal.seal_keys(K, CN2, DN2, NODE)
        for i in range(2):
            out = []
            body = json.dumps({"access_token": f"acc-v{i}", "refresh_token": f"ref-v{i}"}, separators=(",", ":"))
            for f in update_frames(seal.seal(e, m, seal.CLIENT_TO_DEVICE, i, body.encode()), 247, True):
                out += s.w(f)
            rx.feed(out)
            self.assertEqual(rx.got[-1], (SAVED, 1 + i, seal.saved_plaintext(True, f"ref-v{i}")))
        self.assertSaved(s, 4)
        s.close()


@unittest.skipUnless(STALL, "an MGDS_STALL build")
class Stall(unittest.TestCase):
    """Layer 2's 35 s stall clock runs only while the
    device owes the client frames (hello and credentials, a sealed credentials_saved, a give-up [0] or a sealed clear's
    answer) or a sealed clear is part way in, and any client
    write restarts it (mgds_rx, mgds_client_frame); an update part-way (storage lent, or discarded for want of it)
    doesn't start it, but no session frame for 2 min with one part-way ends the session with the link; mgds_woke()
    restarts both after a connected sleep. Ending with the link: the session reset (storage back, a VM session that was open reported closed),
    then port.end_link ("L"), never silently."""

    STALL_MS, IDLE_MS = 35000, 120000

    def session(self, mtu=247, open_vm=True):
        s = Sim(f"mtu {max(mtu, MIN_MTU)}", f"proof {CN1.hex()} {DN1.hex()}")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
        if DATA_PATH and open_vm:
            self.assertIn("ST 2 1", s.cmd("w " + STATE_OPEN.hex()))
        opened_at(s, mtu)
        return s

    def busy(self, s):
        return "busy=1" in s.cmd("show")[0]

    def poll(self, s, ms):
        return s.cmd(f"poll {ms}")

    def test_clock_idle_with_nothing_owed(self):
        s = self.session()
        for _ in range(10):                                     # 10 min, nothing owed: no stall
            self.assertNotIn("L", self.poll(s, 60000))
        # owing starts now: the clock counts from here, not from the hello 10 min ago
        s.cmd("budget 0")
        upd = []
        for f in update_frames(sealed_update(CN1, DN1, 0, "acc-s", "ref-s"), 247, True):
            upd += s.w(f)
        self.assertNotIn("L", self.poll(s, self.STALL_MS - 1))
        self.assertIn("L", self.poll(s, 1))
        s.close()

    def test_owed_hello_35s_ends_session_and_link(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "budget 0")
        s.cmd("w " + SEALED_HELLO.hex())                       # its answer owed: the client takes nothing
        self.assertTrue(self.busy(s))
        self.assertNotIn("L", self.poll(s, self.STALL_MS - 1))
        out = self.poll(s, 1)
        self.assertEqual(out.count("L"), 1)
        self.assertFalse(self.busy(s))                          # the session ended with it
        self.assertNotIn("L", self.poll(s, 120000))             # once
        s.close()

    def test_credentials_part_way_stall(self):
        s = Sim("mtu 100", f"proof {CN1.hex()} {DN1.hex()}", "budget 2")   # hello and one credentials fragment
        s.cmd("w " + SEALED_HELLO.hex())
        self.assertNotIn("L", self.poll(s, self.STALL_MS - 1))
        out = self.poll(s, 1)
        self.assertEqual(out[-1], "L")                          # after the session's own end (its abort offered)
        self.assertIn("A", out)
        self.assertFalse(self.busy(s))
        s.close()

    def test_client_frame_restarts_the_clock(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "budget 0")
        s.cmd("w " + SEALED_HELLO.hex())
        self.assertNotIn("L", self.poll(s, 30000))
        self.assertNotIn("L", s.cmd("w " + credit(1).hex()))  # any client frame (refused here: no hello out yet)
        self.assertNotIn("L", self.poll(s, self.STALL_MS - 1))   # 65 s owing, 35 s - 1 ms since the frame
        self.assertIn("L", self.poll(s, 1))
        s.close()

    def test_owed_answer_stall(self):
        s = self.session()
        s.cmd("budget 0")
        for f in update_frames(sealed_update(CN1, DN1, 0, "acc-s", "ref-s"), 247, True):
            s.w(f)                                              # saved: its sealed answer owed
        self.assertTrue(self.busy(s))
        self.assertNotIn("L", self.poll(s, self.STALL_MS - 1))
        out = self.poll(s, 1)
        if DATA_PATH:
            self.assertEqual(out[-2:], ["ST 0 1", "L"])         # the VM session reported closed, then the link
        else:
            self.assertEqual(out[-1:], ["L"])
        self.assertFalse(self.busy(s))
        s.close()

    def test_clear_part_way_in_stall(self):
        # A sealed clear's first fragment (MTU 23) holds the lent storage: no next fragment for 35 s ends the session
        # with the link, the storage back and nothing erased.
        s = self.session(23)
        frames = clear_frames(0, 23)
        self.assertGreater(len(frames), 1)
        self.assertEqual(s.w(frames[0]), [])
        self.assertIn("borrowed=1", s.cmd("show")[0])
        self.assertNotIn("L", self.poll(s, self.STALL_MS - 1))
        out = self.poll(s, 1)
        self.assertEqual(out[-1], "L")
        show = s.cmd("show")[0]
        self.assertIn("borrowed=0", show)
        self.assertIn("busy=0", show)
        self.assertFalse(any(e.startswith("CL") for e in s.events), "never taken: nothing erased")
        s.close()

    def test_clear_answer_owed_stall(self):
        # The clear taken and erased, its sealed answer owed (no stack buffer): 35 s with no client write ends the
        # session with the link; the erase ran once.
        s = self.session()
        s.cmd("budget 0")
        for f in clear_frames(0):
            s.w(f)
        self.assertIn("CL 1", s.events)
        self.assertTrue(self.busy(s))
        self.assertNotIn("L", self.poll(s, self.STALL_MS - 1))
        out = self.poll(s, 1)
        self.assertEqual(out[-1], "L")
        self.assertFalse(self.busy(s))
        self.assertEqual(s.events.count("CL 1"), 1, "erased once")
        s.close()

    def updframes(self, mtu=64):
        frames = update_frames(sealed_update(CN1, DN1, 0, "acc-" + "x" * 60, "ref-" + "y" * 60), mtu, True)
        self.assertGreaterEqual(len(frames), 4)
        return frames

    def test_update_part_way_no_35s_clock_and_2min_bound(self):
        s = self.session(mtu=64)
        frames = self.updframes()
        self.assertEqual(s.w(frames[0]), [])
        self.assertIn("borrowed=1", s.cmd("show")[0])
        self.assertNotIn("L", self.poll(s, 60000))              # an update part-way doesn't start the 35 s clock
        self.assertNotIn("L", self.poll(s, self.IDLE_MS - 60000 - 1))
        out = self.poll(s, 1)                                   # 2 min, no client frame: ends with the link
        if DATA_PATH:
            self.assertEqual(out[-2:], ["ST 0 1", "L"])
        else:
            self.assertEqual(out[-1:], ["L"])
        show = s.cmd("show")[0]
        self.assertIn("borrowed=0", show)                       # the storage back (wiped)
        self.assertIn("busy=0", show)
        s.close()

    def test_update_fragment_restarts_the_2min_bound(self):
        s = self.session(mtu=64)
        frames = self.updframes()
        s.w(frames[0])
        self.assertNotIn("L", self.poll(s, 100000))
        s.w(frames[1])                                          # a client frame: from here again
        self.assertNotIn("L", self.poll(s, self.IDLE_MS - 1))
        self.assertIn("L", self.poll(s, 1))
        s.close()

    def test_update_finished_late_still_saves(self):
        # Fragments 90 s apart: every one restarts the bound, and the update completes and saves.
        s = self.session(mtu=64)
        frames = self.updframes()
        out = []
        for f in frames:
            self.assertNotIn("L", self.poll(s, 90000))
            out += s.w(f)
        self.assertEqual(out, saved(1, True, "ref-" + "y" * 60, mtu=64))
        self.assertFalse(self.busy(s))
        self.assertNotIn("L", self.poll(s, 600000))
        s.close()

    def test_any_client_write_restarts_the_35s_clock(self):
        # A Control or Data write (mgds_client_frame) counts for the owing clock
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "budget 0")
        s.cmd("w " + SEALED_HELLO.hex())
        self.assertNotIn("L", self.poll(s, 30000))
        s.cmd("cframe")
        self.assertNotIn("L", self.poll(s, self.STALL_MS - 1))
        self.assertIn("L", self.poll(s, 1))
        s.close()

    def test_other_writes_dont_restart_the_update_bound(self):
        # ... but only session frames are progress on an update: a client busy on Control still reaches the 2 min
        s = self.session(mtu=64)
        frames = self.updframes()
        s.w(frames[0])
        self.assertNotIn("L", self.poll(s, 100000))
        s.cmd("cframe")
        self.assertNotIn("L", self.poll(s, self.IDLE_MS - 100000 - 1))
        self.assertIn("L", self.poll(s, 1))
        s.close()

    def test_update_discarded_for_want_of_storage_is_bounded(self):
        # An update part-way with no storage (its fragments discarded) keeps the board awake the same way
        s = self.session(mtu=64)
        s.cmd("storage 0")
        frames = self.updframes()
        self.assertEqual(s.w(frames[0]), [])
        self.assertTrue(self.busy(s))
        self.assertNotIn("L", self.poll(s, 60000))              # no 35 s clock (nothing owed)
        self.assertNotIn("L", self.poll(s, self.IDLE_MS - 60000 - 1))
        out = self.poll(s, 1)
        self.assertEqual(out[-1], "L")
        self.assertFalse(self.busy(s))
        s.close()

    def test_a_connected_sleep_is_not_a_stall(self):
        # The tick runs on through a connected light sleep while mgds_poll doesn't; mgds_woke() at the
        # sleep's exit restarts the clocks, so the first poll after a long sleep doesn't end the session
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "budget 0")
        s.cmd("w " + SEALED_HELLO.hex())
        self.assertNotIn("L", self.poll(s, 10000))
        s.cmd("sleep 60000")
        s.cmd("woke")
        self.assertNotIn("L", self.poll(s, self.STALL_MS - 1))
        self.assertIn("L", self.poll(s, 1))
        s.close()
        # without the wake call the same sleep reads as a stall (the board must call it)
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "budget 0")
        s.cmd("w " + SEALED_HELLO.hex())
        s.cmd("sleep 60000")
        self.assertIn("L", self.poll(s, 1))
        s.close()

    def test_a_connected_sleep_restarts_the_update_bound(self):
        s = self.session(mtu=64)
        frames = self.updframes()
        s.w(frames[0])
        s.cmd("sleep 150000")
        s.cmd("woke")
        self.assertNotIn("L", self.poll(s, self.IDLE_MS - 1))
        self.assertIn("L", self.poll(s, 1))
        s.close()

    def test_without_end_link_the_rules_are_off(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "budget 0", "endlink 0")
        s.cmd("w " + SEALED_HELLO.hex())
        self.assertNotIn("L", self.poll(s, 600000))
        self.assertTrue(self.busy(s))                           # as before MGDS_STALL: the board's own guard decides
        s.close()


@unittest.skipUnless(GIVE_UP_LATER and DATA_PATH, "an MGDS_GIVE_UP_LATER + MGDS_DATA build")
class GiveUpLater(unittest.TestCase):
    """MGDS_GIVE_UP_LATER (from a board's deepest stack chain: a media start reclaiming the storage of a
    held update sealed its [0] inline): mgds_release_storage() only owes the [0]; the next mgds_poll() seals and sends
    it, under the next counter, in order with the port's data (the app's counter rules, AppRx)."""

    def test_release_owes_and_the_poll_sends(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 4096 4096", "dmode buf")
        rx = AppRx()
        rx.feed(s.w(SEALED_HELLO) + s.frames("pump"))
        self.assertIn("ST 2 1", s.cmd("w " + STATE_OPEN.hex()))
        s.cmd("busy 1")                                         # media: the update's commit is held
        for f in update_frames(sealed_update(CN1, DN1, 0, "acc-g", "ref-g"), 247, True):
            self.assertEqual(s.w(f), [])
        self.assertEqual(s.frames("release"), [])                # nothing sealed inside the release
        self.assertIn("busy=1", s.cmd("show")[0])                # owed
        lines = s.cmd("sendnp " + b"port data".hex())            # the port's message handed over meanwhile
        self.assertIn("Q 0", lines)
        lines = s.cmd("poll 1")
        rx.feed(nframes(lines))
        self.assertEqual([g[:2] for g in rx.got[1:]], [(SAVED, 1), (DATA, 2)])   # the [0] first, then the data
        self.assertEqual(rx.got[1][2], seal.saved_plaintext(False, ""))
        self.assertEqual(rx.got[2][2], b"port data")
        self.assertIn("busy=0", s.cmd("show")[0])
        self.assertIn("access=" + ACCESS, s.cmd("show")[0])     # the pair not stored
        s.close()

    def test_release_on_a_legacy_hello_stays_silent(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}")
        s.w(LEGACY_HELLO)
        s.frames("pump")
        s.cmd("busy 1")
        for f in update_frames(sealed_update(CN1, DN1, 0, "acc-g", "ref-g"), 247, True):
            s.w(f)
        self.assertEqual(s.frames("release"), [])
        self.assertEqual(s.frames("poll 1"), [])                 # a client without sealed answers: no answer, as before
        self.assertIn("busy=0", s.cmd("show")[0])
        s.close()


@unittest.skipUnless(DATA_PATH, "an MGDS_DATA build")
class AskQuiet(unittest.TestCase):
    """A gadget keeps its session only for an active task, and a save while closed asks at once only with one:
    mgds_ask_quiet(t, true), a persistent "no active task" bit: layer 2 asks nothing by itself (no re-ask after an unasked closed or
    no_network, no ask behind a saved credentials_update), it survives a save, a session's end and a hello, and
    mgds_task_start(), mgds_open_request(true) or mgds_ask_quiet(t, false) lifts it."""

    session = ReAsk.session
    expect_ask_after = ReAsk.expect_ask_after
    unasked_end = ReAsk.unasked_end
    update = ReAsk.update
    ask_pending = ReAsk.ask_pending

    def silent_10_min(self, s, what):
        self.assertEqual(ask_frames(s.frames("poll 600000")), [], what)

    def test_no_ask_after_an_unasked_closed_or_no_network(self):
        s = self.session()
        s.cmd("askquiet 1")
        for st in (CLOSED, NO_NETWORK, CLOSED, NO_NETWORK):
            self.assertEqual(s.w(state(st)), [], st)
            self.assertFalse(self.ask_pending(s), st)
            self.silent_10_min(s, st)
        s.close()

    def test_no_ask_behind_a_save_while_ended(self):
        """A token refresh alone opens no session: the saved answer goes, nothing behind it (ReAsk's
        test_save_while_ended_asks_as_the_ended_kinds_first asks here without the bit)."""
        s = self.session()
        s.cmd("askquiet 1")
        for st, n in ((CLOSED, 2), (NO_NETWORK, 3), (AUTH_FAILED, 4)):
            self.assertEqual(s.w(state(st)), [], st)
            self.assertEqual(self.update(s, n), [bytes.fromhex("890801")], st)   # saved, and no [1]
            self.silent_10_min(s, st)
        s.close()

    def test_no_ask_behind_a_sealed_save(self):
        s = self.session(sealed=True)
        s.cmd("askquiet 1")
        self.assertEqual(s.w(state(CLOSED)), [])
        out = []
        for f in update_frames(sealed_update(CN1, DN1, 0, "acc-2", "ref-2"), 247, True):
            out += s.w(f)
        self.assertEqual(out, saved(1, True, "ref-2"))
        self.silent_10_min(s, "sealed save")
        s.close()

    def test_survives_a_save_a_session_end_and_a_hello(self):
        s = self.session()
        s.cmd("askquiet 1")
        self.assertEqual(s.w(state(OPEN_ST, 2)), [])
        self.assertEqual(self.update(s, 2), [bytes.fromhex("890801")])          # a save while open (series restart)
        s.frames("poll 60000")
        self.assertEqual(s.w(state(CLOSED, 2)), [], "after a 60 s session (series restart)")
        self.silent_10_min(s, "after the save and the long session")
        s.cmd(f"proof {CN2.hex()} {DN2.hex()}")
        out = s.w(SEALED_HELLO) + s.frames("pump")                              # a new session (both series restart)
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
        self.assertEqual(s.w(state(CLOSED)), [], "after a hello")
        self.silent_10_min(s, "after the hello")
        s.close()

    def test_a_task_lifts_it_and_the_series_goes_on(self):
        s = self.session()
        s.cmd("askquiet 1")
        self.assertEqual(s.w(state(CLOSED)), [])
        self.silent_10_min(s, "quiet")
        s.cmd("openreq 1")                                                     # a task: the port asks
        self.assertEqual(ask_frames(s.frames("poll 0")), [OPEN_REQ1])
        self.assertEqual(s.w(state(CONNECTING)), [])
        self.unasked_end(s, CLOSED, 0, "quiet scheduled nothing: closed's first, at once")
        self.unasked_end(s, CLOSED, 1000, "then 1 s, as without quiet")
        self.assertEqual(self.update(s, 2), [bytes.fromhex("890801")])          # connecting: a save asks nothing
        self.assertEqual(s.w(state(CLOSED)), [OPEN_REQ1], "the series restarted by the save")
        s.close()

    def test_a_task_lifts_it_while_open(self):
        s = self.session()
        s.cmd("askquiet 1")
        self.assertEqual(s.w(state(OPEN_ST, 2)), [])
        s.cmd("openreq 1")                                                     # open: nothing to ask, but lifted
        self.assertEqual(ask_frames(s.frames("poll 1")), [])
        self.unasked_end(s, CLOSED, 0, "lifted: the unasked end asks")
        s.close()

    def test_drops_a_waiting_re_ask_and_an_owed_one_but_not_the_ports_close(self):
        s = self.session()
        self.unasked_end(s, CLOSED, 0, "first")
        self.assertEqual(s.w(state(CLOSED)), [])                               # the 1 s ask waits
        self.assertTrue(self.ask_pending(s))
        s.cmd("askquiet 1")
        self.assertFalse(self.ask_pending(s))
        self.silent_10_min(s, "the waiting ask dropped")
        s.cmd("openreq 1")                                                     # owed, then no task: dropped
        s.cmd("askquiet 1")
        self.assertEqual(s.frames("poll 1"), [])
        s.cmd("openreq 0")                                                     # the port's [0] still goes
        s.cmd("askquiet 1")
        self.assertEqual(s.frames("poll 1"), [OPEN_REQ0])
        s.close()

    def test_a_task_start_asks_under_the_series_rules(self):
        # mg-session-v1.json's task events: the session down, the ended kind's series asks at once if it hasn't asked
        # yet, else at its next step; nothing while open or after not_enrolled.
        s = self.session()
        s.cmd("task 0")
        self.assertEqual(s.w(state(CLOSED)), [], "no task: nothing")
        self.assertEqual(ask_frames(s.frames("task 1")), [OPEN_REQ1], "a task: closed's first ask, at once")
        s.cmd("task 0")
        self.assertEqual(s.w(state(CLOSED)), [], "no task again: nothing")
        self.assertEqual(ask_frames(s.frames("task 1")), [], "closed's series runs: its next step")
        self.expect_ask_after(s, 1000, "the series' next step")
        self.assertEqual(s.w(state(OPEN_ST, 2)), [])
        self.assertEqual(ask_frames(s.frames("task 1")), [], "open: nothing to ask")
        self.assertEqual(s.w(state(NO_NETWORK, 2)), [OPEN_REQ1], "a task still on: no_network's first, at once")
        s.cmd("task 0")
        self.assertEqual(s.w(state(NOT_ENROLLED, 2)), [])
        self.assertEqual(ask_frames(s.frames("task 1")), [], "not_enrolled: asking can't fix it")
        self.silent_10_min(s, "blocked")
        s.close()

    def test_wake_is_a_task_after_the_hello(self):
        s = Sim("mtu 247", "enc 1", f"proof {CN1.hex()} {DN1.hex()}")
        self.assertEqual(s.w(bytes([0x89, 11])), [], "wake before the hello: ignored")
        self.assertEqual(s.w(SEALED_HELLO)[0][:2], bytes([0x89, HELLO]))
        s.frames("pump")
        s.cmd("task 0")
        self.assertEqual(s.w(state(CLOSED)), [], "no task: nothing")
        self.assertEqual(ask_frames(s.w(bytes([0x89, 11]))), [OPEN_REQ1], "wake: a task of its own, at once")
        self.assertEqual(ask_frames(s.w(bytes([0x89, 11]))), [], "a second wake: the series' next step")
        self.expect_ask_after(s, 1000, "the series' next step")
        s.close()

    def test_off_restores_todays_asks(self):
        s = self.session()
        s.cmd("askquiet 1")
        s.cmd("askquiet 0")
        self.unasked_end(s, CLOSED, 0, "off: at once")
        self.unasked_end(s, CLOSED, 1000, "then 1 s")
        self.assertEqual(s.w(state(CLOSED)), [])
        self.assertEqual(self.update(s, 2), [bytes.fromhex("890801"), OPEN_REQ1])  # off: a save while ended asks
        s.close()


@unittest.skipUnless(DATA_PATH, "an MGDS_DATA build")
class ReAskWithoutData(ReAsk):
    """The same series with no mgds_data_t ever attached: the re-ask state lives in mgds_t (4 bytes in its padding),
    so every rule holds, except that a session's open time is in the lent state: without it a session counts as
    short (the series goes on; never asked faster)."""

    def session(self, sealed=True):
        s = Sim("mtu 247", "enc 1", f"proof {CN1.hex()} {DN1.hex()}", "dattach 0")
        out = s.w(SEALED_HELLO) + s.frames("pump")
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))
        return s

    def test_restart_only_after_a_session_open_60s(self):
        s = self.session()
        for i, ms in enumerate([0, 1000, 2000]):
            self.unasked_end(s, CLOSED, ms, f"closed #{i}")
        self.assertEqual(s.w(state(OPEN_ST, 2)), [])
        s.frames("poll 60000")
        self.unasked_end(s, CLOSED, 4000, "no lent state saw it open: counted short, the series goes on")
        s.close()


@unittest.skipUnless(DATA_PATH, "an MGDS_DATA build")
class DataLent(unittest.TestCase):
    """mgds_data_t lent by the board: absent, attached and detached between sessions, attached inside the open
    callback, attached late, detached with a message part-way either way."""

    def keys(self, cn=CN1, dn=DN1):
        return seal.seal_keys(K, cn, dn, NODE)

    def device_msg(self, frames, counter):
        return DataPath.device_msg(self, frames, counter)

    def hello(self, s, hello=SEALED_HELLO):
        out = s.w(hello) + s.frames("pump")
        self.assertEqual(out[0][:2], bytes([0x89, HELLO]))

    def client(self, s, counter, pt, mtu=247):
        e, m = self.keys()
        cm = seal.seal(e, m, seal.CLIENT_TO_DEVICE, counter, pt)
        lines = []
        for f in data_frames(cm, mtu, True):
            lines += s.cmd("w " + f.hex())
        return cm, lines

    def test_absent(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 4096 4096", "dattach 0")
        self.hello(s)
        self.assertEqual(s.cmd("w " + STATE_OPEN.hex()), ["ST 2 1"])        # states still reported
        self.assertEqual(s.cmd("vmstate"), ["VS 2 0"])
        self.assertEqual(s.cmd("cansend 10"), ["CS 0"])
        self.assertEqual(s.cmd("sendmax"), ["SM 0"])
        self.assertIn("Q -2", s.cmd("send 00"))
        cm, lines = self.client(s, 0, b"dropped")
        self.assertEqual(lines, ["N " + credit(len(cm)).hex()])            # dropped and credited, as without MGDS_DATA
        cm, lines = self.client(s, 0, b"replay")                           # its counter was used
        self.assertEqual(lines[0], "N " + err(3).hex())
        self.assertEqual(s.cmd("w " + state(CLOSED, 1).hex()), ["ST 0 1", "N " + OPEN_REQ1.hex()])
        self.assertEqual(s.cmd("vmstate"), ["VS 0 0"])
        s.close()

    def test_no_port_data(self):
        # port.data NULL: attach does nothing, the device behaves as with the state absent.
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 4096 4096", "dattach 0", "dnull", "dattach 1")
        self.hello(s)
        self.assertEqual(s.cmd("w " + STATE_OPEN.hex()), ["ST 2 1"])
        self.assertEqual(s.cmd("cansend 1"), ["CS 0"])
        cm, lines = self.client(s, 0, b"dropped")
        self.assertEqual(lines, ["N " + credit(len(cm)).hex()])
        self.assertEqual(s.cmd("dattach 0"), ["DT 1"])
        s.close()

    def test_detach_and_attach_between_sessions(self):
        # The client's window is 100: a state attached after the hello knows it rounded down (64).
        hello = bytes([0x89, HELLO, 1]) + (4096).to_bytes(4, "little") + (100).to_bytes(4, "little") + b"\x03"
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 4096 4096")
        self.hello(s, hello)
        self.assertEqual(s.cmd("w " + STATE_OPEN.hex()), ["ST 2 1"])
        lines = s.cmd("send " + b"first".hex())
        self.assertEqual(self.device_msg([bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")], 1), b"first")
        cm, lines = self.client(s, 0, env(b"zero"))
        self.assertEqual(lines, ["N " + credit(len(cm)).hex(), "D " + env(b"zero").hex()])
        self.assertEqual(s.cmd("w " + state(CLOSED, 1).hex()), ["ST 0 1", "N " + OPEN_REQ1.hex()])
        self.assertEqual(s.cmd("dattach 0"), ["DT 1"])
        self.assertEqual(s.cmd("w " + state(CONNECTING, 1).hex()), ["ST 1 1"])
        s.cmd("dattach 1")                                                 # before the open
        self.assertEqual(s.cmd("w " + state(OPEN_ST, 2).hex()), ["ST 2 2"])
        self.assertEqual(s.cmd("sendmax"), [f"SM {4096 - 20}"])
        msg = bytes(150)
        lines = s.cmd("send " + msg.hex())
        frames = [bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")]
        self.assertEqual(sum(len(f) - (7 if f[2] & FIRST else 3) for f in frames), 64)   # the rounded window
        for _ in range(2):
            out = s.cmd("w " + credit(64).hex())
            frames += [bytes.fromhex(x[2:]) for x in out if x.startswith("N ")]
        self.assertIn("SENT 1", out)
        self.assertEqual(self.device_msg(frames, 2), msg)                 # the device counter went on
        cm, lines = self.client(s, 1, env(b"one"))                         # so did the client's
        self.assertEqual(lines, ["N " + credit(len(cm)).hex(), "D " + env(b"one").hex()])
        s.close()

    def test_attached_inside_the_open_callback(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 4096 4096", "dlazy 1")
        self.hello(s)
        self.assertEqual(s.cmd("cansend 1"), ["CS 0"])
        self.assertEqual(s.cmd("w " + STATE_OPEN.hex()), ["ST 2 1"])
        lines = s.cmd("send " + b"lazy".hex())
        self.assertEqual(self.device_msg([bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")], 1), b"lazy")
        cm, lines = self.client(s, 0, env(b"in"))
        self.assertEqual(lines, ["N " + credit(len(cm)).hex(), "D " + env(b"in").hex()])
        self.assertEqual(s.cmd("w " + state(CLOSED, 1).hex()), ["ST 0 1", "DT 1", "N " + OPEN_REQ1.hex()])
        self.assertEqual(s.cmd("w " + state(OPEN_ST, 2).hex()), ["ST 2 2"])
        s.close()

    def test_attached_late_carries_nothing_until_the_next_state(self):
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 4096 4096", "dattach 0")
        self.hello(s)
        s.cmd("w " + STATE_OPEN.hex())
        s.cmd("dattach 1")                                                 # the session is already open
        self.assertEqual(s.cmd("cansend 1"), ["CS 0"])
        cm, lines = self.client(s, 0, b"early")
        self.assertEqual(lines, ["N " + credit(len(cm)).hex()])            # dropped, credited
        self.assertEqual(s.cmd("w " + state(CLOSED, 1).hex()), ["ST 0 1", "N " + OPEN_REQ1.hex()])
        self.assertEqual(s.cmd("w " + state(OPEN_ST, 2).hex()), ["ST 2 2"])
        self.assertEqual(s.cmd("cansend 1"), ["CS 1"])
        cm, lines = self.client(s, 1, env(b"now"))
        self.assertEqual(lines, ["N " + credit(len(cm)).hex(), "D " + env(b"now").hex()])
        s.close()

    def test_detach_with_a_message_part_way(self):
        hello = bytes([0x89, HELLO, 1]) + (4096).to_bytes(4, "little") + (50).to_bytes(4, "little") + b"\x03"
        s = Sim("mtu 247", f"proof {CN1.hex()} {DN1.hex()}", "hello 4096 4096")
        self.hello(s, hello)
        s.cmd("w " + STATE_OPEN.hex())
        s.cmd("send " + bytes(200).hex())                                  # counter 1, 50 bytes out
        s.cmd("again 1")
        self.assertEqual(s.cmd("dattach 0"), ["A", "DT 0"])               # its abort found no buffer: not yet
        self.assertEqual(s.frames("poll 1"), [bytes([0x89, ABORT])])
        self.assertEqual(s.cmd("dattach 0"), ["DT 1"])
        s.cmd("dattach 1")
        s.cmd("w " + state(CLOSED, 1).hex())
        s.cmd("w " + state(OPEN_ST, 2).hex())
        lines = s.cmd("send " + b"after".hex())
        self.assertEqual(self.device_msg([bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")], 2), b"after")
        # A client message in flight (in pieces): dropped, its bytes back, the port told, then detached.
        s.cmd("dmode piece")
        s.cmd("w " + state(CLOSED, 2).hex())
        s.cmd("w " + state(OPEN_ST, 3).hex())
        e, m = self.keys()
        fr = data_frames(seal.seal(e, m, seal.CLIENT_TO_DEVICE, 0, bytes(300)), 247, True)
        s.cmd("w " + fr[0].hex())
        out = s.cmd("dattach 0")
        self.assertEqual(out, ["N " + credit(len(fr[0]) - 7).hex(), "PE 0", "DT 1"])
        # A reserved counter never sent is given back by a detach.
        s.cmd("dattach 1")
        s.cmd("w " + state(CLOSED, 3).hex())
        s.cmd("w " + state(OPEN_ST, 4).hex())
        s.cmd("again 1")
        self.assertEqual([x for x in s.cmd("send " + b"held".hex()) if x.startswith("N ")], [])
        self.assertEqual(s.cmd("dattach 0"), ["DT 1"])
        s.cmd("dattach 1")
        s.cmd("w " + state(CLOSED, 4).hex())
        s.cmd("w " + state(OPEN_ST, 5).hex())
        lines = s.cmd("send " + b"three".hex())
        self.assertEqual(self.device_msg([bytes.fromhex(x[2:]) for x in lines if x.startswith("N ")], 3), b"three")
        # Attached again while the detach's abort finds no buffer: the fresh state still owes it.
        s.cmd("w " + state(CLOSED, 5).hex())
        s.cmd("w " + state(OPEN_ST, 6).hex())
        s.cmd("send " + bytes(200).hex())                                  # counter 4, 50 bytes out
        s.cmd("again 1")
        s.cmd("dattach 1")
        self.assertEqual(s.frames("poll 1"), [bytes([0x89, ABORT])])
        s.close()


if __name__ == "__main__":
    unittest.main(verbosity=1)
