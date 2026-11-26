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
"""The Muse apps' firmware update against the XIAO's update path (README.md).

Drives the smp_link gadget (Zephyr MCUmgr + src/mg_ota.c + MCUboot's loader on
native_sim, in the colima container) through the Muse apps' update flows as
esp32/components/muse_gadget_ble/smp/conformance/smplink.py models them
(smplink.AndroidFlow and IOSFlow). Run on the Mac with the container and both
relay ends up (README.md, Running it):

    SMP_LINK_EXE=<build dir>/zephyr/zephyr.exe \\
    SMP_LINK_IMAGES=<dir holding the sim/ images>/sim \\
    python3 -m unittest zephyr/tests/smp_link/e2e.py -v
"""
import hashlib
import json
import os
import subprocess
import sys
import time
import unittest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "esp32", "components", "muse_gadget_ble", "smp", "conformance"))
import smplink as sl  # noqa: E402

IMAGES = os.path.expanduser(os.environ.get("SMP_LINK_IMAGES", ""))
# SMP_LINK_CONTAINER= (empty) runs the gadget on this host (Linux x86-64): no
# container, no relays; its runs go under SMP_LINK_RUN_ROOT.
CONTAINER = os.environ.get("SMP_LINK_CONTAINER", "mg-smp-link")
# The gadget's build (README.md: a build directory made with mktemp -d, never
# a fixed path under /tmp).
EXE = os.environ.get("SMP_LINK_EXE", "")
# The gadget's port (a concurrent run takes another, with its own relays) and
# this run's own directory in the container.
PORT = int(os.environ.get("SMP_LINK_PORT", "47000"))
RUN_ROOT = os.environ.get("SMP_LINK_RUN_ROOT", "/sim") if CONTAINER else os.environ["SMP_LINK_RUN_ROOT"]
RUN_DIR = f"{RUN_ROOT}/e2e-{PORT}-{os.getpid()}"
HEALTHY_MS = 3000
DOCKER = ["docker", "--context", os.environ.get("SMP_LINK_DOCKER_CONTEXT", "colima-zephyr"), "exec", CONTAINER] if CONTAINER else []
RUN_PY = "python3 /work/zephyr/tests/smp_link/run.py" if CONTAINER else f"{sys.executable} {REPO}/zephyr/tests/smp_link/run.py"
SIM_IMAGES = "/simimg/sim" if CONTAINER else IMAGES
PREFIX = os.environ.get("SMP_LINK_IMAGE_PREFIX", "xn54")


def sh(cmd, check=True):
    return subprocess.run(DOCKER + ["bash", "-c", cmd], check=check, capture_output=True, text=True).stdout


def image(name):
    with open(os.path.join(IMAGES, f"{PREFIX}-{name}.bin"), "rb") as f:
        return f.read()


class Gadget:
    """The gadget process in the container, restarted on a fresh flash. Only the
    process group this run started is ever stopped."""

    pgid = None

    def stop(self):
        if self.pgid:
            sh(f"kill -TERM -- -{self.pgid} 2>/dev/null; sleep 0.5", check=False)
            self.pgid = None

    def start(self, init, *args):
        for attempt in (1, 2):
            self.stop()
            argv = " ".join(args)
            # Only the supervisor goes to the background (not the whole && list,
            # whose subshell would hold the output pipe open): $! is its process group.
            out = sh(f"mkdir -p {RUN_DIR} && rm -f {RUN_DIR}/flash.bin.next-args && "
                     f"{{ setsid {RUN_PY} serve --exe {EXE} "
                     f"--flash {RUN_DIR}/flash.bin --init {SIM_IMAGES}/{PREFIX}-{init}.bin --port {PORT} "
                     f"-- --healthy-ms={HEALTHY_MS} {argv} > {RUN_DIR}/serve.log 2>&1 < /dev/null & echo $!; }}")
            self.pgid = int(out.split()[-1])
            try:
                self.wait_log("SIM: up", 60)
                return
            except AssertionError:
                # Under x86 emulation the supervisor itself now and then dies before it
                # writes a line (an empty log): start it once more. Anything the gadget
                # logged is a real failure.
                if attempt == 2 or self.log().strip():
                    raise

    def next_boot(self, args):
        sh(f"echo '{args}' > {RUN_DIR}/flash.bin.next-args")

    def log(self):
        return sh(f"cat {RUN_DIR}/serve.log", check=False)

    def wait_log(self, text, timeout, count=1):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.log().count(text) >= count:
                return True
            time.sleep(0.3)
        raise AssertionError(f"no '{text}' x{count} in the gadget log:\n{self.log()[-3000:]}")


class Base(unittest.TestCase):
    def setUp(self):
        if not IMAGES:
            self.skipTest("set SMP_LINK_IMAGES to the directory holding the test images (README.md)")
        if not EXE:
            self.skipTest("set SMP_LINK_EXE to the gadget's zephyr.exe (README.md)")
        self.g = Gadget()
        self.addCleanup(self.g.stop)

    def link(self, mtu=247):
        lk = sl.Link(PORT, mtu)
        self.addCleanup(lk.close)
        self.assertTrue(lk.connect(30), "no ready")
        return lk

    def status(self, lk):
        return lk.status()

    def wait_status(self, lk, key, value, timeout):
        deadline = time.time() + timeout
        st = None
        while time.time() < deadline:
            st = lk.status()
            if st.get(key) == value:
                return st
            time.sleep(0.3)
        raise AssertionError(f"status {key} never became {value}: {st}")

    def kept_after_reset(self, lk, version):
        """A reset now (an SMP reset after healthy) must boot the same image."""
        c = sl.Client(lk)
        lk.disconnected.clear()
        c.reset()
        self.assertTrue(lk.disconnected.wait(10), "no disconnect after the reset")
        self.assertTrue(lk.connect(60), "no reconnect")
        st = lk.status()
        self.assertEqual(st["running"], version)
        self.assertTrue(st["confirmed"])
        return st


