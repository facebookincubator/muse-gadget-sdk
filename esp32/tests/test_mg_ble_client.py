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

"""tools/mg_ble_client.py without a radio: it reads the protocol from
protocols/mgcommands.h, parses notifications, and decodes the firmware's SBC
and LC3 notifications back to the audio that went in."""

from __future__ import annotations

import array
import asyncio
import ctypes
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import mg_ble_client as C  # noqa: E402
from test_mg_ble import MG, PROTOCOLS, ROOT, SEAL_INCS, SEAL_SRCS, codec_build, compile_and_run  # noqa: E402
from test_mg_codecs import best_snr  # noqa: E402


class MicHears(unittest.TestCase):
    """`test` proves the mic hears, at every capture rate listed, and
    fails on exact digital silence even when the frames are valid."""

    def test_capture_list(self):
        b = bytes.fromhex("0101803e3c003c00" "1302803e10006000c05d10006000")
        self.assertEqual(C.capture_rates(list(b)),
                         {1: [(16000, 60, 60)], 19: [(16000, 16, 96), (24000, 16, 96)]})
        self.assertEqual(C.capture_rates(None), {})
        self.assertEqual(C.capture_rates([19, 2, 0x80, 0x3e]), {})   # cut short: nothing

    def run_check(self, prompt_pcm, base_pcm, rates=(16000,), tone_hz=1000.0, label_scale=1.0, tone_amp=4000,
                  tone_step=False):
        import math
        calls = []

        async def fake_capture(g, dec, codec, rate, rate_param, secs):
            calls.append((rate, rate_param))
            k = (len(calls) - 1) % 3   # baseline, prompt, tone
            if k == 0:
                pcm = base_pcm() if callable(base_pcm) else base_pcm
            elif k == 1:
                pcm = prompt_pcm
            else:
                pcm = [int(tone_amp * math.sin(2 * math.pi * tone_hz * i / rate)) for i in range(int(rate * secs))]
                if tone_step:
                    pcm = [30000 if i < rate // 5 else v + 2000 for i, v in enumerate(pcm)]
            return {"rate": int(rate * label_scale)}, list(pcm)

        class R:
            def __init__(self):
                self.results = []

            def check(self, name, ok, detail=""):
                self.results.append((name, ok, detail))
                return ok

        args = type("A", (), {"prompt_cmd": "true", "hear_db": -50.0, "hear_secs": 1.5,
                              "hear_retry_wait": (0.0, 0.0),
                              "out": str(Path(tempfile.mkdtemp()) / "mg_capture.wav")})()
        old, old_tone = C.capture_once, C.tone_prompt
        C.capture_once = fake_capture
        C.tone_prompt = lambda *a, **k: ["true"]   # plays nothing here
        try:
            r = R()
            asyncio.run(C.hear_check(None, r, None, 19, list(rates), args))
        finally:
            C.capture_once, C.tone_prompt = old, old_tone
        return r.results, calls

    def test_tone_carries_the_verdict(self):
        import math
        speech = [int(300 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(24000)]
        hiss = [(-1) ** i * 3 for i in range(24000)]
        res, calls = self.run_check(speech, hiss)
        self.assertTrue(res and all(ok for _, ok, _ in res), res)
        res, _ = self.run_check([0] * 24000, [0] * 24000)
        self.assertFalse(res[0][1])
        self.assertIn("exact digital silence", res[0][2])
        res, _ = self.run_check(speech, hiss, tone_amp=5)   # the tone barely heard: deaf, whatever speech did
        self.assertFalse(res[0][1], res)

    def test_startup_step_and_dc_are_ignored(self):
        import math
        speech = [int(300 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(24000)]
        hiss = [(-1) ** i * 3 for i in range(24000)]
        # a 30 000-count step in the first 0.2 s and a DC offset under the tone: still 1 kHz
        res, _ = self.run_check(speech, hiss, tone_step=True)
        f = [x for x in res if x[0].startswith("1 kHz tone")]
        self.assertTrue(f and f[0][1] and "peak 1000 Hz" in f[0][2], res)

    def test_tone_frequency_and_cut_short(self):
        import math
        speech = [int(300 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(24000)]
        hiss = [(-1) ** i * 3 for i in range(24000)]
        res, _ = self.run_check(speech, hiss, tone_hz=1500.0)   # 16 kHz audio labeled 24000 moves 1 kHz to 1.5
        t = [x for x in res if x[0].startswith("1 kHz tone")]
        self.assertTrue(t and not t[0][1] and "peak 1500 Hz" in t[0][2], res)
        res, _ = self.run_check(speech[:4000], hiss)   # cut short: no mic verdict
        self.assertFalse(res[0][1])
        self.assertIn("capture cut short", res[0][2])


class SimLinkDevice:
    """The Link setup side of a gadget (link_pairing.c's community v5) for one
    connection, token-only provision_v2 included; `on_provision` gets each
    accepted provision_v2 (the gadget's commit). press() is the button. Needs
    the cryptography package."""

    def __init__(self, node_id: str, on_provision=None, wifi: str = "none"):
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

        self.node_id, self.on_provision, self.wifi = node_id, on_provision, wifi
        self.priv = ec.derive_private_key(2, ec.SECP256R1())
        self.pub = C.b64url(self.priv.public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint))
        self.rx = C.SetupReassembler()
        self.notify = None
        self.keys = None
        self.tx_counter = self.rx_counter = 0
        self.mtu_size = 185

    def send(self, obj):
        import json

        for c in C.setup_chunks(json.dumps(obj).encode(), self.mtu_size):
            self.notify(None, bytearray(c))

    def status(self, status):
        import json

        body = json.dumps({"type": "status", "status": status}).encode()
        self.send(C.seal_record(self.keys["rx"], self.keys["session_id"], self.tx_counter, body, 1))
        self.tx_counter += 1

    def press(self):
        self.status("pairing_confirmed")

    def write(self, data: bytes):
        import json

        from cryptography.hazmat.primitives.asymmetric import ec

        msg = self.rx.feed(bytes(data))
        if msg is None:
            return
        m = json.loads(msg)
        act = m.get("action")
        if act == "get_device_info":
            self.send({"type": "device_info", "node_id": self.node_id, "pairing_protocol": 5, "pairing_auth": "none",
                       "pairing_policy": "confirm_press", "mgcommands": 1, "wifi": self.wifi})
        elif act == "pairing_client_hello":
            dn = bytes(range(16, 32))
            ready = {"type": "pairing_ready", "version": 5, "device_id": "hatch-link:02:00:00:00:00:01",
                     "node_id": self.node_id, "mac": "02:00:00:00:00:01", "model": "hatch_link",
                     "firmware_version": "1.0.0", "pairing_auth": "none", "pairing_auth_epoch": 0,
                     "pairing_policy": "confirm_press", "device_pub": self.pub, "device_nonce": C.b64url(dn)}
            th = C.hashlib.sha256(C.pairing_transcript(ready, m["mobile_pub"], m["mobile_nonce"]).encode()).digest()
            peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), C.unb64url(m["mobile_pub"]))
            keys = C.pairing_keys(self.priv.exchange(ec.ECDH(), peer), C.unb64url(m["mobile_nonce"]), dn, th)
            self.keys = {"rx": keys["rx"], "tx": keys["tx"], "session_id": keys["session_id"]}
            self.tx_counter = self.rx_counter = 0   # every hello starts a session
            self.send(dict(ready, transcript_hash=C.b64url(th), session_id=keys["session_id"]))
        elif act == "pairing_encrypted":
            plain = json.loads(C.open_record(self.keys["tx"], self.keys["session_id"], self.rx_counter, m,
                                             direction=0))
            self.rx_counter += 1
            if plain["action"] == "pairing_client_finished":
                self.status("confirm_required")
            elif plain["action"] == "provision_v2":
                if self.on_provision:
                    self.on_provision(plain)
                self.status("auth_ok")


class MgBleClientTest(unittest.TestCase):
    def test_identity_at_connect(self) -> None:
        # "mg_ble_client <version>" (client_app, 19) in one set_settings write, within the
        # header's limits (cut at whole characters), at every connect unless --no-identity. No device
        # name: client_name (18) is reserved, the gadget reads the GAP name itself.
        p = C.identity_payload(version="043d56af")
        self.assertEqual(p, bytes([65, 19, 13]) + b"mg_ble_client" + bytes([8]) + b"043d56af")
        long = C.identity_payload("Caf\u00e9\u2019s " + "\u65e5" * 20, version="v" * 40)
        n = long[2]
        self.assertLessEqual(n, C.P["MG_CLIENT_APP_FIELD_MAX"])
        long[3:3 + n].decode("utf-8")   # whole characters
        self.assertEqual(long[3 + n], C.P["MG_CLIENT_APP_FIELD_MAX"])
        self.assertEqual(C.utf8_cut("a\nb\x00c", 32), b"abc")
        self.assertEqual(C.parse_client_settings(bytes([63]) + p[1:]), {"client_app": ("mg_ble_client", "043d56af")})
        self.assertRegex(C.client_version(), r"^([0-9a-f]{8}|dev)$")
        self.assertIn("client_app=('mg_ble_client', '043d56af')", C.describe(p, outgoing=True))

        class Rec:
            def __init__(self):
                self.writes = []

            async def start_notify(self, uuid, cb):
                pass

            async def write_gatt_char(self, uuid, data, response=False):
                self.writes.append(bytes(data))

        async def connect(listed) -> tuple[list[bytes], list[bytes]]:
            """What start() writes, then what maybe_identify (status(), after request_status) writes, twice."""
            rec = Rec()
            g = C.Gadget(rec, verbose=False)
            await g.start()
            at_start = list(rec.writes)
            await g.maybe_identify(listed)
            await g.maybe_identify(listed)   # once a connection
            return at_start, rec.writes[len(at_start):]

        both = [0, 2, 7, 19]
        start, after = asyncio.run(connect(both))
        self.assertEqual(start, [])        # nothing before request_status
        self.assertEqual(len(after), 1)
        self.assertEqual(after[0][:2], bytes([65, 19]))
        self.assertEqual(asyncio.run(connect([0, 2, 7]))[1], [])        # not listed: not sent
        self.assertEqual(asyncio.run(connect([0, 2, 7, 18]))[1], [])    # an older gadget's 18 alone: nothing
        old = asyncio.run(connect([0, 2, 7, 18, 19]))[1]                # an older gadget listing both: 19 only
        self.assertEqual(len(old), 1)
        self.assertEqual(old[0][:2], bytes([65, 19]))
        self.assertEqual(asyncio.run(connect(None))[1], [])
        try:
            C.IDENTIFY = False   # --no-identity
            self.assertEqual(asyncio.run(connect(both))[1], [])
        finally:
            C.IDENTIFY = True

    def test_mic_bench_words(self) -> None:
        # The output proof's bench switches: --mic-bench sends the board's word, --bench-word any words (repeatable),
        # on the Nordic UART right after connect, before the first command; nothing without them.
        want = {"esp32": "bench.out=1", "xn54": "bench 1"}
        self.assertEqual(C.bench_words(None, []), [])
        self.assertEqual(C.bench_words("xn54", []), ["bench 1"])
        self.assertEqual(C.bench_words("xn54", ["status"]), ["bench 1", "status"])
        self.assertEqual(C.bench_words("esp32", ["status", "ptt.minrise=0"]), ["bench.out=1", "status", "ptt.minrise=0"])
        seen = {}

        def fake_cmd(args):
            seen["words"] = list(C.BENCH_WORDS)

            async def done():
                return 0
            return done()

        saved = (C.cmd_state, C.BENCH_WORDS)
        try:
            C.cmd_state = fake_cmd
            for board, word in want.items():
                self.assertEqual(C.main(["--mic-bench", board, "--bench-word", "status", "--bench-word", "version",
                                         "state", "idle"]), 0)
                self.assertEqual(seen["words"], [word, "status", "version"])
            self.assertEqual(C.main(["state", "idle"]), 0)
            self.assertEqual(seen["words"], [])
            with self.assertRaises(SystemExit):
                C.main(["--mic-bench", "nope", "state", "idle"])
        finally:
            C.cmd_state, C.BENCH_WORDS = saved

        class Rec:
            def __init__(self):
                self.writes = []

            async def start_notify(self, uuid, cb):
                self.writes.append(("notify", uuid))

            async def write_gatt_char(self, uuid, data, response=False):
                self.writes.append((uuid, bytes(data)))

        async def connect_then_command(words):
            rec = Rec()
            C.BENCH_WORDS = words
            g = C.Gadget(rec, verbose=False)
            await g.start()
            await g.send(C.P["mg_command_request_status"])   # the first command
            return rec.writes

        try:
            w = asyncio.run(connect_then_command(C.bench_words("esp32", ["status"])))
        finally:
            C.BENCH_WORDS = []
        writes = [x for x in w if x[0] != "notify"]
        self.assertEqual(writes[0], (C.P["MG_NUS_RX_UUID"], b"bench.out=1\n"))
        self.assertEqual(writes[1], (C.P["MG_NUS_RX_UUID"], b"status\n"))
        self.assertEqual(writes[2][0], C.P["MG_CONTROL_UUID"])   # the command after them
        last_notify = max(i for i, x in enumerate(w) if x[0] == "notify")
        self.assertLess(last_notify, w.index(writes[0]))   # after the connection's subscriptions

    def test_parsers(self) -> None:
        f = C.parse_change_data_type(bytes.fromhex("08 01 803e 01 3c00 401f"))
        self.assertEqual(f, {"type": 1, "rate": 16000, "channels": 1, "frame_bytes": 60, "frame_us": 8000})
        f = C.parse_change_data_type(bytes([8, 19]))
        self.assertEqual((f["frame_bytes"], f["frame_us"]), (40, 10000))
        s = C.parse_settings(bytes.fromhex("3f 00 01 02 01 04 2c01 07 01"))
        self.assertEqual(s, {"spec_version": 1, "push_to_talk_enabled": 1, "ptt_buzz_freq_hz": 300, "audio_codec": 1})
        self.assertEqual(C.parse_error(bytes.fromhex("19 41 01 0100")), "error on set_settings: unsupported")
        self.assertIn("supported_features[start_mic] [1, 19]", C.describe(bytes.fromhex("10 fe 01 01 13")))

    def test_live_call_helpers(self) -> None:
        # Named from the header, in both directions: requests from the gadget, states from the phone.
        self.assertEqual(C.P["mg_command_live_call"], 0x85)
        self.assertEqual(C.describe(bytes.fromhex("10 01 0a 84 85")),
                         "supported_features start_mic request_status stream_video live_call")
        self.assertEqual(C.describe(bytes.fromhex("85 01")), "live_call start")
        self.assertEqual(C.describe(bytes.fromhex("85 00")), "live_call end")
        self.assertEqual(C.describe(bytes.fromhex("85 02"), outgoing=True), "live_call state live")
        self.assertEqual(C.describe(bytes.fromhex("85 04"), outgoing=True), "live_call state failed")
        self.assertEqual([C.live_call_state(n) for n in C.LIVE_CALL_STATES], [0, 1, 2, 3, 4])
        # The stand-in phone: a start gets connecting, then the answer; an end gets ended.
        self.assertEqual(C.live_call_answer(1, "live", 2.0), [(0.0, 1), (2.0, 2)])
        self.assertEqual(C.live_call_answer(1, "failed"), [(0.0, 1), (1.0, 4)])
        self.assertEqual(C.live_call_answer(1, "none"), [])
        self.assertEqual(C.live_call_answer(0), [(0.0, 3)])
        self.assertEqual(C.live_call_answer(7), [])

    def test_ui_stream_helpers(self) -> None:
        # The same bytes test_mg_ble's firmware harness accepts (mg_proto_harness.c, test_ui).
        self.assertEqual(C.video_start_payload(466, 466, 15, 300, 2, ui=True).hex(),
                         "840207011802d20103d201040f052c0107020801")
        self.assertEqual(C.video_start_payload(232, 232, 30, 450).hex(), "840205011802e80003e800041e05c201")
        self.assertEqual(C.hid_map_payload("digitizer", (0.25, 0.125, 0.5, 0.375), True).hex(),
                         "8602010040002000800060" + "01")
        self.assertEqual(C.hid_map_payload("rel", (0, 0, 1, 1), False, (1000, 2160)).hex(),
                         "860203" + "00000000ffffffff00" + "e8037008")
        self.assertEqual(C.hid_map_payload("off").hex(), "860200")
        caps = C.parse_video_caps(bytes.fromhex(
            "84 01 0a  01 01 18  02 d201 d201 01  03 f000 f000  04 1e  05 00100000  06 e800 e800 1e c201"
            "  08 f401  0a 3c  0b d201 d201 0f 00100000 02  0d 01".replace(" ", "")))
        self.assertEqual(caps["display"], (466, 466, True))
        self.assertEqual(caps["tile_split"], 60)
        self.assertTrue(caps["early_keyframe"])
        self.assertEqual(caps["ui"], {"w": 466, "h": 466, "fps": 15, "max_frame": 4096, "tiles": 2})
        # still (14) and rotated (16) parse; from 17 on an unknown or reserved id (17 itself is reserved here) is
        # skipped by its length byte, and what follows it still parses. An unknown id under 17 ends the parse.
        caps = C.parse_video_caps(bytes.fromhex(
            ("84 01 06  0d 01  0e d201 d201 00800000 0000 0000 01 1b  10 f000 2801 1e 2c01 01"
             "  11 04 f000 f000  2a 03 aabbcc  09 0f").replace(" ", "")))
        self.assertEqual(caps["still"], {"w": 466, "h": 466, "max_bytes": 32768, "codecs": [27]})
        self.assertEqual(caps["rotated"], {"w": 240, "h": 296, "fps": 30, "kbps": 300, "turn": 1})
        self.assertEqual((caps["skipped"], caps["tiles"]), ([17, 42], 15))
        self.assertNotIn("unknown", caps)
        caps = C.parse_video_caps(bytes.fromhex("84 01 02  0f 01  09 02".replace(" ", "")))
        self.assertEqual((caps["unknown"], "tiles" in caps), (15, False))
        # A length running past the payload ends the parse.
        self.assertEqual(C.parse_video_caps(bytes.fromhex("840101" "2a05aabb"))["unknown"], 42)
        v = C.parse_video_update(bytes.fromhex("8405" "00200000" "40000000" "0200" "0000" "5a" "0700" "1e00"))
        self.assertEqual((v["avail"], v["received"], v["shown"], v["decode_ms"], v["last_number"], v["last_age_ms"]),
                         (8192, 64, 2, 90, 7, 30))
        self.assertNotIn("last_number", C.parse_video_update(bytes.fromhex("8405" "00200000" "40000000")))
        self.assertEqual(C.parse_touch(bytes.fromhex("8601 02 d101 d101 7011 00")),
                         {"action": "up", "x": 465, "y": 465, "time": 4464, "id": 0})
        self.assertEqual(C.parse_hid_status(bytes.fromhex("8603 0103")),
                         {"mode": "digitizer", "bonded": True, "host": True})
        # Access units from an Annex-B stream: SPS + PPS + IDR, then two P pictures
        # (first_mb_in_slice 0 starts each), a second slice stays with its picture.
        sps, pps = b"\x00\x00\x00\x01\x67\x42\x00\x1e", b"\x00\x00\x00\x01\x68\xce"
        idr, p1 = b"\x00\x00\x00\x01\x65\x88\x84", b"\x00\x00\x00\x01\x41\x9a\x02"
        p1b, p2 = b"\x00\x00\x01\x41\x40\x11", b"\x00\x00\x00\x01\x41\x9a\x04"
        aus = C.split_access_units(sps + pps + idr + p1 + p1b + p2)
        self.assertEqual(aus, [sps + pps + idr, p1 + p1b, p2])
        self.assertTrue(C.is_keyframe(aus[0], 1) and not C.is_keyframe(aus[1], 1))
        two = len(aus[0]).to_bytes(4, "little") + aus[0] + aus[0]
        self.assertTrue(C.is_keyframe(two, 2))
        f = C.video_frame(3, True, aus[0])
        self.assertEqual(f[:8].hex(), "5601" "0300" + len(aus[0]).to_bytes(4, "little").hex())

    def test_decodes_the_firmware_codecs(self) -> None:
        srcs, defs, incs = codec_build(True)
        with tempfile.TemporaryDirectory(prefix="mg-client-") as tmp:
            t = Path(tmp)
            compile_and_run(self, [ROOT / "tests/mg_codec_harness.c", MG / "mg_codec.c"], defs,
                            [MG, PROTOCOLS, *incs], vendor=srcs,
                            args=[str(t / "voice.sbc"), str(t / "voice.lc3")], cwd=t)
            ref = array.array("h", (t / "input.raw").read_bytes())
            dec = C.Decoder(cache=t / "cache")
            sbc = (t / "voice.sbc").read_bytes()
            # Whole frames per notification, as the gadget sends them: 3 SBC frames of 60 bytes.
            chunks = [sbc[i:i + 180] for i in range(0, len(sbc), 180)]
            pcm, frames = dec.decode({"type": 1, "rate": 16000, "frame_bytes": 60, "frame_us": 8000}, chunks)
            self.assertEqual(frames, len(sbc) // 60)
            self.assertGreater(best_snr(ref, array.array("h", pcm)), 20.0)
            with self.assertRaises(ValueError):
                dec.decode({"type": 1}, [sbc[:90]])   # a frame and a half
            lc3 = (t / "voice.lc3").read_bytes()
            chunks = [lc3[i:i + 80] for i in range(0, len(lc3), 80)]
            pcm, frames = dec.decode(C.parse_change_data_type(bytes([8, 19])), chunks)
            self.assertEqual(frames, len(lc3) // 40)
            self.assertGreater(best_snr(ref, array.array("h", pcm)), 12.0)
            # A clip format this decoder doesn't build is refused, never decoded as 10 ms frames.
            with self.assertRaises(ValueError):
                dec.decode(dict(C.parse_change_data_type(bytes([8, 19])), frame_us=7500), chunks)
            C.write_wav(t / "out.wav", pcm)
            self.assertGreater((t / "out.wav").stat().st_size, 44)


    def test_att_mtu_on_bluez(self) -> None:
        """BlueZ reports 23 until the MTU is acquired: `test` reads the negotiated one, or skips the check."""
        class BleakClientBlueZDBus:
            def __init__(self, fails=False):
                self._mtu_size, self.fails = None, fails

            async def _acquire_mtu(self):
                if self.fails:
                    raise RuntimeError("no characteristic to acquire")
                self._mtu_size = 247

        class Client:
            def __init__(self, backend):
                self._backend = backend

            @property
            def mtu_size(self):
                return self._backend._mtu_size or 23

        self.assertEqual(asyncio.run(C.att_mtu(Client(BleakClientBlueZDBus()))), 247)
        self.assertIsNone(asyncio.run(C.att_mtu(Client(BleakClientBlueZDBus(fails=True)))))
        other = type("BleakClientCoreBluetooth", (), {"_mtu_size": None})()
        self.assertEqual(asyncio.run(C.att_mtu(Client(other))), 23)   # read as reported, nothing acquired

    def test_gadget_text_is_printed_without_control_characters(self) -> None:
        """Advertised names and Nordic UART lines can't drive the terminal: control characters but \\n and \\t
        are shown as \\xNN (a carriage return is dropped)."""
        seen: dict = {}
        C.note_advertisement(seen, "AA:01", "MuseGadget-\x1b[2J\x1b]0;x\x07A1\u009b", -50, [])
        self.assertEqual(seen["AA:01"]["name"], "MuseGadget-\\x1b[2J\\x1b]0;x\\x07A1\\x9b")
        g = C.Gadget(None, verbose=False)
        g._on_nus(None, bytearray(b"mg.ble: link ready\r\n\x1b[31mred\x08\tok\n"))
        self.assertEqual("".join(g.nus), "mg.ble: link ready\n\\x1b[31mred\\x08\tok\n")
        self.assertEqual(C.printable("caf\u00e9 \u2713"), "caf\u00e9 \u2713")   # printable text is kept as is
        # Bidi overrides and zero-width characters can reorder or hide text: shown, not passed through.
        self.assertEqual(C.printable("a\u202eb\u200bc\u2066d"), "a\\u202eb\\u200bc\\u2066d")

    def test_clip_info_format(self) -> None:
        """queue decodes each clip by the format its clip_info carries, not a fixed LC3 frame size."""
        head = bytes([C.P["mg_command_audio_queue"], C.P["mg_audio_queue_command_clip_info"]]) + (2).to_bytes(2, "little")
        lc3 = C.P["mg_data_type_audio_lc3"]
        body = bytes([lc3]) + (1500).to_bytes(4, "little") + (4000).to_bytes(4, "little")
        # LC3 at 16 kb/s: 20 B per 10 ms, then start_unix_ms.
        info = C.parse_clip_info(head + body + (16000).to_bytes(2, "little") + b"\x01" + (20).to_bytes(2, "little")
                                 + (10000).to_bytes(2, "little") + (1_700_000_000_000).to_bytes(8, "little"))
        self.assertEqual((info["index"], info["codec"], info["start_ms"], info["size"]), (2, lc3, 1500, 4000))
        self.assertEqual((info["format"]["rate"], info["format"]["frame_bytes"], info["format"]["frame_us"]),
                         (16000, 20, 10000))
        self.assertEqual(info["start_unix_ms"], 1_700_000_000_000)
        # Older firmware: no format, so 16000 and the codec's defaults.
        old = C.parse_clip_info(head + body)
        self.assertEqual((old["format"]["rate"], old["format"]["frame_bytes"]),
                         (16000, C.P["MG_LC3_DEFAULT_FRAME_BYTES"]))
        self.assertNotIn("start_unix_ms", old)

    def test_playback_helpers(self) -> None:
        caps = C.parse_capabilities(bytes.fromhex(
            "1d 01 05 01 02 01 06 02 00400000 03 02 803e0000 80bb0000 04 0001 05 01"))
        self.assertEqual(caps, {"codecs": [1, 6], "buffer_size": 16384, "sample_rates": [16000, 48000],
                                "max_bitrate": 256, "max_channels": 1})
        # A device that takes mg_stream_audio_flush lists it (one reply stream per Live call).
        caps = C.parse_capabilities(bytes.fromhex(
            "1d 01 06 01 02 01 06 02 00400000 03 02 803e0000 80bb0000 04 0001 05 01 06 01"))
        self.assertEqual(caps["flush"], 1)
        # A device that takes the live depth lists its floor (ms), after flush.
        caps = C.parse_capabilities(bytes.fromhex(
            "1d 01 07 01 02 01 06 02 00400000 03 02 803e0000 80bb0000 04 0001 05 01 06 01 07 4600"))
        self.assertEqual((caps["flush"], caps["live_depth"]), (1, 70))
        # Cut short in buffer_size: what came before stands, nothing raises (describe() runs in the
        # notification callback, and a raise there dropped the reply).
        cut = bytes.fromhex("1d 01 03 01 02 01 06 02 0040".replace(" ", ""))
        self.assertEqual(C.parse_capabilities(cut), {"codecs": [1, 6], "truncated": True})
        self.assertIn("report_capabilities", C.describe(cut))
        vcut = bytes.fromhex("84 01 02 01 01 18 02 d201".replace(" ", ""))
        self.assertEqual(C.parse_video_caps(vcut), {"codecs": [24], "truncated": True})
        self.assertIsInstance(C.describe(vcut), str)
        self.assertEqual(C.start_streaming(1, 16000).hex(" "), "1d 02 03 01 01 02 80 3e 00 00 04 01")
        self.assertIn("stop_streaming keep", C.describe(bytes.fromhex("1d 03 00")))
        text = ("I (1) mg.play: stream end (keep): sbc 16000 Hz, 250 frames in, 250 decoded (0 bad), 0 underruns, "
                "110 ms silence\nI (2) mg.play: stream bytes: 15000 received (0 dropped), 2000 ms played\n"
                "I (3) mg.play: stream level: decoded rms -15.1 peak -12.0 dBFS, speaker rms -15.1 peak -12.0 dBFS\n")
        st = C.parse_stream_stats(text)
        self.assertEqual((st["how"], st["decoded"], st["played_ms"], st["peak"], st["speaker_off"], st["off_ms"]),
                         ("keep", 250, 2000, -12.0, False, 0))
        st = C.parse_stream_stats(text.replace("speaker rms -15.1 peak -12.0 dBFS",
                                               "speaker rms -120.0 peak -120.0 dBFS, speaker off"))
        self.assertTrue(st["speaker_off"])
        self.assertEqual((st["rms"], st["decoded_rms"]), (-120.0, -15.1))
        st = C.parse_stream_stats(text.replace("peak -12.0 dBFS\n", "peak -12.0 dBFS, speaker off 500 ms\n"))
        self.assertEqual((st["speaker_off"], st["off_ms"]), (False, 500))
        m = C.FACE_RE.search("I (5123) mg.face: face thinking")
        self.assertEqual(m.group(1), "thinking")
        self.assertEqual(C.FACE_RE.search("I (1) mg.face: ring responding\r").group(1), "responding")

    def test_token_proof_against_the_vectors(self) -> None:
        import json
        data = json.loads((PROTOCOLS / "test-vectors/mg-token-proof-v1.json").read_text(encoding="utf-8"))
        for v in data["vectors"]:
            k = C.proof_key(v["access_token"], v["node_id"])
            cn, dn = bytes.fromhex(v["client_nonce"]), bytes.fromhex(v["device_nonce"])
            self.assertEqual(k.hex(), v["K"], v["name"])
            self.assertEqual(C.proof_mac(k, True, cn, dn).hex(), v["device_mac"], v["name"])
            self.assertEqual(C.proof_mac(k, False, cn, dn).hex(), v["client_mac"], v["name"])
        self.assertEqual(C.P["mg_command_token_proof"], 34)
        self.assertEqual(C.P["mg_error_code_proof_required"], 6)
        self.assertEqual((C.P["MG_TOKEN_PROOF_NONCE_LEN"], C.P["MG_TOKEN_PROOF_MAC_LEN"]), (16, 32))

    def test_proof_record_is_private(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            old = os.environ.get("MG_BLE_CLIENT_HOME")
            os.environ["MG_BLE_CLIENT_HOME"] = str(Path(tmp) / "home")
            try:
                k = bytes(range(32))
                path = C.save_proof_record("AB:CD/12-34", "homelink-a7f5b4", k, "bench-access-x")
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)
                self.assertNotIn("bench-access-x", path.read_text())   # the key, never the token
                rec = C.load_proof_record("ab:cd/12-34")
                self.assertEqual((rec["proof_key"], rec["node_id"]), (k, "homelink-a7f5b4"))
                self.assertIsNone(C.load_proof_record("other"))
            finally:
                if old is None:
                    os.environ.pop("MG_BLE_CLIENT_HOME", None)
                else:
                    os.environ["MG_BLE_CLIENT_HOME"] = old

    def test_setup_framing(self) -> None:
        chunks = C.setup_chunks(b"x" * 400, 185)
        self.assertEqual([len(c) for c in chunks], [180, 180, 49])
        self.assertEqual(chunks[1][:3], bytes([0xFE, 1, 3]))
        r = C.SetupReassembler()
        self.assertEqual([r.feed(c) for c in chunks], [None, None, b"x" * 400])
        self.assertEqual(r.feed(b"error_pairing_unavailable"), b"error_pairing_unavailable")
        self.assertEqual(C.unb64url(C.b64url(b"\xff\x00\x01")), b"\xff\x00\x01")

    def test_scans_keep_every_payloads_uuids(self) -> None:
        # A gadget takes turns between the Link setup and musegadgets payloads
        # from one address: whichever packet came last, both commands find it.
        import types

        mg, setup = C.P["MG_SERVICE_UUID"], C.SETUP_SERVICE_UUID
        packets = [("AA:01", "MuseGadget-A1", -60, [setup.upper()], {0xFFFF: b"\x02"}),
                   ("AA:01", "", -55, [mg], {}),
                   ("BB:02", "other", -40, ["0000180f-0000-1000-8000-00805f9b34fb"], {}),
                   # Set up: its musegadgets advertisement only, a catalog name (Gadget setup v2).
                   ("CC:03", "MuseGadget-XN54-3C3C3C", -50, [mg], {}),
                   # Wi-Fi Link firmware, set up: name only to find, flags without bit 1.
                   ("DD:04", "MuseGadget-D4", -70, [setup], {0xFFFF: b"\x01"})]

        class FakeScanner:
            def __init__(self, detection_callback):
                self.cb = detection_callback

            async def __aenter__(self):
                for addr, name, rssi, uuids, mfg in packets:
                    self.cb(types.SimpleNamespace(address=addr, name=None),
                            types.SimpleNamespace(local_name=name, rssi=rssi, service_uuids=uuids,
                                                  manufacturer_data=mfg))
                return self

            async def __aexit__(self, *exc):
                return False

        args = types.SimpleNamespace(scan_secs=0, address=None, name=None)
        saved = sys.modules.get("bleak")
        sys.modules["bleak"] = types.SimpleNamespace(BleakScanner=FakeScanner)
        try:
            setup_hits = asyncio.run(C.find_setup(args))
            mg_hits = asyncio.run(C.find(args))
        finally:
            if saved is None:
                del sys.modules["bleak"]
            else:
                sys.modules["bleak"] = saved
        # Setup finds gadgets not set up (Link setup advertised) and set-up ones
        # (musegadgets only, or bit 0 set): those it sets up again.
        self.assertEqual(setup_hits, [("CC:03", "MuseGadget-XN54-3C3C3C", -50, True), ("AA:01", "MuseGadget-A1", -55, False),
                                      ("DD:04", "MuseGadget-D4", -70, True)])
        self.assertEqual(mg_hits, [("AA:01", "MuseGadget-A1", -55, True, 0x02), ("CC:03", "MuseGadget-XN54-3C3C3C", -50, True, None),
                                   ("DD:04", "MuseGadget-D4", -70, False, 0x01)])
        self.assertEqual(C.link_flags_text(0x02), "Link setup 0x02 (Muse Gadget over Bluetooth, not set up)")
        self.assertEqual(C.link_flags_text(0x03), "Link setup 0x03 (Muse Gadget over Bluetooth, set up)")
        self.assertEqual(C.link_flags_text(0x00), "Link setup 0x00 (Wi-Fi device, not set up)")
        self.assertEqual(C.link_flags_text(None), "")
        self.assertTrue(C.gadget_name("MuseGadget-XN54-4D5E6F") and C.gadget_name("MuseGadget-1A2B3C"))
        self.assertTrue(C.gadget_name("MG-XN54-4D5E6F"))   # the deferred prefix, as a fallback
        self.assertFalse(C.gadget_name("Speaker-1234") or C.gadget_name(""))

    def test_setup_crypto_against_link_vectors(self) -> None:
        try:
            import cryptography  # noqa: F401
        except ImportError:
            self.skipTest("pip install cryptography for the Link setup client tests")
        import json
        v = next(x for x in json.loads((ROOT / "tests/vectors/link_pairing_v5.json").read_text())["vectors"]
                 if x["name"] == "community_v5")
        s = C.PairingSession(None, private_value=int(v["mobile_private_scalar_hex"], 16),
                             mobile_nonce=C.unb64url(v["mobile_nonce"]))
        self.assertEqual(s.mobile_pub, v["mobile_pub"])
        transcript = C.pairing_transcript(v, s.mobile_pub, v["mobile_nonce"])
        self.assertEqual(transcript, v["transcript"])
        th = C.hashlib.sha256(transcript.encode()).digest()
        self.assertEqual(C.b64url(th), v["transcript_hash"])
        keys = C.pairing_keys(bytes.fromhex(v["ecdh_secret_hex"]), C.unb64url(v["mobile_nonce"]),
                              C.unb64url(v["device_nonce"]), th)
        self.assertEqual(keys["session_secret"].hex(), v["session_secret_hex"])
        self.assertEqual(keys["tx"].hex(), v["mobile_tx_key_hex"])
        self.assertEqual(keys["rx"].hex(), v["mobile_rx_key_hex"])
        self.assertEqual(keys["session_id"], v["session_id"])
        self.assertEqual(C.record_aad(keys["session_id"], 0, 0).decode(), v["client_finished_aad"])
        env = C.seal_record(keys["tx"], keys["session_id"], 0, v["client_finished_plaintext"].encode())
        self.assertEqual((env["ciphertext"], env["tag"]), (v["client_finished_ciphertext"], v["client_finished_tag"]))
        self.assertEqual(env["action"], "pairing_encrypted")
        # The whole pairing_ready check, ECDH included.
        ready = dict(type="pairing_ready", version=5, pairing_auth="none", pairing_auth_epoch=0,
                     pairing_policy="confirm_press", transcript_hash=v["transcript_hash"],
                     session_id=v["session_id"], **{k: v[k] for k in ("device_id", "node_id", "mac", "model",
                                                                       "firmware_version", "device_pub",
                                                                       "device_nonce")})
        s.accept_ready(ready)
        self.assertEqual(s.keys["tx"].hex(), v["mobile_tx_key_hex"])
        with self.assertRaises(RuntimeError):
            s.accept_ready(dict(ready, transcript_hash=C.b64url(bytes(32))))
        # Records the device seals (direction 1) open, and only with the right counter.
        dev = C.seal_record(keys["rx"], keys["session_id"], 0, b'{"type":"status"}', direction=1)
        self.assertEqual(C.open_record(keys["rx"], keys["session_id"], 0, dev), b'{"type":"status"}')
        with self.assertRaises(ValueError):
            C.open_record(keys["rx"], keys["session_id"], 1, dev)

    def test_setup_flow_against_a_simulated_device(self) -> None:
        try:
            from cryptography.hazmat.primitives.asymmetric import ec
            from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
        except ImportError:
            self.skipTest("pip install cryptography for the Link setup client tests")
        import json

        class Device:
            """The device side of link_pairing.c for one setup, plus a token-only provision_v2."""

            def __init__(self, wifi="none"):
                self.wifi, self.got, self.notify = wifi, None, None
                self.priv = ec.derive_private_key(2, ec.SECP256R1())
                self.pub = C.b64url(self.priv.public_key().public_bytes(Encoding.X962,
                                                                        PublicFormat.UncompressedPoint))
                self.rx = C.SetupReassembler()
                self.tx_counter = self.rx_counter = 0
                self.mtu_size = 185

            def send(self, obj):
                for c in C.setup_chunks(json.dumps(obj).encode(), self.mtu_size):
                    self.notify(None, bytearray(c))

            def status(self, status):
                body = json.dumps({"type": "status", "status": status}).encode()
                self.send(C.seal_record(self.keys["rx"], self.keys["session_id"], self.tx_counter, body, 1))
                self.tx_counter += 1

            async def start_notify(self, uuid, cb):
                assert uuid == C.SETUP_TX_UUID
                self.notify = cb

            async def write_gatt_char(self, uuid, data, response=False):
                assert uuid == C.SETUP_RX_UUID
                msg = self.rx.feed(bytes(data))
                if msg is None:
                    return
                m = json.loads(msg)
                act = m.get("action")
                if act == "get_device_info":
                    info = {"type": "device_info", "node_id": "homelink-000001", "pairing_protocol": 5,
                            "pairing_auth": "none", "pairing_policy": "confirm_press", "mgcommands": 1}
                    if self.wifi != "required":
                        info["wifi"] = self.wifi
                    self.send(info)
                elif act == "pairing_client_hello":
                    dn = bytes(range(16, 32))
                    ready = {"type": "pairing_ready", "version": 5, "device_id": "hatch-link:02:00:00:00:00:01",
                             "node_id": "homelink-000001", "mac": "02:00:00:00:00:01", "model": "hatch_link",
                             "firmware_version": "1.0.0", "pairing_auth": "none", "pairing_auth_epoch": 0,
                             "pairing_policy": "confirm_press", "device_pub": self.pub, "device_nonce": C.b64url(dn)}
                    th = C.hashlib.sha256(C.pairing_transcript(ready, m["mobile_pub"], m["mobile_nonce"])
                                          .encode()).digest()
                    peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), C.unb64url(m["mobile_pub"]))
                    keys = C.pairing_keys(self.priv.exchange(ec.ECDH(), peer), C.unb64url(m["mobile_nonce"]), dn, th)
                    # The device's rx is the mobile's tx.
                    self.keys = {"rx": keys["rx"], "tx": keys["tx"], "session_id": keys["session_id"]}
                    self.send(dict(ready, transcript_hash=C.b64url(th), session_id=keys["session_id"]))
                elif act == "pairing_encrypted":
                    plain = json.loads(C.open_record(self.keys["tx"], self.keys["session_id"], self.rx_counter,
                                                     m, direction=0))
                    self.rx_counter += 1
                    if plain["action"] == "pairing_client_finished":
                        self.status("confirm_required")
                    elif plain["action"] == "provision_v2":
                        assert "ssid" not in plain and plain["token_type"] == "device"
                        self.got = plain
                        self.status("auth_ok")

        async def run(dev):
            link = C.SetupLink(dev)
            await link.start()
            prompts = []

            def press(msg):
                prompts.append(msg)
                dev.status("pairing_confirmed")   # the button

            got = await C.setup_flow(link, "bench-access-1", "bench-refresh-1", prompt=press)
            return got, prompts

        dev = Device()
        got, prompts = asyncio.run(run(dev))
        self.assertEqual(len(prompts), 1)
        self.assertIn("press", prompts[0])
        self.assertEqual(dev.got["access_token"], "bench-access-1")
        self.assertEqual(dev.got["refresh_token"], "bench-refresh-1")
        self.assertEqual((got["node_id"], got["wifi"], got["mgcommands"]), ("homelink-000001", "none", True))
        self.assertEqual(got["proof_key"], C.proof_key("bench-access-1", "homelink-000001"))

        # --confirm-serial: the console presses the button and nobody is prompted.
        import tty

        master, slave = os.openpty()
        tty.setraw(slave)
        self.addCleanup(os.close, slave)
        self.addCleanup(os.close, master)
        seen = []

        def console():
            line = os.read(master, 64)
            seen.append(line)
            os.write(master, b"@pair.confirm confirmed\n")

        async def run_serial(dev):
            link = C.SetupLink(dev)
            await link.start()
            confirm = C.serial_confirm(os.ttyname(slave))

            async def press_over_serial():
                ok = await confirm()
                if ok:
                    dev.status("pairing_confirmed")   # what the press does on the device
                return ok

            return await C.setup_flow(link, "bench-access-2", "bench-refresh-2",
                                      prompt=lambda m: self.fail(f"prompted: {m}"), confirm=press_over_serial)

        import threading
        threading.Thread(target=console, daemon=True).start()
        dev = Device()
        got = asyncio.run(run_serial(dev))
        self.assertEqual(seen, [b">pair.confirm\n"])
        self.assertEqual(dev.got["access_token"], "bench-access-2")
        self.assertIsNone(C.serial_confirm(None))
        # A device that needs Wi-Fi isn't set up token-only.
        with self.assertRaises(RuntimeError):
            asyncio.run(run(Device(wifi="required")))

    async def _reprovision(self, fake, client, g) -> None:
        """Gadget setup v2: reprovision_flow on one connection against the firmware's
        own token proof code. The simulated Link side's commit hands the protocol code
        the new key, as the gadget's provisioning commit does; the old key must then
        fail the proof and the new one match."""
        try:
            import cryptography  # noqa: F401
        except ImportError:
            return   # the Link side needs it; the rest of _play doesn't
        node = "homelink-a7f5b4"

        def commit(plain):
            k = C.proof_key(plain["access_token"], node)
            fake.fake_set_proof_key((ctypes.c_ubyte * 32).from_buffer_copy(k))

        client.setup = SimLinkDevice(node, on_provision=commit)
        link = C.SetupLink(client)
        await link.start()
        old = C.proof_key("bench-access-first", node)
        fake.fake_set_proof_key((ctypes.c_ubyte * 32).from_buffer_copy(old))   # set up already
        prompts = []

        def press(msg):
            prompts.append(msg)
            client.setup.press()

        got = await C.reprovision_flow(link, g, old, "bench-access-second", "bench-refresh-second", prompt=press)
        self.assertEqual(len(prompts), 1, "the confirmation is asked for again")
        self.assertIn("never the talk key", prompts[0], "a set-up gadget's re-setup isn't confirmed by the talk key")
        self.assertEqual((got["proof_before"], got["proof_old"], got["proof_new"]),
                         ("match", "device_mac_mismatch", "match"))
        self.assertTrue(got["ok"])
        self.assertEqual(got["proof_key"], C.proof_key("bench-access-second", node))
        # A gadget that kept its old key (old firmware, or a failed commit) is caught.
        client.setup.on_provision = None
        got = await C.reprovision_flow(link, g, got["proof_key"], "bench-access-third", "r", prompt=press)
        self.assertEqual((got["proof_old"], got["proof_new"]), ("match", "device_mac_mismatch"))
        self.assertFalse(got["ok"])
        fake.fake_set_proof_key((ctypes.c_ubyte * 32).from_buffer_copy(old))
        self.assertEqual(await C.run_proof(g, old, clear=True), "match")   # back as it was

    def test_dfu_image_and_requests(self) -> None:
        """The MCUboot image parsing dfu relies on, and (with smpclient) the SMP packets it sends."""
        import hashlib
        import struct

        body = bytes(range(256)) * 10
        hdr = struct.pack("<IIHHII", C.MCUBOOT_IMAGE_MAGIC, 0, 0x200, 0, len(body), 0)
        hdr += struct.pack("<BBHI", 0, 1, 1, 0) + b"\0" * 4
        hdr += b"\xff" * (0x200 - len(hdr))
        sha = hashlib.sha256(hdr + body).digest()
        tlvs = struct.pack("<HH", 0x10, 32) + sha + struct.pack("<HH", 0x22, 4) + b"sig!"
        img = hdr + body + struct.pack("<HH", C.MCUBOOT_TLV_INFO_MAGIC, 4 + len(tlvs)) + tlvs
        info = C.mcuboot_image_info(img)
        self.assertEqual(info["version"], "0.1.1")
        self.assertEqual(info["sha256"], sha)
        self.assertEqual(info["size"], len(img))
        with self.assertRaises(ValueError):
            C.mcuboot_image_info(b"\0" * 64)  # an unsigned zephyr.bin
        # A protected TLV area (e.g. a security counter) comes first.
        prot = struct.pack("<HH", C.MCUBOOT_TLV_PROT_INFO_MAGIC, 12) + struct.pack("<HH", 0x50, 4) + b"\1\0\0\0"
        hdr2 = hdr[:10] + struct.pack("<H", len(prot)) + hdr[12:]
        self.assertEqual(C.mcuboot_image_info(hdr2 + body + prot + img[len(hdr) + len(body):])["sha256"], sha)
        try:
            import smpclient  # noqa: F401
        except ImportError:
            self.skipTest("smpclient isn't installed: SMP packet checks skipped")
        test, = C.dfu_state_requests(sha, "test")
        confirm, = C.dfu_state_requests(sha, "confirm")
        self.assertEqual(C.dfu_state_requests(sha, None), [])
        # SMP v2 header: op 2 (write), group 1 (image), id 0 (state); CBOR {hash, confirm}.
        for req, flag in ((test, b"\xf4"), (confirm, b"\xf5")):
            raw = req.BYTES
            self.assertEqual(raw[0] & 0x07, 2)
            self.assertEqual(int.from_bytes(raw[4:6], "big"), 1)
            self.assertEqual(raw[7], 0)
            self.assertEqual(int.from_bytes(raw[2:4], "big"), len(raw) - 8)
            self.assertIn(b"dhashX\x20" + sha, raw)
            self.assertTrue(raw.endswith(b"gconfirm" + flag))

        class Im:
            def __init__(self, slot, active):
                self.slot, self.version, self.hash = slot, f"0.1.{slot}", bytes([slot]) * 32
                self.active, self.confirmed, self.pending = active, active, not active
        lines = C.format_image_states([Im(0, True), Im(1, False)])
        self.assertIn("slot 0: 0.1.0", lines[0])
        self.assertIn("active confirmed", lines[0])
        self.assertIn("pending", lines[1])

    def test_dfu_waits_for_the_gadgets_press(self) -> None:
        """An update write answered rc 10 goes again on the same connection until the press admits it (rc 0); rc 11
        stops dfu with the message."""
        from types import SimpleNamespace as NS

        class FakeClient:
            def __init__(self, rcs):
                self.rcs, self.sent = list(rcs), []

            async def request(self, req, timeout_s=None):
                self.sent.append(req)
                return NS(rc=self.rcs.pop(0), off=0)

        req = object()
        c = FakeClient([10, 10, 0])
        rsp = asyncio.run(C.smp_request_pressed(c, req, "upload", interval_s=0))
        self.assertEqual((rsp.rc, len(c.sent)), (0, 3))
        self.assertTrue(all(r is req for r in c.sent), "the same first write, unchanged")
        c = FakeClient([10, 11])
        with self.assertRaises(SystemExit) as e:
            asyncio.run(C.smp_request_pressed(c, req, "upload", interval_s=0))
        self.assertIn("not confirmed: no press in time or cancelled; run dfu again", str(e.exception))
        self.assertEqual(asyncio.run(C.smp_request_pressed(FakeClient([0]), req, "upload")).rc, 0)

    def test_dfu_pipelined_upload(self) -> None:
        """smp_upload against a fake SMP server that behaves like Zephyr's img_mgmt:
        requests handled in order, each answered with the next offset."""
        try:
            import smpclient  # noqa: F401
            from smp import header as smphdr
            from smp import image_management as smpimg
            from smpclient import SMPClient
            from smpclient.transport import SMPTransport
        except ImportError:
            self.skipTest("smpclient isn't installed")

        class FakeServer(SMPTransport):
            def __init__(self, drop_at: int | None = None):
                self.rx = bytearray()     # the image as written
                self.out: list[bytes] = []
                self.inflight = 0
                self.max_inflight = 0
                self.drop_at = drop_at
                self.requests = 0
                self.initialize(2475)

            @property
            def mtu(self) -> int:
                return 244

            async def connect(self, address, timeout_s):
                pass

            async def disconnect(self):
                pass

            async def send(self, data: bytes) -> None:
                self.inflight += 1
                self.max_inflight = max(self.max_inflight, self.inflight)
                h = smphdr.Header.loads(data[:smphdr.Header.SIZE])
                req = smpimg.ImageUploadWriteRequest.loads(data)
                self.requests += 1
                assert len(data) <= 2475, len(data)
                if self.drop_at is not None and self.requests == self.drop_at:
                    self.drop_at = None   # a lost request: no answer at all
                    self.inflight -= 1
                    return
                if req.off == len(self.rx):
                    self.rx += req.data
                # Out of order (after a loss): img_mgmt answers with where it is.
                self.out.append(smpimg.ImageUploadWriteResponse(sequence=h.sequence, off=len(self.rx)).BYTES)

            async def receive(self) -> bytes:
                while not self.out:
                    await asyncio.sleep(0)
                self.inflight -= 1
                return self.out.pop(0)

            async def send_and_receive(self, data: bytes) -> bytes:
                await self.send(data)
                return await self.receive()

        image = bytes(range(256)) * 400  # 100 KB
        for window, drop in ((1, None), (2, None), (3, None), (2, 7)):
            fake = FakeServer(drop)
            client = SMPClient(fake, "fake")
            offs = []
            if drop is None:
                sent = asyncio.run(C.smp_upload(client, image, window, offs.append, timeout_s=1.0))
            else:
                # A lost request times out its response; the next ones answer with the
                # server's offset and the upload resumes from it.
                async def run():
                    try:
                        return await C.smp_upload(client, image, window, offs.append, timeout_s=0.5)
                    except asyncio.TimeoutError:
                        return None
                sent = asyncio.run(run())
                if sent is None:
                    continue  # a loss with nothing behind it is a timeout, as with smpclient
            self.assertEqual(bytes(fake.rx), image, f"window {window}")
            self.assertEqual(offs[-1], len(image))
            self.assertLessEqual(fake.max_inflight, window)
            if window > 1 and drop is None:
                self.assertEqual(fake.max_inflight, window, "requests pipelined")
            # About 2.4 KB of image per request, not one ATT write's worth.
            self.assertLess(sent, len(image) // 2000 + 3)

    def test_smp_transport_receive_splits_responses(self) -> None:
        """Two responses arriving back to back come out one at a time."""
        try:
            from smp import image_management as smpimg
        except ImportError:
            self.skipTest("smpclient isn't installed")
        T = C.smp_transport_class()
        t = T()
        a = smpimg.ImageUploadWriteResponse(sequence=1, off=100).BYTES
        b = smpimg.ImageUploadWriteResponse(sequence=2, off=2500).BYTES
        t._buffer.extend(a + b)
        self.assertEqual(asyncio.run(t.receive()), a)
        self.assertEqual(asyncio.run(t.receive()), b)
        t.initialize(2475)
        t.chunk_cap = 1000
        self.assertEqual(t.max_unencoded_size, 1000)

    def _fake_lib(self, tmp: str) -> ctypes.CDLL:
        """tests/mg_fake_gadget.c on the firmware's protocol, session and playback code, as a library."""
        srcs, defs, incs = codec_build(False)
        lib = Path(tmp) / ("fake.dylib" if sys.platform == "darwin" else "fake.so")
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmd = [*cc, "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-shared", "-fPIC", "-w", *[f"-D{d}" for d in defs]]
        for inc in (MG, MG / "include", ROOT / "components/muse_vstream/include", PROTOCOLS, *incs, *SEAL_INCS):
            cmd += ["-I", str(inc)]
        cmd += [str(ROOT / "tests/mg_fake_gadget.c"), str(ROOT / "tests/mg_token_proof_ref.c"), *map(str, SEAL_SRCS),
                *(str(MG / f) for f in ("mg_proto.c", "mg_session.c", "mg_ui.c", "mg_play.c", "mg_resample.c", "mg_codec.c",
                                        "mg_queue.c")), str(ROOT / "components/muse_vstream/muse_vstream.c"), *map(str, srcs),
                "-lm", "-o", str(lib)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr[-4000:])
        return ctypes.CDLL(str(lib))

    def test_seal_against_the_vectors(self) -> None:
        """The client's seal, unseal and one-write sealed framing against the seal vectors."""
        import json
        sv = json.loads((PROTOCOLS / "test-vectors/mg-session-seal-v1.json").read_text())
        for v in sv["vectors"]:
            e, m = C.seal_keys(bytes.fromhex(v["K"]), bytes.fromhex(v["client_nonce"]),
                               bytes.fromhex(v["device_nonce"]), v["node_id"])
            self.assertEqual((e.hex(), m.hex()), (v["enc_key"], v["mac_key"]), v["name"])
            pt, sealed = v["plaintext"].encode(), bytes.fromhex(v["sealed"])
            self.assertEqual(C.seal(e, m, v["direction"], v["counter"], pt), sealed, v["name"])
            self.assertEqual(C.unseal(e, m, v["direction"], v["counter"], sealed), pt, v["name"])
            self.assertIsNone(C.unseal(e, m, v["direction"], v["counter"] + 1, sealed), v["name"])   # the counter
            self.assertIsNone(C.unseal(e, m, 3 - v["direction"], v["counter"], sealed), v["name"])   # the direction
        # sealed_data: the client's data on counter 2 is the vector's one write byte for byte.
        sd = sv["sealed_data"]
        step = next(x for x in sd["steps"] if x["step"] == "client: data (counter 2)")
        e, m = C.seal_keys(bytes.fromhex(sd["K"]), bytes.fromhex(sd["client_nonce"]),
                           bytes.fromhex(sd["device_nonce"]), sd["node_id"])
        s = C.SealedSession(e, m, C.P["mg_session_hello_flag_sealed"], [])
        s.ctr_out = 2   # this session took 0 and 1 first
        frame = s.sealed_frame(C.P["mg_session_data"], bytes.fromhex(step["plaintext"]))
        self.assertEqual(frame.hex(), step["frames"][0])
        self.assertEqual(s.ctr_out, 3)

    def test_sealed_session_against_the_firmware(self) -> None:
        """The sealed session against the firmware's own session code (mg_proto.c, mg_session.c): no hello before a
        proof, the sealed hello and its credentials after one, and the sealed clear. No radio, no hardware."""
        with tempfile.TemporaryDirectory(prefix="mg-fake-") as tmp:
            asyncio.run(self._sealed(self._fake_lib(tmp)))

    async def _sealed(self, fake) -> None:
        CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int)
        uuids = {C.P["MG_CONTROL_UUID"]: 0, C.P["MG_DATA_UUID"]: 1}
        names = {0: C.P["MG_CONTROL_UUID"], 1: C.P["MG_DATA_UUID"]}

        class FakeClient:
            mtu_size = 247

            def __init__(self):
                self.handlers = {}

            async def start_notify(self, uuid, cb):
                self.handlers[uuid] = cb

            async def write_gatt_char(self, uuid, data, response=False):
                buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(bytes(data))
                fake.fake_write(uuids[uuid], buf, len(data))

        client = FakeClient()
        g = C.Gadget(client, verbose=False)
        await g.start()
        notify = CB(lambda ch, d, n: client.handlers[names[ch]](None, bytearray(d[:n])))
        nus = CB(lambda ch, d, n: None)
        node, k = "homelink-a7f5b4", C.proof_key("bench-access-0123", "homelink-a7f5b4")

        fake.fake_session(1)
        fake.fake_init(3072, 247, notify, nus)
        fake.fake_set_proof_key((ctypes.c_ubyte * 32).from_buffer_copy(k))
        st = await g.status()
        self.assertIn(C.P["mg_command_session"], st["commands"])
        with self.assertRaises(RuntimeError):
            await C.sealed_hello(g, k, node)   # no proof on this connection yet
        self.assertEqual(await C.run_proof(g, k), "match")
        s = await C.sealed_hello(g, k, node)
        self.assertTrue(s.echoed & C.P["mg_session_hello_flag_sealed"])
        self.assertIn("access_token", s.fields)
        self.assertEqual([p for p in g.drain() if p[0] == C.P["mg_command_error"]], [])

        # A gadget that declares sealed_clear: the unsealed clear is refused and erases nothing; the sealed
        # mg_session_clear after a match erases, and the gadget's sealed answer says so.
        fake.fake_sealed_clear(1)
        fake.fake_init(3072, 247, notify, nus)
        g.drain()
        st = await g.status()
        self.assertTrue(st["typed"][C.P["mg_command_session"]][0] & C.P["mg_session_cap_sealed_clear"])
        with self.assertRaisesRegex(RuntimeError, "insecure"):
            await C.run_proof(g, k, clear=True)
        self.assertEqual(fake.fake_proof_clears(), 0)
        self.assertEqual(await C.run_proof(g, k), "match")
        self.assertTrue(await C.sealed_clear(g, k, node))
        self.assertEqual(fake.fake_proof_clears(), 1)
        self.assertEqual(await C.run_proof(g, k), "not_found")
        fake.fake_sealed_clear(0)
        fake.fake_session(0)

    def test_proof_and_settings_against_the_firmware(self) -> None:
        """The client's settings, token proof, clear and re-provisioning against the firmware's own protocol code."""
        with tempfile.TemporaryDirectory(prefix="mg-fake-") as tmp:
            asyncio.run(self._against_firmware(self._fake_lib(tmp)))

    async def _against_firmware(self, fake) -> None:
        CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int)
        uuids = {C.P["MG_CONTROL_UUID"]: 0, C.P["MG_DATA_UUID"]: 1}

        class FakeClient:
            mtu_size = 247

            def __init__(self):
                self.handlers = {}
                self.setup = None   # a SimLinkDevice on the same connection, for Link setup

            async def start_notify(self, uuid, cb):
                self.handlers[uuid] = cb
                if uuid == C.SETUP_TX_UUID:
                    self.setup.notify = cb

            async def write_gatt_char(self, uuid, data, response=False):
                if uuid == C.SETUP_RX_UUID:
                    self.setup.write(data)
                    return
                buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(bytes(data))
                fake.fake_write(uuids[uuid], buf, len(data))

        client = FakeClient()
        g = C.Gadget(client, verbose=False)
        await g.start()
        names = {0: C.P["MG_CONTROL_UUID"], 1: C.P["MG_DATA_UUID"]}
        notify = CB(lambda ch, d, n: client.handlers[names[ch]](None, bytearray(d[:n])))
        nus = CB(lambda ch, d, n: g._on_nus(None, bytearray(d[:n]) + b"\n"))
        fake.fake_init(3072, 247, notify, nus)
        # The typed settings list: what a C6-like board accepts, no haptics.
        st = await g.status()
        self.assertEqual(st["settings"], [0, 2, 7, 10, 19])   # 19: who is connected (18 reserved)
        # The client named its app right after request_status (listed), on the firmware's own protocol code.
        self.assertTrue(g.identified)
        await g.send(C.P["mg_command_get_settings"], 19)
        self.assertEqual(C.parse_client_settings(await g.expect(C.P["mg_command_get_settings"])),
                         {"client_app": ("mg_ble_client", C.client_version())})
        await g.identify("v\u2019 2")
        await g.send(C.P["mg_command_get_settings"], 19)
        who = C.parse_client_settings(await g.expect(C.P["mg_command_get_settings"]))
        self.assertEqual(who, {"client_app": ("mg_ble_client", "v\u2019 2")})
        self.assertIn(C.P["mg_command_assistant_state"], st["commands"])
        self.assertEqual(await C.set_setting(g, "haptics_enabled", 1) != [], True)
        await g.send(C.P["mg_command_assistant_state"], C.assistant_state("responding"))
        self.assertEqual(fake.fake_assistant(), 3)
        # Face lines on the log mirror are picked out whole, across notifications.
        g.face_log = True
        g._on_nus(None, bytearray(b"I (1) mg.face: face thi"))
        g._on_nus(None, bytearray(b"nking\nI (2) mg.play: x\n"))
        self.assertEqual(g.faces, ["thinking"])
        g.face_log = False
        # The token proof against the firmware's own protocol code: no key, a match, a
        # wrong key (the device's MAC is caught before any confirm), then clear.
        self.assertIn(C.P["mg_command_token_proof"], st["commands"])
        k = C.proof_key("bench-access-0123", "homelink-a7f5b4")
        self.assertEqual(await C.run_proof(g, k), "not_found")
        key = (ctypes.c_ubyte * 32).from_buffer_copy(k)
        fake.fake_set_proof_key(key)
        self.assertEqual(await C.run_proof(g, k), "match")
        self.assertEqual(await C.run_proof(g, C.proof_key("other-token", "homelink-a7f5b4")),
                         "device_mac_mismatch")
        self.assertEqual(await C.run_proof(g, k, clear=True), "match")
        self.assertEqual(fake.fake_proof_clears(), 1)
        self.assertEqual(await C.run_proof(g, k), "not_found")   # the fake forgot its key
        await self._reprovision(fake, client, g)
        # speaker_volume: read, turn the speaker off, out of range refused.
        self.assertEqual(await C.get_setting(g, "speaker_volume"), 70)
        self.assertEqual(await C.set_setting(g, "speaker_volume", 0), [])
        self.assertEqual(await C.get_setting(g, "speaker_volume"), 0)
        self.assertIn("error", " ".join(await C.set_setting(g, "speaker_volume", 101)))
        self.assertEqual(await C.set_setting(g, "speaker_volume", 60), [])
        # Push-to-talk during playback: the device ends it with stop_streaming drop.
        g.stops.clear()
        await client.write_gatt_char(C.P["MG_CONTROL_UUID"], C.start_streaming(1, 16000))
        await asyncio.sleep(0.05)
        fake.fake_claim(1)
        self.assertEqual([s[1] for s in g.stops], [C.P["mg_stream_audio_buffer_drop"]])
        await client.write_gatt_char(C.P["MG_CONTROL_UUID"], C.start_streaming(1, 16000))
        self.assertTrue(any("busy" in e for e in g.stream_errors))
        fake.fake_claim(0)


    def test_a_sealed_message_is_bounded_before_any_tag_check(self):
        ses, cred = C.P["mg_command_session"], C.P["mg_session_credentials"]
        first, last = C.P["mg_session_flag_first"], C.P["mg_session_flag_last"]

        class G:
            def __init__(self, frags):
                self.frags = list(frags)

            async def expect(self, command, timeout=3.0, sub=None):
                if not self.frags:
                    raise TimeoutError("none")
                return self.frags.pop(0)

        def frag(fl, body, total=None):
            head = bytes([ses, cred, fl]) + (total.to_bytes(4, "little") if total is not None else b"")
            return head + body

        def read(*frags):
            return asyncio.run(C.read_sealed_message(G(frags), cred, 30))
        self.assertEqual(read(frag(first, b"a" * 20, 25), frag(last, b"b" * 5)), b"a" * 20 + b"b" * 5)
        bad = [
            [frag(last, b"x")],                                   # a continuation with no first
            [frag(first | last, b"x" * 31, 31)],                  # over the cap
            [frag(first, b"x" * 10, 12), frag(last, b"y" * 5)],   # past the total
            [frag(first, b"x" * 10, 20), frag(first, b"y" * 10, 20)],
            [frag(first, b"x" * 10, 20), frag(last, b"y" * 5)],   # short of it
            [bytes([ses, cred])],                                 # no flags byte
        ]
        for frags in bad:
            with self.assertRaises(RuntimeError, msg=frags):
                read(*frags)
        with self.assertRaises(TimeoutError):
            read(frag(first, b"x" * 10, 20))


