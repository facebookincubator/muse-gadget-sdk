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

"""Firmware updates over BLE: the Muse Gadgets
MCUmgr SMP core (components/muse_gadget_ble/smp/, vendored) under the ESP32's
real ESP-IDF backend (mg_smp_esp.c), on the host.

tests/mg_smp_esp_server.c runs both in mg_ble.c's configuration (inline, one
1 KB slot, buf_count 3, revert) over the core's fake-GATT link, above a
simulated ESP-IDF OTA layer (two slots with otadata states, a bootloader with
or without rollback, the boot guard's healthy mark). The Muse apps' own flows,
as the core's suite models them from the app code (smplink.AndroidFlow /
IOSFlow), drive it: a picked-file update from both apps at every MTU, an
interrupted upload resumed at the reported offset, a corrupted or unsigned
payload refused before anything is committed, a test image that crashes
before healthy coming back as the old image, the healthy-mark confirm with
and without a phone, who may update (a set-up gadget only for a connection
whose token proof matched: rc 11 with nothing changed, reads open, an allowed
transfer finishing on its link; in SAFE only one the user entered with the key),
and a unit whose bootloader can't roll back reporting the truth.
"""

from __future__ import annotations

import hashlib
import json
import os
import queue
import random
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MG = ROOT / "components" / "muse_gadget_ble"
SMP = MG / "smp"
sys.path.insert(0, str(SMP / "conformance"))
sys.path.insert(0, str(SMP))
sys.path.insert(0, str(ROOT / "tools" / "muse"))

import mcuboot_wrap  # noqa: E402
import pack  # noqa: E402
import smplink as sl  # noqa: E402

NEW, PENDING, VALID, ABORTED = 0, 1, 2, 4
RB_UNPROVEN, RB_PROVEN, RB_ABSENT = 0, 1, 2


# The build the server stands for (tests/smp_esp_fakes/sdkconfig.h): a
# Waveshare S3 1.75C Muse Gadget, ESP32-S3 (chip id 9), with BLE updates.
THIS_BOARD = dict(chip=9, product="W175", board="waveshare_s3_175c", flags=0x03)
C5_CHIP = 23   # ESP32-C5: the default Home Link image's chip (as a VM's device.ota sends)


def board_desc(product: str, board: str, chip: int, flags: int) -> bytes:
    """components/muse_image_id's muse_board_desc_t (64 bytes)."""
    return (struct.pack("<IBBH", 0x4442474D, 1, flags, chip) + product.encode().ljust(16, b"\0")
            + board.encode().ljust(32, b"\0") + bytes(8))


def fake_app(n: int = 60000, version: str = "1.2.3+gabc", seed: int = 1, chip: int = THIS_BOARD["chip"],
             product: str | None = THIS_BOARD["product"], board: str = THIS_BOARD["board"],
             flags: int = THIS_BOARD["flags"], elf: bytes | None = None) -> bytes:
    """Enough of an ESP-IDF app image: 0xE9, the header with its chip id at 12,
    the esp_app_desc_t (version, ELF SHA-256) at 32, the board description at
    288 (none with product=None: an image built before it), then filler."""
    r = random.Random(seed)
    desc = (struct.pack("<II8x", 0xABCD5432, 0) + version.encode().ljust(32, b"\0") + b"\0" * (32 + 16 + 16 + 32)
            + bytes(r.getrandbits(8) for _ in range(32)))
    if elf is not None:   # the ELF SHA-256 (its first four bytes: the build id)
        desc = desc[:-32] + elf
    desc = desc.ljust(256, b"\0")
    body = bytes([0xE9]) + b"\0" * 11 + struct.pack("<H", chip) + b"\0" * 18 + desc
    if product is not None:
        body += board_desc(product, board, chip, flags)
    return body + bytes(r.getrandbits(8) for _ in range(n - len(body)))


def envelope(version: str = "1.2.3+gabc", n: int = 60000, seed: int = 1, **image) -> bytes:
    app = fake_app(n, version, seed, **image)
    return mcuboot_wrap.wrap(app, mcuboot_wrap.app_version(app))