class Update(Base):
    """A picked-file update from both apps."""

    def run_flow(self, flow, init, new, mtu=247):
        self.g.start(init)
        lk = self.link(mtu)
        img = image(new)
        f = (sl.AndroidFlow if flow == "android" else sl.IOSFlow)(lk, img)
        r = f.run()
        self.assertTrue(r.ok, f"{flow}: {r.failure} {r.log}")
        self.assertLess(r.reconnect_s, 60)
        ver = sl.image_version(img)
        st = lk.status()
        self.assertEqual(st["running"], ver)
        # The app's confirm, sent within seconds of the reset, waits for
        # the healthy mark: until then the image is still on test.
        if not st["healthy"]:
            self.assertFalse(st["confirmed"], "confirmed before healthy")
        self.wait_status(lk, "confirmed", True, HEALTHY_MS / 1000 + 10)
        self.assertIn("confirm latched until the boot is healthy", self.g.log())
        self.kept_after_reset(lk, ver)
        print(f"\n  {flow} MTU {mtu}: upload {r.upload_s:.1f} s, reset to reconnect {r.reconnect_s:.1f} s")
        return r

    def test_android(self):
        self.run_flow("android", "A", "B")

    def test_ios(self):
        self.run_flow("ios", "A", "B")

    def test_android_mtu_23(self):
        self.run_flow("android", "A", "C", mtu=23)

    def test_android_mtu_517(self):
        """MTU 517 (the S22): no write may pass 512 bytes (Android refuses longer; so does the link)."""
        self.g.start("A")
        lk = self.link(517)
        self.assertLessEqual(sl.Client(lk).params()["buf_size"], 512)
        lk.close()
        self.run_flow("android", "A", "B", mtu=517)
        self.assertNotIn("refused", self.g.log())