SMP_SERVICE = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
SMP_CHAR = "da2e7828-fbce-4e01-ae9e-261174997c48"


class FakeBleak:
    """bleak against one gadget's GATT database (the musegadgets service and SMP's), keeping bleak's service
    filter: a BleakClient opened with services=[...] discovers only those (CoreBluetooth discovers only them,
    BlueZ's backend leaves the rest out of its collection), and start_notify on a characteristic it didn't
    discover raises, as bleak's does."""

    GATT = {C.P["MG_SERVICE_UUID"].lower(): [C.P["MG_CONTROL_UUID"].lower(), C.P["MG_DATA_UUID"].lower()],
            SMP_SERVICE: [SMP_CHAR]}

    def __init__(self):
        import types

        fake = self
        self.opened: list = []

        try:
            from bleak.backends.device import BLEDevice
            device = BLEDevice("AA:BB:CC:DD:EE:FF", "MuseGadget-XN54-DDEEFF", None)
        except ImportError:
            device = types.SimpleNamespace(address="AA:BB:CC:DD:EE:FF", name="MuseGadget-XN54-DDEEFF")

        class Char:
            def __init__(self, uuid):
                self.uuid = uuid
                self.max_write_without_response_size = 244

        class Services:
            def __init__(self, chars):
                self.chars = chars

            def get_characteristic(self, uuid):
                return self.chars.get(str(uuid).lower())

        class Backend:
            pass

        class BleakClient:
            def __init__(self, dev, services=None, timeout=10.0, disconnected_callback=None, **kwargs):
                self.address = dev.address
                self.filter = None if services is None else {str(u).lower() for u in services}
                self.services = None
                self._backend = Backend()
                self.mtu_size = 247
                self.notifying: list[str] = []
                fake.opened.append(self)

            async def connect(self):
                self.services = Services({c: Char(c) for svc, chars in fake.GATT.items()
                                          if self.filter is None or svc in self.filter for c in chars})

            async def start_notify(self, uuid, callback):
                if self.services.get_characteristic(uuid) is None:
                    raise ValueError(f"Characteristic {uuid} was not found!")
                self.notifying.append(str(uuid).lower())

            async def disconnect(self):
                pass

        class BleakScanner:
            @staticmethod
            async def find_device_by_address(address, timeout=10.0):
                return device if address == device.address else None

        self.BleakClient, self.BleakScanner = BleakClient, BleakScanner
        self.module = types.SimpleNamespace(BleakClient=BleakClient, BleakScanner=BleakScanner)

    def __enter__(self):
        # smpclient's BLE transport took its names from bleak at import: they're swapped too.
        self.smpble = sys.modules.get("smpclient.transport.ble")
        if self.smpble is not None:
            self.smpble_saved = (self.smpble.BleakClient, self.smpble.BleakScanner)
            self.smpble.BleakClient, self.smpble.BleakScanner = self.BleakClient, self.BleakScanner
        self.saved = sys.modules.get("bleak")
        sys.modules["bleak"] = self.module
        return self

    def __exit__(self, *exc):
        if self.saved is None:
            del sys.modules["bleak"]
        else:
            sys.modules["bleak"] = self.saved
        if self.smpble is not None:
            self.smpble.BleakClient, self.smpble.BleakScanner = self.smpble_saved
        return False