def cc() -> list[str]:
    c = shlex.split(os.environ.get("CC", "cc"))
    if not c or shutil.which(c[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return c


class Server:
    exe: Path | None = None
    tmp = tempfile.TemporaryDirectory()

    @classmethod
    def build(cls) -> Path:
        if cls.exe is None:
            out = Path(cls.tmp.name) / "mg_smp_esp_server"
            sanitize = [] if os.environ.get("MG_NO_SANITIZE") else ["-fsanitize=address,undefined"]
            r = subprocess.run([*cc(), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *sanitize,
                                "-DMG_SMP_TEST_HOOKS",
                                "-I", str(ROOT / "tests" / "smp_esp_fakes"), "-I", str(MG), "-I", str(SMP),
                                "-I", str(ROOT / "components" / "muse_image_id" / "include"),
                                "-I", str(ROOT / "components" / "muse_restart" / "include"),
                                str(ROOT / "components" / "muse_image_id" / "muse_image_id.c"),
                                str(ROOT / "components" / "muse_restart" / "muse_local_press_core.c"),
                                str(ROOT / "tests" / "mg_smp_esp_server.c"), str(SMP / "mg_smp.c"),
                                str(MG / "mg_smp_front.c"),
                                str(SMP / "mg_smp_sha256.c"), str(MG / "mg_smp_esp.c"), str(MG / "mg_smp_esp_util.c"),
                                "-o", str(out)], text=True, capture_output=True)
            if r.returncode:
                raise AssertionError(r.stdout + r.stderr)
            cls.exe = out
        return cls.exe

    def __init__(self, *args: str) -> None:
        self.log_path = Path(self.tmp.name) / f"server-{os.getpid()}-{time.monotonic_ns()}.log"
        self.logf = open(self.log_path, "w")
        self.p = subprocess.Popen([str(self.build()), "--port", "0", "--boot-ms", "200", *args],
                                  stdout=subprocess.PIPE, stderr=self.logf, text=True)
        self.port = int(self.p.stdout.readline().split()[1])
        self.links: list[sl.Link] = []

    def link(self, mtu: int = 247) -> sl.Link:
        lk = sl.Link(self.port, mtu)
        assert lk.connect(5), "no ready"
        self.links.append(lk)
        return lk

    def log(self) -> str:
        self.logf.flush()
        return self.log_path.read_text()

    def stop(self) -> None:
        for lk in self.links:
            lk.close()
        self.p.terminate()
        self.p.wait(5)
        self.logf.close()
        self.p.stdout.close()
        err = self.log_path.read_text()
        assert "AddressSanitizer" not in err and "runtime error" not in err, err[-3000:]
        assert "BUG:" not in err, err[-3000:]


class EspServerBase(unittest.TestCase):
    def setUp(self) -> None:
        self.servers: list[Server] = []

    def tearDown(self) -> None:
        for s in self.servers:
            s.stop()

    def server(self, *args: str) -> Server:
        s = Server(*args)
        self.servers.append(s)
        return s

    def wait_status(self, lk: sl.Link, pred, timeout: float = 5.0) -> dict:
        deadline = time.time() + timeout
        st = lk.status()
        while not pred(st) and time.time() < deadline:
            time.sleep(0.1)
            st = lk.status()
        return st


class AppFlows(EspServerBase):
    """A picked-file update from both apps."""

    def test_both_apps_at_every_mtu(self) -> None:
        for mtu in (23, 185, 247, 517):
            for flow in ("android", "ios"):
                with self.subTest(mtu=mtu, flow=flow):
                    srv = self.server("--healthy-ms", "300")
                    lk = srv.link(mtu)
                    env = envelope("1.4.%d+g0123abcd" % mtu, 40000 if mtu == 23 else 80000, seed=mtu)
                    r = (sl.AndroidFlow if flow == "android" else sl.IOSFlow)(lk, env).run()
                    self.assertTrue(r.ok, f"{flow} at MTU {mtu}: {r.failure}\n{srv.log()[-3000:]}")
                    if flow == "android":
                        self.assertIn("buf_size=512 window=3", r.log)   # buf_count 3: Android's window; 512: no write over 512
                    st = self.wait_status(lk, lambda s: s["state"] == VALID)
                    self.assertEqual(st["running"], "1.4.%d+0" % mtu)
                    info = pack.info(env)
                    self.assertEqual("0x%08x" % st["build_id"], info["build_id"])   # the hash promise's id
                    self.assertEqual((st["state"], st["rollback"], st["drops"]), (VALID, RB_PROVEN, 0))
                    srv.stop()
                    self.servers.remove(srv)

    def test_interrupted_upload_resumes_at_the_reported_offset(self) -> None:
        for flow in (sl.AndroidFlow, sl.IOSFlow):
            with self.subTest(flow=flow.__name__):
                srv = self.server()
                lk = srv.link(247)
                env = envelope("2.0.0", 120000, seed=7)
                with self.assertRaises(InterruptedError):
                    flow(lk, env, interrupt_at=50000).run()
                lk.drop_link()
                self.assertTrue(lk.connect(5))
                r = flow(lk, env).run()
                self.assertTrue(r.ok, r.failure)
                self.assertIsNotNone(r.resumed_at)
                self.assertGreaterEqual(r.resumed_at, 50000)   # not from the start
                self.assertEqual(self.wait_status(lk, lambda s: s["state"] == VALID)["running"], "2.0.0+0")

    def test_corrupted_payload_is_refused_before_anything_is_committed(self) -> None:
        srv = self.server()
        lk = srv.link(247)
        env = bytearray(envelope("3.0.0", 50000, seed=9))
        env[20000] ^= 0x40   # one flipped bit in the app
        r = sl.AndroidFlow(lk, bytes(env)).run()
        self.assertFalse(r.ok)
        self.assertEqual(r.rcs[-1], 9)   # ECORRUPT on the last chunk
        st = lk.status()
        self.assertEqual((st["running"], st["boots"]), ("1.0.0+0", 1))   # still the factory image, no reset
        self.assertNotIn("committed", srv.log())
        self.assertNotIn("checked in", srv.log())   # esp_ota_end never ran
        # Image state stays truthful: the refused upload doesn't read as an image in the other slot.
        self.assertFalse(st["other_present"])
        self.assertEqual(st["erases"], 1)

    def test_another_chips_or_boards_image_is_refused_before_anything_is_written(self) -> None:
        # The app's first 352 bytes (header and board description) are held
        # until they say the image is this board's: another chip's (the C5
        # Home Link image), a Home Link image for the same chip (no
        # product code, board "none": it would pass the chip id and the dev
        # key's signature), another product's, an image built before the
        # board description, and this board's without BLE updates (it would
        # drop the BLE recovery tier) are refused with EINVAL (rc 3), and
        # nothing reaches the slot: no write, no erase.
        cases = (
            ("built for another chip", dict(chip=C5_CHIP, product="", board="none", flags=0)),
            ("built for another board", dict(product="", board="none", flags=0)),
            ("built for another board", dict(product="SCW1", board="sensecap_watcher")),
            ("built for another board", dict(product="W175", board="waveshare_s3_175")),
            ("no board description", dict(product=None)),
            ("it has no Bluetooth update", dict(flags=0x01)),
        )
        for i, (why, image) in enumerate(cases):
            for flow in (sl.AndroidFlow, sl.IOSFlow):
                with self.subTest(why=why, image=image, flow=flow.__name__):
                    srv = self.server()
                    lk = srv.link(247)
                    r = flow(lk, envelope("5.0.%d" % i, 30000, seed=20 + i, **image)).run()
                    self.assertFalse(r.ok)
                    if flow is sl.AndroidFlow:
                        self.assertEqual(r.rcs[-1], 3, r.rcs)   # EINVAL on the chunk that completed the head
                    else:
                        self.assertEqual(r.failure, "Upload failed (error 3)")
                    st = lk.status()
                    self.assertEqual((st["writes"], st["erases"], st["other_present"]), (0, 0, False), st)
                    self.assertEqual((st["running"], st["boots"], st["claimed"]), ("1.0.0+0", 1, False))
                    self.assertIn("update refused before writing: " + why, srv.log())
                    srv.stop()
                    self.servers.remove(srv)

    def test_a_refused_image_leaves_the_other_slot_as_it_was(self) -> None:
        # After an update the other slot holds the previous image; a wrong
        # image refused at its head doesn't touch it, so image state still
        # lists it truthfully. This board's image still installs over it.
        srv = self.server("--healthy-ms", "300")
        lk = srv.link(247)
        self.assertTrue(sl.AndroidFlow(lk, envelope("6.0.0", 30000, seed=30)).run().ok)
        st = self.wait_status(lk, lambda s: s["state"] == VALID)
        self.assertEqual((st["running"], st["slot"], st["other_present"]), ("6.0.0+0", 1, True))
        writes, erases = st["writes"], st["erases"]
        r = sl.IOSFlow(lk, envelope("6.1.0", 30000, seed=31, chip=C5_CHIP, product="", board="none", flags=0)).run()
        self.assertFalse(r.ok)
        st = lk.status()
        self.assertEqual((st["writes"], st["erases"], st["other_present"]), (writes, erases, True))
        self.assertTrue(sl.AndroidFlow(lk, envelope("6.2.0", 30000, seed=32)).run().ok)
        self.assertEqual(self.wait_status(lk, lambda s: s["running"] == "6.2.0+0")["running"], "6.2.0+0")

    def test_upload_is_refused_while_a_wifi_ota_runs(self) -> None:
        # One update at a time: a Wi-Fi OTA holds the other slot, so the
        # upload is refused at once as busy (rc 10) and nothing is written.
        srv = self.server("--wifi-ota")
        lk = srv.link(247)
        r = sl.AndroidFlow(lk, envelope("3.2.0", 30000, seed=12)).run()
        self.assertFalse(r.ok)
        self.assertEqual(r.rcs[-1], 10)
        st = lk.status()
        self.assertEqual((st["running"], st["boots"], st["ota_open"], st["claimed"]), ("1.0.0+0", 1, False, False))
        self.assertFalse(st["other_present"])
        self.assertIn("a Wi-Fi update is running", srv.log())

    def test_the_slot_claim_is_held_only_while_an_update_is_open(self) -> None:
        # Claimed at the upload's start, given back when the core gives its
        # state back (link gone), so a Wi-Fi OTA can run after a Bluetooth one
        # that was left without a test.
        srv = self.server("--resume-hold-s", "1")
        lk = srv.link(247)
        self.assertFalse(lk.status()["claimed"])
        flow = sl.AndroidFlow(lk, envelope("3.3.0", 30000, seed=13))
        flow.upload(sl.Result())   # the whole upload, no test
        self.assertTrue(lk.status()["claimed"])
        lk.drop_link()   # the phone goes away without a test
        st = self.wait_status(lk, lambda s: s["mem_state"] == 0, 10)
        self.assertFalse(st["claimed"])

    def test_unsigned_payload_is_refused_at_test(self) -> None:
        srv = self.server()
        lk = srv.link(247)
        r = sl.IOSFlow(lk, envelope("3.1.0+gbadsig", 30000, seed=10)).run()
        self.assertFalse(r.ok)   # esp_ota_end (the RSA check) refuses it at the test write
        st = lk.status()
        self.assertEqual((st["running"], st["boots"]), ("1.0.0+0", 1))
        self.assertIn("update refused", srv.log())
        self.assertFalse(st["other_present"])   # a refused image doesn't stay listed

    def test_a_test_image_that_crashes_before_healthy_comes_back_old(self) -> None:
        srv = self.server("--healthy-ms", "1000")
        lk = srv.link(247)
        r = sl.AndroidFlow(lk, envelope("4.0.0+gcrash", 30000, seed=11)).run()
        # The app may catch the crashing image's short life or the old one after it.
        st = self.wait_status(lk, lambda s: s["boots"] >= 3, 10)
        self.assertEqual((st["running"], st["boots"], st["slot"]), ("1.0.0+0", 3, 0), srv.log()[-2000:])
        self.assertIn("never confirmed: back to ota_0", srv.log())
        # The app may have read the new image (and confirmed it, held until healthy) in
        # its few ms of life: the app says done before the healthy mark, and the crash
        # still brings the old image back. Otherwise it saw the old image already.
        if not r.ok:
            self.assertTrue("OLD_IMAGE_RUNNING" in (r.failure or "") or "NO_RESTART" in (r.failure or ""), r.failure)
        # The old image reports its own hash, never the envelope's.
        lk.connected.clear()
        self.assertTrue(lk.connect(5))
        active = next(s for s in sl.Client(lk).image_state() if s.get("active"))
        self.assertNotEqual(active["hash"], sl.image_hash(envelope("4.0.0+gcrash", 30000, seed=11)))


class UpdateAuth(EspServerBase):
    """Who may update: on a set-up gadget an upload, image state write,
    reset or erase runs only on a connection whose token proof matched; reads
    stay open; a transfer that started allowed finishes on its connection and
    a link drop ends that. The core asks mg_ble.c's smp_may_change (the server
    stands for it: --auth and the link's 0x09 hook); a refusal is rc 11 with nothing
    changed, on the real ESP-IDF backend."""

    def unproven(self, *extra: str) -> tuple[Server, sl.Link]:
        srv = self.server("--auth", "unproven", *extra)
        return srv, srv.link(247)

    def assert_untouched(self, srv: Server, lk: sl.Link, boots: int = 1) -> None:
        st = lk.status()
        self.assertEqual((st["claimed"], st["ota_open"], st["other_present"]), (False, False, False), st)
        self.assertEqual((st["writes"], st["erases"], st["held"], st["mem_state"]), (0, 0, 0, 0), st)
        self.assertEqual((st["running"], st["boots"]), ("1.0.0+0", boots), st)
        self.assertNotIn("committed", srv.log())

    def test_refused_ops_change_nothing_reads_open(self) -> None:
        srv, lk = self.unproven()
        c = sl.Client(lk)
        env = envelope("8.0.0", 20000, seed=40)
        self.assertGreater(c.params()["buf_size"], 0)   # reads stay open
        self.assertEqual(len(c.image_state()), 1)
        self.assertEqual(c.request(2, 0, 0, {"d": "hi"}).get("r"), "hi")
        sha = hashlib.sha256(env).digest()
        cases = [
            ("upload", lambda: c.request(2, 1, 1, {"off": 0, "len": len(env), "sha": sha, "data": env[:200]})),
            ("upload at an offset", lambda: c.request(2, 1, 1, {"off": 4096, "data": env[4096:4296]})),
            ("test", lambda: c.request(2, 1, 0, {"hash": sl.image_hash(env), "confirm": False})),
            ("confirm", lambda: c.request(2, 1, 0, {"confirm": True})),
            ("reset", lambda: c.request(2, 0, 5, {})),
            ("erase", lambda: c.request(2, 1, 5, {})),
        ]
        for name, call in cases:
            self.assertEqual(c.rc(call()), 11, name)
        time.sleep(0.3)
        self.assertTrue(lk.connected.is_set(), "a refused reset dropped the link")
        self.assert_untouched(srv, lk)
        self.assertEqual(lk.status()["auth_refused"], len(cases))
        for flow in (sl.AndroidFlow, sl.IOSFlow):   # both apps fail cleanly with error 11
            r = flow(lk, env).run()
            self.assertFalse(r.ok)
            self.assertIn("11", r.failure)
            self.assert_untouched(srv, lk)

    def test_one_write_with_a_read_and_a_mutating_request(self) -> None:
        # Per SMP request, not per ATT write: [image state read][reset / upload / test] in one write.
        env = envelope("8.1.0", 20000, seed=41)
        for second in ("reset", "upload", "test"):
            with self.subTest(second=second):
                srv, lk = self.unproven()
                c = sl.Client(lk)
                s1, f1 = c.frame(0, 1, 0, {})
                if second == "reset":
                    s2, f2 = c.frame(2, 0, 5, {})
                elif second == "upload":
                    s2, f2 = c.frame(2, 1, 1, {"off": 0, "len": len(env), "sha": hashlib.sha256(env).digest(),
                                               "data": env[:64]})
                else:
                    s2, f2 = c.frame(2, 1, 0, {"hash": sl.image_hash(env), "confirm": False})
                self.assertLessEqual(len(f1 + f2), lk.max_write)
                c.pending[s1] = None
                c.pending[s2] = None
                lk.write(f1 + f2)   # one ATT write
                self.assertIn("images", c.wait(s1, 5))
                self.assertEqual(c.rc(c.wait(s2, 5)), 11)
                time.sleep(0.3)
                self.assertTrue(lk.connected.is_set())
                self.assert_untouched(srv, lk)
                srv.stop()
                self.servers.remove(srv)

    def test_proven_works_and_a_fresh_proof_confirms_after_the_reboot(self) -> None:
        srv, lk = self.unproven("--healthy-ms", "60000")   # the self-confirm mark far away
        lk.auth(True)
        env = envelope("8.2.0", 30000, seed=42)
        f = sl.AndroidFlow(lk, env)
        f.upload(sl.Result())
        f.c.test(sl.image_hash(env))
        lk.disconnected.clear()
        f.c.reset()
        self.assertTrue(lk.disconnected.wait(5))
        self.assertTrue(lk.connect(5))
        st = lk.status()
        self.assertEqual((st["running"], st["state"], st["auth"]), ("8.2.0+0", PENDING, 0), st)
        c = sl.Client(lk)
        self.assertEqual(c.rc(c.request(2, 1, 0, {"confirm": True})), 11, "confirm without a fresh proof")
        lk.auth(True)   # as the apps do: prove again after the reconnect, then confirm
        self.assertEqual(c.rc(c.request(2, 1, 0, {"confirm": True})), 0)
        self.assertEqual(lk.status()["state"], PENDING)   # held until the healthy mark

    def test_an_allowed_transfer_finishes_when_the_proof_lapses(self) -> None:
        srv, lk = self.unproven()
        lk.auth(True)
        env = envelope("8.3.0", 60000, seed=43)
        f = sl.AndroidFlow(lk, env, interrupt_at=len(env) // 2)
        with self.assertRaises(InterruptedError):
            f.upload(sl.Result())
        lk.auth(False)   # the proof lapses mid-transfer; the link stays up
        f2 = sl.AndroidFlow(lk, env)   # the client's retry from 0 with the same len + sha
        f2.upload(sl.Result())
        f2.c.test(sl.image_hash(env))   # the state write after staging: the same transfer
        time.sleep(0.3)
        lk.disconnected.clear()
        f2.c.reset()   # the reset after the commit: the same transfer
        self.assertTrue(lk.disconnected.wait(5))
        self.assertTrue(lk.connect(5))
        st = lk.status()
        self.assertEqual((st["running"], st["auth_refused"]), ("8.3.0+0", 0), st)

    def test_a_link_drop_ends_the_allowance(self) -> None:
        srv, lk = self.unproven()
        lk.auth(True)
        env = envelope("8.4.0", 60000, seed=44)
        f = sl.AndroidFlow(lk, env, interrupt_at=len(env) // 2)
        with self.assertRaises(InterruptedError):
            f.upload(sl.Result())
        lk.drop_link()
        self.assertTrue(lk.connect(5))   # a new connection, not proven
        c = sl.Client(lk)
        rsp = c.request(2, 1, 1, {"off": 0, "len": len(env), "sha": hashlib.sha256(env).digest(), "data": env[:200]})
        self.assertEqual(c.rc(rsp), 11)
        writes = lk.status()["writes"]
        self.assertGreater(lk.status()["mem_state"], 0, "the held upload was dropped")
        lk.auth(True)
        r = sl.AndroidFlow(lk, env)
        res = sl.Result()
        r.upload(res)
        self.assertIsNotNone(res.resumed_at, "the resume restarted from 0")
        self.assertGreater(lk.status()["writes"], writes)
        r.c.test(sl.image_hash(env))
        lk.drop_link()   # the commit's connection goes before the reset
        self.assertTrue(lk.connect(5))
        c = sl.Client(lk)
        self.assertEqual(c.rc(c.request(2, 0, 5, {})), 11, "the reset of a gone connection's commit, unproven")
        self.assertEqual(lk.status()["boots"], 1)
        lk.auth(True)
        lk.disconnected.clear()
        c.reset()
        self.assertTrue(lk.disconnected.wait(5))
        self.assertTrue(lk.connect(5))
        self.assertEqual(lk.status()["running"], "8.4.0+0")

    def test_both_app_flows_when_proven(self) -> None:
        for flow in (sl.AndroidFlow, sl.IOSFlow):
            with self.subTest(flow=flow.__name__):
                srv = self.server("--auth", "proven", "--healthy-ms", "300")
                lk = srv.link(247)
                r = flow(lk, envelope("8.5.0", 30000, seed=45)).run()
                self.assertTrue(r.ok, r.failure)
                st = self.wait_status(lk, lambda s: s["state"] == VALID)
                self.assertEqual(st["running"], "8.5.0+0")
                self.assertGreater(st["auth_asks"], 0)
                self.assertEqual(st["auth_refused"], 0)
                srv.stop()
                self.servers.remove(srv)


class GlueMayChange(unittest.TestCase):
    """mg_ble.c's update gate, compiled from the glue itself (smp_gate_now through smp_front_whole: mg_smp_gate.h,
    mgcommands.h Token proof) with the real request front, muse_local_press core and SHA-256, and a stand-in core
    that asks the hooks for every change: a proven connection, a SAFE the user entered with the key and
    bench/automation/test builds take an update at once; a self-entered SAFE can't prompt (rc 11); any other link
    (set up or not) asks for ONE confirm press, the front answering rc 10 while it asks and 11 for a refusal itself,
    the core's hooks saying yes to what it let through. Each refusal is logged once."""

    @staticmethod
    def frames() -> dict:
        def smp(group: int, cmd: int, body: dict, seq: int = 0) -> bytes:
            b = sl.cbor_encode(body)
            return bytes([2, 0]) + struct.pack(">HH", len(b), group) + bytes([seq, cmd]) + b
        img = bytes(range(100))
        forged = bytes(range(100, 200))
        sha = hashlib.sha256(img).digest()

        def nested(k: int) -> bytes:   # {"x": [[...[0]...]]}: the map, then k arrays (depth k + 1)
            return bytes([0xa1, 0x61, ord("x")]) + bytes([0x81]) * k + b"\x00"

        def raw(op: int, group: int, cmd: int, b: bytes) -> bytes:
            return bytes([op, 0]) + struct.pack(">HH", len(b), group) + bytes([0, cmd]) + b
        up0 = sl.cbor_encode({"image": 0, "len": 100, "off": 0, "sha": sha, "data": img[:50]})
        assert up0[0] == 0xa5   # five pairs: UP0_DEEP adds a sixth, 21 deep
        return {
            "DEPTH8": raw(0, 1, 0, nested(7)),
            "DEPTH9": raw(0, 1, 0, nested(8)),
            "DEEP": raw(0, 0, 0, nested(497)),   # a ~500-deep echo, 509 bytes
            "UP0_DEEP": raw(2, 1, 1, bytes([0xa6]) + up0[1:] + bytes([0x61, ord("z")]) + bytes([0x81]) * 20 + b"\x00"),
            "UP0": smp(1, 1, {"image": 0, "len": 100, "off": 0, "sha": sha, "data": img[:50]}),
            "UP0_NOSHA": smp(1, 1, {"image": 0, "len": 100, "off": 0, "data": img[:50]}),
            "UP0_OTHER": smp(1, 1, {"image": 0, "len": 100, "off": 0, "sha": hashlib.sha256(forged).digest(),
                                    "data": forged[:50]}),
            "FORGED0": smp(1, 1, {"image": 0, "len": 100, "off": 0, "sha": sha, "data": forged[:50]}),
            "UP50": smp(1, 1, {"off": 50, "data": img[50:]}, 1),
            "UP50_LONG": smp(1, 1, {"off": 50, "data": img[50:] + bytes(8)}, 1),
            "UP0_OVER": smp(1, 1, {"image": 0, "len": 40, "off": 0, "sha": sha, "data": img[:50]}),
            "FORGED50": smp(1, 1, {"off": 50, "data": forged[50:]}, 1),
            "STATE": smp(1, 0, {"hash": hashlib.sha256(img[:80]).digest(), "confirm": False}, 2),
            "RESET": smp(0, 5, {}, 3),
            "ERASE": smp(1, 5, {"slot": 1}),
        }

    def test_answers(self) -> None:
        ble = (MG / "mg_ble.c").read_text()
        i = ble.index("static mg_smp_gate_t smp_gate_now(void)")
        fn = ble[i:ble.index("\n}\n", ble.index("static void smp_front_whole(")) + 3]
        self.assertIn(".may_change = smp_may_change,", ble)
        self.assertIn(".refusal_rc = smp_refusal_rc,", ble)
        self.assertIn(".may_upload = smp_may_upload,", ble)
        self.assertIn('#include "mg_smp_gate.h"', ble)
        self.assertIn("#if MG_NO_PERSON   /* no person in the loop: bench, automation and test images */\n"
                      "#define SMP_RELEASE false", ble)
        self.assertIn("\nCONFIG_MUSE_GADGET_AUTOMATION=y\n", (ROOT / "devices" / "sdkconfig.muse-bench").read_text())
        self.assertNotIn("SMP_SETUP_WINDOW", ble)   # no pre-armed hold
        # The gate takes this connection's matched proof, never owner_ok (true on an unset gadget); Forget ends it.
        self.assertIn("bool ok = !safe && !s_smp_forgot && mg_proto_proof_matched(&s_proto);", fn)
        self.assertNotIn("mg_proto_owner_ok", fn)
        done = ble[ble.index("void mg_ble_proof_clear_done(bool erased)"):]
        done = done[:done.index("\n}\n")]
        self.assertIn("s_smp_forgot = true;\n        muse_local_press_forget();", done)
        # Every SMP write goes through the front (the FIFO's), as the server's do.
        self.assertIn("smp_feed(&f->front, f->out, (size_t)n);", ble)
        self.assertIn("mg_smp_front_rx(rx, msg + 1, n - 1, mg_now_ms(), &s_smp_front_out);", ble)
        glue = (ROOT / "main" / "mg_glue.c").read_text()
        self.assertEqual(glue.count(".local_press_ask = glue_update_ask,"), 1)   # normal mode only
        self.assertNotIn("local_press_open", glue)
        safe = glue[glue.index("static void start_safe(void) {"):]
        self.assertNotIn("local_press_ask", safe[:safe.index("\n}\n")])   # a self-entered SAFE can't prompt
        frames = "".join("static const uint8_t %s[] = { %s };\n" % (k, ", ".join(str(b) for b in v))
                         for k, v in self.frames().items())
        src = ('#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
               '#include "mg_smp_backend.h"\n#include "mg_smp_gate.h"\n#include "mg_smp_front.h"\n'
               '#include "muse_local_press.h"\n'
               "static bool rel, locked;\n#define SMP_RELEASE rel\n"
               "static muse_local_press_t w; static int64_t now;\n"
               "static uint8_t ask(const struct muse_update_req *req) {\n"
               "  return (uint8_t)muse_local_press_core_ask(&w, now, locked, req); }\n"
               "bool muse_local_press_bound(muse_update_ident_t *id) { return muse_local_press_core_bound(&w, id); }\n"
               "static int s_smp_refusal = MG_SMP_EACCESSDENIED;\n"
               "typedef struct { bool safe_mode; uint8_t recovery_mode; bool safe_updates_open;\n"
               "  uint8_t (*local_press_ask)(const struct muse_update_req *); } plat_t;\n"
               "typedef struct { int x; } mg_proto_t;\n"
               "static plat_t s_platform; static mg_proto_t s_proto; static bool owner, unset;\n"
               "static volatile bool s_smp_forgot; static mg_smp_front_slot_t *s_smp_slot; static bool s_smp_front_ok;\n"
               "static mg_smp_front_up_t *s_smp_up; static bool holds;\n"
               "static const void *s_smp_env; static int aborts, result;\n"
               "static int warned, locks; static char last[240];\n"
               "static __attribute__((unused)) bool mg_proto_owner_ok(const mg_proto_t *p) { (void)p; return owner || unset; }\n"
               "static bool mg_proto_proof_matched(const mg_proto_t *p) { (void)p; return owner; }\n"
               "static void mg_lock(void) { locks++; }\nstatic void mg_unlock(void) { locks--; }\n"
               "static void *smp_alloc(size_t n) { return calloc(1, n); }\n"
               "static bool mg_smp_upload_active(void) { return holds; }\n"
               "static void mg_smp_abort(const void *env) { (void)env; aborts++; }\n"
               "/* The front's own answer: {\"rc\": N} in a read or write response. */\n"
               "static int smp_send(void *ctx, const uint8_t *b, uint16_t n) { (void)ctx;\n"
               "  result = n == 13 && (b[0] == 1 || b[0] == 3) && b[8] == 0xa1 ? b[12] : 98; return 0; }\n"
               '#define TAG "mg.ble"\n'
               "#define ESP_LOGW(t, fmt, ...) (warned++, (void)(t), snprintf(last, sizeof(last), fmt, __VA_ARGS__))\n"
               "static bool smp_may_change(void *ctx, uint8_t op);\n"
               "static bool smp_may_upload(void *ctx, uint32_t len, const uint8_t *sha32);\n"
               "static int smp_refusal_rc(void *ctx);\n"
               "/* A stand-in for the core: it asks its hooks for every change it's handed (0: it goes on). */\n"
               "static int fed;\n"
               "static void mg_smp_rx(const void *env, const uint8_t *f, uint16_t n) { (void)env; (void)n; fed++;\n"
               "  if ((f[0] & 7) != 2) { result = 0; return; }   /* a read: never asked */\n"
               "  uint8_t op = f[4] == 0 ? MG_SMP_OP_RESET : f[7] == 1 ? MG_SMP_OP_UPLOAD : f[7] == 0 ? MG_SMP_OP_STATE\n"
               "             : MG_SMP_OP_ERASE;\n"
               "  bool ok = op == MG_SMP_OP_UPLOAD ? smp_may_upload(NULL, 0, NULL) : smp_may_change(NULL, op);\n"
               "  result = ok ? 0 : smp_refusal_rc(NULL); }\n"
               + fn + frames +
               "static void dev(bool safe, bool button, bool own, bool release) {\n"
               "  s_platform.safe_mode = safe; s_platform.recovery_mode = 0; s_platform.safe_updates_open = button;\n"
               "  s_platform.local_press_ask = ask; owner = own; rel = release; locked = false; unset = false;\n"
               "  s_smp_forgot = false; aborts = 0; free(s_smp_slot); s_smp_slot = NULL; free(s_smp_up); s_smp_up = NULL;\n"
               "  holds = false;\n"
               "  muse_local_press_core_init(&w); now = 1000;\n}\n"
               "/* The rc a request gets: 0, or the refusal's (with its one log line). */\n"
               "static int run(const uint8_t *f, size_t n) { warned = 0; result = -1; smp_front_whole(NULL, f, n);\n"
               "  if (locks || (result == 0 && warned) || (result != 0 && warned != 1)) return 99;\n"
               "  return result; }\n"
               "#define R(f) run(f, sizeof(f))\n"
               "static int ops(void) { return !R(UP0) && !R(UP50) && !R(STATE) && !R(RESET) && !R(ERASE); }\n"
               "static bool press(void) { return muse_local_press_core_note(&w, now, locked); }\n"
               "int main(void) {\n"
               "  int a, r, p, p2, b4, late, other;\n"
               "  /* safe, button, owner_ok, release */\n"
               "  dev(0,0,1,1); printf(\"%d \", R(UP0));   /* proven (the apps): at once */\n"
               "  dev(1,1,0,1); printf(\"%d \", R(UP0));   /* the key's SAFE: at once */\n"
               "  dev(0,0,0,0); printf(\"%d \", ops());    /* a bench, automation or test build: every op */\n"
               "  dev(1,0,0,1); printf(\"%d \", R(UP0));   /* a self-entered SAFE: no way to prompt */\n"
               "  /* a set-up gadget whose phone is away (unproven): only the press */\n"
               "  dev(0,0,0,1); b4 = press(); a = R(UP0); now += 1000; r = R(UP0); p = press();\n"
               "  printf(\"%d,%d,%d,%d,%d \", b4, a, r, p, R(UP0));\n"
               "  printf(\"%d \", !R(UP50) && !R(STATE) && !R(RESET) && !R(ERASE));   /* the rest of that update */\n"
               "  dev(0,0,0,1); a = R(UP0); now += MUSE_UPDATE_PROMPT_MS; late = press(); r = R(UP0);\n"
               "  printf(\"%d,%d,%d,%d \", a, late, r, R(UP0));   /* no press: rc 11 once, then it asks again */\n"
               "  dev(0,0,0,1); a = R(UP0); muse_local_press_core_cancel(&w, now); printf(\"%d,%d \", a, R(UP0));\n"
               "  dev(0,0,0,1); muse_local_press_core_arm(&w, now); printf(\"%d \", R(UP0));   /* the wired arm */\n"
               "  dev(0,0,0,1); locked = true; a = R(UP0); p = press(); locked = false; p2 = press();\n"
               "  printf(\"%d,%d,%d,%d \", a, p, p2, R(UP0));   /* the passcode first, then the press */\n"
               "  /* the press binds to its upload for 600 s, across a link drop; another image asks */\n"
               "  dev(0,0,0,1); a = R(UP0); p = press(); muse_local_press_core_link_new(&w); now += 120000; r = R(UP0);\n"
               "  other = R(UP0_OTHER); now += 480000; muse_local_press_core_link_new(&w);\n"
               "  printf(\"%d,%d,%d,%d,%d \", a, p, r, other, R(UP0));\n"
               "  /* a gadget that isn't set up (owner_ok true) and an unproven central: rc 10 and the prompt */\n"
               "  dev(0,0,0,1); unset = true; a = R(UP0); printf(\"%d,%d \", a, muse_local_press_core_prompting(&w, now));\n"
               "  /* round 2: no SHA-256, a lone chunk, past the length, not the bound bytes, an early state write */\n"
               "  dev(0,0,0,1); a = R(UP0_NOSHA); printf(\"%d,%d,%d \", a, muse_local_press_core_prompting(&w, now), R(UP50));\n"
               "  dev(0,0,0,1); R(UP0); press(); R(UP0); a = R(UP50_LONG); printf(\"%d,%d,%d \", a, R(STATE), aborts);\n"
               "  dev(0,0,0,1); R(FORGED0); press(); r = R(FORGED0); a = R(FORGED50); printf(\"%d,%d,%d \", r, a, aborts);\n"
               "  dev(0,0,0,1); R(UP0); press(); R(UP0); a = R(STATE); printf(\"%d,%d \", a, R(UP50));\n"
               "  /* a press on an erase covers that erase, on its link */\n"
               "  dev(0,0,0,1); R(ERASE); press(); a = R(ERASE); r = R(RESET); muse_local_press_core_link_new(&w);\n"
               "  printf(\"%d,%d,%d \", a, r, R(ERASE));\n"
               "  /* Forget (mg_ble_proof_clear_done): the proof and the binding end */\n"
               "  dev(0,0,1,1); a = R(UP0); s_smp_forgot = true; muse_local_press_core_forget(&w, now); r = R(UP0);\n"
               "  dev(0,0,0,1); R(UP0); press(); p = R(UP0); muse_local_press_core_forget(&w, now); now += 10000;\n"
               "  printf(\"%d,%d,%d,%d \", a, r, p, R(UP0));\n"
               "  /* nesting: 8 deep reaches the core, 9 deep and ~500 deep are refused (rc 3) before it, proven or not */\n"
               "  dev(0,0,1,1); fed = 0; a = R(DEPTH8); r = fed; p = R(DEPTH9); p2 = R(DEEP); other = R(UP0_DEEP);\n"
               "  dev(0,0,0,1); late = R(UP0_DEEP);\n"
               "  printf(\"%d,%d,%d,%d,%d,%d,%d \", a, r, p, p2, other, late, fed);\n"
               "  /* past the upload's length, on a proven link and on a bench build (no press): rc 11, the upload ends; an\n"
               "   * offset-0 request longer than its own len too; a chunk within it goes on */\n"
               "  for (int bench = 0; bench < 2; bench++) {\n"
               "    if (bench) dev(0,0,0,0); else dev(0,0,1,1);\n"
               "    a = R(UP0); holds = true; r = R(UP50); p = R(UP50_LONG); p2 = aborts; other = R(UP0_OVER);\n"
               "    printf(\"%d,%d,%d,%d,%d,%d \", a, r, p, p2, other, aborts);\n"
               "  }\n"
               "  printf(\"\\n\");\n"
               "  return 0;\n}\n")
        with tempfile.TemporaryDirectory() as t:
            Path(t, "a.c").write_text(src)
            r = subprocess.run([*cc(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(SMP), "-I", str(MG),
                                "-I", str(ROOT / "components" / "muse_restart" / "include"), str(Path(t, "a.c")),
                                str(ROOT / "components" / "muse_restart" / "muse_local_press_core.c"),
                                str(MG / "mg_smp_front.c"), str(SMP / "mg_smp_sha256.c"),
                                "-o", str(Path(t, "a"))], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            out = subprocess.run([str(Path(t, "a"))], capture_output=True, text=True).stdout.split()
        self.assertEqual(out, ["0", "0", "1", "11", "0,10,10,1,0", "1", "10,0,11,10", "10,11", "0", "10,0,1,0",
                               "10,1,0,10,10", "10,1", "11,0,11", "11,10,1", "0,11,1", "11,11", "0,10,10",
                               "0,10,0,10", "0,1,3,3,3,3,1", "0,0,11,1,11,2", "0,0,11,1,11,2"], out)

    def test_safe_glue_says_how_safe_began(self) -> None:
        glue = (ROOT / "main" / "mg_glue.c").read_text()
        safe = glue[glue.index("static void start_safe(void) {"):]
        safe = safe[:safe.index("\n}\n")]
        self.assertLess(safe.index("platform.safe_updates_open = muse_bootguard_safe_why() == MUSE_BOOT_WHY_BUTTON;"),
                        safe.index("mg_ble_init(&platform);"))


class UpdateStopLine(EspServerBase):
    """The "Update stopped" line says what the stop left,
    from the backend's own state: "Nothing was installed" for a stop before
    the commit (still on the old version), the image's state after it."""

    UI_INSTALLING, UI_STOPPED = 2, 3

    def test_a_refused_file_installed_nothing(self) -> None:
        srv = self.server()
        lk = srv.link(247)
        env = bytearray(envelope("3.0.1", 50000, seed=19))
        env[20000] ^= 0x40
        self.assertFalse(sl.AndroidFlow(lk, bytes(env)).run().ok)
        st = lk.status()
        self.assertEqual((st["ui"], st["ui_installed"], st["ui_stop_text"]),
                         (self.UI_STOPPED, False, "Nothing was installed"))

    def test_an_upload_given_up_installed_nothing(self) -> None:
        srv = self.server("--resume-hold-s", "1")
        lk = srv.link()
        with self.assertRaises(InterruptedError):
            sl.AndroidFlow(lk, envelope("8.2.0", 120000, seed=23), 40000).run()
        lk.drop_link()
        st = self.wait_status(lk, lambda s: s["ui"] == self.UI_STOPPED, 6)
        self.assertEqual((st["ui"], st["ui_installed"], st["ui_stop_text"]),
                         (self.UI_STOPPED, False, "Nothing was installed"))

    def test_the_whole_file_without_its_test_installed_nothing(self) -> None:
        # "Installing" from the last byte on, but never committed: the link
        # going leaves the old version running.
        srv = self.server("--resume-hold-s", "1")
        lk = srv.link()
        sl.AndroidFlow(lk, envelope("3.4.0", 30000, seed=14)).upload(sl.Result())
        self.assertEqual(lk.status()["ui"], self.UI_INSTALLING)
        lk.drop_link()
        st = self.wait_status(lk, lambda s: s["ui"] == self.UI_STOPPED, 10)
        self.assertEqual((st["ui"], st["ui_installed"], st["ui_stop_text"]),
                         (self.UI_STOPPED, False, "Nothing was installed"))

    def test_a_stop_after_the_commit_says_it_is_installed(self) -> None:
        srv = self.server("--resume-hold-s", "1")
        lk = srv.link()
        env = envelope("3.5.0", 30000, seed=15)
        sl.AndroidFlow(lk, env).upload(sl.Result())
        sl.Client(lk).test(sl.image_hash(env))   # committed: boots next, pending its test boot
        st = lk.status()
        self.assertEqual((st["ui"], st["ui_installed"]), (self.UI_INSTALLING, True))
        lk.drop_link()   # gone without the reset
        st = self.wait_status(lk, lambda s: s["ui"] == self.UI_STOPPED, 10)
        self.assertEqual((st["ui"], st["ui_installed"], st["ui_stop_text"]),
                         (self.UI_STOPPED, True, "Installed; restart to finish"))
        self.assertIn("update stopped after its commit", srv.log())

    def test_the_reset_on_its_way_keeps_installing(self) -> None:
        # The normal flow: test, then reset; the link closing for the restart isn't a stop.
        srv = self.server()
        lk = srv.link()
        self.assertTrue(sl.AndroidFlow(lk, envelope("3.6.0", 30000, seed=16)).run().ok)
        self.assertNotIn("update stopped after", srv.log())

class SmpCoreV9V10(EspServerBase):
    """The core's smp-v9 and smp-v10 changes, as this port answers them: an upload carrying the running image's id
    (its version and build id) with other bytes is refused at its last chunk (rc 6), the running slot reporting no
    hash of its own; a test naming the staged upload commits it before any "already running" answer, so an image
    whose trial reverted is tested again; after a dropped link, image state reads don't keep an interrupted upload:
    only its own resume stops the hold's clock."""

    def running_twin(self, lk: sl.Link, seed: int) -> bytes:
        st = lk.status()
        app = fake_app(40000, st["version_string"], seed=seed, elf=struct.pack("<I", st["build_id"]) + bytes(range(100, 128)))
        self.assertEqual(pack.idf_build_id(app), st["build_id"])
        return mcuboot_wrap.wrap(app, mcuboot_wrap.app_version(app))

    def test_an_image_with_the_running_id_and_other_bytes_is_refused_at_its_last_chunk(self) -> None:
        srv = self.server()
        lk = srv.link(247)
        env = self.running_twin(lk, 52)
        before = lk.status()
        r = sl.AndroidFlow(lk, env).run()
        self.assertFalse(r.ok)
        self.assertEqual(r.rcs[-1], 6, (r.rcs, srv.log()[-1500:]))   # EBADSTATE
        self.assertIn("smp: upload refused, running id on another image", srv.log())
        st = lk.status()
        self.assertEqual((st["running"], st["boots"]), (before["running"], before["boots"]), st)
        for im in sl.Client(lk).image_state():
            self.assertNotEqual(im.get("hash"), sl.image_hash(env), im)   # no slot names it, the running one least

    def test_a_reverted_image_uploaded_again_is_tested_again(self) -> None:
        srv = self.server("--healthy-ms", "1000")
        lk = srv.link(247)
        env = envelope("4.0.0+gcrash", 30000, seed=11)   # its test boot resets before healthy: the old image back
        sl.AndroidFlow(lk, env).run()
        st = self.wait_status(lk, lambda s: s["boots"] >= 3, 10)
        self.assertEqual((st["running"], st["slot"]), ("1.0.0+0", 0), srv.log()[-2000:])
        lk.connected.clear()
        self.assertTrue(lk.connect(5))
        sl.AndroidFlow(lk, env).upload(sl.Result())
        imgs = sl.Client(lk).test(sl.image_hash(env)).get("images", [])
        staged = [i for i in imgs if i.get("hash") == sl.image_hash(env)]
        self.assertEqual(len(staged), 1, imgs)
        self.assertTrue(staged[0].get("pending") and not staged[0].get("active"), staged[0])

    def test_image_state_reads_dont_keep_an_interrupted_upload(self) -> None:
        srv = self.server("--resume-hold-s", "1")
        lk = srv.link(247)
        flow = sl.AndroidFlow(lk, envelope("3.3.1", 60000, seed=23), interrupt_at=20000)
        with self.assertRaises(InterruptedError):
            flow.upload(sl.Result())
        self.assertTrue(lk.status()["claimed"])
        lk.drop_link()
        self.assertTrue(lk.connect(5))
        c = sl.Client(lk)
        end = time.time() + 3.0
        while time.time() < end:
            c.image_state()
            time.sleep(0.2)
        st = self.wait_status(lk, lambda s: not s["claimed"], 3)
        self.assertFalse(st["claimed"], srv.log()[-1200:])
        self.assertIn("smp: interrupted upload not resumed, dropped", srv.log())


class HealthyMark(EspServerBase):
    """The healthy mark gates nothing:
    it only times the confirm."""

    def upload_test_reset(self, lk: sl.Link, env: bytes) -> None:
        c = sl.Client(lk)
        f = sl.AndroidFlow(lk, env)
        f.upload(sl.Result())
        c.test(sl.image_hash(env))
        lk.disconnected.clear()
        c.reset()
        self.assertTrue(lk.disconnected.wait(5))
        self.assertTrue(lk.connect(10))

    def test_the_staged_upload_shows_in_slot_1_before_the_test(self) -> None:
        # What mcumgr tools (and mg_ble_client dfu) check after the last chunk.
        srv = self.server()
        lk = srv.link()
        env = envelope("5.0.1", 30000, seed=18)
        sl.AndroidFlow(lk, env).upload(sl.Result())
        slot1 = next(s for s in sl.Client(lk).image_state() if s["slot"] == 1)
        self.assertEqual(slot1["hash"], sl.image_hash(env))
        self.assertFalse(slot1.get("pending"))

    def test_a_test_image_confirms_itself_at_healthy_without_a_phone(self) -> None:
        srv = self.server("--healthy-ms", "800")
        lk = srv.link()
        self.upload_test_reset(lk, envelope("5.0.0", 30000, seed=12))
        st = lk.status()
        self.assertEqual((st["running"], st["state"], st["healthy"]), ("5.0.0+0", PENDING, False))
        lk.drop_link()   # the phone goes away; nobody sends a confirm
        st = self.wait_status(lk, lambda s: s["state"] == VALID, 5)
        self.assertEqual((st["state"], st["confirms"]), (VALID, 1))
        self.assertTrue(st["healthy"])

    def test_a_record_that_cant_be_saved_stops_the_update_screen(self) -> None:
        # The core saves its record after finish closed the slot's handle; if that save fails, the
        # backend's abort used to see no open handle and did nothing: "Installing" stayed on the screen (and kept
        # it awake) and the claim stayed, so a Wi-Fi OTA was refused as busy until a restart. Now the screen stops
        # with nothing installed, the claim goes back, and nothing is set to boot.
        srv = self.server("--fail-blob", "rec0")
        lk = srv.link()
        env = envelope("5.0.5", 30000, seed=22)
        sl.AndroidFlow(lk, env).upload(sl.Result())
        with self.assertRaises(sl.SmpError):
            sl.Client(lk).test(sl.image_hash(env))
        st = lk.status()
        self.assertEqual((st["ui"], st["claimed"], st["ui_installed"], st["running"]), (3, False, False, "1.0.0+0"),
                         srv.log()[-1500:])
        self.assertIn("smp: record write failed, not committing", srv.log())

    def test_a_keep_that_fails_is_tried_again(self) -> None:
        # A transient keep error (the otadata write) at the healthy mark is tried again, not marked done
        # with the image left to roll back at the next reset (mg_smp_esp_keep + mg_ble.c's paced retries).
        srv = self.server("--healthy-ms", "300", "--fail-keeps", "2")
        lk = srv.link()
        self.upload_test_reset(lk, envelope("5.0.2", 30000, seed=19))
        lk.drop_link()
        st = self.wait_status(lk, lambda s: s["state"] == VALID, 8)
        self.assertEqual((st["running"], st["state"], st["confirms"]), ("5.0.2+0", VALID, 1), srv.log()[-1500:])
        self.assertEqual(srv.log().count("keep: otadata write failed"), 2)

    def test_a_keep_that_keeps_failing_is_given_up_and_rolls_back(self) -> None:
        # Bounded: 8 tries (paced), then given up with a line; the image is still a test image, so the next reset
        # goes back to the old one.
        srv = self.server("--healthy-ms", "300", "--fail-keeps", "99")
        lk = srv.link()
        self.upload_test_reset(lk, envelope("5.0.3", 30000, seed=20))
        lk.drop_link()
        deadline = time.time() + 10
        while "mark: the keep failed 8 times" not in srv.log() and time.time() < deadline:
            time.sleep(0.1)
        self.assertIn("mark: the keep failed 8 times", srv.log())
        self.assertEqual(srv.log().count("keep: otadata write failed"), 8)
        st = lk.status()
        self.assertEqual((st["running"], st["state"]), ("5.0.3+0", PENDING), st)
        self.assertTrue(lk.connect(5))
        lk.disconnected.clear()
        sl.Client(lk).reset()
        st = self.wait_status(lk, lambda s: s["boots"] >= 3, 8)
        self.assertEqual(st["running"], "1.0.0+0", srv.log()[-1500:])

    def test_a_verdict_whose_write_fails_is_kept_later(self) -> None:
        # rb_keep: the verdict is marked kept only once written (it was cleared first and the write's result
        # ignored); the mark's rounds try again, with a line, and the test image itself is still kept.
        srv = self.server("--healthy-ms", "300", "--fail-blob", "rb")
        lk = srv.link()
        self.upload_test_reset(lk, envelope("5.0.4", 30000, seed=21))
        lk.drop_link()
        st = self.wait_status(lk, lambda s: s["state"] == VALID, 8)
        self.assertEqual(st["state"], VALID)
        deadline = time.time() + 10
        while "mark: the keep failed 8 times" not in srv.log() and time.time() < deadline:
            time.sleep(0.1)
        self.assertGreaterEqual(srv.log().count("rollback verdict not kept"), 2, srv.log()[-1500:])

    def test_an_early_app_confirm_is_held_until_healthy(self) -> None:
        srv = self.server("--healthy-ms", "2500")
        lk = srv.link()
        self.upload_test_reset(lk, envelope("5.1.0", 30000, seed=13))
        c = sl.Client(lk)
        c.check(c.confirm(), "confirm")   # rc 0 at once
        st = lk.status()
        self.assertEqual((st["state"], st["healthy"], st["confirms"]), (PENDING, False, 0))   # held for the mark
        st = self.wait_status(lk, lambda s: s["state"] == VALID, 6)
        self.assertEqual((st["state"], st["confirms"]), (VALID, 1))

    def test_writes_answer_before_healthy_and_an_unhealthy_test_boot_reverts(self) -> None:
        # A boot that never gets healthy (SAFE mode before its mark, say) still takes a
        # whole update; the test image it boots never confirms, and the next reset
        # brings the old one back.
        srv = self.server("--healthy-ms", "-1")
        lk = srv.link()
        self.upload_test_reset(lk, envelope("5.2.0", 30000, seed=14))
        st = lk.status()
        self.assertEqual((st["running"], st["state"], st["healthy"]), ("5.2.0+0", PENDING, False))
        c = sl.Client(lk)
        lk.disconnected.clear()
        c.reset()   # answered before healthy too
        self.assertTrue(lk.disconnected.wait(5))
        self.assertTrue(lk.connect(10))
        st = lk.status()
        self.assertEqual((st["running"], st["slot"], st["confirms"]), ("1.0.0+0", 0, 0))


class Memory(EspServerBase):
    """Update support holds nothing between updates; the core's memory comes
    from mem_get only while an update runs, and goes back after it."""

    def test_nothing_held_at_boot_or_after_an_update(self) -> None:
        srv = self.server("--healthy-ms", "300")
        lk = srv.link()
        st = lk.status()
        self.assertEqual((st["held"], st["mem_state"], st["mem_buffers"]), (0, 0, 0))
        sl.Client(lk).image_state()   # a read takes memory for its answer only
        r = sl.AndroidFlow(lk, envelope("8.0.0", 30000, seed=21)).run()
        self.assertTrue(r.ok, r.failure)
        st = self.wait_status(lk, lambda s: s["state"] == VALID)
        lk.drop_link()
        st = self.wait_status(lk, lambda s: s["held"] == 0, 5)
        self.assertEqual((st["held"], st["mem_state"], st["mem_buffers"]), (0, 0, 0), st)
        self.assertGreater(st["mem_gets"], 0)

UPDATE_V2 = ROOT.parent / "protocols" / "test-vectors" / "mg-update-v2.json"


class UpdateV2Vectors(EspServerBase):
    """protocols/test-vectors/mg-update-v2.json (Bluetooth updates from a link without the proof take one physical
    press, set up or not, after Forget too) replayed byte for byte through mg_ble.c's request front (mg_smp_front.c,
    the real muse_local_press core, the slot's running SHA-256) and the real core behind it (the server's --gate):
    rc 10 while the gadget asks, 0 after the press, 11 for an upload naming no SHA-256, a chunk with no upload
    started on its link, a write past the bound length, a last chunk or state write while the slot isn't the bound
    image, and once after the prompt timed out or was cancelled (a cancel binds nothing); a press on an erase, a reset
    or a state write covers that request on its link only; Forget ends the binding and the link's proof; an injected
    press never confirms; a proven link, the wired console's arm, a button recovery and bench or automation builds
    need no press; a self-entered recovery has no way to prompt (rc 11); a passcode lock comes first; a new link asks
    again. An allowed write the vectors answer 0 is anything but 10 or 11 here (the real core then checks the
    request itself: the vectors' 64-byte image is too short for it, rc 3)."""

    def test_the_vectors(self) -> None:
        data = json.loads(UPDATE_V2.read_text())
        self.assertEqual(data["prompt_timeout_ms"], 30000)
        self.assertEqual(len(data["exchanges"]), 26)
        self.assertEqual(data["confirm_window_ms"], 600000)
        self._replay(data["exchanges"])

    def test_a_set_up_gadget_whose_phone_is_away(self) -> None:
        # A web updater updates a set-up gadget with no phone, unproven, with only the press; a press before the
        # update's prompt doesn't count, and with no press it ends refused. (Confidential reads stay refused
        # unproven: mg_proto_harness's output-proof and clips rows, unchanged.)
        upload = json.loads(UPDATE_V2.read_text())["exchanges"][0]["steps"][1]["smp"]
        self._replay([{"name": "a set-up gadget whose phone is away", "device": {"proof": False}, "steps": [
            {"event": "press", "note": "a press before any update: no prompt shows, it doesn't count"},
            {"smp": upload, "rc": 10, "note": "the upload starts: rc 10, the prompt shows"},
            {"event": "time", "ms": 30000, "note": "no press: the prompt ends"},
            {"smp": upload, "rc": 11, "note": "refused"},
            {"smp": upload, "rc": 10, "note": "a new attempt: the prompt again"},
            {"event": "press", "note": "the press during the prompt"},
            {"smp": upload, "rc": 0, "note": "the upload runs, no phone, no token"},
        ]}])

    def test_proof_gates_an_update_press_opens_no_window(self) -> None:
        # mg-session-v1.json proof_gates, "an update press opens no window": its smp steps, the press and the injected
        # presses through the server's front and core (the press confirms that upload and nothing else; another image
        # waits, rc 10). Its re-setup steps, on the same gadget: test_link_gadget_setup.
        session = json.loads(UPDATE_V2.with_name("mg-session-v1.json").read_text())
        ex = next(e for e in session["proof_gates"]["exchanges"] if e["name"] == "an update press opens no window")
        steps = [st for st in ex["steps"] if st.get("event") != "resetup_request"]
        self.assertEqual([st.get("event") or st["rc"] for st in steps], [10, "press", 0, 10, "injected_press",
                                                                        "injected_press"])
        self._replay([{"name": ex["name"], "device": {"proof": False}, "steps": steps}])

    def _replay(self, cases: list) -> None:
        events = {"press": 1, "proof": 3, "cancel": 4, "unlock": 5, "injected_press": 7, "forget": 8, "set_up": 9}
        for case in cases:
            with self.subTest(case=case["name"]):
                dev = case["device"]
                srv = self.server("--gate")
                lk = srv.link(247)
                lk.sock.sendto(bytes([0x0C, dev.get("holds_token", True), dev.get("release", True),
                                      dev.get("button_recovery", False), dev.get("console_arm", False),
                                      dev.get("locked", False), dev.get("self_recovery_unprompted", False)]), lk.addr)
                if dev.get("proof"):
                    lk.sock.sendto(b"\x0d\x03", lk.addr)
                time.sleep(0.05)
                for i, st in enumerate(case["steps"]):
                    ev = st.get("event")
                    if ev == "time":
                        lk.sock.sendto(b"\x0d\x06" + struct.pack("<I", st["ms"]), lk.addr)
                        time.sleep(0.05)
                        continue
                    if ev == "reconnect":
                        lk.drop_link()
                        time.sleep(0.1)
                        self.assertTrue(lk.connect(10))
                        continue
                    if ev:
                        self.assertIn(ev, events, f"mg-update-v2.json: an event this replay doesn't know: {ev!r}")
                        lk.sock.sendto(bytes([0x0D, events[ev]]), lk.addr)
                        time.sleep(0.05)
                        continue
                    frame = bytes.fromhex(st["smp"])
                    while not lk.responses.empty():
                        lk.responses.get_nowait()
                    lk.disconnected.clear()
                    lk.write_message(frame)
                    try:
                        rsp = lk.responses.get(timeout=3)
                    except queue.Empty:
                        self.fail(f"step {i} ({st.get('note')}): no answer; {srv.log()[-1200:]}")
                    self.assertEqual(rsp[6], frame[6])
                    body = sl.cbor_decode(rsp[8:])
                    rc = body.get("rc", 0) if isinstance(body, dict) else 0
                    if isinstance(body, dict) and isinstance(body.get("err"), dict):
                        rc = body["err"].get("rc", rc)
                    if st["rc"] in (10, 11):
                        self.assertEqual(rc, st["rc"], (i, st.get("note"), body, srv.log()[-800:]))
                    else:
                        self.assertNotIn(rc, (10, 11), (i, st.get("note"), body, srv.log()[-800:]))
                    if frame[4:6] == b"\x00\x00" and frame[7] == 5 and st["rc"] == 0 and i + 1 < len(case["steps"]):
                        # a reset the gate allowed: the real device restarts; the next steps run on the new link
                        self.assertTrue(lk.disconnected.wait(5))
                        self.assertTrue(lk.connect(10))
                srv.stop()
                self.servers.remove(srv)


class NestingDepth(EspServerBase):
    """Every SMP request is checked for its CBOR nesting before any decoder sees it (mg_smp_front.c
    mg_smp_front_depth_ok, iterative, MG_SMP_FRONT_MAX_DEPTH 8, the body's map counting as one), proven or not, update
    or not: 8 deep reaches the core, 9 deep is refused rc 3, and a ~500-deep 509-byte echo (the zcbor recursion
    #527's review found on the XIAO) is refused rc 3 with nothing crashing (ASan). The core's own skip is bounded too
    (smp/mg_smp.c cr_skip_body, depth > 8)."""

    maxDiff = None

    @staticmethod
    def request(op: int, group: int, cmd: int, body: bytes) -> bytes:
        return bytes([op, 0]) + struct.pack(">HH", len(body), group) + bytes([0, cmd]) + body

    def ask(self, srv: Server, lk: sl.Link, frame: bytes):
        while not lk.responses.empty():
            lk.responses.get_nowait()
        lk.write_message(frame)
        try:
            rsp = lk.responses.get(timeout=3)
        except queue.Empty:
            self.fail(f"no answer; {srv.log()[-1200:]}")
        return sl.cbor_decode(rsp[8:])

    @staticmethod
    def truncating_payload() -> bytes:
        """{"d": "x", "z": <a text string whose 8-byte length is 2^32>, "y": [[...480 deep...]]}: a 32-bit decoder
        reads the length as 0 and goes on into the nest."""
        return (bytes([0xa3, 0x61, ord("d"), 0x61, ord("x"), 0x61, ord("z"), 0x7b]) + (1 << 32).to_bytes(8, "big")
                + bytes([0x61, ord("y")]) + bytes([0x81]) * 480 + b"\x00")

    def test_the_walker_fails_closed(self) -> None:
        # mg_smp_front_depth_ok itself: well-formed bodies to 8 deep pass (a definite map as iOS sends, an indefinite
        # one as Android does, an empty body); everything it can't walk to the end is refused, whatever the host's
        # size_t: lengths, counts and tags past 32 bits (their low 32 bits made to fit what's left, as a truncating
        # 32-bit decoder would read them), a length or count past the request, a cut-short item, a stray break, a
        # wrong chunk in a chunked string, bytes after the body's item.
        def m(*pairs) -> bytes:   # a definite map of text keys to raw CBOR values
            return bytes([0xa0 + len(pairs)]) + b"".join(bytes([0x60 + len(k)]) + k.encode() + v for k, v in pairs)
        u64 = lambda major, v: bytes([major << 5 | 27]) + v.to_bytes(8, "big")   # noqa: E731
        cases = {
            "empty": (b"", 1),
            "ios upload": (sl.cbor_encode({"image": 0, "len": 100, "off": 0, "sha": bytes(32), "data": bytes(50)}), 1),
            "android upload": (sl.cbor_encode({"off": 50, "data": bytes(50)}, indefinite=True), 1),
            "depth 8": (m(("x", bytes([0x81]) * 7 + b"\x00")), 1),
            "depth 9": (m(("x", bytes([0x81]) * 8 + b"\x00")), 0),
            "truncating length, deep nest": (self.truncating_payload(), 0),
            "text length 2^32 + 3, 3 bytes left": (m(("z", u64(3, (1 << 32) + 3) + b"abc")), 0),
            "byte length 2^32 + 3, 3 bytes left": (m(("z", u64(2, (1 << 32) + 3) + b"abc")), 0),
            "array count 2^32 + 1, 1 item": (m(("z", u64(4, (1 << 32) + 1) + b"\x00")), 0),
            "map count 2^32 + 1, 1 pair": (m(("z", u64(5, (1 << 32) + 1) + b"\x00\x00")), 0),
            "tag 2^32": (m(("z", u64(6, 1 << 32) + b"\x00")), 0),
            "length past the request": (m(("z", bytes([0x58, 40]) + bytes(10))), 0),
            "count past the request": (m(("z", bytes([0x98, 40]) + bytes(10))), 0),
            "a pair missing": (bytes([0xa2, 0x61, ord("x"), 0x00]), 0),
            "a head cut short": (bytes([0xa1, 0x61, ord("x"), 0x1a, 0x00]), 0),
            "a stray break": (m(("x", b"\xff")), 0),
            "a chunk of the wrong kind": (m(("x", b"\x7f\x41a\xff")), 0),
            "bytes after the body": (m(("x", b"\x00")) + b"\x00", 0),
            "an 8-byte integer": (m(("x", u64(0, (1 << 64) - 1))), 1),
        }
        drv = r"""
#include <stdio.h>
#include <string.h>
#include "mg_smp_front.h"
int main(int argc, char **argv) {
    static uint8_t f[1100];
    FILE *in = argc > 1 ? fopen(argv[1], "r") : NULL;
    char line[2400];
    while (in && fgets(line, sizeof line, in)) {
        size_t n = strlen(line) / 2;
        for (size_t i = 0; i < n; i++) { unsigned b; sscanf(line + 2 * i, "%2x", &b); f[i] = (uint8_t)b; }
        printf("%d\n", mg_smp_front_depth_ok(f, n));
    }
    return in ? 0 : 1;
}
"""
        with tempfile.TemporaryDirectory() as t:
            Path(t, "d.c").write_text(drv)
            frames = [NestingDepth.request(2, 0, 0, body) for body, _ in cases.values()]
            Path(t, "in.txt").write_text("".join(f.hex() + "\n" for f in frames))
            r = subprocess.run([*cc(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                                "-I", str(MG), "-I", str(SMP), "-I", str(ROOT / "components" / "muse_restart" / "include"),
                                str(Path(t, "d.c")), str(MG / "mg_smp_front.c"), str(SMP / "mg_smp_sha256.c"),
                                "-o", str(Path(t, "d"))], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(Path(t, "d")), str(Path(t, "in.txt"))], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
        got = dict(zip(cases, (int(x) for x in r.stdout.split())))
        self.assertEqual(got, {k: v for k, (_, v) in cases.items()})

    def test_depth(self) -> None:
        nested = lambda k: bytes([0xa1, 0x61, ord("x")]) + bytes([0x81]) * k + b"\x00"   # noqa: E731
        for args in ((), ("--gate",)):
            with self.subTest(args=args):
                srv = self.server(*args)
                lk = srv.link(247)
                self.assertIn("images", self.ask(srv, lk, self.request(0, 1, 0, nested(7))))   # 8 deep: image list
                self.assertEqual(self.ask(srv, lk, self.request(0, 1, 0, nested(8))), {"rc": 3})   # 9 deep
                deep = self.request(0, 0, 0, nested(497))
                self.assertLessEqual(len(deep), 512)
                self.assertEqual(self.ask(srv, lk, deep), {"rc": 3})   # ~500 deep: refused, no crash
                self.assertEqual(self.ask(srv, lk, self.request(2, 1, 1, nested(497))), {"rc": 3})   # an upload too
                # #527's zcbor finding: an 8-byte length a 32-bit decoder truncates to 0, then a deep nest behind it
                self.assertEqual(self.ask(srv, lk, self.request(2, 0, 0, self.truncating_payload())), {"rc": 3})
                self.assertIn("images", self.ask(srv, lk, self.request(0, 1, 0, b"")))   # and it still answers
                srv.stop()   # no AddressSanitizer report
                self.servers.remove(srv)


class NoSmuggling(EspServerBase):
    """A request whose header declares more than the request front holds (MG_SMP_FRONT_MAX, the core's slot) is
    answered EMSGSIZE from that header alone, and the rest of its bytes are dropped by the front, never forwarded: a
    request embedded in them (an upload start with no SHA-256, a state write, the reset) reaches no decoder and gets
    no answer, so it can't start a transfer past the front's checks (#525 council: the front used to pass those bytes
    on, where the core parsed them as new requests). With no hooks, and with --gate on a proven and an unproven link.
    """

    @staticmethod
    def frame(group: int, cmd: int, body: dict, seq: int) -> bytes:
        b = sl.cbor_encode(body)
        return bytes([2, 0]) + struct.pack(">HH", len(b), group) + bytes([seq, cmd]) + b

    def responses(self, lk: sl.Link, wait: float = 0.8) -> list:
        time.sleep(wait)
        out = []
        while not lk.responses.empty():
            out.append(lk.responses.get_nowait())
        return out

    def test_an_oversized_header_carries_nothing_in(self) -> None:
        env = envelope("3.0.0", 3000, seed=31)
        embedded = {
            "upload start, no SHA-256": self.frame(1, 1, {"image": 0, "len": len(env), "off": 0, "data": env[:100]}, 7),
            "state write": self.frame(1, 0, {"hash": bytes(32), "confirm": True}, 7),
            "reset": self.frame(0, 5, {}, 7),
        }
        oversized = bytes([2, 0]) + struct.pack(">HH", 1092, 1) + bytes([3, 1])   # an upload declaring 1100 bytes
        for mode, args, proven in (("no hooks", (), False), ("--gate, proven", ("--gate",), True),
                                   ("--gate, unproven", ("--gate",), False)):
            for name, inner in embedded.items():
                with self.subTest(mode=mode, embedded=name):
                    srv = self.server(*args)
                    lk = srv.link(247)
                    if proven:
                        lk.sock.sendto(b"\x0d\x03", lk.addr)
                    time.sleep(0.05)
                    before = lk.status()
                    while not lk.responses.empty():
                        lk.responses.get_nowait()
                    lk.disconnected.clear()
                    lk.write(oversized)
                    lk.write(inner)   # a whole request of its own, in the oversized one's bytes
                    lk.write(bytes(40))   # more of them
                    got = self.responses(lk)
                    self.assertEqual([r[6] for r in got], [3], (name, [r.hex() for r in got], srv.log()[-800:]))
                    self.assertNotEqual(sl.cbor_decode(got[0][8:]).get("rc", 0), 0)   # EMSGSIZE, from the header
                    self.assertFalse(lk.disconnected.is_set(), "the embedded reset ran")
                    st = lk.status()
                    self.assertFalse(st["ota_open"], st)
                    self.assertEqual((st["boots"], st["auth_asks"]), (before["boots"], before["auth_asks"]), st)
                    # Once the oversized request's bytes are all in, the next request is read as one.
                    left = 1092 - len(inner) - 40
                    while left:
                        k = min(left, 200)
                        lk.write(bytes(k))
                        left -= k
                    self.assertEqual(self.responses(lk, 0.3), [])
                    c = sl.Client(lk)
                    self.assertTrue(c.image_state())
                    srv.stop()
                    self.servers.remove(srv)


class Smuggling(EspServerBase):
    """The core's smp-v12.1 Smuggling rows (smp/conformance/test_conformance.py, tag smp-v12.1), ported to this
    port's front and its smp-v11 core with the local fix (smp/VENDORED.md). (2) The rest of a request answered from
    its header alone (too large to hold, or no memory for it) is never parsed, whatever it holds. (1) An allowed
    request the core then refuses carries nothing: no other upload goes on unasked on that link. And a reset after a
    failed upload is asked. Differences from v12.1, by design here: the front reads a request that follows an
    oversized one in the same write as a request of its own, through every front check (v12.1 drops the rest of that
    write), so the oversized row's last write ends with the request; an upload request with no data is refused by the
    front (rc 11) before the core sees it (v12.1's core answers rc 3); the swap row holds by the front's rule that a
    chunk on a link the press opened goes on only for the bound upload (mg_smp_front_gate: smp-v11 would let the held
    upload's chunk through and carry it); the server has one gated mode (--gate) for
    v12.1's --g95b and --auth unproven --confirm. TODO (the smp-v12.1 re-vendor PR): v12.1's "the unasked state write
    names the carried upload" row; smp-v11 asks no hook for any state write on the link carrying a staged upload."""

    @staticmethod
    def drain(lk: sl.Link, wait: float = 0.5) -> list:
        time.sleep(wait)
        out = []
        while not lk.responses.empty():
            out.append(lk.responses.get_nowait())
        return out

    @staticmethod
    def rc_of(frame: bytes) -> int:
        return sl.cbor_decode(frame[8:]).get("rc", 0)

    @staticmethod
    def embedded(c: sl.Client, img: bytes) -> list:
        """Real requests: an upload start, a state write for that image, a reset."""
        return [c.frame(2, 1, 1, {"off": 0, "len": len(img), "sha": hashlib.sha256(img).digest(), "data": img[:100]})[1],
                c.frame(2, 1, 0, {"hash": sl.image_hash(img), "confirm": False})[1],
                c.frame(2, 0, 5, {})[1]]

    def assert_untouched(self, lk: sl.Link, before: dict) -> None:
        st = lk.status()
        self.assertTrue(lk.connected.is_set(), "a reset ran")
        self.assertFalse(st["ota_open"], "an upload started")
        self.assertFalse(st["claimed"])
        self.assertEqual((st["running"], st["boots"]), (before["running"], before["boots"]))

    def test_the_rest_of_an_oversized_request_is_never_parsed(self) -> None:
        img = envelope("5.0.0", 3000, seed=51)
        for hooks, proof in (((), None), (("--auth", "proven"), None), (("--gate",), True), (("--gate",), False)):
            with self.subTest(hooks=hooks, proof=proof):
                srv = self.server(*hooks)
                lk = srv.link(247)
                if proof:
                    lk.sock.sendto(b"\x0d\x03", lk.addr)
                before = lk.status()   # after the proof (the server takes the link's datagrams in order)
                c = sl.Client(lk)
                up, st, rs = self.embedded(c, img)
                w = lk.max_write
                fill = bytes([0xA0]) * w
                # the body: each real request starts a write; the oversized request ends mid-write
                parts = [fill[:w - 8], up, fill, st, rs, fill, fill, fill]
                tail = fill[:50]
                body_len = sum(map(len, parts)) + len(tail)
                self.assertGreater(8 + body_len, 1024)   # more than the front holds (the core's slot)
                hdr = struct.pack(">BBHHBB", 2, 0, body_len, 1, 0x77, 1)
                for x in [hdr + parts[0]] + parts[1:] + [tail]:
                    lk.write(x)
                    time.sleep(0.01)
                got = self.drain(lk)
                self.assertEqual(len(got), 1, f"answered {len(got)} times: the tail was parsed")
                self.assertEqual((got[0][6], self.rc_of(got[0])), (0x77, 7), "EMSGSIZE for the oversized request")
                self.assert_untouched(lk, before)
                self.assertTrue(c.image_state())   # the next request is read as one
                time.sleep(0.3)
                self.assertEqual(lk.status()["held"], 0)
                srv.stop()
                self.servers.remove(srv)

    def test_the_rest_of_a_request_refused_for_memory_is_never_parsed(self) -> None:
        img = envelope("5.1.0", 20000, seed=52)
        srv = self.server()
        lk = srv.link(247)
        before = lk.status()
        c = sl.Client(lk)
        rsp = c.request(2, 1, 1, {"off": 0, "len": len(img), "data": img[:200]})
        self.assertEqual(c.rc(rsp), 0, rsp)
        lk.drop_link()   # the upload's state stays for its resume, its buffers go
        self.assertTrue(lk.connect(5))
        self.assertGreater(lk.status()["mem_state"], 0)
        c = sl.Client(lk)
        w = lk.max_write
        rs = c.frame(2, 0, 5, {})[1]
        # one well-formed item, so the front's walk hands it to the core: {"x": <bytes>}, with a real reset starting
        # the second write inside the byte string
        inner = bytes([0xA0]) * (w - 8 - 6) + rs + bytes(20)
        body = bytes([0xA1, 0x61, ord("x"), 0x59]) + struct.pack(">H", len(inner)) + inner
        hdr = struct.pack(">BBHHBB", 2, 0, len(body), 1, 0x55, 1)
        lk.mem_fail(1)   # the request buffers are refused
        time.sleep(0.02)
        lk.write(hdr + body[:w - 8])
        time.sleep(0.05)
        lk.write(body[w - 8:])   # a real reset starts this write
        got = self.drain(lk)
        self.assertEqual([(g[6], self.rc_of(g)) for g in got], [(0x55, 2)], "ENOMEM once, the rest not parsed")
        self.assertTrue(lk.connected.is_set(), "the reset in the refused request's tail ran")
        self.assertEqual(lk.status()["running"], before["running"])
        self.assertTrue(c.image_state())

    def test_an_allowed_request_the_core_refuses_carries_nothing(self) -> None:
        # B starts with no SHA-256 on a proven link and is held; on a new, unproven link a press binds A; a request
        # for A the core would refuse (no data) must not make that link carry B.
        a, b = envelope("5.2.0", 20000, seed=53), envelope("5.3.0", 20000, seed=54)
        srv = self.server("--gate")
        lk = srv.link(247)
        lk.sock.sendto(b"\x0d\x03", lk.addr)   # proven
        before = lk.status()
        c = sl.Client(lk)
        rsp = c.request(2, 1, 1, {"off": 0, "len": len(b), "data": b[:200]})
        self.assertEqual(c.rc(rsp), 0, rsp)
        off = rsp["off"]
        lk.drop_link()
        self.assertTrue(lk.connect(5))   # a new link: unproven
        c = sl.Client(lk)
        first_a = {"off": 0, "len": len(a), "sha": hashlib.sha256(a).digest(), "data": a[:200]}
        self.assertEqual(c.rc(c.request(2, 1, 1, first_a)), 10, "the press prompt for A")
        lk.sock.sendto(b"\x0d\x01", lk.addr)   # the press
        lk.status()
        rsp = c.request(2, 1, 1, {"off": 0, "len": len(a), "sha": hashlib.sha256(a).digest()})   # no data
        self.assertNotEqual(c.rc(rsp), 0, rsp)   # the front refuses it (11); v12.1's core answers 3
        rcs = []
        while off < len(b):
            rsp = c.request(2, 1, 1, {"off": off, "data": b[off:off + 200]})
            rcs.append(c.rc(rsp))
            if rcs[-1]:
                break
            off = rsp["off"]
        self.assertEqual(rcs[0], 11, "B's chunk on the new link is lone: refused")
        self.assertNotEqual(c.rc(c.request(2, 1, 0, {"hash": sl.image_hash(b), "confirm": False})), 0)
        lk.disconnected.clear()
        self.assertNotEqual(c.rc(c.request(2, 0, 5, {})), 0, "the reset was not asked")
        self.assertFalse(lk.disconnected.wait(0.5))
        st = lk.status()
        self.assertEqual((st["running"], st["boots"]), (before["running"], before["boots"]))
        self.assertFalse(any(i.get("pending") for i in c.image_state()), "something was staged")

    def test_a_reset_after_a_failed_upload_is_asked(self) -> None:
        srv = self.server("--auth", "unproven")
        lk = srv.link(247)
        lk.sock.sendto(b"\x09\x01", lk.addr)   # proven
        self.assertEqual(lk.status()["auth"], 1)
        img = bytearray(envelope("5.4.0", 20000, seed=55))
        img[1000] ^= 1   # the payload no longer matches its hash
        c = sl.Client(lk)
        rsp = c.request(2, 1, 1, {"off": 0, "len": len(img), "data": bytes(img[:200])})
        while c.rc(rsp) == 0 and rsp["off"] < len(img):
            o = rsp["off"]
            rsp = c.request(2, 1, 1, {"off": o, "data": bytes(img[o:o + 200])})
        self.assertEqual(c.rc(rsp), 9)
        lk.sock.sendto(b"\x09\x00", lk.addr)   # the proof gone
        self.assertEqual(lk.status()["auth"], 0)
        lk.disconnected.clear()
        self.assertEqual(c.rc(c.request(2, 0, 5, {})), 11, "no upload is carried any more: the reset is asked")
        self.assertFalse(lk.disconnected.wait(0.5))


class TheCoreMarksOnlyWhatItTook(EspServerBase):
    """The vendored core's local change (smp/VENDORED.md): a request the hooks allowed marks the transfer as this
    connection's (F_XAUTH: its later chunks ask no hook) only once the core took it. A held upload, resumed on a new
    link: a request the hook allowed but the core refused (an upload start whose len is too short) leaves no mark, so
    the held upload's next chunk, after the link's proof is gone, is asked about and refused (rc 11); smp-v11 let it
    through unasked. And the front keeps the held upload's length past an upload start the core refuses (a len under
    32 + 40): its next chunk isn't taken as one past the length."""

    def ask(self, srv: Server, lk: sl.Link, body: dict) -> dict:
        b = sl.cbor_encode(body)
        return NestingDepth.ask(self, srv, lk, bytes([2, 0]) + struct.pack(">HH", len(b), 1) + bytes([0, 1]) + b)

    def test_a_refused_request_leaves_no_mark(self) -> None:
        env = envelope("4.0.0", 3000, seed=41)
        sha = hashlib.sha256(env).digest()
        srv = self.server("--auth", "unproven")
        lk = srv.link(247)
        lk.sock.sendto(b"\x09\x01", lk.addr)   # proven
        time.sleep(0.05)
        self.assertNotIn("rc", self.ask(srv, lk, {"image": 0, "len": len(env), "off": 0, "sha": sha, "data": env[:400]}))
        self.assertEqual(self.ask(srv, lk, {"off": 400, "data": env[400:800]}).get("off"), 800)
        lk.drop_link()   # the upload is held for its resume
        time.sleep(0.1)
        self.assertTrue(lk.connect(10))
        lk.sock.sendto(b"\x09\x01", lk.addr)   # proven on the new link
        time.sleep(0.05)
        # the held upload's start again, but a 16-byte SHA-256: the hook allows it, the core refuses it (EINVAL)
        self.assertEqual(self.ask(srv, lk, {"image": 0, "len": len(env), "off": 0, "sha": sha[:16], "data": env[:5]}),
                         {"rc": 3})
        lk.sock.sendto(b"\x09\x00", lk.addr)   # the proof gone
        time.sleep(0.05)
        rsp = self.ask(srv, lk, {"off": 800, "data": env[800:1200]})
        self.assertEqual(rsp, {"rc": 11}, srv.log()[-800:])
        self.assertNotIn("past its upload's length", srv.log())

    def test_a_refused_start_keeps_the_held_length(self) -> None:
        env = envelope("4.1.0", 3000, seed=42)
        sha = hashlib.sha256(env).digest()
        srv = self.server("--auth", "proven")
        lk = srv.link(247)
        self.assertNotIn("rc", self.ask(srv, lk, {"image": 0, "len": len(env), "off": 0, "sha": sha, "data": env[:400]}))
        self.assertEqual(self.ask(srv, lk, {"image": 0, "len": 10, "off": 0, "data": env[:5]}), {"rc": 3})
        rsp = self.ask(srv, lk, {"off": 400, "data": env[400:800]})
        self.assertEqual(rsp.get("off"), 800, (rsp, srv.log()[-800:]))
        self.assertNotIn("past its upload's length", srv.log())


class PastTheLength(EspServerBase):
    """An image upload write past its upload's length gets rc 11 and ends the upload, however the link was opened
    (smp-v12 on a gated platform; mg_smp_front.c mg_smp_front_past_len, ahead of the vendored smp-v11 core, which
    drops the extra bytes and answers rc 0): a last chunk 8 bytes too long on a proven link and on a bench build, and
    an offset-0 request whose data is longer than its own len. A chunk within the length goes on as before."""

    def ask(self, srv: Server, lk: sl.Link, body: dict) -> dict:
        b = sl.cbor_encode(body)
        frame = bytes([2, 0]) + struct.pack(">HH", len(b), 1) + bytes([0, 1]) + b
        self.assertLessEqual(len(frame), 512)
        return NestingDepth.ask(self, srv, lk, frame)

    def test_past_the_length(self) -> None:
        env = envelope("2.0.0", 3000, seed=21)
        sha = hashlib.sha256(env).digest()
        for mode in ("proven", "bench"):
            with self.subTest(mode=mode):
                srv = self.server("--gate")
                lk = srv.link(247)
                if mode == "bench":   # a bench or automation build: release false, no press
                    lk.sock.sendto(bytes([0x0C, 1, 0, 0, 0, 0, 0]), lk.addr)
                else:   # the link's token proof matched
                    lk.sock.sendto(b"\x0d\x03", lk.addr)
                time.sleep(0.05)
                step = 400
                self.assertNotIn("rc", self.ask(srv, lk, {"image": 0, "len": len(env), "off": 0, "sha": sha,
                                                          "data": env[:step]}))
                off = step
                while off + step < len(env):
                    self.assertEqual(self.ask(srv, lk, {"off": off, "data": env[off:off + step]}).get("rc", 0), 0)
                    off += step
                self.assertTrue(lk.status()["ota_open"])
                rsp = self.ask(srv, lk, {"off": off, "data": env[off:] + bytes(8)})
                self.assertEqual(rsp.get("rc", 0), 11, (rsp, srv.log()[-800:]))   # past the length: refused
                st = lk.status()
                self.assertFalse(st["ota_open"], st)   # the upload ended: its OTA session closed
                self.assertNotEqual(self.ask(srv, lk, {"off": off, "data": env[off:]}).get("rc", 0), 0)
                self.assertIn("past its upload's length", srv.log())
                # An offset-0 request whose data is longer than its own len.
                self.assertEqual(self.ask(srv, lk, {"image": 0, "len": 100, "off": 0, "sha": sha,
                                                    "data": env[:120]}), {"rc": 11})
                srv.stop()
                self.servers.remove(srv)


class NoRollbackBootloader(EspServerBase):
    """A unit whose bootloader was built without rollback: proven on its first
    test boot, then every commit says permanent."""

    def test_verdict_and_truthful_state(self) -> None:
        srv = self.server("--no-rollback", "--healthy-ms", "200")
        lk = srv.link()
        self.assertEqual(lk.status()["rollback"], RB_UNPROVEN)
        r = sl.AndroidFlow(lk, envelope("6.0.0", 30000, seed=15)).run()
        self.assertTrue(r.ok, r.failure)
        st = lk.status()
        self.assertEqual((st["running"], st["state"], st["rollback"], st["can_revert"]), ("6.0.0+0", NEW, RB_ABSENT, False))
        self.assertIn("NO rollback", srv.log())
        # The next update: its commit is permanent, and image state says so.
        env = envelope("6.1.0", 30000, seed=16)
        c = sl.Client(lk)
        sl.AndroidFlow(lk, env).upload(sl.Result())
        slots = c.check(c.test(sl.image_hash(env)), "test")["images"]
        staged = next(s for s in slots if s["slot"] == 1)
        self.assertTrue(staged["pending"] and staged["permanent"], slots)


class EspUtil(unittest.TestCase):
    def test_version_build_id_and_verdict(self) -> None:
        drv = r'''
#include <stdio.h>
#include "mg_smp_esp.h"
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        mg_smp_version_t v;
        mg_smp_esp_parse_version(argv[i], &v);
        printf("%u.%u.%u+%u\n", v.major, v.minor, v.revision, (unsigned)v.build);
    }
    unsigned char sha[32] = { 0x78, 0x56, 0x34, 0x12 };
    printf("%08x\n", (unsigned)mg_smp_esp_build_id(sha));
    int states[] = { 0, 1, 2, -1 };
    for (int s = 0; s < 4; s++)
        printf("%d %d %d\n", mg_smp_esp_verdict(states[s], 0, 0), mg_smp_esp_verdict(states[s], 1, 1),
               mg_smp_esp_verdict(states[s], 1, 2));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as t:
            (Path(t) / "d.c").write_text(drv)
            exe = Path(t) / "d"
            subprocess.run([*cc(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MG), "-I", str(SMP),
                            str(Path(t) / "d.c"), str(MG / "mg_smp_esp_util.c"), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe), "1.2.3", "1.2.3+g0123abcd", "v10.20.300.dirty", "1.0.0+gx.dirty",
                                  "999.0.0", "x"], capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(out[:6], ["1.2.3+0", "1.2.3+0", "10.20.300+0", "1.0.0+0", "255.0.0+0", "0.0.0+0"])
        self.assertEqual(out[6], "12345678")
        # the packer reads the same id from the app's descriptor
        app = fake_app(4000)
        sha4 = app[32 + 144:32 + 148]
        self.assertEqual(pack.idf_build_id(app), struct.unpack("<I", sha4)[0])
        rows = [tuple(int(x) for x in out[7 + 3 * i:10 + 3 * i]) for i in range(4)]
        self.assertEqual(rows, [(RB_ABSENT,) * 3, (RB_PROVEN,) * 3, (RB_UNPROVEN, RB_PROVEN, RB_ABSENT),
                                (RB_UNPROVEN, RB_PROVEN, RB_ABSENT)])

    def test_wrap_refuses_an_image_without_smp(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            bare = Path(t) / "app.bin"
            bare.write_bytes(fake_app(5000))   # no SMP service UUID in it
            with self.assertRaises(SystemExit):
                mcuboot_wrap.main([str(bare)])
            ok = Path(t) / "smp.bin"
            uuid_le = bytes.fromhex("8D53DC1D1DB74CD3868B8A527460AA84")[::-1]
            ok.write_bytes(fake_app(5000)[:4000] + uuid_le + bytes(984))
            self.assertEqual(mcuboot_wrap.main([str(ok)]), 0)
            # The envelope is private whatever the umask, an existing file too (the app may carry the owner's SDK
            # token).
            out = Path(t) / "smp.mcuboot.bin"
            out.write_bytes(b"old")
            out.chmod(0o644)
            old = os.umask(0)
            try:
                self.assertEqual(mcuboot_wrap.main([str(ok), str(out)]), 0)
            finally:
                os.umask(old)
            self.assertEqual(out.stat().st_mode & 0o777, 0o600)
            self.assertTrue(pack.info(out.read_bytes())["hash_ok"])

    def test_wrap_is_the_fleet_packer(self) -> None:
        app = fake_app(5000, "3.4.5+gdeadbeef")
        self.assertEqual(mcuboot_wrap.app_version(app), "3.4.5+gdeadbeef")
        env = mcuboot_wrap.wrap(app, mcuboot_wrap.app_version(app))
        self.assertEqual(env, pack.pack(app, (3, 4, 5, 0), pack.idf_build_id(app)))
        info = pack.info(env)
        self.assertTrue(info["hash_ok"])
        self.assertEqual(info["version"], "3.4.5+0")
        self.assertEqual(info["build_id"], "0x%08x" % pack.idf_build_id(app))
        sys.path.insert(0, str(ROOT / "tools"))
        try:
            import mg_ble_client
        except ImportError:
            self.skipTest("mg_ble_client's imports unavailable")
        self.assertEqual(mg_ble_client.mcuboot_image_info(env)["sha256"].hex(), info["tlv_sha256"])


class GlueRing(unittest.TestCase):
    """mg_ble.c's SMP FIFO ring (mg_fifo.h: length-prefixed writes, one producer, one consumer): order,
    wrap-around and a full ring, compiled from the glue itself with mg_lock stubbed (test_mg_ble runs mg_fifo.h
    across its counters' wrap)."""

    def test_order_wraparound_and_full(self) -> None:
        ble = (MG / "mg_ble.c").read_text()
        def grab(start: str) -> str:
            i = ble.index(start)
            return ble[i:ble.index("\n}\n", i) + 3]
        defs = "\n".join(l for l in ble.splitlines() if l.startswith(("#define SMP_BUF", "#define SMP_WINDOW",
                                                                     "#define SMP_WRITE_MAX", "#define SMP_FIFO")))
        st = ble[ble.index("typedef struct {\n    mg_fifo_t idx;"):]
        st = st[:st.index("} smp_fifo_mem_t;") + len("} smp_fifo_mem_t;")]
        src = ("#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
               "#include \"mg_fifo.h\"\n#include \"mg_smp_front.h\"\n"
               "static void mg_lock(void) {}\nstatic void mg_unlock(void) {}\n" + defs + "\n" + st + "\n"
               + grab("static bool fifo_push(") + grab("static int fifo_pop(") + r"""
int main(void) {
    static smp_fifo_mem_t f;
    uint8_t in[600], out[600];
    uint32_t seq_in = 0, seq_out = 0, pushed = 0, popped = 0, full = 0;
    srand(7);
    for (int round = 0; round < 200000; round++) {
        if (rand() % 3) {
            uint16_t n = (uint16_t)(1 + rand() % (SMP_WRITE_MAX + 1));
            for (uint16_t k = 0; k < n; k++) in[k] = (uint8_t)(seq_in * 31 + k);
            if (fifo_push(&f, in, n)) { seq_in++; pushed++; } else full++;
        } else {
            int n = fifo_pop(&f, out, sizeof(out));
            if (n < 0) continue;
            for (int k = 0; k < n; k++) if (out[k] != (uint8_t)(seq_out * 31 + k)) { printf("BAD %u %d\n", seq_out, k); return 1; }
            seq_out++; popped++;
        }
        if (f.idx.head - f.idx.tail > SMP_FIFO) { printf("OVER\n"); return 1; }
    }
    while (fifo_pop(&f, out, sizeof(out)) >= 0) popped++;
    printf("%u %u %u\n", pushed, popped, full);
    return pushed == popped && full > 0 ? 0 : 1;
}
""")
        with tempfile.TemporaryDirectory() as t:
            Path(t, "r.c").write_text(src)
            exe = Path(t, "r")
            subprocess.run([*cc(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I", str(MG), "-I", str(SMP), "-I", str(ROOT / "components" / "muse_restart" / "include"),
                            str(Path(t, "r.c")), "-o", str(exe)], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


class ConfirmedAtHealthy(unittest.TestCase):
    def test_a_test_image_is_not_confirmed_at_boot(self) -> None:
        # With BLE updates a test image is confirmed at the healthy mark, never at boot: an early confirm would
        # keep an image that can't get healthy instead of rolling it back. No host harness runs app.c's boot.
        app = (ROOT / "main" / "app.c").read_text()
        run = app[app.index("run_ble_standalone(void) {"):]
        run = run[:run.index("bool setup_complete")]
        self.assertLess(run.index("#if CONFIG_MUSE_GADGET_BLE_SMP"), run.index("esp_ota_mark_app_valid_cancel_rollback"))


if __name__ == "__main__":
    unittest.main()