class Resume(Base):
    """An interrupted upload resumes from the reported offset."""

    def resume(self, flow):
        self.g.start("A")
        lk = self.link()
        img = image("B")
        cls = sl.AndroidFlow if flow == "android" else sl.IOSFlow
        f = cls(lk, img, interrupt_at=len(img) // 2)
        with self.assertRaises(InterruptedError):
            f.upload(sl.Result()) if flow == "android" else f.upload_once(sl.Result(), 512)
        lk.drop_link()
        self.assertTrue(lk.connect(10))
        r = cls(lk, img).run()
        self.assertTrue(r.ok, r.failure)
        self.assertIsNotNone(r.resumed_at, "the second upload started over")
        self.assertGreaterEqual(r.resumed_at, len(img) // 2)
        self.assertEqual(lk.status()["running"], sl.image_version(img))
        print(f"\n  {flow}: resumed at {r.resumed_at} of {len(img)}")

    def test_android(self):
        self.resume("android")

    def test_ios(self):
        self.resume("ios")

    def upload_frame(self, c, img):
        """Upload chunk 0 as one request within the 512-byte buffer: two writes at MTU 247."""
        import hashlib
        return c.frame(2, 1, 1, {"off": 0, "len": len(img), "sha": hashlib.sha256(img).digest(),
                                 "data": img[:400]})

    def test_stale_partial_dropped_after_gap(self):
        """A request's first write, then nothing for longer than the gap (CONFIG_MG_SMP_RX_GAP_MS,
        2 s): the next request starts afresh instead of being read as the rest of the old one."""
        self.g.start("A")
        lk = self.link()
        c = sl.Client(lk)
        _, msg = self.upload_frame(c, image("B"))
        lk.write(msg[:lk.mtu - 3])  # only the first of its writes
        time.sleep(2.5)
        slots = c.image_state()     # answered: the stale partial was dropped
        self.assertTrue(slots[0]["active"])
        self.assertIn("SMP: dropped a partly received request", self.g.log())

class Refused(Base):
    """A corrupted payload is refused before anything is committed."""

    def test_corrupt_payload(self):
        self.g.start("A")
        lk = self.link()
        bad = image("B-corrupt")
        r = sl.AndroidFlow(lk, bad).run()
        self.assertFalse(r.ok)
        self.assertTrue(r.failure.startswith("OLD_IMAGE_RUNNING"), r.failure)
        st = lk.status()
        self.assertEqual(st["running"], sl.image_version(image("A")))
        self.assertTrue(st["confirmed"])
        self.assertEqual(st["swap"], "none")
        self.assertIn("Image in the secondary slot is not valid", self.g.log())


class Gate(Base):
    """On a release build (--release), on a set-up gadget, an update write over Bluetooth
    without the token proof is answered busy (rc 10) while the gadget asks for one button
    press; reads, echo and SAFE mode asked with the button stay open, and a SAFE the gadget
    entered itself refuses (rc 11), set up or not. A gadget that holds no device token asks
    the same. An automation build takes it with no press."""

    def test_automation_needs_no_press(self):
        self.g.start("A", "--set-up")
        lk = self.link()
        c = sl.Client(lk)
        self.assertEqual(c.rc(self.upload_first_chunk(c)), 0)

    def test_automation_not_set_up_needs_no_press(self):
        self.g.start("A")
        lk = self.link()
        c = sl.Client(lk)
        self.assertEqual(c.rc(self.upload_first_chunk(c)), 0)

    EBUSY = 10  # MGMT_ERR_EBUSY: the press prompt
    EACCESSDENIED = 11  # MGMT_ERR_EACCESSDENIED: refused, no prompt

    def upload_first_chunk(self, c):
        img = image("B")
        sha = __import__("hashlib").sha256(img).digest()
        return c.request(2, 1, 1, {"off": 0, "len": len(img), "sha": sha, "data": img[:64]})

    def test_no_token_needs_the_press(self):
        """A release build that holds no device token (not set up, or after Forget) asks an
        unproven update for the same one press: rc 10, an injected press doesn't confirm,
        a real one does (then rc 0)."""
        self.g.start("A", "--release")
        lk = self.link()
        c = sl.Client(lk)
        self.assertEqual(c.rc(self.upload_first_chunk(c)), self.EBUSY)
        self.assertIn("refused: no token proof: waiting for one button press", self.g.log())
        lk.sock.sendto(b"\x42", lk.addr)  # link.c T_INJECTED
        time.sleep(0.3)
        self.assertIn("an injected press: not a confirm", self.g.log())
        self.assertEqual(c.rc(self.upload_first_chunk(c)), self.EBUSY)
        self.press(lk)
        self.assertIn("a press: confirmed the update", self.g.log())
        self.assertEqual(c.rc(self.upload_first_chunk(c)), 0)

    def test_unproven_refused(self):
        self.g.start("A", "--set-up", "--release")
        lk = self.link()
        c = sl.Client(lk)
        self.assertTrue(c.image_state())  # reads stay open
        self.assertEqual(c.rc(c.echo("x")), 0)
        self.assertEqual(c.rc(self.upload_first_chunk(c)), self.EBUSY)
        with self.assertRaises(sl.SmpError) as e:
            c.confirm()
        self.assertEqual(e.exception.rc, self.EBUSY)
        self.assertEqual(c.rc(c.request(2, 0, 5, {})), self.EBUSY)  # reset
        self.assertIn("refused: no token proof", self.g.log())
        st = lk.status()
        self.assertEqual(st["running"], sl.image_version(image("A")))
        self.assertEqual(st["swap"], "none")

    def test_deep_request_dropped(self):
        """An unproven echo nesting ~500 arrays crashed the SMP work queue's stack in zcbor's skip;
        smp.c drops a request deeper than 8 before decoding and the gadget goes on."""
        self.g.start("A", "--set-up", "--release")
        lk = self.link()
        c = sl.Client(lk)
        # The echo as an attacker sends it (509 B with its header): {"d": "x", "z": 493 nested
        # arrays}; os_mgmt's map decode skips the unknown "z" with zcbor's recursive skip.
        body = b"\xa2\x61d\x61x\x61z" + b"\x81" * 493 + b"\x00"
        self.assertLessEqual(8 + len(body), 512)
        seq = c.send(2, 0, 0, body)
        with self.assertRaises(sl.SmpError):
            c.wait(seq, 2.0)  # dropped: no handler ran, nothing answers it
        self.assertEqual(c.rc(c.echo("x")), 0)  # still up, the same boot
        self.assertIn("nesting deeper than 8: dropped", self.g.log())
        self.assertEqual(self.g.log().count("SIM: up"), 1)

    def test_long_length_deep_echo_dropped(self):
        """The depth gate fails closed: {"d": "x", "z": <text whose 8-byte length is 2^32>,
        "y": 480 nested arrays}. On a 32-bit target zcbor casts that length to 0, skips nothing
        and recurses through "y"; a gate that left a length past the buffer to the handler let
        it through. smp.c refuses any length of 2^32 or more and drops it; the gadget goes on."""
        self.g.start("A", "--set-up", "--release")
        lk = self.link()
        c = sl.Client(lk)
        body = (b"\xa3\x61d\x61x\x61z\x7b\x00\x00\x00\x01\x00\x00\x00\x00\x61y"
                + b"\x81" * 480 + b"\x00")
        self.assertLessEqual(8 + len(body), 512)
        seq = c.send(2, 0, 0, body)
        with self.assertRaises(sl.SmpError):
            c.wait(seq, 2.0)  # dropped: no handler ran, nothing answers it
        self.assertEqual(c.rc(c.echo("x")), 0)  # still up, the same boot
        self.assertIn("not whole CBOR or nesting deeper than 8: dropped", self.g.log())
        self.assertEqual(self.g.log().count("SIM: up"), 1)

    def test_proven_updates(self):
        self.g.start("A", "--set-up", "--proven", "--release")
        lk = self.link()
        img = image("B")
        r = sl.AndroidFlow(lk, img).run()
        self.assertTrue(r.ok, f"{r.failure} {r.log}")
        self.assertEqual(lk.status()["running"], sl.image_version(img))
        self.assertNotIn("refused: no token proof", self.g.log())

    def test_safe_from_failed_boots_refuses(self):
        # A SAFE the gadget entered by itself has no way to prompt: rc 11 (EACCESSDENIED), never
        # asked; reads stay open.
        self.g.start("A", "--set-up", "--safe", "--release")
        lk = self.link()
        c = sl.Client(lk)
        self.assertTrue(c.image_state())
        self.assertEqual(c.rc(self.upload_first_chunk(c)), self.EACCESSDENIED)
        self.assertIn("has no way to prompt", self.g.log())
        self.assertNotIn("waiting for one button press", self.g.log())

    def test_safe_from_failed_boots_refuses_not_set_up(self):
        # The same on a gadget that holds no device token: rc 11, never asked.
        self.g.start("A", "--safe", "--release")
        lk = self.link()
        c = sl.Client(lk)
        self.assertTrue(c.image_state())
        self.assertEqual(c.rc(self.upload_first_chunk(c)), self.EACCESSDENIED)
        self.assertIn("has no way to prompt", self.g.log())

    def test_safe_by_button_open(self):
        self.g.start("A", "--set-up", "--safe", "--safe-button", "--release")
        lk = self.link()
        c = sl.Client(lk)
        self.assertEqual(c.rc(self.upload_first_chunk(c)), 0)
        self.assertNotIn("refused:", self.g.log())

    def press(self, lk):
        lk.sock.sendto(b"\x41", lk.addr)  # link.c T_PRESS: a real press
        time.sleep(0.3)

    def raw_upload(self, c, data, claim_len, claim_sha, step=256):
        """An upload of `data` that names claim_len / claim_sha in its first request."""
        off = 0
        while off < len(data):
            body = {"off": off, "data": data[off:off + step]}
            if off == 0:
                body.update({"len": claim_len, "sha": claim_sha})
            rc = c.rc(c.request(2, 1, 1, body))
            if rc:
                return rc
            off += step
        return 0

    def test_a_press_admits_its_upload(self):
        self.g.start("A", "--set-up", "--release")
        lk = self.link()
        c = sl.Client(lk)
        img = image("B")
        self.assertEqual(c.rc(self.upload_first_chunk(c)), self.EBUSY)
        self.press(lk)
        self.assertIn("a press: confirmed the update", self.g.log())
        r = sl.Result()
        sl.AndroidFlow(lk, img).upload(r)
        self.assertFalse([x for x in r.rcs if x], f"{r.rcs} {r.log}")
        self.assertEqual(c.rc(c.request(2, 1, 0, {"hash": sl.image_hash(img), "confirm": False})), 0)
        self.assertEqual(lk.status()["swap"], "test")

    def test_other_bytes_under_the_binding_refused(self):
        """The press binds to the upload's claimed length and
        SHA-256; on a new link another image's bytes under that claim never get staged."""
        self.g.start("A", "--set-up", "--release")
        lk = self.link()
        c = sl.Client(lk)
        img = image("B")
        bad = image("B-corrupt")
        self.assertEqual(len(bad), len(img))
        self.assertEqual(c.rc(self.upload_first_chunk(c)), self.EBUSY)
        self.press(lk)
        r = sl.Result()
        sl.AndroidFlow(lk, img).upload(r)
        self.assertFalse([x for x in r.rcs if x], f"{r.rcs} {r.log}")
        lk.close()
        lk2 = self.link()
        c2 = sl.Client(lk2)
        sha = __import__("hashlib").sha256(img).digest()
        # The claim matches, so the upload resumes with no prompt; its last chunk is hashed
        # with what is already in slot1: not the bound image, rc 11, and the binding ends.
        self.assertEqual(self.raw_upload(c2, bad, len(img), sha), 11)
        self.assertIn("slot1 doesn't hold the image the press was bound to", self.g.log())
        self.assertEqual(
            c2.rc(c2.request(2, 1, 0, {"hash": sl.image_hash(bad), "confirm": False})), 10)
        self.assertEqual(lk2.status()["swap"], "none")

    def small_upload(self, c, n=1024):
        """The first n bytes of image B as an image of its own (len n, its own SHA-256)."""
        img = image("B")[:n]
        return img, hashlib.sha256(img).digest()

    def test_an_upload_without_its_sha_refused(self):
        """Without the proof an upload's first chunk must name its SHA-256: one that names
        none gets rc 11 and no prompt, and so does a later chunk with no upload started on
        its link; the one that names it then prompts."""
        self.g.start("A", "--set-up", "--release")
        lk = self.link()
        c = sl.Client(lk)
        img, sha = self.small_upload(c)
        self.assertEqual(c.rc(c.request(2, 1, 1, {"off": 0, "len": len(img), "data": img[:256]})),
                         11)
        self.assertIn("refused: an upload that names no SHA-256", self.g.log())
        self.assertEqual(c.rc(c.request(2, 1, 1, {"off": 256, "data": img[256:512]})), 11)
        self.assertIn("refused: a chunk with no upload started on this link", self.g.log())
        self.assertNotIn("firmware update asked without the token proof", self.g.log())
        first = {"off": 0, "len": len(img), "sha": sha, "data": img[:256]}
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 10)
        self.assertEqual(self.g.log().count("firmware update asked without the token proof"), 1)

    def test_a_write_past_the_bound_length_refused(self):
        """A press binds to the upload's length: a chunk that writes past it gets rc 11 and
        ends the binding, so the upload's next chunk is refused too (no upload on the link)."""
        self.g.start("A", "--set-up", "--release")
        lk = self.link()
        c = sl.Client(lk)
        img, sha = self.small_upload(c)
        first = {"off": 0, "len": len(img), "sha": sha, "data": img[:256]}
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 10)
        self.press(lk)
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 0)
        for off in (256, 512):
            self.assertEqual(c.rc(c.request(2, 1, 1, {"off": off, "data": img[off:off + 256]})), 0)
        self.assertEqual(c.rc(c.request(2, 1, 1, {"off": 768, "data": img[768:] + bytes(8)})), 11)
        self.assertIn("refused: a write past the bound length", self.g.log())
        self.assertEqual(c.rc(c.request(2, 1, 1, {"off": 768, "data": img[768:]})), 11)
        self.assertEqual(lk.status()["swap"], "none")

    def test_the_last_chunk_is_hashed(self):
        """Bytes that aren't the bound image are refused at the last chunk (rc 11), on the
        press's own link, and the binding ends: the state write asks again."""
        self.g.start("A", "--set-up", "--release")
        lk = self.link()
        c = sl.Client(lk)
        img, sha = self.small_upload(c)
        forged = bytes(b ^ 0x5A for b in img[768:])
        first = {"off": 0, "len": len(img), "sha": sha, "data": img[:256]}
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 10)
        self.press(lk)
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 0)
        for off in (256, 512):
            self.assertEqual(c.rc(c.request(2, 1, 1, {"off": off, "data": img[off:off + 256]})), 0)
        self.assertEqual(c.rc(c.request(2, 1, 1, {"off": 768, "data": forged})), 11)
        self.assertIn("refused: slot1 doesn't hold the image the press was bound to", self.g.log())
        self.assertEqual(c.rc(c.request(2, 1, 0, {"hash": sha, "confirm": False})), 10)
        self.assertEqual(lk.status()["swap"], "none")

    def forget(self, lk):
        lk.sock.sendto(b"\x43", lk.addr)  # link.c T_FORGET
        time.sleep(0.3)

    def test_forget_ends_the_binding(self):
        """Forget ends the press's binding: the upload's next chunk gets rc 11 (no upload on
        the link), and the same upload asks again, on its link and on a new one."""
        self.g.start("A", "--set-up", "--release")
        lk = self.link()
        c = sl.Client(lk)
        img, sha = self.small_upload(c)
        first = {"off": 0, "len": len(img), "sha": sha, "data": img[:256]}
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 10)
        self.press(lk)
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 0)
        self.forget(lk)
        self.assertEqual(c.rc(c.request(2, 1, 1, {"off": 256, "data": img[256:512]})), 11)
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 10)  # within 10 s of the last prompt
        lk.close()
        lk2 = self.link()
        c2 = sl.Client(lk2)
        n = self.g.log().count("firmware update asked without the token proof")
        self.assertEqual(c2.rc(c2.request(2, 1, 1, first)), 10)
        self.assertEqual(self.g.log().count("firmware update asked without the token proof"), n + 1)

    def test_forget_ends_the_proof(self):
        """A proven link updates with no press until Forget; after it the same link asks for
        the press, its transfer under way included (the proof's carry-on ends too)."""
        self.g.start("A", "--set-up", "--proven", "--release")
        lk = self.link()
        c = sl.Client(lk)
        img, sha = self.small_upload(c)
        first = {"off": 0, "len": len(img), "sha": sha, "data": img[:256]}
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 0)
        self.forget(lk)
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 10)
        self.assertIn("refused: no token proof: waiting for one button press", self.g.log())
        self.assertEqual(c.rc(c.request(2, 1, 1, {"off": 256, "data": img[256:512]})), 11)