class SmpProvesFirst(unittest.TestCase):
    """A set-up gadget takes SMP update writes only on a token-proven connection, so `dfu` proves on the
    SMP client's own BLE connection, with the saved proof record, before its first write. That connection
    has to reach MG Control as well as SMP: FakeBleak keeps bleak's service filter."""

    ADDR = "AA:BB:CC:DD:EE:FF"

    @staticmethod
    def smp(client):
        import types
        return types.SimpleNamespace(_transport=types.SimpleNamespace(_client=client))

    def prove(self, smp, record, result="match", unproven=False):
        calls = []

        async def fake_proof(g, k, clear=False):
            calls.append(("proof", g.client is smp._transport._client, k))
            return result

        saved = (C.load_proof_record, C.run_proof)
        C.load_proof_record = lambda addr: record
        C.run_proof = fake_proof
        try:
            return asyncio.run(C.smp_prove(smp, self.ADDR, unproven)), calls
        finally:
            C.load_proof_record, C.run_proof = saved

    def run_prove(self, record, result="match", unproven=False):
        with FakeBleak() as fb:
            client = asyncio.run(C.ble_open(self.ADDR, 1.0))
        self.assertIsNone(client.filter, "the connection discovers every service")
        r, calls = self.prove(self.smp(client), record, result, unproven)
        return r, calls, fb, client

    def test_proves_on_the_smp_connection(self):
        r, calls, fb, client = self.run_prove({"proof_key": b"k" * 32})
        self.assertEqual(r, "match")
        self.assertEqual(calls, [("proof", True, b"k" * 32)])
        self.assertEqual(client.notifying, [C.P["MG_CONTROL_UUID"].lower()])
        self.assertEqual(len(fb.opened), 1)

    def test_a_connection_filtered_to_smp_cannot_prove(self):
        # smpclient 7.4.0 opens its connection with services=(SMP service,): MG Control isn't there.
        with FakeBleak() as fb:
            client = fb.BleakClient(asyncio.run(fb.BleakScanner.find_device_by_address(self.ADDR)),
                                    services=[SMP_SERVICE])
            asyncio.run(client.connect())
        with self.assertRaisesRegex(ValueError, "not found"):
            self.prove(self.smp(client), {"proof_key": b"k" * 32})

    def test_the_smp_transport_reaches_mg_control(self):
        """The dfu transport's own connection: SMP's notifications and the write size as smpclient sets
        them, and the proof on the same client. smpclient's stock transport, on the same fake, can't prove."""
        try:
            import smpclient.transport.ble as smpble
        except ImportError:
            self.skipTest("smpclient[ble] isn't installed")
        async def connect(t):
            await t.connect(self.ADDR, 1.0)
            return t

        T = C.smp_transport_class()   # imports smpclient's transport before FakeBleak swaps its names
        with FakeBleak():
            t = asyncio.run(connect(T()))
        self.assertIsNone(t._client.filter)
        self.assertEqual(t._client.notifying, [SMP_CHAR])
        self.assertEqual(t.mtu, 244)
        r, calls = self.prove(self.smp(t._client), {"proof_key": b"k" * 32})
        self.assertEqual((r, calls), ("match", [("proof", True, b"k" * 32)]))
        self.assertEqual(t._client.notifying, [SMP_CHAR, C.P["MG_CONTROL_UUID"].lower()])

        with FakeBleak():
            stock = asyncio.run(connect(smpble.SMPBLETransport()))
        self.assertEqual(stock._client.filter, {SMP_SERVICE})
        with self.assertRaisesRegex(ValueError, "not found"):
            self.prove(self.smp(stock._client), {"proof_key": b"k" * 32})

    def test_no_record_stops_unless_unproven(self):
        with self.assertRaises(SystemExit):
            self.run_prove(None)
        self.assertEqual(self.run_prove(None, unproven=True)[:2], ("unproven", []))

    def test_anything_but_a_match_stops_before_any_write(self):
        # not_found included: a look-alike that answers not_found to the saved key gets nothing.
        for result in ("mismatch", "device_mac_mismatch", "not_found"):
            with self.assertRaises(SystemExit):
                self.run_prove({"proof_key": b"k" * 32}, result)

    @staticmethod
    def mcuboot_image(body: bytes) -> bytes:
        import hashlib
        import struct
        hdr = struct.pack("<IIHHII", C.MCUBOOT_IMAGE_MAGIC, 0, 0x200, 0, len(body), 0)
        hdr += struct.pack("<BBHI", 0, 1, 1, 0) + b"\0" * 4
        hdr += b"\xff" * (0x200 - len(hdr))
        tlvs = struct.pack("<HH", 0x10, 32) + hashlib.sha256(hdr + body).digest()
        return hdr + body + struct.pack("<HH", C.MCUBOOT_TLV_INFO_MAGIC, 4 + len(tlvs)) + tlvs

    def run_dfu(self, body: bytes, **flags):
        """dfu's check of the image before it connects: 'connects', or the SystemExit that stops it."""
        import types

        args = types.SimpleNamespace(image="zephyr.signed.bin", unproven=False, address=None, allow_token_image=False)
        for k, v in flags.items():
            setattr(args, k, v)
        C.dfu_image_check(args, self.mcuboot_image(body))
        return "connects"

    def test_real_tokens_need_an_address(self):
        """setup and reprovision send --tokens only to the gadget --address names, checked before any scan."""
        import types

        with self.assertRaisesRegex(SystemExit, "--tokens needs --address"):
            C.tokens_need_address(types.SimpleNamespace(tokens="-", address=None))
        try:
            import bleak  # noqa: F401
        except ImportError:
            self.skipTest("bleak isn't installed: the commands themselves aren't run")

        class Scans(Exception):
            pass

        async def find_setup(args):
            raise Scans()

        saved = C.find_setup
        C.find_setup = find_setup
        try:
            for cmd in (C.cmd_setup, C.cmd_reprovision):
                args = types.SimpleNamespace(tokens="-", address=None, name=None, scan_secs=0, confirm_serial=None)
                with self.assertRaisesRegex(SystemExit, "--tokens needs --address"):
                    asyncio.run(cmd(args))
                args.address = "AA:BB:CC:DD:EE:FF"
                with self.assertRaises(Scans):
                    asyncio.run(cmd(args))
                args.tokens, args.address = None, None   # generated bench tokens: any gadget
                with self.assertRaises(Scans):
                    asyncio.run(cmd(args))
        finally:
            C.find_setup = saved

    @staticmethod
    def fake_token() -> bytes:
        """A token of the real shape (as esp32 main/identity.c takes one), made here so none sits in the repo."""
        import base64
        return b"mgst_" + base64.urlsafe_b64encode(os.urandom(32)).rstrip(b"=")

    def test_dfu_token_check_matches_the_token_shape(self):
        # Every ESP32 image carries the firmware's own mgst_ strings (identity.c's check, a console hint): not tokens.
        # Near misses too: a last character that carries more than four bits, one character short.
        own = b"\0".join([b"mgst_", b"or send >sdk_token=mgst_... on the console)", b"mgst_" + b"x" * 43,
                          b"mgst_" + b"A" * 42])
        self.assertEqual(C.image_token_findings(b"\0" * 64 + own + b"\0" * 64), [])
        self.assertEqual(self.run_dfu(b"\0" * 64 + own + b"\0" * 64), "connects")
        token = self.fake_token()
        self.assertEqual(len(token), 48)
        self.assertEqual(C.image_token_findings(own + b"\0" + token + b"\0"), ["it holds an SDK token (mgst_...)"])
        with self.assertRaisesRegex(SystemExit, "--allow-token-image"):
            self.run_dfu(b"\0" * 64 + own + b"\0" + token + b"\0" * 64)

    def test_dfu_refuses_a_token_image_in_every_mode(self):
        token_body = b"\0" * 64 + self.fake_token() + b"\0" * 64
        with self.assertRaisesRegex(SystemExit, "--allow-token-image"):
            self.run_dfu(token_body)
        self.assertEqual(self.run_dfu(token_body, allow_token_image=True), "connects")
        self.assertEqual(self.run_dfu(b"\0" * 256), "connects")
        # --unproven never sends one, the flag or not.
        with self.assertRaisesRegex(SystemExit, "proven gadget"):
            self.run_dfu(token_body, unproven=True, address="AA:BB", allow_token_image=True)
        self.assertEqual(self.run_dfu(b"\0" * 256, unproven=True, address="AA:BB"), "connects")

    def test_dfu_refuses_a_filled_token_slot(self):
        marker = b"MG-SDK-TOKEN-SLOT-v1:"
        blank = marker + b"\0" * (128 - len(marker))
        self.assertEqual(self.run_dfu(b"\0" * 64 + blank + b"\0" * 64), "connects")
        # Not blank: something was written into the slot behind the marker (no mgst_ prefix to find).
        filled = marker + b"tok-" + b"y" * 40 + b"\0" * (128 - len(marker) - 44)
        with self.assertRaisesRegex(SystemExit, "token slot .* isn't blank"):
            self.run_dfu(b"\0" * 64 + filled + b"\0" * 64)
        self.assertEqual(self.run_dfu(b"\0" * 64 + filled + b"\0" * 64, allow_token_image=True), "connects")
        # Only an MCUboot image's payload is read for the slot: other bytes are an unknown format.
        self.assertEqual(C.image_token_findings(b"\0" * 64 + filled), [])

    def test_unproven_needs_an_address_and_a_token_free_image(self):
        def check(data, address):
            C.unproven_dfu_check(type("A", (), {"address": address})(), data)
        with self.assertRaises(SystemExit):
            check(b"image", None)
        with self.assertRaises(SystemExit):
            check(b"image ... " + self.fake_token(), "AA:BB")
        check(b"image", "AA:BB")
        check(b"image ... mgst_0123", "AA:BB")   # the prefix alone isn't a token

    def test_dfu_does_not_reset_after_a_failed_slot_check(self):
        """dfu --test --reset: an image that isn't in the second slot afterwards is neither marked nor reset into."""
        import types

        sent = []

        class ResetWrite:
            pass

        mod = types.ModuleType("smpclient.requests.os_management")
        mod.ResetWrite = ResetWrite
        names = ("smpclient", "smpclient.requests", "smpclient.requests.os_management")
        saved_mods = {n: sys.modules.get(n) for n in names}
        for n in names[:2]:
            if saved_mods[n] is None:
                sys.modules[n] = types.ModuleType(n)
        sys.modules[names[2]] = mod

        class Client:
            _transport = types.SimpleNamespace(mtu=247, max_unencoded_size=240)

            async def connect(self, timeout):
                pass

            async def request(self, req):
                sent.append(req)
                return types.SimpleNamespace(rc=0)

            async def disconnect(self):
                pass

        async def pick(args):
            return "AA:BB", None

        async def smp_client(args, addr):
            return Client()

        async def none(*a, **k):
            return None

        async def listing(client):
            return []   # nothing in slot 1: the check fails

        async def upload(client, data, window, progress):
            return 1

        async def pressed(client, req, what):
            sent.append(req)
            return types.SimpleNamespace(rc=0)

        patch = {"pick": pick, "smp_client": smp_client, "smp_prove": none, "smp_params": none, "smp_list": listing,
                 "smp_upload": upload, "smp_request_pressed": pressed}
        saved = {k: getattr(C, k) for k in patch}
        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / "zephyr.signed.bin"
            img.write_bytes(SmpProvesFirst.mcuboot_image(b"\0" * 256))
            args = types.SimpleNamespace(image=str(img), test=True, confirm=False, unproven=False, address="AA:BB",
                                         allow_token_image=False, window=1, reset=True)
            try:
                for k, v in patch.items():
                    setattr(C, k, v)
                rc = asyncio.run(C.cmd_dfu(args))
            finally:
                for k, v in saved.items():
                    setattr(C, k, v)
                for n, m in saved_mods.items():
                    if m is None:
                        sys.modules.pop(n, None)
                    else:
                        sys.modules[n] = m
        self.assertEqual(rc, 1)
        self.assertEqual(sent, [], "a mark or a reset after the failed check")

    def test_queue_clear_needs_the_listed_queue(self):
        def status(clips, used):
            return bytes([C.P["mg_command_audio_queue"], C.P["mg_audio_queue_command_status"], 1]) \
                + used.to_bytes(4, "little") + (65536).to_bytes(4, "little") + clips.to_bytes(2, "little")
        self.assertTrue(C.queue_unchanged(status(2, 900), status(2, 900)))
        self.assertFalse(C.queue_unchanged(status(2, 900), status(3, 1400)), "a clip recorded since the listing")
        self.assertFalse(C.queue_unchanged(status(2, 900), status(2, 1000)))
        self.assertFalse(C.queue_unchanged(status(2, 900), b"\0" * 5))

    def test_queue_names_are_unique_per_batch(self):
        """A second batch (the gadget numbers its clips from 0 again) never overwrites the first's files."""
        with tempfile.TemporaryDirectory() as d:
            first = C.clip_path(d, "20261125-230000", 0)
            first.write_bytes(b"one")
            self.assertNotEqual(C.clip_path(d, "20261125-230500", 0), first, "the next batch reuses clip 0's name")
            again = C.clip_path(d, "20261125-230000", 0)
            self.assertNotEqual(again, first, "a batch in the same second overwrote clip 0")
            first.unlink()
            first.with_suffix(".lc3").write_bytes(b"one")
            self.assertNotEqual(C.clip_path(d, "20261125-230000", 0), first, "the clip's .lc3 overwritten")
            self.assertNotEqual(C.clip_path(d, "20261125-230000", 1), first)


