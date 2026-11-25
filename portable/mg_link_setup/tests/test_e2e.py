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
"""End-to-end: the phone (phone.py, `cryptography`) against the host gadget
(sim.c: the C core, the token proof and the two-slot store). Needs MGLS_SIM
(the sim binary) and MGLS_REPO (the repository root, for the vectors).
"""

import json
import os
import random
import string
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phone import Phone, Sim, b64u, proof_key, unb64u  # noqa: E402

SIM = os.environ.get("MGLS_SIM", os.path.join(os.path.dirname(__file__), "build", "sim"))
REPO = os.environ.get("MGLS_REPO",
                      os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..")))
SDK = "mgst_" + "T" * 43  # synthetic, the shape of a real one
NODE = "homelink-000001"  # the sim's default address 02:00:00:00:00:01
B64STD = string.ascii_letters + string.digits + "+/"


def token(n: int, seed: int) -> str:
    r = random.Random(seed)
    return "".join(r.choice(B64STD) for _ in range(n))


IOS_EXTRA = {"username": "someone@example.com", "api_url": "https://api.muse.ai/hatch",
             "api_url_v2": "https://api.muse.ai", "noise_host": "hatch.metaaivm.com"}


class Base(unittest.TestCase):
    sim_args: tuple = ("--sdk", SDK)

    def setUp(self):
        self.sim = Sim(SIM, *self.sim_args)
        self.closed = []

    def restart_sim(self, *args):
        """A fresh simulator in place of the current one; tearDown checks every one a test created."""
        self.sim.close()
        self.closed.append(self.sim)
        self.sim = Sim(SIM, *args)

    def tearDown(self):
        self.sim.close()
        sims = self.closed + [self.sim]
        for i, s in enumerate(sims):
            self.assertEqual(s.problems, [], f"harness checks (simulator {i + 1} of {len(sims)})")

    def phone(self, mtu=247, **kw) -> Phone:
        p = Phone(self.sim, mtu, **kw)
        p.connect()
        return p

    def setup_device(self, mtu=247, access=None, refresh=None, extra=None):
        access = access or token(600, 1)
        refresh = refresh or token(1300, 2)
        p = self.phone(mtu)
        info = p.get_device_info()
        conf = p.pair()
        st = p.provision(access, refresh, IOS_EXTRA if extra is None else extra)
        self.assertEqual(st, {"type": "status", "status": "auth_ok"})
        p.poll()
        return p, info, conf, access, refresh

    def wait_disconnect(self, p, ms):
        before = self.sim.disconnects
        p.poll(ms - 5)
        self.assertEqual(self.sim.disconnects, before, "disconnected too early")
        p.poll(10)
        self.assertEqual(self.sim.disconnects, before + 1, "no disconnect")


class DeviceInfoAndVectors(Base):
    def test_device_info(self):
        p = self.phone()
        info = p.get_device_info()
        self.assertEqual(info, {
            "type": "device_info", "node_id": NODE, "version": "1.0.0",
            "device_id": "hatch-link:02:00:00:00:00:01", "mac": "02:00:00:00:00:01",
            "model": "hatch_link", "pairing_protocol": 5, "pairing_auth": "none",
            "pairing_auth_epoch": 0, "pairing_policy": "confirm_press", "build_sha": "",
            "wifi": "none", "mgcommands": 1})
        self.assertEqual(self.sim.cmd("read"), ["R idle"])
        self.assertEqual(self.sim.state()["ws"], 0, "workspace back after a plaintext exchange")

    def test_link_pairing_v5_vector(self):
        with open(os.path.join(REPO, "esp32/tests/vectors/link_pairing_v5.json")) as f:
            v = next(x for x in json.load(f)["vectors"] if x["name"] == "community_v5")
        p = self.phone(private_value=1, mobile_nonce=unb64u(v["mobile_nonce"]))
        self.assertEqual(p.mobile_pub, v["mobile_pub"])
        self.sim.cmd("eph " + v["device_private_scalar_hex"])
        self.sim.cmd("nonce " + unb64u(v["device_nonce"]).hex())
        p.write(p.hello())
        ready = p.recv()
        for k in ("device_id", "node_id", "mac", "firmware_version", "device_pub",
                  "device_nonce", "transcript_hash", "session_id"):
            self.assertEqual(ready[k], v[k], k)
        p.accept_ready(ready)
        self.assertEqual(p.keys["secret"].hex(), v["session_secret_hex"])
        self.assertEqual(p.keys["tx"].hex(), v["mobile_tx_key_hex"])
        self.assertEqual(p.keys["rx"].hex(), v["mobile_rx_key_hex"])
        # The vector's own client_finished record, byte for byte.
        p.write({"action": "pairing_encrypted", "session_id": v["session_id"], "counter": "0",
                 "ciphertext": v["client_finished_ciphertext"], "tag": v["client_finished_tag"]})
        p.tx_ctr = 1
        self.assertEqual(p.status(), {"type": "status", "status": "confirm_required"})
        self.assertEqual(self.sim.cmd("read"), ["R encrypted_status"])


class FullSetup(Base):
    def check_set_up(self, p, access, refresh, conf):
        self.assertEqual(conf.get("sdk_token"), SDK)
        d = self.sim.dump()
        self.assertEqual(d["access"], access.encode())
        self.assertEqual(d["refresh"], refresh.encode())
        k = proof_key(access, NODE)
        self.assertEqual(d["k"], k)
        s = self.sim.state()
        self.assertEqual((s["setup"], s["wifi_skipped"], s["seq"], s["other_blank"]), (1, 1, 1, 1))
        # The consumed session keeps the workspace until a disconnect or
        # mgls_abort(); nothing is pending, so the loop may stop polling.
        self.assertEqual((s["ws"], s["idle"], s["active"]), (1, 1, 0))
        self.assertEqual(p.proof(k), 1)
        self.assertIn("provisioned", self.sim.events)
        self.sim.cmd("abort")
        self.assertEqual(self.sim.state()["ws"], 0, "workspace back after mgls_abort")

    def test_mtus(self):
        for mtu in (23, 185, 247, 498):
            with self.subTest(mtu=mtu):
                self.restart_sim("--sdk", SDK)
                p, _, conf, a, r = self.setup_device(mtu)
                self.check_set_up(p, a, r, conf)

    def test_measured_token_lengths(self):
        p, _, conf, a, r = self.setup_device(185, token(403, 3), token(1396, 4))
        self.check_set_up(p, a, r, conf)

    def test_without_optional_fields_and_escaping(self):
        p = self.phone()
        conf = p.pair()
        a, r = token(500, 5), token(1200, 6)
        self.assertEqual(p.provision(a, r, {}, ios_escaping=False)["status"], "auth_ok")
        p.poll()
        self.check_set_up(p, a, r, conf)

    def test_survives_reboot(self):
        p, _, _, a, _ = self.setup_device()
        self.sim.cmd("disconnect")
        self.sim.cmd("reboot")
        p = self.phone()
        self.assertEqual(p.proof(proof_key(a, NODE)), 1)
        self.assertEqual(self.sim.state()["setup"], 1)

    def test_backpressure(self):
        p = self.phone(23)
        self.sim.cmd("again 3")  # the first three notifications find no buffer
        p.write(p.hello())
        self.assertIsNone(p.recv())
        for _ in range(4):
            p.poll()
        p.accept_ready(p.recv())
        self.sim.cmd("again 2")
        p.write(p.seal({"action": "pairing_client_finished"}))
        self.assertFalse(self.sim.state()["armed"], "armed before the prompt went out")
        p.poll()                            # no buffer at first, then it goes out
        self.assertEqual(p.status()["status"], "confirm_required")
        self.assertFalse(self.sim.state()["armed"], "armed by the poll that sent the prompt")
        p.poll()
        self.assertTrue(self.sim.state()["armed"])


class Press(Base):
    def test_press_without_session_is_not_ours(self):
        p = self.phone()
        self.assertIn("R press 0", self.sim.cmd("press"))
        p.write(p.hello())
        p.accept_ready(p.recv())
        self.assertIn("R press 0", self.sim.cmd("press"))  # before client_finished
        p.write(p.seal({"action": "pairing_client_finished"}))
        self.assertEqual(p.status()["status"], "confirm_required")
        p.poll()
        self.assertIsNone(p.recv(), "an early press confirmed something")

    def test_press_before_the_prompt_went_out_is_ignored(self):
        p = self.phone()
        p.write(p.hello())
        p.accept_ready(p.recv())
        self.sim.cmd("again 1000")
        p.write(p.seal({"action": "pairing_client_finished"}))
        self.assertIn("R press 1", self.sim.cmd("press"))  # pending, not armed: swallowed
        self.sim.cmd("again 0")
        p.poll()
        self.assertEqual(p.status()["status"], "confirm_required")
        p.poll()
        p.poll()
        self.assertIsNone(p.recv(), "the ignored press was remembered")
        self.assertIn("R press 2", self.sim.cmd("press"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")
        self.assertIn("R press 0", self.sim.cmd("press"))  # it's the firmware's again

    def test_one_press_confirms_with_no_edge_check(self):
        # No edge check and no press time: a press in the very poll that put the prompt up confirms.
        p = self.phone()
        p.write(p.hello())
        p.accept_ready(p.recv())
        p.write(p.seal({"action": "pairing_client_finished"}))   # the poll at p.t sends confirm_required
        self.assertEqual(p.status()["status"], "confirm_required")
        p.poll(0)                                                 # the prompt is up, in the same millisecond
        self.assertTrue(self.sim.state()["armed"])
        self.assertIn("R press 2", self.sim.cmd("press"))
        p.poll(0)
        self.assertEqual(p.status()["status"], "pairing_confirmed")

    def test_an_answer_before_the_arm_keeps_it(self):
        # The prompt finds no buffer and goes out at the start of the next poll, which has a message from the phone
        # to answer: the answer waits for the poll that arms the press, so the arm isn't lost.
        p = self.phone()
        p.write(p.hello())
        p.accept_ready(p.recv())
        self.sim.cmd("again 2")
        p.write(p.seal({"action": "pairing_client_finished"}))   # no buffer: the prompt stays queued
        self.assertIsNone(p.recv())
        p.write(p.seal({"action": "no_such_action"}))           # this poll sends the prompt
        self.assertEqual(p.status()["status"], "confirm_required")
        self.assertIsNone(p.recv(), "answered before the arm")
        p.poll()
        self.assertTrue(self.sim.state()["armed"], "the answer took the arm's mark")
        self.assertEqual(p.status()["status"], "error_unknown_action")
        self.assertIn("R press 2", self.sim.cmd("press"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")

    def test_the_prompt_has_no_timer(self):
        # The prompt stays up until a press, a dismiss or the phone giving up: the gadget never times it out.
        p = self.phone()
        p.pair(press=False)
        for ms in (59_000, 1_100, 600_000):
            p.poll(ms)
            self.assertIsNone(p.recv())
        self.assertNotIn("timeout", self.sim.events)
        self.assertEqual(self.sim.state()["armed"], 1)
        self.assertIn("R press 2", self.sim.cmd("press"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")

    def test_the_back_gesture_dismisses_a_first_setups_prompt(self):
        p = self.phone()
        p.pair(press=False)
        self.assertIn("R dismiss 1", self.sim.cmd("dismiss"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirm_timeout")
        self.assertIn("timeout", self.sim.events)
        self.wait_disconnect(p, 100)
        self.assertIn("R press 0", self.sim.cmd("press"))
        self.assertEqual(self.sim.state()["ws"], 0)

    def test_confirmed_idle_timeout(self):
        p = self.phone()
        p.pair()
        p.poll(119_000)
        self.assertIsNone(p.recv())
        p.poll(1_100)
        self.assertEqual(p.status()["status"], "pairing_confirm_timeout")
        self.wait_disconnect(p, 100)
        self.assertEqual(self.sim.state()["setup"], 0)

    def test_client_finished_timeout(self):
        p = self.phone()
        p.write(p.hello())
        p.accept_ready(p.recv())
        p.poll(60_100)
        self.assertIn("session_ended", self.sim.events)
        p.write(p.seal({"action": "pairing_client_finished"}))
        self.assertIsNone(p.recv())  # no keys, plaintext blocked: nothing
        self.wait_disconnect(p, 300)

    def test_disconnect_mid_confirmation(self):
        p = self.phone()
        p.pair(press=False)
        self.sim.cmd("disconnect")
        s = self.sim.state()
        self.assertEqual((s["armed"], s["active"], s["ws"]), (0, 0, 0))
        self.assertIn("R press 0", self.sim.cmd("press"))
        p = self.phone()
        p.pair()


class Reprovision(Base):
    def test_set_up_gadget_takes_a_new_setup(self):
        p, _, _, a1, _ = self.setup_device()
        k1 = proof_key(a1, NODE)
        self.assertEqual(p.proof(k1), 1)
        self.sim.cmd("disconnect")
        # Another phone, a new connection, no forget first (contract 7).
        p2 = self.phone()
        self.assertEqual(p2.get_device_info()["wifi"], "none")
        p2.pair()
        a2, r2 = token(610, 11), token(1310, 12)
        self.assertEqual(p2.provision(a2, r2, IOS_EXTRA)["status"], "auth_ok")
        k2 = proof_key(a2, NODE)
        self.assertEqual(p2.proof(k1), 0, "the old K still matches")
        self.assertEqual(p2.proof(k2), 1)
        d = self.sim.dump()
        self.assertEqual((d["access"], d["refresh"], d["k"]), (a2.encode(), r2.encode(), k2))
        s = self.sim.state()
        self.assertEqual((s["setup"], s["seq"], s["other_blank"]), (1, 2, 1))

    def test_same_connection_and_proof_reset_at_commit(self):
        p, _, _, a1, _ = self.setup_device()
        self.assertEqual(p.proof(proof_key(a1, NODE)), 1)
        self.assertEqual(self.sim.state()["matched"], 1)
        p2 = Phone(self.sim, 247, t0=p.t)  # the app's Link setup on the live connection
        p2.pair()
        a2 = token(600, 21)
        self.assertEqual(p2.provision(a2, token(1300, 22), IOS_EXTRA)["status"], "auth_ok")
        self.assertEqual(self.sim.state()["matched"], 0, "proof state survived the commit")
        self.assertEqual(p2.proof(proof_key(a2, NODE)), 1)

    def test_abandoned_reprovisioning_keeps_the_old_set(self):
        p, _, _, a1, _ = self.setup_device()
        self.sim.cmd("disconnect")
        p2 = self.phone()
        p2.pair()
        self.sim.cmd("disconnect")  # gone before provision_v2
        self.assertEqual(self.sim.state()["setup"], 1)
        self.assertEqual(self.phone().proof(proof_key(a1, NODE)), 1)


class FailClosed(Base):
    def test_a_config_without_set_up_refuses_every_hello(self):
        # mgls_init() refuses a config without set_up() (MGLS_E_CONFIG) and the core fails closed: a hello gets
        # error_pairing_unavailable in plaintext and nothing shows, even once the hook is back at run time (a board
        # that forgot it can't treat a re-setup as a first setup).
        self.setup_device()
        self.sim.cmd("disconnect")
        self.assertIn("R init -2", self.sim.cmd("badconfig"))
        p = self.phone()
        p.write(p.hello())
        self.assertEqual(p.recv(), "error_pairing_unavailable")
        self.assertEqual(self.sim.state()["armed"], 0)
        self.assertEqual(self.sim.dump()["access"], token(600, 1).encode(), "the stored set stays")


class NoSetUpHook(Base):
    """With no set_up hook the core can't tell a set-up gadget from a new one, so it fails closed: every hello gets
    error_pairing_unavailable, no prompt shows and no press confirms; the stored set stays."""

    def test_a_set_up_gadget_with_no_hook_shows_no_prompt_and_confirms_nothing(self):
        self.setup_device()
        self.sim.cmd("disconnect")
        self.sim.cmd("setuphook off")
        p = self.phone()
        p.write(p.hello())
        self.assertEqual(p.recv(), "error_pairing_unavailable")
        self.assertIsNone(p.recv(), "nothing else")
        self.assertEqual(self.sim.state()["armed"], 0)
        self.assertIn("R press 0", self.sim.cmd("press"))           # MGLS_PRESS_NONE: no pairing waits
        self.assertEqual(self.sim.state()["setup"], 1)

    def test_a_new_gadget_with_no_hook_refuses_the_hello_too(self):
        self.sim.cmd("setuphook off")
        p = self.phone()
        p.write(p.hello())
        self.assertEqual(p.recv(), "error_pairing_unavailable")
        self.assertEqual(self.sim.state()["armed"], 0)
        self.assertEqual(self.sim.state()["setup"], 0)


class Phone2(Phone):
    """The app on connection 2 (the sim's "multiconn": mgls_conn_*(), its notifications as "O 2 HEX")."""

    def connect(self):
        self.sim.cmd(f"connect2 {self.t}")

    def write_raw(self, data: bytes, poll: bool = True):
        for c in self.chunks(data):
            self.sim.cmd("rx2 " + c.hex())
        if poll:
            self.poll()

    def _drain(self):
        while self.sim.other:
            conn, _, h = self.sim.other.pop(0).partition(" ")
            assert conn == "2", conn
            n = bytes.fromhex(h)
            if n[:1] != b"\xfe":
                self.inbox.append(n.decode())   # a bare plaintext status
                continue
            m = self.rx.feed(n)
            if m is not None:
                try:
                    self.inbox.append(json.loads(m))
                except ValueError:
                    self.inbox.append(m.decode())


class ReSetupPromptRate(Base):
    """On a set-up gadget's release build a new re-setup prompt within 10 s of a dismiss on the same connection is
    refused (may_prompt(), since_ms: that dismiss): error_operation_in_progress in plaintext, nothing shown. A hello
    on the prompt's connection while it is up takes it over; one from another connection is busy. No hook (test and
    bench builds): every hello goes on."""

    def test_a_hello_while_the_prompt_is_up_takes_it_over(self):
        # Hello A, hello B 2 s later on the same connection: B takes the prompt over (never refused, the board not
        # asked, the prompt never leaves the screen) and the one press confirms B.
        self.sim.cmd("promptgap 10000")
        self.setup_device()
        self.sim.cmd("disconnect")
        a = self.phone()
        a.pair(press=False)
        self.assertEqual(self.sim.asks, ["since=none ok=1"])
        events = len(self.sim.events)
        a.poll(2000)
        b = Phone(self.sim, 247, t0=a.t)                          # another hello on the link
        b.pair(press=False)
        self.assertEqual(self.sim.asks, ["since=none ok=1"], "not asked, not refused")
        self.assertEqual(self.sim.events[events:], ["session_started"], "the prompt never left the screen")
        self.assertIn("R press 2", self.sim.cmd("presskey other"))
        b.poll()
        self.assertEqual(b.status()["status"], "pairing_confirmed", "under B's keys")

    def test_a_press_while_a_takeover_runs_confirms_it(self):
        # The prompt is still up while B's handshake runs: a press then is the prompt's, and confirms B once B's
        # confirm_required has gone out.
        self.setup_device()
        self.sim.cmd("disconnect")
        a = self.phone()
        a.pair(press=False)
        b = Phone(self.sim, 247, t0=a.t)
        b.write(b.hello())
        b.accept_ready(b.recv())
        self.assertIn("R press 2", self.sim.cmd("presskey other"), "the prompt's key: no talk turn")
        b.write(b.seal({"action": "pairing_client_finished"}))
        self.assertEqual(b.status()["status"], "confirm_required")
        b.poll()
        self.assertEqual(b.status()["status"], "pairing_confirmed")

    def test_a_new_prompt_within_10_s_of_a_dismiss_is_refused(self):
        # With no prompt up, a hello on the same connection within 10 s of a dismiss (here in the very poll that ends
        # the dismissed prompt: since 0) gets error_operation_in_progress and shows nothing; a new connection prompts
        # at once.
        self.sim.cmd("promptgap 10000")
        self.setup_device()
        self.sim.cmd("disconnect")
        p = self.phone()
        p.pair(press=False)
        self.assertIn("R dismiss 1", self.sim.cmd("dismiss"))
        p.write(p.hello(), poll=False)
        p.poll(0)
        self.assertEqual(p.status()["status"], "pairing_confirm_timeout")
        self.assertEqual(self.sim.asks[-1], "since=0 ok=0")
        self.assertEqual(p.recv(), "error_operation_in_progress")
        self.assertEqual(self.sim.state()["armed"], 0)
        self.wait_disconnect(p, 100)
        p.connect()
        q = Phone(self.sim, 247, t0=p.t)
        q.pair(press=False)
        self.assertEqual(self.sim.asks[-1], "since=none ok=1")

    def test_a_hello_from_another_connection_is_busy(self):
        # Only a hello on the prompt's own connection takes it over. Connection 0's hello shows the prompt;
        # connection 2's hello gets error_operation_in_progress (in plaintext, at its first write, to connection 2
        # only) and changes nothing; the press confirms connection 0's hello.
        self.sim.cmd("multiconn")
        self.setup_device()
        self.sim.cmd("disconnect")
        a = self.phone()
        a.pair(press=False)
        self.sim.cmd(f"connect2 {a.t}")
        b = Phone(self.sim, 247, t0=a.t)
        for c in b.chunks(json.dumps(b.hello(), separators=(",", ":")).encode()):
            self.sim.cmd("rx2 " + c.hex())
        a.poll()
        self.assertEqual(self.sim.other, ["2 " + b"error_operation_in_progress".hex()])
        self.assertIsNone(a.recv(), "nothing to the prompt's connection")
        self.assertEqual(self.sim.state()["armed"], 1)
        self.assertIn("R press 2", self.sim.cmd("presskey other"))
        a.poll()
        self.assertEqual(a.status()["status"], "pairing_confirmed", "under connection 0's keys")

    def test_no_hook_every_hello_goes_on(self):
        self.setup_device()                                        # promptgap off: the default
        self.sim.cmd("disconnect")
        p = self.phone()
        p.pair(press=False)
        self.assertIn("R dismiss 1", self.sim.cmd("dismiss"))
        p.write(p.hello(), poll=False)
        p.poll(0)
        self.assertEqual(p.status()["status"], "pairing_confirm_timeout")
        q = Phone(self.sim, 247, t0=p.t)
        q.pair(press=False)
        self.assertEqual(self.sim.asks, [])
        self.assertEqual(self.sim.state()["armed"], 1)

    def test_an_invalid_hello_is_refused_first(self):
        self.sim.cmd("promptgap 10000")
        self.setup_device()
        self.sim.cmd("disconnect")
        p = self.phone()
        h = p.hello()
        h["version"] = 4
        p.write(h)
        self.assertEqual(p.recv(), "error_pairing_invalid_hello")
        self.assertEqual(self.sim.asks, [], "not asked")


# The session vectors' proof_gates section: its re-setup exchanges replay here (MG_SESSION_VECTORS points at another
# copy).
SESSION_VECTORS = os.environ.get("MG_SESSION_VECTORS",
                                 os.path.join(REPO, "protocols", "test-vectors", "mg-session-v1.json"))
RESETUP_EVENTS = ("resetup_request", "dismiss", "press", "setup_ends", "tap_elsewhere", "setup_mode")


class ReSetupPromptVectors(Base):
    """mg-session-v1.json's proof_gates re-setup exchanges, on this core. Connection 1 is the sim's connection 0,
    connection 2 its connection 2 (multiconn). A release build's board answers may_prompt() with the section's
    resetup_prompt_ms; an automation build names the test default. A talk-key-only device sets setup_mode() (always
    in setup mode on an automation build) and in setup mode reports its key as MGLS_KEY_OTHER. Exchanges for another
    kind of device are skipped with the reason (skip_reason)."""

    @staticmethod
    def skip_reason(ex):
        dev = ex["device"]
        if any("event" not in st for st in ex["steps"]):
            return "SMP steps: this core has no SMP gate"
        if not dev.get("dismiss_drops_link", True):
            return "a dismiss that keeps the link: this core's dismiss ends the setup link"
        if not dev.get("bad_hello_encrypted", True):
            return "a bad takeover hello refused in plaintext: this core refuses it before the wipe, sealed under " \
                   "the current session"
        if dev.get("bad_hello_drops_link", False):
            return "a bad takeover hello that drops the link: this core keeps it (the next hello prompts afresh)"
        return None

    def test_the_prompt_exchanges(self):
        with open(SESSION_VECTORS) as f:
            pg = json.load(f)["proof_gates"]
        exchanges = [ex for ex in pg["exchanges"] if any(st.get("event") in RESETUP_EVENTS for st in ex["steps"])]
        ran = []
        for ex in exchanges:
            with self.subTest(ex["name"]):
                reason = self.skip_reason(ex)
                if reason:
                    self.skipTest(reason)
                self.replay(pg, ex)
                ran.append(ex["name"])
        self.assertGreaterEqual(len(ran), 4)

    def replay(self, pg, ex):
        self.restart_sim(*self.sim_args)
        dev = ex["device"]
        release, talk_only = dev.get("release", True), dev.get("talk_key_only", False)
        setup_mode = not release                       # test and bench builds: always in setup mode
        sm_at = None                                   # when a release board's setup mode began (5 min)
        self.sim.cmd("multiconn")
        self.sim.cmd(f"promptgap {pg['resetup_prompt_ms']}" if release else "promptgap off")
        if talk_only:
            self.sim.cmd(f"setupmode {int(setup_mode)}")
        if dev.get("holds_token", True):
            self.setup_device()
            self.sim.cmd("disconnect")
        mtu = ex["mtu"]
        t = 1000
        base = {1: self.phone(mtu)}                    # each connection's link (its own clock is t)
        sessions = {}                                  # (conn, hello) -> the Phone that sent it
        hellos = {1: 0, 2: 0}
        last = {"prompt_conn": None, "prompt_hello": None, "confirmed_conn": None, "confirmed_hello": None}

        def link(conn):
            if conn not in base:
                base[conn] = Phone2(self.sim, mtu, t0=t)
                base[conn].connect()
            return base[conn]

        for i, st in enumerate(ex["steps"]):
            what = f"{ex['name']} step {i}: {st['note']}"
            ev, conn = st["event"], st.get("conn", 1)
            want = st.get("resetup")
            if ev == "time":
                t += st["ms"]
                self.sim.cmd(f"poll {t}")
                for ph in base.values():
                    ph._drain()
                continue
            if ev in ("reconnect", "setup_mode"):
                self.sim.cmd("disconnect")
                if ev == "setup_mode":
                    setup_mode, sm_at = True, t
                    self.sim.cmd("setupmode 1")
                base = {1: Phone(self.sim, mtu, t0=t)}
                base[1].connect()
                hellos = {1: 0, 2: 0}
                sessions.clear()
                last = dict(last, prompt_conn=None, prompt_hello=None)   # a new link: no prompt up
                if want is not None:
                    self.assertEqual(want["prompt_conn"], None, what)
                continue
            if ev == "resetup_request":
                hellos[conn] += 1
                setup = st["setup"]
                lk = link(conn)
                q = (Phone if conn == 1 else Phone2)(self.sim, mtu, t0=t)
                armed_before = self.sim.events.count("confirm_armed")
                events = len(self.sim.events)
                h = q.hello()
                if st.get("invalid"):
                    h["mobile_pub"] = b64u(b"\x05" + bytes(64))   # valid top-level fields, a bad mobile_pub
                q.write(h)
                t = q.t
                got = q.recv()
                if setup["status"] == "confirm_required":
                    q.accept_ready(got)
                    q.write(q.seal({"action": "pairing_client_finished"}))
                    self.assertEqual(q.status()["status"], "confirm_required", what)
                    q.poll()
                    t = q.t
                    self.assertEqual(self.sim.state()["armed"], 1, what)
                    shown = self.sim.events.count("confirm_armed") > armed_before
                    self.assertEqual("shown" if shown else "taken_over", setup["prompt"], what)
                    sessions[(conn, hellos[conn])] = q
                elif setup.get("encrypted"):
                    # Refused before anything changed: sealed under the session still current (the prompt's).
                    cur = sessions[(conn, last["prompt_hello"])]
                    self.assertEqual(cur.open(got)["status"], setup["status"], what)
                    self.assertIsNone(q.recv(), what)
                else:
                    self.assertEqual(got, setup["status"], what)
                    self.assertEqual(got.encode().hex(), setup["plaintext"], what)
                    self.assertIsNone(q.recv(), what)
                if setup.get("prompt") == "ended":
                    self.assertEqual(self.sim.state()["armed"], 0, what)
                    self.assertIn("session_ended", self.sim.events[events:], what)
                lk.t = t
            elif ev == "press":
                key = st.get("key")
                if sm_at is not None and t - sm_at >= pg.get("setup_mode_ms", 300000):
                    setup_mode = False
                other = key != "talk" or (talk_only and setup_mode)
                r = self.sim.cmd("presskey other" if other else "press")
                if last["prompt_conn"] is None:
                    self.assertIn("R press 0", r, what)            # no prompt up: the press is the firmware's
                else:
                    self.assertTrue(any(x.startswith("R press ") and x != "R press 0" for x in r), what)
            elif ev == "injected_press":
                pass                                   # a press the stack injects reaches no core call: a no-op
            elif ev == "dismiss":
                self.assertIn("R dismiss 1", self.sim.cmd("dismiss"), what)
            elif ev == "tap_elsewhere":
                pass                                   # a tap away from the disc reaches no core call
            elif ev == "setup_ends":
                q = sessions[(want["confirmed_conn"], want["confirmed_hello"])]
                q.t = t
                self.assertEqual(q.provision(token(600, 90 + i), token(1300, 90 + i), IOS_EXTRA)["status"],
                                 "auth_ok", what)
                t = q.t
            else:
                self.fail(f"{what}: unknown event {ev}")
            if ev in ("press", "dismiss"):
                t += 10
                self.sim.cmd(f"poll {t}")
                if want["confirmed_hello"] is not None and (want["confirmed_hello"], want["confirmed_conn"]) != \
                        (last["confirmed_hello"], last["confirmed_conn"]):
                    q = sessions[(want["confirmed_conn"], want["confirmed_hello"])]
                    self.assertEqual(q.status()["status"], "pairing_confirmed", what)
                    if dev.get("holds_token", True) and release:
                        setup_mode, sm_at = False, None   # setup mode ends at the confirmed re-setup
                elif last["prompt_conn"] is not None and want["prompt_conn"] is None:
                    q = sessions[(last["prompt_conn"], last["prompt_hello"])]
                    self.assertEqual(q.status()["status"], "pairing_confirm_timeout", what)
            if want is not None:
                self.assertEqual(self.sim.state()["armed"], int(want["prompt_conn"] is not None), what)
                last = want


class ReSetupConfirm(Base):
    """Setting up a gadget that is set up: one press of a key that isn't the talk key (MGLS_KEY_OTHER) while the
    prompt is up, or one tap on the Confirm disc (mgls_confirm_gesture), with no hold and no press time. The talk key
    only dismisses it (no talk turn); the back gesture dismisses it; a one-key board takes it only in setup mode."""

    def resetup_prompt(self):
        self.setup_device()
        self.sim.cmd("disconnect")
        p = self.phone()
        p.pair(press=False)
        return p

    def test_another_key_confirms(self):
        p = self.resetup_prompt()
        self.assertIn("R press 2", self.sim.cmd("presskey other"), "the prompt's key: no talk turn")
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")
        a2 = token(600, 81)
        self.assertEqual(p.provision(a2, token(1300, 82))["status"], "auth_ok")
        self.assertEqual(self.sim.dump()["access"], a2.encode())
        self.assertIn("R press 0", self.sim.cmd("press"), "the firmware's again")

    def test_the_talk_key_dismisses_a_re_setup_prompt(self):
        # Never confirmed by the talk key (mgls_press() is the talk key): it dismisses the prompt, no talk turn; the
        # stored set stays.
        p = self.resetup_prompt()
        self.assertIn("R press 3", self.sim.cmd("press"))        # MGLS_PRESS_DISMISSED
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirm_timeout")
        self.wait_disconnect(p, 100)
        self.assertEqual((self.sim.state()["setup"], self.sim.state()["armed"]), (1, 0))
        self.assertEqual(self.sim.dump()["access"], token(600, 1).encode(), "the old set")

    def test_a_one_key_board_re_sets_up_only_in_setup_mode(self):
        # Its setup_mode() hook: outside setup mode a re-setup hello gets error_pairing_unavailable in plaintext and
        # nothing shows; in setup mode its key isn't the talk key (MGLS_KEY_OTHER) and confirms. A first setup isn't
        # asked.
        self.sim.cmd("setupmode 0")
        self.setup_device()
        self.sim.cmd("disconnect")
        p = self.phone()
        p.write(p.hello())
        self.assertEqual(p.recv(), "error_pairing_unavailable")
        self.assertEqual(self.sim.state()["armed"], 0)
        self.sim.cmd("setupmode 1")
        p.pair(press=False)
        self.assertIn("R press 2", self.sim.cmd("presskey other"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")

    def test_a_first_setup_takes_the_talk_key(self):
        p = self.phone()
        p.pair(press=False)
        self.assertIn("R press 2", self.sim.cmd("press"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")

    def test_the_back_gesture_dismisses_the_prompt(self):
        p = self.resetup_prompt()
        self.assertIn("R dismiss 1", self.sim.cmd("dismiss"))
        self.assertIsNone(p.recv(), "nothing before the next poll")
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirm_timeout", "dismissed from the next poll")
        self.wait_disconnect(p, 100)
        self.assertEqual((self.sim.state()["setup"], self.sim.state()["armed"]), (1, 0), "the stored set stays")
        self.assertIn("R dismiss 0", self.sim.cmd("dismiss"), "no prompt up: nothing")
        self.assertIn("R press 0", self.sim.cmd("press"), "the firmware's again")

    def test_dismiss_with_no_prompt_up(self):
        self.assertIn("R dismiss 0", self.sim.cmd("dismiss"), "no session at all")
        p = self.phone()
        p.write(p.hello())
        p.accept_ready(p.recv())
        self.assertIn("R dismiss 0", self.sim.cmd("dismiss"), "before the prompt")
        p.write(p.seal({"action": "pairing_client_finished"}))
        self.assertEqual(p.status()["status"], "confirm_required")
        p.poll()
        self.assertIn("R press 2", self.sim.cmd("press"))
        self.assertIn("R dismiss 0", self.sim.cmd("dismiss"), "once it confirmed")
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")

    def test_ota_url_is_ignored_and_logged(self):
        # A re-setup never takes an ota_url: logged once in every build (token-only takes none anyway); a first
        # setup doesn't log it.
        self.setup_device(extra=dict(IOS_EXTRA, ota_url="https://x/fw.bin"))
        self.assertNotIn("L mgls: re-setup: ota_url ignored", self.sim.lines)
        self.sim.cmd("disconnect")
        p = self.phone()
        p.pair()
        self.assertEqual(p.provision(token(600, 83), token(1300, 84), dict(IOS_EXTRA, ota_url="https://x/fw.bin"))
                         ["status"], "auth_ok")
        self.assertEqual(self.sim.lines.count("L mgls: re-setup: ota_url ignored"), 1)

    def test_a_press_before_the_prompt_is_swallowed(self):
        self.setup_device()
        self.sim.cmd("disconnect")
        p = self.phone()
        p.write(p.hello())
        p.accept_ready(p.recv())
        self.sim.cmd("again 1000")
        p.write(p.seal({"action": "pairing_client_finished"}))
        self.assertIn("R press 1", self.sim.cmd("presskey other"))  # the prompt not up: swallowed, not remembered
        self.sim.cmd("again 0")
        p.poll()
        self.assertEqual(p.status()["status"], "confirm_required")
        p.poll()
        self.assertIsNone(p.recv(), "the swallowed press was remembered")
        self.assertIn("R press 2", self.sim.cmd("presskey other"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")

    def test_on_screen_confirm(self):
        self.assertIn("R gesture 0", self.sim.cmd("gesture"), "nothing armed")
        p = self.resetup_prompt()
        self.assertIn("R gesture 1", self.sim.cmd("gesture"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")

    def test_on_screen_confirm_on_a_first_setup(self):
        p = self.phone()
        p.pair(press=False)
        self.assertIn("R gesture 1", self.sim.cmd("gesture"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")

    def test_bench_press_confirms_a_re_setup(self):
        p = self.resetup_prompt()
        self.assertIn("R press 2", self.sim.cmd("simpress"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")


class TakeoverRefused(Base):
    """A hello on the prompt's connection takes the prompt over only if it is valid. One refused (a malformed key or
    nonce before anything changes, or a key the key agreement refuses after the old session is gone) ends the prompt
    (session_ended), so no prompt is left up without a session: a talk press then is the firmware's again, back
    ends nothing, and the next valid hello shows a new prompt that only a new confirm confirms."""

    OFF_CURVE = b64u(b"\x04" + bytes(64))   # decodes as a point, refused by the key agreement

    def prompt(self, resetup=True):
        if resetup:
            self.setup_device()                   # the owner's set (token(600, 1))
            self.sim.cmd("disconnect")
        p = self.phone()
        p.pair(press=False)
        self.assertEqual(self.sim.state()["armed"], 1)
        return p

    def refused_takeover(self, p, field, value):
        events = len(self.sim.events)
        bad = Phone(self.sim, 247, t0=p.t)        # the same connection
        h = bad.hello()
        h[field] = value
        bad.write(h)
        p.t = bad.t
        self.refusal = bad.recv()                 # what came back to the refused hello's write
        self.assertEqual(self.sim.state()["armed"], 0, "the prompt ended")
        self.assertEqual([e for e in self.sim.events[events:] if e not in ("borrow", "release")], ["session_ended"])
        return p

    def ghost_sequence(self, field, value, resetup=True):
        # The review's sequence: a refused takeover hello, a talk press, back, a valid hello on the same connection.
        p = self.refused_takeover(self.prompt(resetup), field, value)
        self.assertIn("R press 0", self.sim.cmd("press"), "no prompt up: the talk key is the firmware's")
        self.assertIn("R dismiss 0", self.sim.cmd("dismiss"), "nothing to dismiss")
        p.poll()
        events = len(self.sim.events)
        c = Phone(self.sim, 247, t0=p.t)
        c.pair(press=False)
        self.assertIn("confirm_armed", self.sim.events[events:], "a new prompt")
        self.assertEqual(self.sim.state()["armed"], 1)
        c.poll()
        c.poll(1000)
        self.assertIsNone(c.recv(), "the earlier press confirmed nothing")
        self.assertNotIn("confirmed", self.sim.events[events:])
        self.assertIn("R press 2", self.sim.cmd("presskey other"), "a fresh confirm")
        c.poll()
        self.assertEqual(c.status()["status"], "pairing_confirmed")
        return c

    def test_a_malformed_key_ends_a_re_setup_prompt(self):
        c = self.ghost_sequence("mobile_pub", "AA")
        a2 = token(600, 91)
        self.assertEqual(c.provision(a2, token(1300, 92))["status"], "auth_ok", "only after the fresh confirm")
        self.assertEqual(self.sim.dump()["access"], a2.encode())

    def test_a_key_without_the_point_prefix_ends_a_re_setup_prompt(self):
        self.ghost_sequence("mobile_pub", b64u(b"\x05" + bytes(64)))
        self.assertEqual(self.sim.dump()["access"], token(600, 1).encode(), "the old set until a new provision")

    def test_a_malformed_nonce_ends_a_re_setup_prompt(self):
        self.ghost_sequence("mobile_nonce", b64u(bytes(15)))

    def test_an_invalid_version_ends_a_re_setup_prompt(self):
        self.ghost_sequence("version", 4)

    def test_a_key_the_key_agreement_refuses_ends_a_re_setup_prompt(self):
        self.ghost_sequence("mobile_pub", self.OFF_CURVE)

    def test_a_malformed_key_ends_a_first_setups_prompt(self):
        self.ghost_sequence("mobile_pub", "AA", resetup=False)

    def test_the_refusal_is_sealed_under_the_ended_session(self):
        # Refused before anything changed: the old session still had its keys, so its client gets the refusal
        # sealed (no plaintext after pairing_ready), then the session ends.
        p = self.refused_takeover(self.prompt(), "mobile_pub", "AA")
        self.assertEqual(p.open(self.refusal)["status"], "error_pairing_invalid_hello")
        self.assertIsNone(p.recv())
        self.assertEqual(self.sim.state()["active"], 0)

    def test_back_ends_a_prompt_while_a_takeover_runs(self):
        # A first setup's prompt; hello B takes it over; a talk press is kept for B (one press confirms a first
        # setup); back while B's handshake runs ends the prompt and the press with it: B is never confirmed.
        p = self.prompt(resetup=False)
        b = Phone(self.sim, 247, t0=p.t)
        b.write(b.hello())
        b.accept_ready(b.recv())
        self.assertIn("R press 2", self.sim.cmd("press"), "kept for B")
        self.assertIn("R dismiss 1", self.sim.cmd("dismiss"), "back ends it, in any phase")
        b.poll()
        self.assertEqual(b.status()["status"], "pairing_confirm_timeout")
        self.assertEqual(self.sim.state()["armed"], 0)
        self.wait_disconnect(b, 100)
        self.assertNotIn("confirmed", self.sim.events)


class AfterAuthOk(Base):
    def test_auth_ok_held_back_keeps_the_core(self):
        # PH_DONE with auth_ok still held back by the stack: another connection's write is busy (it doesn't take the
        # core), and auth_ok goes out once the stack has a buffer.
        self.sim.cmd("multiconn")
        p = self.phone()
        p.pair()
        self.sim.cmd("again 1000")
        p.write(p.seal(json.dumps({"action": "provision_v2", "access_token": token(600, 1),
                                   "refresh_token": token(1300, 2), "token_type": "device"}).encode()))
        self.assertIn("provisioned", self.sim.events)
        self.assertIsNone(p.recv(), "held back")
        self.sim.cmd(f"connect2 {p.t}")
        b = Phone2(self.sim, 247, t0=p.t)
        b.write_raw(json.dumps(b.hello(), separators=(",", ":")).encode(), poll=False)
        self.assertEqual(self.sim.other, ["2 " + b"error_operation_in_progress".hex()])
        self.sim.other.clear()
        self.sim.cmd("again 0")
        p.poll()
        self.assertEqual(p.status(), {"type": "status", "status": "auth_ok"})


class OneProvisionPerSession(Base):
    """After auth_ok the session is consumed; a failed attempt doesn't consume it."""

    def test_second_provision_on_the_same_session_is_refused(self):
        p, _, _, a1, r1 = self.setup_device()
        st = p.provision(token(600, 61), token(1300, 62), IOS_EXTRA)
        self.assertEqual(st, {"type": "status", "status": "error_pairing_confirm_required"})
        p.poll(1000)
        self.assertEqual(self.sim.disconnects, 0)
        d = self.sim.dump()
        self.assertEqual((d["access"], d["refresh"]), (a1.encode(), r1.encode()))
        self.assertEqual(self.sim.state()["seq"], 1)
        self.assertEqual(p.proof(proof_key(a1, NODE)), 1)
        # unpair and wifi_scan on the consumed session: refused the same way.
        p.write(p.seal({"action": "unpair"}))
        self.assertEqual(p.status()["status"], "error_pairing_confirm_required")
        self.assertEqual(self.sim.state()["setup"], 1)
        # A new handshake and press on the same connection: provisions again.
        p2 = Phone(self.sim, 247, t0=p.t)
        p2.pair()
        a2 = token(600, 63)
        self.assertEqual(p2.provision(a2, token(1300, 64))["status"], "auth_ok")
        self.assertEqual(self.sim.dump()["access"], a2.encode())

    def test_after_abort_a_provision_gets_nothing(self):
        p, _, _, a1, _ = self.setup_device()
        p.poll()
        self.assertEqual(self.sim.state()["idle"], 1)
        self.sim.cmd("abort")  # the firmware takes its RAM back
        p.write(p.seal({"action": "provision_v2", "access_token": token(600, 65),
                        "refresh_token": token(1300, 66), "token_type": "device"}))
        self.assertIsNone(p.recv())
        self.wait_disconnect(p, 300)
        self.assertEqual(self.sim.dump()["access"], a1.encode())

    def test_retry_after_refused_fields(self):
        p = self.phone()
        p.pair()
        st = p.provision(token(600, 67), "", {})
        self.assertEqual(st["status"], "error_missing_credentials")
        st = p.provision(token(600, 67), token(1300, 68), {"ssid": "home", "password": "pw"})
        self.assertEqual(st["status"], "error_wifi_unsupported")
        self.assertEqual(self.sim.state()["setup"], 0)
        a = token(600, 69)
        self.assertEqual(p.provision(a, token(1300, 70))["status"], "auth_ok")
        self.assertEqual(self.sim.dump()["access"], a.encode())
        p.poll(1000)
        self.assertEqual(self.sim.disconnects, 0)

    def test_retry_after_a_failed_commit(self):
        _, _, _, a1, _ = self.setup_device()
        self.sim.cmd("disconnect")
        p = self.phone()
        p.pair()
        self.sim.cmd("flashfail 5")
        self.assertEqual(p.provision(token(600, 71), token(1300, 72))["status"], "error_storage")
        self.assertEqual(self.sim.dump()["access"], a1.encode())
        # Retried at once on the same session (the reference ends the
        # connection 500 ms after error_storage): taken, and the success
        # cancels that disconnect.
        a2, r2 = token(600, 73), token(1300, 74)
        self.assertEqual(p.provision(a2, r2)["status"], "auth_ok")
        p.poll(1000)
        self.assertEqual(self.sim.disconnects, 0)
        d = self.sim.dump()
        self.assertEqual((d["access"], d["refresh"]), (a2.encode(), r2.encode()))
        s = self.sim.state()
        self.assertEqual((s["seq"], s["other_blank"]), (2, 1))
        self.assertEqual(p.proof(proof_key(a2, NODE)), 1)
        # And the consumed session refuses a third.
        self.assertEqual(p.provision(token(600, 75), token(1300, 76))["status"],
                         "error_pairing_confirm_required")


class Storage(Base):
    def test_commit_failure_keeps_the_old_set(self):
        p, _, _, a1, r1 = self.setup_device()
        self.sim.cmd("disconnect")
        p2 = self.phone()
        p2.pair()
        self.sim.cmd("flashfail 5")
        self.assertEqual(p2.provision(token(600, 31), token(1300, 32))["status"],
                         "error_storage")
        self.assertIn("storage_failed", self.sim.events)
        self.wait_disconnect(p2, 500)
        d = self.sim.dump()
        self.assertEqual((d["access"], d["refresh"]), (a1.encode(), r1.encode()))
        s = self.sim.state()
        self.assertEqual((s["setup"], s["seq"], s["other_blank"]), (1, 1, 1))
        self.assertEqual(self.phone().proof(proof_key(a1, NODE)), 1)

    def test_commit_failure_on_a_fresh_gadget(self):
        p = self.phone()
        p.pair()
        self.sim.cmd("flashfail 1")
        self.assertEqual(p.provision(token(600, 33), token(1300, 34))["status"],
                         "error_storage")
        self.assertEqual(self.sim.state()["setup"], 0)
        self.assertEqual(self.sim.dump(), {})

    def test_every_fault_position(self):
        _, _, _, a1, r1 = self.setup_device()
        a2, r2 = token(600, 35), token(1300, 36)
        for cut in range(1, 75):
            with self.subTest(cut=cut):
                self.sim.cmd("disconnect")
                self.sim.notes.clear()
                p = self.phone()
                p.pair()
                self.sim.cmd(f"flashfail {cut}")
                st = p.provision(a2, r2)["status"]
                self.sim.cmd("flashfail 0")
                d = self.sim.dump()
                if st == "auth_ok":
                    self.assertEqual(d["access"], a2.encode())
                    a1, r1, a2, r2 = a2, r2, a1, r1  # the next round replaces it again
                else:
                    self.assertEqual(st, "error_storage")
                    self.assertEqual((d["access"], d["refresh"]), (a1.encode(), r1.encode()))
                self.sim.cmd("disconnect")
                self.sim.cmd("reboot")
                d = self.sim.dump()
                self.assertEqual((d["access"], d["refresh"]), (a1.encode(), r1.encode()))
                self.assertEqual(self.sim.state()["other_blank"], 1, "two token sets")

    def test_oversized_provision_is_refused(self):
        p = self.phone(498)
        p.pair()
        st = p.provision(token(600, 41), token(3000, 42), IOS_EXTRA)
        self.assertEqual(st["status"], "error_storage")
        self.wait_disconnect(p, 500)
        self.assertEqual(self.sim.state()["setup"], 0)
        # And the gadget is fine afterwards.
        p, _, conf, a, r = self.setup_device()
        self.assertEqual(self.sim.dump()["access"], a.encode())

    def test_token_over_its_limit(self):
        p = self.phone()
        p.pair()
        self.assertEqual(p.provision(token(1100, 43), token(1000, 44), {})["status"],
                         "error_storage")
        self.assertEqual(self.sim.state()["setup"], 0)

    def test_pair_the_session_cant_hand_back(self):
        # The device session hands the pair back in one credentials document of at most 2,048 B,
        # {"node_id":"..","access_token":"..","refresh_token":"..","access_expires_at":N} (N up to 20 digits, where
        # the board keeps one): with this node_id the pair is at most 1,941 B. One byte over, or a byte that document
        # can't carry, is refused before anything is stored.
        self.setup_device()
        before = self.sim.dump()
        for what, a, r in (("one byte over", token(600, 91), token(1342, 92)),
                           ("a quote", token(600, 93), token(1300, 94) + '"'),
                           ("a control character", token(600, 95), "\x01" + token(1300, 96)),
                           ("a backslash", "\\" + token(600, 97), token(1300, 98))):
            with self.subTest(what):
                self.sim.cmd("disconnect")
                p = self.phone()
                p.pair()
                self.assertEqual(p.provision(a, r)["status"], "error_storage")
                d = self.sim.dump()
                self.assertEqual((d["access"], d["refresh"]), (before["access"], before["refresh"]))
        self.sim.cmd("disconnect")
        p = self.phone()
        p.pair()
        a, r = token(600, 99), token(1341, 100)       # the document exactly 2,048 B with a 20-digit expiry
        self.assertEqual(p.provision(a, r)["status"], "auth_ok")
        self.assertEqual(self.sim.dump()["refresh"], r.encode())

    def test_unpair(self):
        self.setup_device()
        self.sim.cmd("disconnect")
        p = self.phone()
        p.pair()
        p.write(p.seal({"action": "unpair"}))
        self.assertEqual(p.status()["status"], "unpaired")
        self.assertIn("unpaired", self.sim.events)
        self.wait_disconnect(p, 300)
        self.assertEqual(self.sim.state()["setup"], 0)

    def test_proof_clear(self):
        p, _, _, a, _ = self.setup_device()
        self.assertEqual(p.proof(proof_key(a, NODE), clear=True), 1)
        self.assertIn("cleared", self.sim.events)
        self.assertEqual(self.sim.state()["setup"], 0)
        self.assertEqual(p.proof(proof_key(a, NODE)), "not_found")


class Records(Base):
    def paired(self) -> Phone:
        p = self.phone()
        p.pair()
        return p

    def expect_reset(self, p, env):
        p.write(env)
        self.assertIsNone(p.recv(), "a reply to a bad record")
        self.assertEqual(self.sim.state()["active"], 0, "the session survived")
        self.wait_disconnect(p, 300)

    def test_replay(self):
        p = self.paired()
        self.expect_reset(p, p.seal({"action": "pairing_client_finished"}, counter=0))

    def test_counter_skip(self):
        p = self.paired()
        self.expect_reset(p, p.seal({"action": "provision_v2"}, counter=5))

    def test_tag_tampered(self):
        p = self.paired()
        env = p.seal({"action": "wifi_scan"})
        tag = bytearray(unb64u(env["tag"]))
        tag[0] ^= 1
        env["tag"] = b64u(bytes(tag))
        self.expect_reset(p, env)

    def test_ciphertext_tampered(self):
        p = self.paired()
        env = p.seal({"action": "wifi_scan"})
        ct = bytearray(unb64u(env["ciphertext"]))
        ct[-1] ^= 0x40
        env["ciphertext"] = b64u(bytes(ct))
        self.expect_reset(p, env)

    def test_wrong_session(self):
        p = self.paired()
        env = p.seal({"action": "wifi_scan"})
        env["session_id"] = b64u(bytes(16))
        self.expect_reset(p, env)

    def test_second_client_finished(self):
        p = self.paired()
        p.write(p.seal({"action": "pairing_client_finished"}))
        self.assertEqual(p.status()["status"], "error_pairing_decrypt")
        self.wait_disconnect(p, 300)


class Errors(Base):
    def test_plaintext_rules(self):
        p = self.phone()
        p.write({"action": "provision_v2", "access_token": "a", "refresh_token": "r",
                 "token_type": "device"})
        self.assertEqual(p.recv(), "error_encryption_required")
        self.assertEqual(self.sim.cmd("read"), ["R error_encryption_required"])
        p.write({"action": "get_version"})
        self.assertIsNone(p.recv())  # never answered (as the reference)
        p.write({"action": "frobnicate"})
        self.assertIsNone(p.recv())  # error_unknown_action is not a plaintext status
        h = p.hello()
        h["version"] = 4
        p.write(h)
        self.assertEqual(p.recv(), "error_pairing_invalid_hello")
        h = p.hello()
        h["version"] = 5.0  # the integer 5 only (as the compact reference)
        p.write(h)
        self.assertEqual(p.recv(), "error_pairing_invalid_hello")
        h = p.hello()
        h["pairing_policy"] = "confirm_app"
        p.write(h)
        self.assertEqual(p.recv(), "error_pairing_invalid_hello")
        h = p.hello()
        pub = bytearray(unb64u(h["mobile_pub"]))
        pub[-1] ^= 1  # off the curve
        h["mobile_pub"] = b64u(bytes(pub))
        p.write(h)
        self.assertEqual(p.recv(), "error_pairing_invalid_hello")
        p.write_raw(b"{not json")
        self.assertIsNone(p.recv())
        # A good hello, then plaintext is ignored.
        p.write(p.hello())
        p.accept_ready(p.recv())
        p.write({"action": "get_device_info"})
        self.assertIsNone(p.recv())

    def test_encrypted_errors(self):
        p = self.phone()
        p.write(p.hello())
        p.accept_ready(p.recv())
        p.write(p.seal({"action": "pairing_client_finished"}))
        self.assertEqual(p.status()["status"], "confirm_required")
        p.write(p.seal({"action": "provision_v2", "access_token": "a", "refresh_token": "r",
                        "token_type": "device"}))
        self.assertEqual(p.status()["status"], "error_pairing_confirm_required")
        self.assertIn("R press 2", self.sim.cmd("press"))
        p.poll()
        self.assertEqual(p.status()["status"], "pairing_confirmed")
        p.write(p.seal({"action": "wifi_scan"}))
        self.assertEqual(p.status(), {"type": "wifi_scan_result", "networks": []})
        p.write(p.seal({"action": "provision_v2", "access_token": "a", "refresh_token": "r",
                        "token_type": "device", "ssid": "home", "password": "pw"}))
        self.assertEqual(p.status()["status"], "error_wifi_unsupported")
        p.write(p.seal({"action": "provision_v2", "access_token": "a", "token_type": "device"}))
        self.assertEqual(p.status()["status"], "error_missing_credentials")
        p.write(p.seal({"action": "provision_v2", "access_token": "a", "refresh_token": "r",
                        "token_type": "user"}))
        self.assertEqual(p.status()["status"], "error_missing_credentials")
        p.write(p.seal({"action": "get_device_info"}))
        self.assertEqual(p.status()["status"], "error_unknown_action")
        p.write(p.seal(b"{oops"))
        self.assertEqual(p.status()["status"], "error_invalid_command")
        # ssid/password empty or null are token-only.
        self.assertEqual(p.provision("acc", "ref", {"ssid": "", "password": None})["status"],
                         "auth_ok")

    def test_workspace_denied(self):
        p = self.phone()
        self.sim.cmd("deny 1")
        p.write({"action": "get_device_info"})
        self.assertIsNone(p.recv())
        self.assertEqual(p.get_device_info()["node_id"], NODE)

    def test_notification_failure_disconnects(self):
        p = self.phone()
        p.write(p.hello())
        p.accept_ready(p.recv())
        self.sim.cmd("notifyfail 1")
        p.write(p.seal({"action": "pairing_client_finished"}))
        self.assertIsNone(p.recv())
        p.poll()
        self.assertEqual(self.sim.disconnects, 1)
        self.assertEqual(self.sim.state()["ws"], 0)

    def test_hello_restarts(self):
        p = self.phone()
        p.write(p.hello())
        p.accept_ready(p.recv())
        p.write(p.seal({"action": "pairing_client_finished"}))
        self.assertEqual(p.status()["status"], "confirm_required")
        p.poll()
        self.assertTrue(self.sim.state()["armed"])
        p2 = Phone(self.sim, 247, t0=p.t)
        p2.write(p2.hello())  # plaintext hello: still taken mid-session
        p2.accept_ready(p2.recv())
        self.assertTrue(self.sim.state()["armed"], "the prompt stays up: taken over")
        p2.write(p2.seal({"action": "pairing_client_finished"}))
        self.assertEqual(p2.status()["status"], "confirm_required")


class NoSdkToken(Base):
    sim_args = ()

    def test_pairs_without_sdk_token(self):
        p = self.phone()
        conf = p.pair()
        self.assertEqual(conf, {"type": "status", "status": "pairing_confirmed"})
        self.assertIn("no_sdk_token", self.sim.events)

    def test_overlong_sdk_token_is_not_sent(self):
        self.sim.cmd("sdk " + "x" * 97)
        p = self.phone()
        self.assertNotIn("sdk_token", p.pair())
        self.assertIn("no_sdk_token", self.sim.events)


class LongestIdentity(Base):
    sim_args = ("--node", "n" * 32, "--device", "d" * 32, "--mac", "m" * 32,
                "--fw", "f" * 32, "--sdk", "s" * 96)

    def test_worst_case_messages_fit(self):
        for mtu in (23, 247):
            with self.subTest(mtu=mtu):
                p = self.phone(mtu)
                self.assertEqual(p.get_device_info()["node_id"], "n" * 32)
                conf = p.pair()
                self.assertEqual(conf["sdk_token"], "s" * 96)
                a = token(600, 51)
                self.assertEqual(p.provision(a, token(1300, 52), IOS_EXTRA)["status"],
                                 "auth_ok")
                self.assertEqual(self.sim.dump()["k"], proof_key(a, "n" * 32))
                self.sim.cmd("disconnect")


if __name__ == "__main__":
    unittest.main(verbosity=1)