class Owner(Base):
    """An image staged for the next boot before a setup is dropped at the
    setup's commit (an ownership change), after a completed upload too; an
    upload's transfer carries nothing over it."""

    def setup_committed(self, lk):
        lk.sock.sendto(b"\x40", lk.addr)  # link.c T_SETUP_COMMITTED
        time.sleep(0.3)

    def stage(self, lk, img):
        sl.AndroidFlow(lk, img).upload(sl.Result())
        c = sl.Client(lk)
        self.assertEqual(c.rc(c.request(2, 1, 0, {"hash": sl.image_hash(img), "confirm": False})), 0)
        return c

    def test_completed_upload_marked_test_is_dropped_at_setup(self):
        self.g.start("A", "--set-up", "--release", "--healthy-ms=1000")
        lk = self.link()
        time.sleep(1.5)
        lk.auth(True)
        self.stage(lk, image("B"))
        self.assertEqual(lk.status()["swap"], "test")
        self.setup_committed(lk)
        self.assertIn("an image staged before this setup is dropped", self.g.log())
        self.assertEqual(lk.status()["swap"], "none")

    def test_carry_on_ends_at_an_ownership_change(self):
        self.g.start("A", "--set-up", "--release", "--healthy-ms=1000")
        lk = self.link()
        time.sleep(1.5)
        lk.auth(True)
        img = image("B")
        sl.AndroidFlow(lk, img).upload(sl.Result())
        lk.auth(False)          # the proof lapses: the carry-on would allow the state write
        self.setup_committed(lk)
        c = sl.Client(lk)
        self.assertEqual(c.rc(c.request(2, 1, 0, {"hash": sl.image_hash(img), "confirm": False})),
                         10)  # MGMT_ERR_EBUSY: the press prompt
        self.assertEqual(lk.status()["swap"], "none")

    def test_a_press_admission_ends_at_an_ownership_change(self):
        """An upload a press admitted before a setup carries
        nothing over it: its next chunk is refused and it asks again."""
        self.g.start("A", "--set-up", "--release")
        lk = self.link()
        c = sl.Client(lk)
        img = image("B")
        sha = __import__("hashlib").sha256(img).digest()
        first = {"off": 0, "len": len(img), "sha": sha, "data": img[:64]}
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 10)
        lk.sock.sendto(b"\x41", lk.addr)  # link.c T_PRESS
        time.sleep(0.3)
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 0)
        self.setup_committed(lk)
        # rc 11: the binding ended, so no upload is started on this link (mg-update-v2).
        self.assertEqual(c.rc(c.request(2, 1, 1, {"off": 64, "data": img[64:128]})), 11)
        self.assertEqual(c.rc(c.request(2, 1, 1, first)), 10)