class ProvesOnItsOwnConnection(unittest.TestCase):
    """A set-up gadget takes audio, a stream or a call's states only on a token-proven connection: play (plain,
    without --loopback or --volume), ui and call prove on their own connection, with the saved key, before
    their first write past request_status."""

    ADDR = "AA:BB:CC:DD:EE:FF"

    class Stop(Exception):
        pass

    def run_cmd(self, cmd, **flags):
        import types

        log: list = []
        cmds = [C.P[n] for n in ("mg_command_request_status", "mg_command_stream_audio", "mg_command_stream_video",
                                 "mg_command_live_call", "mg_command_set_settings", "mg_command_token_proof")]
        stop = self.Stop

        class G:
            def __init__(self, client, verbose=False):
                self.client, self.nus, self.data, self.nus_watch = client, [], [], None

            async def start(self, nus=True):
                pass

            async def status(self):
                return {"data_type": b"", "commands": cmds, "typed": {}, "settings": None, "others": [],
                        "recovery": None}

            async def command_list(self, typed=None):
                return bytes([C.P["mg_command_supported_features"]] + cmds)

            async def send(self, command, *params):
                log.append(("send", command))
                if command != C.P["mg_command_request_status"]:
                    raise stop(command)

            async def expect(self, command, timeout=3.0, sub=None):
                raise TimeoutError

            def drain(self):
                return []

        class Client:
            def __init__(self, addr, timeout=20.0):
                self.address, self.mtu_size = addr, 247

            async def __aenter__(self):
                return self

            async def __aexit__(self, *exc):
                return False

        async def pick(args):
            return self.ADDR, None

        async def prove(g, addr, st):
            log.append(("proof", addr))
            return "match"

        async def att_mtu(client):
            return 247

        args = types.SimpleNamespace(**flags)
        saved = {k: getattr(C, k) for k in ("Gadget", "pick", "prove_connection", "att_mtu", "Decoder",
                                            "load_ui_frames")}
        saved_bleak = sys.modules.get("bleak")
        C.Gadget, C.pick, C.prove_connection, C.att_mtu = G, pick, prove, att_mtu
        C.Decoder = lambda: None
        C.load_ui_frames = lambda f: ([b"\0"], 1)
        sys.modules["bleak"] = types.SimpleNamespace(BleakClient=Client)
        try:
            with self.assertRaises(self.Stop):
                asyncio.run(cmd(args))
        finally:
            for k, v in saved.items():
                setattr(C, k, v)
            if saved_bleak is None:
                sys.modules.pop("bleak", None)
            else:
                sys.modules["bleak"] = saved_bleak
        writes = [e for e in log if e != ("send", C.P["mg_command_request_status"])]
        return writes

    def test_plain_play_proves_first(self):
        writes = self.run_cmd(C.cmd_play, wav=None, sweep=False, rate=16000, tone=1000.0, secs=0.1, level=-12.0,
                              loopback=False, volume=None, verbose=False, bitpool=26, out="x.wav", address=None)
        self.assertEqual(writes[0], ("proof", self.ADDR), f"play wrote before the token proof: {writes}")

    def test_ui_proves_first(self):
        writes = self.run_cmd(C.cmd_ui, file="x.264", bands=None, index=None, verbose=False, address=None)
        self.assertEqual(writes[0], ("proof", self.ADDR), f"ui wrote before the token proof: {writes}")

    def test_call_proves_first(self):
        # The gadget's own start, answered: its states go to a proven connection.
        writes = self.run_cmd(C.cmd_call, state=None, start=False, answer="live", connect=1.0, end_after=None,
                              secs=1.0, address=None)
        self.assertEqual(writes[0], ("proof", self.ADDR), f"call wrote before the token proof: {writes}")


if __name__ == "__main__":
    unittest.main()