class Truth(Base):
    """An image that never gets healthy reverts; one that does is kept."""

    def test_never_healthy_reverts(self):
        """The app sees the new image and confirms it, but the boot never gets healthy (a
        watchdog reset): the confirm never takes effect and MCUboot swaps the old one back."""
        self.g.start("A")
        lk = self.link()
        self.g.next_boot("--crash-ms=6000")
        r = sl.AndroidFlow(lk, image("B")).run()
        self.assertTrue(r.ok, r.failure)   # the app saw B running and confirmed it
        self.g.wait_log("SIM: crash before healthy", 30)
        lk.drop_link()  # a crash sends no disconnect: the phone notices by the supervision timeout
        self.assertTrue(lk.connect(60))
        st = lk.status()
        self.assertEqual(st["running"], sl.image_version(image("A")), "a never-healthy image was kept")
        self.assertTrue(st["confirmed"])

    def test_kept_after_a_link_without_a_confirm(self):
        """Upload, test, reset, and no client until the boot is healthy: the image stays on
        test (no link has proved ATT works); a link that confirms nothing then keeps it."""
        self.g.start("A")
        lk = self.link()
        img = image("B")
        f = sl.AndroidFlow(lk, img)
        f.upload(sl.Result())
        f.c.test(sl.image_hash(img))
        lk.disconnected.clear()
        f.c.reset()
        self.assertTrue(lk.disconnected.wait(10))
        lk.close()
        self.g.wait_log("the image keeps itself after a link proves ATT works", 60)
        time.sleep(1)
        self.assertNotIn("image smp_link confirmed (healthy;", self.g.log())
        lk2 = self.link()
        self.g.wait_log("image smp_link confirmed (healthy;", 30)
        st = lk2.status()
        self.assertEqual(st["running"], sl.image_version(img))
        self.assertTrue(st["confirmed"])


class Memory(Base):
    """Update support takes no base memory. SMP's buffers, work queue and image writer
    come from the malloc arena only while SMP is in use (status smp_blocks: blocks out now)."""

    IDLE_S = 2.0 + 1.0     # CONFIG_MG_SMP_SESSION_IDLE_MS + margin

    def blocks_after(self, lk, wait_s):
        deadline = time.time() + wait_s + 5
        n = None
        time.sleep(wait_s)
        while time.time() < deadline:
            n = lk.status()["smp_blocks"]
            if n == 0:
                return 0
            time.sleep(0.5)
        return n

    def test_nothing_held_after_requests(self):
        """Image list and params: the work queue and buffers exist for the requests, then go."""
        self.g.start("A")
        lk = self.link()
        c = sl.Client(lk)
        c.params()
        c.image_state()
        self.assertEqual(self.blocks_after(lk, self.IDLE_S), 0)

    def test_nothing_held_after_upload_and_test(self):
        self.g.start("A")
        lk = self.link()
        img = image("B")
        f = sl.AndroidFlow(lk, img)
        f.upload(sl.Result())
        f.c.test(sl.image_hash(img))
        self.assertEqual(self.blocks_after(lk, self.IDLE_S), 0)

    def test_stopped_upload_released_then_resumed(self):
        """A dropped link mid-upload: at the session's end the image writer is freed (its
        buffered bytes dropped), and the upload resumes from what is on flash."""
        self.g.start("A")
        lk = self.link()
        img = image("B")
        f = sl.AndroidFlow(lk, img, interrupt_at=len(img) // 2)
        with self.assertRaises(InterruptedError):
            f.upload(sl.Result())
        lk.drop_link()
        self.assertTrue(lk.connect(10))
        self.assertEqual(self.blocks_after(lk, self.IDLE_S), 0)
        self.assertIn("firmware upload idle: writer freed", self.g.log())
        r = sl.AndroidFlow(lk, img).run()
        self.assertTrue(r.ok, r.failure)
        self.assertIsNotNone(r.resumed_at, "the upload started over")
        block = 4096   # CONFIG_IMG_BLOCK_BUF_SIZE
        self.assertEqual(r.resumed_at % block, 0, r.resumed_at)
        self.assertGreater(r.resumed_at, len(img) // 2 - block - 2475)
        self.assertEqual(lk.status()["running"], sl.image_version(img))
        print(f"\n  released and resumed at {r.resumed_at} of {len(img)}")

    def test_stopped_in_first_block_starts_over(self):
        """A dropped link inside the first 4 KiB leaves nothing on flash: after the release
        the retry starts over and finishes (it once got off 0 back forever, nothing written)."""
        self.g.start("A")
        lk = self.link()
        img = image("B")
        f = sl.AndroidFlow(lk, img, interrupt_at=1024)
        with self.assertRaises(InterruptedError):
            f.upload(sl.Result())
        lk.drop_link()
        self.assertTrue(lk.connect(10))
        self.assertEqual(self.blocks_after(lk, self.IDLE_S), 0)
        r = sl.AndroidFlow(lk, img).run()
        self.assertTrue(r.ok, r.failure)
        self.assertEqual(lk.status()["running"], sl.image_version(img))

    def test_ios_resumes_after_release(self):
        """iOS after a release: its retry at off 0 with the same sha gets the rewound offset."""
        self.g.start("A")
        lk = self.link()
        img = image("B")
        f = sl.IOSFlow(lk, img, interrupt_at=len(img) // 2)
        with self.assertRaises(InterruptedError):
            f.upload_once(sl.Result(), 512)
        lk.drop_link()
        self.assertTrue(lk.connect(10))
        self.assertEqual(self.blocks_after(lk, self.IDLE_S), 0)
        r = sl.IOSFlow(lk, img).run()
        self.assertTrue(r.ok, r.failure)
        self.assertIsNotNone(r.resumed_at)


class CarryOn(Base):
    """The update gate's carry-on: once a connection's upload passed the proof
    check, the rest of that transfer finishes on that connection even if the
    proof lapses (the client challenges again): its chunks, a retry from 0 with
    the same len and sha, then the state write and the reset once the last
    chunk is in. A new upload, an erase, or anything on another connection is
    asked again; a link drop ends it. The proof is the link's auth message
    (0x09, lk.auth): each connection starts unproven here (--set-up, no --proven)."""

    EBUSY = 10  # MGMT_ERR_EBUSY: the press prompt (no press in these runs)

    def start(self):
        self.g.start("A", "--set-up", "--release", "--healthy-ms=120000")
        return self.link()

    def first_chunk(self, c, img):
        return c.request(2, 1, 1, {"off": 0, "len": len(img), "sha": hashlib.sha256(img).digest(),
                                   "data": img[:64]})

    def test_the_transfer_finishes_after_the_proof_lapses(self):
        lk = self.start()
        lk.auth(True)
        img = image("B")
        half = len(img) // 2
        lapsed = []

        def lapse(off):
            if off >= half and not lapsed:
                lapsed.append(off)
                lk.auth(False)   # a new challenge: the proof is gone, the link stays

        f = sl.AndroidFlow(lk, img, on_progress=lapse)
        f.upload(sl.Result())
        self.assertTrue(lapsed, "the proof never lapsed mid-upload")
        c = sl.Client(lk)
        self.assertEqual(c.rc(c.request(2, 1, 0, {"hash": sl.image_hash(img), "confirm": False})), 0)
        self.assertIn("finishing the transfer this connection started", self.g.log())
        lk.disconnected.clear()
        c.reset()
        self.assertTrue(lk.disconnected.wait(10), "the reset after the commit didn't run")
        self.assertTrue(lk.connect(60), "no reconnect")
        self.assertEqual(lk.status()["running"], sl.image_version(img))
        # After the reboot a new connection: the confirm needs a fresh proof.
        c = sl.Client(lk)
        with self.assertRaises(sl.SmpError) as e:
            c.confirm()
        self.assertEqual(e.exception.rc, self.EBUSY)
        lk.auth(True)
        c.confirm()   # before healthy: latched, applied at the healthy mark (mg_ota_policy.h)
        self.assertIn("confirm latched until the boot is healthy", self.g.log())

    def test_a_new_upload_and_an_erase_need_the_proof(self):
        lk = self.start()
        lk.auth(True)
        b = image("B")
        sl.AndroidFlow(lk, b).upload(sl.Result())
        lk.auth(False)
        c = sl.Client(lk)
        self.assertEqual(c.rc(self.first_chunk(c, image("C"))), self.EBUSY)
        self.assertIn("refused: no token proof: waiting for one button press", self.g.log())
        self.assertEqual(c.rc(c.request(2, 1, 5, {})), self.EBUSY)   # erase
        st = lk.status()
        self.assertEqual(st["staged_version"], sl.image_version(b), st)
        self.assertEqual(st["swap"], "none", st)

    def test_a_retry_from_zero_continues(self):
        lk = self.start()
        lk.auth(True)
        img = image("B")
        f = sl.AndroidFlow(lk, img, interrupt_at=len(img) // 2)
        with self.assertRaises(InterruptedError):
            f.upload(sl.Result())
        lk.auth(False)
        c = sl.Client(lk)
        rsp = self.first_chunk(c, img)   # iOS restarts at 0 with the same len and sha
        self.assertEqual(c.rc(rsp), 0, rsp)
        r = sl.Result()
        sl.AndroidFlow(lk, img).upload(r)
        self.assertEqual(lk.status()["staged_version"], sl.image_version(img))

    def test_a_link_drop_ends_the_allowance(self):
        """Proven on the first connection, cut at half; the resume on a new,
        unproven connection is refused with nothing changed; proven, it resumes."""
        lk = self.start()
        lk.auth(True)
        img = image("B")
        f = sl.AndroidFlow(lk, img, interrupt_at=len(img) // 2)
        with self.assertRaises(InterruptedError):
            f.upload(sl.Result())
        lk.drop_link()
        self.assertTrue(lk.connect(30))
        with self.assertRaises(sl.SmpError) as e:
            sl.AndroidFlow(lk, img).upload(sl.Result())
        self.assertEqual(e.exception.rc, self.EBUSY)
        c = sl.Client(lk)
        self.assertEqual(c.rc(c.request(2, 1, 0, {"hash": sl.image_hash(img), "confirm": False})),
                         self.EBUSY)
        self.assertEqual(c.rc(c.request(2, 0, 5, {})), self.EBUSY)   # reset
        st = lk.status()
        self.assertEqual(st["running"], sl.image_version(image("A")), st)
        self.assertEqual(st["swap"], "none", st)
        lk.auth(True)
        r = sl.Result()
        sl.AndroidFlow(lk, img).upload(r)
        self.assertGreaterEqual(r.resumed_at or 0, 4096, "the proven upload started over")

    def test_one_write_with_a_read_and_a_reset(self):
        """Per request, not per write (one write can pack [read][reset]).
        Zephyr's SMP transport reassembles one request per write by the first
        header's length: a write carrying [image state read][OS reset] overflows
        it and is dropped whole, so neither runs; unproven, the reset alone gets
        rc 10 (the press prompt). Nothing restarts either way."""
        lk = self.start()
        c = sl.Client(lk)
        s1, f1 = c.frame(0, 1, 0, {})
        s2, f2 = c.frame(2, 0, 5, {})
        self.assertLessEqual(len(f1 + f2), lk.max_write)
        c.pending[s1] = None
        c.pending[s2] = None
        lk.write(f1 + f2)   # ONE ATT write
        with self.assertRaises(sl.SmpError):
            c.wait(s2, 3)
        self.assertIn("reassembly refused a write", self.g.log())
        time.sleep(0.5)
        self.assertTrue(lk.connected.is_set(), "the reset ran")
        self.assertEqual(lk.status()["boot"], 1)
        self.assertIn("images", c.request(0, 1, 0, {}))   # the link still answers
        self.assertEqual(c.rc(c.request(2, 0, 5, {})), self.EBUSY)   # the reset alone
        time.sleep(0.5)
        self.assertTrue(lk.connected.is_set(), "the reset ran")


class SourceGen(Base):
    """An SMP request's source is the connection its first fragment came on.
    A central that never subscribes (so GATT's disconnect cleanup skips the
    SMP transport's partial request) leaves the first part of an OS reset,
    its remaining length that of the next client's first request, and
    disconnects; a proven central then connects and sends that request. Its
    bytes must not complete the old one: the partial is dropped before they
    are collected, the new request is answered and the reset never runs."""

    def test_a_partial_request_from_a_central_that_never_subscribed(self):
        self.g.start("A", "--set-up", "--release", "--healthy-ms=120000")
        lk = self.link()
        c = sl.Client(lk)
        # The proven central's first request: an image state read.
        _, nxt = sl.Client(lk).frame(0, 1, 0, {})
        # The stranger's OS reset (os_mgmt ignores a malformed body and
        # resets all the same), its length counting the next request's bytes.
        _, rst = c.frame(2, 0, 5, {})
        hdr = bytearray(rst[:8])
        body = len(rst) - 8 + len(nxt)
        hdr[2], hdr[3] = body >> 8, body & 0xFF
        lk.write(bytes(hdr) + rst[8:])   # all but the last len(nxt) bytes
        lk.sock.sendto(b"\x02\x01", lk.addr)   # disconnect, never subscribed
        lk.connected.clear()
        time.sleep(0.05)
        self.assertTrue(lk.connect(30))
        lk.auth(True)
        c = sl.Client(lk)
        c.seq = nxt[6]
        rsp = c.request(0, 1, 0, {})      # the same bytes as nxt, well under the 2 s gap
        self.assertIn("images", rsp)
        self.assertIn("never subscribed: a partial request stays", self.g.log())
        self.assertIn("SMP: dropped a partly received request from a connection that's gone",
                      self.g.log())
        time.sleep(1.0)                   # the reset would drop the link at 100 ms
        self.assertTrue(lk.connected.is_set(), "the old reset ran")
        st = lk.status()
        self.assertEqual(st["boot"], 1, st)
        self.assertEqual(st["running"], sl.image_version(image("A")), st)


class SharedArena(Base):
    """SMP shares the malloc arena with the device session, whose ~30 KB block lives from an
    answered hello to the connection's end, so an update can overlap it. The harness's arena is 64 KB (the XIAO's ~146 KB): with 30 KB held,
    both flows still update, and with 46 KB held (18 KB left) too."""

    def run_with_held(self, kb, flow):
        self.g.start("A", f"--hold-kb={kb}")
        self.assertIn(f"holding {kb} KB of the malloc arena", self.g.log())
        lk = self.link()
        img = image("B")
        r = (sl.AndroidFlow if flow == "android" else sl.IOSFlow)(lk, img).run()
        self.assertTrue(r.ok, f"{flow} with {kb} KB held: {r.failure} {r.log}")
        self.assertNotIn("no memory for the SMP work queue", self.g.log())

    def test_android_session_block_held(self):
        self.run_with_held(30, "android")

    def test_android_tight_arena(self):
        self.run_with_held(46, "android")


if __name__ == "__main__":
    unittest.main()
