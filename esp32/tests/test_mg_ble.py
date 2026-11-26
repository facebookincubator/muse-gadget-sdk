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

"""musegadgets BLE: the protocol state machine
(components/muse_gadget_ble/mg_proto.c) with the token proof against
protocols/test-vectors/mg-token-proof-v1.json, the offline clip queue with
power cuts, and the BLE/Wi-Fi routing policy, compiled for the host."""

from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MG = ROOT / "components/muse_gadget_ble"
PROTOCOLS = ROOT.parent / "protocols"
PROOF_VECTORS = PROTOCOLS / "test-vectors/mg-token-proof-v1.json"
# The sealed device session's shared seal and the credentials-only session
# (portable/mg_device_session layers 1 and 2, vendored unchanged) on the
# component's token proof crypto: built with every harness that builds
# mg_session.c, as components/muse_gadget_ble does.
SEAL_SRCS = [ROOT.parent / "portable/mg_device_session/src/mg_session_seal.c", MG / "mg_seal_crypto.c",
             ROOT.parent / "portable/mg_device_session/src/mg_device_session.c"]
SEAL_INCS = [MG / "seal", ROOT.parent / "portable/mg_device_session/include"]


def proof_vectors_header(directory: Path) -> Path:
    """mg_proof_vectors.h: the token proof vectors as C strings (hex, and the UTF-8 inputs)."""
    data = json.loads(PROOF_VECTORS.read_text(encoding="utf-8"))

    def c(text: str) -> str:
        return '"' + "".join(f"\\x{b:02x}" for b in text.encode("utf-8")) + '"'

    rows = []
    for v in data["vectors"]:
        f = v["frames"]
        rows.append("    { " + ", ".join([
            c(v["access_token"]), c(v["node_id"]), c(v["client_nonce"]), c(v["device_nonce"]), c(v["K"]),
            c(v["device_mac"]), c(v["client_mac"]), c(f["challenge"]), c(f["response"]), c(f["confirm"]),
        ]) + " },")
    header = directory / "mg_proof_vectors.h"
    header.write_text(
        "#pragma once\n/* Generated from protocols/test-vectors/mg-token-proof-v1.json. */\n"
        "static const struct {\n    const char *access, *node, *cn, *dn, *k, *dmac, *cmac, *challenge, "
        "*response, *confirm;\n} PROOF_VECTORS[] = {\n" + "\n".join(rows) + "\n};\n")
    return header


def compile_and_run(test: unittest.TestCase, sources: list[Path], defines: list[str], includes: list[Path],
                    extra: list[str] | None = None, args: list[str] | None = None, cwd: Path | None = None,
                    vendor: list[Path] | None = None) -> str:
    """Builds the harness and firmware sources strictly, vendored codec sources as upstream ships them."""
    cc = shlex.split(os.environ.get("CC", "cc"))
    if any(Path(src).name == "mg_session.c" for src in sources):
        sources = [*sources, *(x for x in SEAL_SRCS if x not in sources)]
        includes = [*includes, *SEAL_INCS]
    with tempfile.TemporaryDirectory(prefix="mg-test-") as tmp:
        exe = Path(tmp) / "harness"
        # _DEFAULT_SOURCE: M_PI, strnlen and posix_memalign under -std=c11 on glibc.
        common = ["-std=c11", "-D_DEFAULT_SOURCE", "-O1", *[f"-D{d}" for d in defines]]
        for inc in includes:
            common += ["-I", str(inc)]
        objs = []
        for i, src in enumerate(vendor or []):
            obj = Path(tmp) / f"v{i}_{src.stem}.o"
            r = subprocess.run([*cc, *common, "-w", "-c", str(src), "-o", str(obj)], text=True, capture_output=True)
            test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            objs.append(str(obj))
        cmd = [*cc, *common, "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
               "-Wno-missing-field-initializers", *[str(s) for s in sources], *objs, *(extra or []),
               "-lm", "-o", str(exe)]
        r = subprocess.run(cmd, text=True, capture_output=True)
        test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = subprocess.run([str(exe), *(args or [])], text=True, capture_output=True, cwd=cwd or tmp)
        test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout


LC3_SRCS = ("attdet.c", "bits.c", "bwdet.c", "energy.c", "lc3.c", "ltpf.c",
            "mdct.c", "plc.c", "sns.c", "spec.c", "tables.c", "tns.c")


def lc3_build(fixed: bool) -> tuple[list[Path], list[str], list[Path]]:
    """One LC3 implementation as components/liblc3 builds it: the fixed-point
    lc3fx (xplat/lc3fx) or the floating-point liblc3 (xplat/liblc3)."""
    if fixed:
        l = ROOT.parent / "xplat/lc3fx"
        defs = ["LC3_DEC_LTPF=0", "LC3_DT_MASK=8", "LC3_SR_MASK=6"]
    else:
        l = ROOT.parent / "xplat/liblc3"
        defs = ["LC3_ENC_LTPF=0", "LC3_DEC_LTPF=0", "LC3_PLUS=0", "LC3_PLUS_HR=0", "LC3_DT_MASK=8",
                "LC3_SR_MASK=6"]
    return [l / "src" / f for f in LC3_SRCS], defs, [l / "include", l / "src"]


def codec_build(lc3: bool, fixed: bool = False, lc3_24k: bool = True) -> tuple[list[Path], list[str], list[Path]]:
    """Sources, defines and includes of the vendored codecs as the components build them.
    `fixed` picks the fixed-point LC3 (xplat/lc3fx) instead of liblc3."""
    sbc = ROOT.parent / "xplat/libsbc"
    srcs = [sbc / "src/sbc.c", sbc / "src/bits.c"]
    defs = ["SBC_MAX_CHANNELS=1", "SBC_WITH_4SB=0", "SBC_SMALL_CRC=1", "SBC_FAST_DCT=1",
            f"MG_WITH_LC3={int(lc3)}", f"MG_LC3_24K={int(lc3 and lc3_24k)}"]
    incs = [sbc / "include", sbc / "src"]
    if lc3:
        s, d, i = lc3_build(fixed)
        return srcs + s, defs + d, incs + i
    return srcs, defs, incs



class NusRingAcrossALink(unittest.TestCase):
    """mg_ble.c's NUS log ring, compiled from the glue itself (nus_ring_reset and nus_drain, the stack faked): a new
    link that empties the ring while a drain is between its copy and its debit (the host task's connect, during the
    drain's notify) moves the ring's generation on, so the drain debits nothing: the count never underflows, and the
    last link's retained bytes never go to the new, unproven central (#525 council). The new link's own reply, queued
    after the reset, goes out whole, and nothing else."""

    def test_a_connect_during_a_drain(self) -> None:
        ble = (MG / "mg_ble.c").read_text()

        def fn(head: str) -> str:
            i = ble.index(head)
            return ble[i:ble.index("\n}\n", i) + 3]
        decl = "".join(l + "\n" for l in ble.splitlines()
                       if l.startswith(("static uint32_t s_nus_gen;", "#define NUS_RING ")))
        conn = ble[ble.index("    case BLE_GAP_EVENT_CONNECT:"):]
        conn = conn[:conn.index("        break;")]
        if "static void nus_ring_reset(void)\n{" in ble:
            self.assertIn("nus_ring_reset();   /* nothing queued for an earlier link goes to this one */", conn)
            reset = fn("static void nus_ring_reset(void)\n{")
        else:   # a glue without it: what its connect did
            self.assertIn("s_nus_len = 0;   /* nothing queued for an earlier link goes to this one */", conn)
            reset = "static void nus_ring_reset(void) { s_nus_len = 0; }\n"
        drv = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mg_nus.h"
typedef int portMUX_TYPE;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define BLE_HS_CONN_HANDLE_NONE 0xffff
struct os_mbuf { uint8_t d[256]; uint16_t n; };
static portMUX_TYPE s_nus_mux;
static char ring[4096];
static char *s_nus_ring = ring;
static size_t s_nus_head, s_nus_len;
static volatile uint8_t s_nus_inflight;
static bool s_nus_sub = true;
static uint16_t s_conn = 1, s_nus_tx_handle = 9;
@DECL@
static uint16_t ble_att_mtu(uint16_t c) { (void)c; return 247; }
static int os_msys_num_free(void) { return 100; }
static struct os_mbuf mb;
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *b, uint16_t n) { memcpy(mb.d, b, n); mb.n = n; return &mb; }
static void nus_put(const char *t);
static int connect_in_notify;   /* a new link comes up while this notify is in the stack */
static char sent[8192];
static size_t sent_n;
static void nus_ring_reset(void);
static int ble_gatts_notify_custom(uint16_t c, uint16_t h, struct os_mbuf *om) {
    (void)c; (void)h;
    if (sent_n + om->n < sizeof(sent)) memcpy(sent + sent_n, om->d, om->n);
    sent_n += om->n;
    if (connect_in_notify && !--connect_in_notify) {
        nus_ring_reset();          /* the host task's BLE_GAP_EVENT_CONNECT */
        nus_put("NEWLINK\n");      /* the new link's own status reply */
    }
    return 0;
}
@RESET@
@DRAIN@
static void nus_put(const char *t) {
    for (size_t i = 0; t[i]; i++) s_nus_ring[(s_nus_head + s_nus_len++) % NUS_RING] = t[i];
}
int main(void) {
    /* The last link's mirror: 600 bytes queued (the drain takes 244 a notify). */
    for (int i = 0; i < 60; i++) nus_put("OLDLINK-x\n");
    connect_in_notify = 1;
    (void)nus_drain();   /* the first notify goes, then the connect */
    size_t first = sent_n;
    sent_n = 0;
    s_nus_inflight = 0;
    for (int i = 0; i < 20 && nus_drain(); i++) s_nus_inflight = 0;
    s_nus_inflight = 0;
    (void)nus_drain();
    sent[sent_n < sizeof(sent) ? sent_n : sizeof(sent) - 1] = 0;
    printf("%zu %zu %zu [%s]\n", first, s_nus_len, sent_n, sent);
    return 0;
}
"""
        src = (drv.replace("@DECL@", decl).replace("@RESET@", reset)
               .replace("@DRAIN@", fn("static bool nus_drain(void)\n{")))
        with tempfile.TemporaryDirectory(prefix="mg-nus-") as t:
            Path(t, "d.c").write_text(src)
            cc = shlex.split(os.environ.get("CC", "cc"))
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                                "-fsanitize=address,undefined", "-I", str(MG), str(Path(t, "d.c")),
                                "-o", str(Path(t, "d"))], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(Path(t, "d"))], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        first, left, n, text = r.stdout.split(" ", 3)
        self.assertEqual((first, left), ("244", "0"), r.stdout)   # one notify of the old link's, then nothing owed
        self.assertEqual(text.strip(), "[NEWLINK\n]", r.stdout)   # the new link gets its own reply, and only that
        self.assertEqual(n, "8")

class MgBleTest(unittest.TestCase):
    def test_protocol_queue_and_routing(self) -> None:
        # The token proof's crypto is the host reference here; test_mg_token_proof
        # runs the same harness on the device's PSA implementation.
        # LC3 at 24 kHz listed (the 24 kHz gate on) and not (the shipping default).
        for lc3, fixed, k24 in ((False, False, False), (True, False, True), (True, True, True), (True, False, False)):
            with self.subTest(lc3=lc3, fixed=fixed, lc3_24k=k24), tempfile.TemporaryDirectory(prefix="mg-gen-") as gen:
                proof_vectors_header(Path(gen))
                srcs, defs, incs = codec_build(lc3, fixed, k24)
                out = compile_and_run(
                    self,
                    [ROOT / "tests/mg_proto_harness.c", MG / "mg_proto.c", MG / "mg_linkpol.c", MG / "mg_session.c", MG / "mg_ui.c", MG / "mg_queue.c",
                     MG / "mg_route.c", MG / "mg_codec.c", MG / "mg_play.c", MG / "mg_resample.c",
                     ROOT / "components/muse_vstream/muse_vstream.c", ROOT / "tests/mg_token_proof_ref.c"],
                    defs, [Path(gen), MG, MG / "include", ROOT / "components/muse_vstream/include", PROTOCOLS, *incs], vendor=srcs)
                self.assertIn("PASS mg_proto", out)
                if lc3:
                    self.assertIn("lc3 fits the link: a 96 kb/s capture at MTU 100 takes 96-byte frames", out)
                if not lc3:
                    self.check_stream_vectors(out)
                # speaker_volume reports owed until they leave (the volume report check).
                for line in ("spk owed, busy stack: refused, owed, sent once the stack took it (level 0)",
                             "spk owed, full outbox: 16 replies first, then the report (level 25), 0 dropped",
                             "spk owed, disconnect with one owed: sent at the next ready (level 60)",
                             "spk owed, change while disconnected: sent at the ready (level 70)",
                             "spk owed, no duplicate: nothing at a reconnect with the level the client saw",
                             "spk owed, client write: not echoed, the owed report superseded"):
                    self.assertIn(line, out)
                # The bench guard's held turn (ptt.minrise): off is today's turn to the
                # byte and the instant; a turn that never rose sends nothing; one that rose sends all of
                # itself from touch-down, in order, 0 dropped (3.4 s held too, on three links); a reply at
                # touch-down is held through it and resumes when it never rose.
                for line in ("minrise off: ", "identical to a turn that never asks",
                             "minrise 15, never rose (rise ", "0 control messages, 0 Data bytes, ptt_nothing 1",
                             "minrise 15, rose at ", "minrise long hold, ready link: ",
                             "minrise long hold, sleep link: ", "minrise long hold, ready link, 32 KB (C6): ",
                             "minrise grow: ", "minrise and a reply: rose",
                             "minrise and a reply, never rose: the reply held through the turn, then resumed"):
                    self.assertIn(line, out)
                for line in out.splitlines():
                    if line.startswith("minrise long hold"):
                        self.assertIn(" in order, 0 dropped", line)
                # Mic output only to a central that passed this connection's
                # token proof. Talk before the proof is held whole and sent in order at the match; a failed
                # proof, a lost key or the link's end discards it; a full hold stops the capture and keeps
                # what it holds (the C6's 32 KB, PSRAM's whole turn); the bench switch lifts the gate; no key,
                # no output; receive paths ungated; a re-proof holds from its challenge.
                for line in ("output proof, talk before the proof: 0 Data bytes before it",
                             "output proof, proof fails: 20 held frames discarded unsent, the capture ended; no key: discarded",
                             "output proof, link drops mid-hold: 40 held frames discarded",
                             "output proof, hold full (C6, 32 KB): capture stopped at frame 546",
                             "output proof, PSRAM hold: 2000 frames (16 s) held whole",
                             "output proof, bench switch: no gate",
                             "owner reads: unproven on a set-up release gadget, client_app skipped then proof_required, get_unix_time proof_required; proven, both whole",
                             "output proof, bench switch over the Nordic UART: this connection only",
                             "output proof, no token: no capture, start_mic and read_clip proof_required, 0 Data bytes",
                             "clips proof-gated: set up and unproven, status 0 clips / 0 bytes, clip_info and read_clip proof_required; proven, the real ones; not set up, status and clip_info as they are",
                             "queue mount read error: the clips before it kept, none written over the unread rest",
                             "stop answers: two keeps while draining, two keeps at the end; keep then drop, two drops at once",
                             "credit overrun, every build: audio past the credit dropped whole and counted (received 1344); video past it dropped and counted, a keyframe asked for once",
                             "queue wipe: the log gone at once, the whole store erased sector by sector (blank ones skipped), no clips meanwhile; a power cut resumes from the mark; residue past an own clear wiped too",
                             # The clips go when the pairing changes hands.
                             "output proof, ownership change: the clips cleared; the next proven owner finds none, after a reboot too",
                             "output proof, receive: Control answers before any proof; streams to a set-up gadget wait for it, then playback 160 B and video",
                             "output proof, re-proof mid-capture: held from the challenge",
                             "output proof, staleness: released 2.9 s after the capture's end: sent; 3.1 s: discarded, counted",
                             "output proof, start_mic before the proof: opens with a challenge outstanding (held), else proof_required; bench switch: neither applies",
                             "PASS output proof",
                             # Streams to a set-up gadget need the token proof.
                             "stream vectors: streams to a set-up device: ",
                             "stream vectors: streams and the bench override on an automation build: ",
                             "stream vectors: streams and the bench override on a release build: ",
                             "stream vectors: streams to a device that isn't set up: ",
                             "haptics behind the proof: release haptic_play proof_required until the proof, stop always runs; bench plays unproven",
                             "skills: none offered, answered unsupported unproven, release and bench",
                             "streams end with their proof: a failed re-proof, the token's clear and the bench switch off each end the open audio and video",
                             "PASS destructive commands: power_off and the queue's erase need the proof once set up",
                             "PASS stored settings: set_settings that stores needs the proof once set up; the clock stores only then",
                             "PASS owner_ok: not set up, or this connection's proof",
                             # A client's start_mic needs a press of the device's own (release builds).
                             "press for mic: refused without a press, taken 5000 ms after one, refused at 5001",
                             "press for mic: a wake event counts, an injected press doesn't",
                             "press for mic: the proof first; a relay with a forwarded proof and no press refused",
                             "press for mic: an automation build takes the proof alone",
                             "press for mic: a press 2^32 ms back is never taken again",
                             "PASS press for mic",
                             # A parked command never runs on the voice task (its stack can be in PSRAM).
                             "PASS waiting commands: a parked set_settings never runs on the voice task's audio path, only on the worker's pump",
                             # The mute switch discards a turn still waiting on the proof.
                             "PASS mute: a turn not kept and still held for the proof is discarded, nothing of it sent after the proof",
                             "PASS throughput test: a stalled link ends a run in 2 s",
                             "PASS throughput test open unproven on a set-up gadget, release and bench"):
                    self.assertIn(line, out)
                # The held live turn: a press in the connect
                # burst is held, SBC 16 kHz, and goes live once proven and ready; not by +3 s it's a valid
                # SBC clip; a failed proof discards it; a dropped link queues it; no queue keeps holding.
                for line in ("held turn, proven and ready at +2 s: button_down, change_data_type SBC",
                             "held turn, not proven and ready by +3 s: a 21600 B SBC 16 kHz clip of all 360 frames, nothing live",
                             "held turn, proof fails in the window: 100 frames discarded, nothing queued",
                             "held turn, link drops in the window: a 7200 B clip of all 120 frames",
                             "held turn, no queue at the deadline: kept; live when ready 2.9 s after its end, discarded and counted at 3.1 s",
                             "held turn, hold full (C6, 32 KB): stopped at frame 546",
                             "held turn, bench switch: proven, live at the client's ready",
                             "held turn, unproven central asks: request_status says the pre-capture type",
                             "PASS held live turn"):
                    self.assertIn(line, out)

    def test_the_clips_go_when_the_pairing_changes_hands(self) -> None:
        # A setup reset and a new setup's commit (token-only or full) reach
        # app.c passcode_forget() only after the pairing's own erase or commit succeeded (a failed
        # re-provision keeps the old owner's clips with the old setup); it clears the offline clips
        # through mg_glue and mg_ble, under the protocol's lock, and wipes the whole partition.
        # mg_proto_harness's (6b) runs the clear and the next owner's reads.
        def body(text: str, head: str) -> str:
            i = text.index(head)
            return text[i:text.index("\n}\n", i)]

        app = (ROOT / "main/app.c").read_text()
        forget = body(app, "static bool passcode_forget(const char *why) {")
        self.assertIn("#if CONFIG_MUSE_GADGET_BLE_AUDIO\n    // So are the offline clips: the next owner never reads the "
                      "last one's.\n", forget)
        self.assertIn('if (mg_glue_queue_forget(why)) (void)config_erase_key("mg_q_forget");', forget)
        commit = body(app, "static bool commit_setup(void) {")
        self.assertLess(commit.index("if (resetup && !config_reprovision_commit()) return false;"),
                        commit.index('passcode_forget("Link setup");'))
        reset = body(app, "static bool reset_setup_with_gate_held(const char *source,")
        self.assertLess(reset.index("if (!setup_wipe_to_clean(scope)) {"), reset.index('passcode_forget("setup reset");'))
        glue = (ROOT / "main/mg_glue.c").read_text()
        self.assertIn("bool mg_glue_queue_forget(const char *why) {\n    return mg_ble_queue_forget(why);\n}", glue)
        mgble = (MG / "mg_ble.c").read_text()
        ble = body(mgble, "bool mg_ble_queue_forget(const char *why)\n{")
        # Under the lock: the wipe's mark first (NVS), then the head's erase and the rest armed
        # (mg_queue_wipe_begin) and the background wipe started; test_queue_wipe runs the wipe.
        self.assertLess(ble.index("mg_lock();"), ble.index("bool marked = queue_wipe_mark(to);"))
        self.assertLess(ble.index("bool marked = queue_wipe_mark(to);"),
                        ble.index("bool ok = mg_queue_wipe_begin(&s_queue, to);"))
        self.assertIn("s_have_queue = false;", ble)   # a failed clear leaves nothing readable this boot
        # A boot that finds the mark wipes again before anything reads the queue.
        mount = body(mgble, "static void mount_queue(void)\n{")
        self.assertLess(mount.index("s_have_queue = mg_queue_mount(&s_queue, &st);"),
                        mount.index("s_have_queue = mg_queue_wipe_begin(&s_queue, wipe);"))
        proto = (MG / "mg_proto.c").read_text()
        self.assertIn("p->transfer = false;\n    return !p->cfg.queue || mg_queue_clear(p->cfg.queue);",
                      body(proto, "bool mg_proto_queue_forget(mg_proto_t *p)\n{"))

    def test_requests_never_read_past_their_end(self) -> None:
        # Requests in heap buffers of exactly their length under ASan: a
        # 1-byte setting last, values cut short, start_mic's rate and bitrate
        # cut short (the XIAO's set_one read 2 bytes for 1-byte settings).
        cc = shlex.split(os.environ.get("CC", "cc"))
        probe = subprocess.run([*cc, "-fsanitize=address", "-x", "c", "-", "-o", os.devnull],
                               input="int main(void){return 0;}", text=True, capture_output=True)
        if probe.returncode != 0:
            self.skipTest("no AddressSanitizer here")
        with tempfile.TemporaryDirectory(prefix="mg-gen-") as gen:
            proof_vectors_header(Path(gen))
            srcs, defs, incs = codec_build(True, False, True)
            out = compile_and_run(
                self,
                [ROOT / "tests/mg_settings_asan_harness.c", MG / "mg_proto.c", MG / "mg_session.c", MG / "mg_ui.c", MG / "mg_queue.c",
                 MG / "mg_route.c", MG / "mg_codec.c", MG / "mg_play.c", MG / "mg_resample.c",
                 ROOT / "components/muse_vstream/muse_vstream.c", ROOT / "tests/mg_token_proof_ref.c"],
                defs, [Path(gen), MG, MG / "include", ROOT / "components/muse_vstream/include", PROTOCOLS, *incs],
                extra=["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"], vendor=srcs)
            self.assertIn("PASS no over-read", out)


    def test_output_proof_wiring(self) -> None:
        # The output proof's gate is on in every firmware build (mg_ble sets
        # cfg.output_proof outside any #if or proof-key condition, before the proto's init), so
        # no release image differs from a bench one; the harness covers the protocol side.
        ble = (MG / "mg_ble.c").read_text()
        init = ble[ble.index("mg_proto_config_t cfg = {"):]
        init = init[:init.index("mg_proto_init(&s_proto, &cfg, &ops, &settings);")]
        at = init.index("cfg.output_proof = true;")
        self.assertEqual(init[:at].count("#if") - init[:at].count("#endif"), 0, "set under an #if")
        line_start = init.rfind("\n", 0, at) + 1
        self.assertEqual(init[line_start:at], "    ", "set inside a block")
        # The USB console's bench switch (kept across connections) is open in every build; the Nordic UART
        # reaches only its own connection's, never the USB one, nor through !IN or ble_console.
        glue = (ROOT / "main/muse_glue.c").read_text()
        word = glue[glue.index('if (!strncmp(line, "bench.out", 9)) {'):]
        word = word[:word.index("mg_ble_bench_out_word(line, out, sizeof(out))")]
        self.assertNotIn("automation off", word)
        self.assertNotIn("mg_ble_bench_out_word(line", ble.replace("bool mg_ble_bench_out_word(const char *line", ""))
        mg_glue = (ROOT / "main/mg_glue.c").read_text()
        bench_input = mg_glue[mg_glue.index("static int ble_bench_input(char *line) {"):]
        bench_input = bench_input[:bench_input.index("\n}\n")]
        ble_console = mg_glue[mg_glue.index("static bool ble_console(char *line) {"):]
        ble_console = ble_console[:ble_console.index("\n}\n")]
        for allow in (bench_input, ble_console):
            self.assertNotIn("bench.out", allow)
        # The bench override over Bluetooth: the Nordic UART's bench.out=1|0 is this connection's
        # switch, which a release build takes only on a proven connection (it holds '='), logged on
        # and off with the connection, ended (and logged) at the disconnect.
        nus = ble[ble.index("static void nus_command(char *line)\n{"):]
        nus = nus[:nus.index("\n}\n")]
        self.assertLess(nus.index("if (!mg_unlock_remote_ok(line, proven, s_platform.safe_mode)) {"),
                        nus.index("if (nus_bench_out(line)) {"))
        word = ble[ble.index("static bool nus_bench_out(const char *line)\n{"):]
        word = word[:word.index("\n}\n")]
        self.assertIn("mg_proto_set_output_bench_conn(&s_proto, line[10] == '1');", word)
        self.assertNotIn("mg_proto_set_output_bench(&s_proto", word)
        self.assertIn('"bench.out on (Nordic UART, connection %u, handle %u): mic output goes to this central "', word)
        self.assertIn('"bench.out off (Nordic UART, connection %u, handle %u): bench.out=0"', word)
        disc = ble[ble.index("case BLE_GAP_EVENT_DISCONNECT:"):]
        disc = disc[:disc.index("case BLE_GAP_EVENT_SUBSCRIBE:")]
        self.assertLess(disc.index('"bench.out off (Nordic UART, connection %u, handle %u): disconnected"'),
                        disc.index("mg_proto_disconnect(&s_proto);"))
        # Camera output over the device session goes only while this connection's proof holds:
        # checked at the request and again before the frame is sent.
        app = (ROOT / "main/app.c").read_text()
        cam = app[app.index("static bool camera_output_ok(void) {"):]
        cam = cam[:cam.index("\n}\n")]
        self.assertIn("#if CONFIG_MUSE_GADGET_BLE_SESSION\n    return mg_ble_output_ok();", cam)
        task = app[app.index("static void watcher_camera_capture_task(void *arg) {"):]
        task = task[:task.index("noise_ctrl_send_command_result(")]
        self.assertIn("if (ok && !camera_output_ok()) {", task)
        req = app[app.index('if (strcmp(command, "camera.capture") == 0) {'):]
        req = req[:req.index("calloc(")]
        self.assertIn("if (!camera_output_ok()) {", req)
        # A held capture that fills stops the capture (mg_voice ends it on the refusal; muse_voice's
        # loop breaks on mg_voice_audio false, turning the mic ring off).
        voice = (ROOT / "components/muse/muse_voice.c").read_text()
        self.assertIn("if (!mg_voice_audio(s_chunk, MUSE_AUDIO_CHUNK)) {\n            break;", voice)
        # A held live turn starts before its button_down (so the gesture is held with it), on both voice
        # loops, and is a BLE turn to the UI until it went to the clip queue.
        hold = voice.index("mg_voice_held_begin();")
        self.assertLess(hold, voice.index("gesture_down();", hold))
        self.assertIn("route == MG_ROUTE_BLE || route == MG_ROUTE_QUEUE || route == MG_ROUTE_HELD", voice)
        self.assertIn("if (ble && mg_voice_turn_queued()) {", voice)
        main_voice = (ROOT / "main/voice.c").read_text()
        mg_turn = main_voice[main_voice.index("static void mg_turn("):]
        self.assertLess(mg_turn.index("mg_voice_held_begin();"), mg_turn.index("mg_voice_gesture(true)"))
        voice_ble = (MG / "mg_voice.c").read_text()
        # Nothing about a held capture reaches an unproven central: the NUS status
        # line's live flag and hold counts, and the link's activity. The log mirror stays open.
        status = ble[ble.index('if (!strcmp(line, "status")) {'):]
        status = status[:status.index('mg_ble_reply_stats(false, replies, sizeof(replies));')]
        self.assertIn("p->live && visible", status)
        self.assertIn('if (!visible) {\n            ESP_LOGI(TAG, "output: %s", mg_proto_output_state(p));', status)
        self.assertIn("if (n > 0 && nus_tag_allowed(line)) {", ble)   # console output open for automation
        self.assertNotIn("nus_capture_quiet", ble)
        self.assertIn('(s_capture.requested || held_capture) && capture_visible ? "push-to-talk capture"', ble)
        self.assertIn("if (!quiet) {\n            mg_ble_link_activity();", voice_ble)
        self.assertIn("if (down && sent && !quiet) {", voice_ble)
        self.assertIn('if (!quiet) {\n        mg_ble_link_action("push-to-talk capture");', voice_ble)
        # Each of these rules is one switch.
        cfg = (MG / "mg_config.h").read_text()
        voice_ble = (MG / "mg_voice.c").read_text()
        route = voice_ble[voice_ble.index("mg_route_t mg_voice_route(bool wifi_ready)"):]
        route = route[:route.index("mg_route_decide(POLICY, &in)")]
        self.assertIn("if (in.held_ok && !(in.queue_available && in.queue_enabled)) {", route)   # boards that can hold it
        self.assertIn("mg_ble_can_hold(", route)
        self.assertIn("MG_HELD_TURN_MIN_HOLD_MS", route)
        for define, value in (("MG_OUTPUT_STALE_MS", "3000"), ("MG_HELD_TURN_MIN_HOLD_MS", "2000"),
                              ("MG_HELD_TURN_MS", "3000"), ("MG_HELD_TURN_FAIL_QUEUES", "0"),
                              ("MG_HELD_TURN_DROP_QUEUES", "1"), ("MG_HELD_TURN_NO_QUEUE_HOLDS", "1"),
                              ("MG_HELD_TURN_SBC", "1")):
            self.assertIn(f"#define {define} {value}\n", cfg)

    def check_stream_vectors(self, out: str) -> None:
        # The session vectors' streams_and_updates (mg-session-v1.json): the stream exchanges' writes and every
        # notification, byte for byte, through the real mg_proto (mg_proto_harness's test_stream_proof prints them).
        doc = json.loads((PROTOCOLS / "test-vectors" / "mg-session-v1.json").read_text())
        harness = (ROOT / "tests/mg_proto_harness.c").read_text()
        writes = {"audio": re.search(r'#define SP_AUDIO "([^"]+)"', harness).group(1).replace(" ", ""),
                  "video": re.search(r'#define SP_VIDEO "([^"]+)"', harness).group(1).replace(" ", "")}
        for ex in doc["streams_and_updates"]["exchanges"]:
            if not ex["name"].startswith("streams"):
                continue   # the SMP update writes: the update owner's rows
            want = []
            for st in ex["steps"]:
                if "write" in st:
                    self.assertIn(st["write"], writes.values(), ex["name"])
                    want.append("|".join(st["notify"]))
            line = next((l for l in out.splitlines() if l.startswith(f"stream vectors: {ex['name']}: ")), None)
            self.assertIsNotNone(line, ex["name"])
            got = line.split(": ", 2)[2].split()
            self.assertEqual(len(got), len(want), ex["name"])
            for g, w in zip(got, want):
                if w.startswith("19"):
                    self.assertEqual(g, w, ex["name"])   # an error: byte for byte
                else:
                    # A taken stream's buffer_update: the reference's fields byte for byte; this firmware's
                    # buffer_update carries later fields after them (the reference models none).
                    self.assertTrue(g.startswith(w), f"{ex['name']}: {g} vs {w}")

    def test_throughput_test_stays_open(self) -> None:
        # The throughput test runs for an unproven central on a set-up gadget, on every build (mg_proto_harness
        # test_throughput_open): no proof gate and no build switch for it.
        proto = (MG / "mg_proto.c").read_text()
        action = proto[proto.index("static void cmd_device_action("):]
        action = action[:action.index("\n}\n")]
        tp = action[action.index("if (a[0] != mg_device_action_throughput_test) {"):]
        self.assertNotIn("proof_required", tp)
        self.assertNotIn("MG_PROOF_MATCHED", tp)
        for f in (MG / "mg_ble.c", MG / "mg_proto.h", MG / "mg_proto.c"):
            self.assertNotIn("tp_open", f.read_text(), f.name)

    def test_press_for_mic_window_and_sources(self) -> None:
        # The session vectors' mic_gate window is the firmware's; only the gadget's own talk presses count (its talk
        # key from the board's poll, a finger on a talk surface), and release builds turn the rule on.
        mic = json.loads((PROTOCOLS / "test-vectors/mg-session-v1.json").read_text())["mic_gate"]
        src = (MG / "include/mg_press_src.h").read_text()
        self.assertEqual(int(re.search(r"#define MG_PRESS_WINDOW_MS (\d+)", src).group(1)), mic["press_window_ms"])
        self.assertIn("mg_error_code_press_required = 9,", (PROTOCOLS / "mgcommands.h").read_text())
        muse = ROOT / "components/muse"
        inp = (muse / "muse_input.c").read_text()
        poll = inp[inp.index("unsigned own = muse_board->poll_buttons();"):]
        self.assertLess(poll.index("muse_input_press(MUSE_PRESS_KEY);"), poll.index("unsigned keys = own | bench_talk();"))
        self.assertEqual(inp.count("muse_input_press(MUSE_PRESS_"), 1)
        for f in ("muse_home.c", "muse_ui.c"):
            self.assertIn("if (muse_touch_real()) {\n            muse_input_press(MUSE_PRESS_PANEL);", (muse / f).read_text(), f)
        touch = (muse / "muse_touch.c").read_text()
        self.assertLess(touch.index("s_real = scripted < 0;"), touch.index("if (scripted >= 0) {"))
        glue = (ROOT / "main/muse_glue.c").read_text()
        self.assertIn("muse_input_set_press_hook(glue_press);", glue)
        ble = (MG / "mg_ble.c").read_text()
        self.assertIn("#if !MG_NO_PERSON\n    cfg.press_gate = true;", ble)   # automation images take injected presses

    def test_parked_commands_run_on_the_worker_only(self) -> None:
        # cmdq_run runs only from mg_proto_capture_pump, which only the worker task calls; the voice task's audio
        # path (mg_proto_send_data, the capture's controls) pumps without it. mg_proto_harness's test_cmdq_worker_only
        # runs it.
        proto = (MG / "mg_proto.c").read_text()
        self.assertEqual(proto.count("cmdq_run(p);"), 1)
        pump = proto[proto.index("static bool capture_pump(mg_proto_t *p, bool worker)"):]
        pump = pump[:pump.index("\n}\n")]
        self.assertIn("if (worker) {\n            cmdq_run(p);", pump)
        self.assertEqual(proto.count("capture_pump(p, true)"), 1)
        self.assertIn("bool mg_proto_capture_pump(mg_proto_t *p)\n{\n    return capture_pump(p, true);\n}", proto)
        self.assertNotIn("mg_proto_capture_pump(p)", proto)   # every caller inside the core only sends
        ble = (MG / "mg_ble.c").read_text()
        self.assertEqual(ble.count("mg_proto_capture_pump("), 1)
        worker = ble[ble.index("static void worker(void *arg)"):]
        self.assertIn("mg_proto_capture_pump(&s_proto);", worker[:worker.index("\n}\n")])
        for f in ("mg_voice.c",):
            self.assertNotIn("mg_proto_capture_pump(", (MG / f).read_text(), f)

    def test_mute_discards_a_turn_waiting_on_the_proof(self) -> None:
        # mg_voice_end(false) (the mute switch, a press too short): a turn still held for the proof is discarded
        # (mg_proto_live_discard) before anything could end it with a stop_mic the proof would release.
        voice = (MG / "mg_voice.c").read_text()
        end = voice[voice.index("void mg_voice_end(bool keep)"):]
        end = end[end.index("if (!keep) {"):]
        end = end[:end.index("        return;\n    }")]
        self.assertLess(end.index("unsent = mg_proto_live_discard(p);"), end.index("mg_proto_live_end(p, true);"))

    def test_start_mic_armed_state_expires_on_a_64_bit_clock(self) -> None:
        # A client's start_mic arms the capture (op_capture) with the time on the 64-bit esp_timer; a capture not
        # begun within the press window never begins, so an armed request can't open the mic long after the press
        # it rode on. The press window itself is on ops.mono_ms (mg_proto_harness: a press 2^32 ms back is gone).
        ble = (MG / "mg_ble.c").read_text()
        cap = ble[ble.index("static void op_capture(void *ctx, bool start, const mg_audio_format_t *fmt)"):]
        cap = cap[:cap.index("\n}\n")]
        self.assertIn("s_capture.armed_us = start ? esp_timer_get_time() : 0;", cap)
        self.assertIn(".mono_ms = op_mono,", ble)
        self.assertIn("return esp_timer_get_time() / 1000;", ble[ble.index("static int64_t op_mono(void *ctx)"):])
        self.assertIn("int64_t armed_us;", (MG / "mg_ble_priv.h").read_text())
        voice = (MG / "mg_voice.c").read_text()
        req = voice[voice.index("bool mg_voice_capture_requested(void)"):]
        req = req[:req.index("\n}\n")]
        self.assertIn("esp_timer_get_time() - req->armed_us > MG_PRESS_WINDOW_MS * 1000LL", req)
        self.assertLess(req.index("req->requested = false;"), req.index("bool r = req->requested"))
        proto = (MG / "mg_proto.h").read_text()
        self.assertIn("int64_t press_ms;", proto)

    def test_update_fifo_survives_its_index_wrap(self) -> None:
        # The SMP glue's FIFO (mg_fifo.h): free-running uint32 byte counts on a 2,738-byte ring. Started just short
        # of UINT32_MAX, thousands of writes of every length cross the counters' wrap: each comes out whole, in
        # order, and the ring never jams (head % size jumped at the wrap and misread every length after it).
        driver = r"""
#include <stdio.h>
#include <stdlib.h>
#include "mg_fifo.h"
#define SIZE 2738u
static uint8_t ring[SIZE];
static uint32_t rnd = 12345;
static uint32_t next(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }
int main(void) {
    mg_fifo_t f = { .head = UINT32_MAX - 4000u, .tail = UINT32_MAX - 4000u };
    f.wpos = f.rpos = f.head % SIZE;   /* where free-running counts would have put them */
    uint32_t in = 0, out = 0, wrapped = 0;
    uint8_t msg[521], got[521];
    static uint16_t lens[200000];   /* each write's length, by its number */
    for (int round = 0; round < 200000 && out < 20000; round++) {
        if (next() % 2) {
            uint16_t n = (uint16_t)(next() % 521);
            for (uint16_t i = 0; i < n; i++) msg[i] = (uint8_t)(in * 31u + i);
            uint32_t before = f.head;
            if (mg_fifo_push(&f, ring, SIZE, msg, n)) {
                lens[in++] = n;
                wrapped |= f.head < before;
            }
        } else {
            int n = mg_fifo_pop(&f, ring, SIZE, got, sizeof(got));
            if (n < 0) continue;
            if (n != lens[out]) { printf("write %u: %d bytes, want %u\n", out, n, lens[out]); return 1; }
            for (int i = 0; i < n; i++) {
                if (got[i] != (uint8_t)(out * 31u + (uint32_t)i)) { printf("write %u byte %d wrong\n", out, i); return 1; }
            }
            out++;
        }
        if (f.head - f.tail > SIZE || f.wpos >= SIZE || f.rpos >= SIZE) { printf("indices broken\n"); return 1; }
    }
    while (mg_fifo_pop(&f, ring, SIZE, got, sizeof(got)) >= 0) out++;
    if (!wrapped || out != in || out < 20000 || !mg_fifo_empty(&f)) { printf("wrapped %u in %u out %u\n", wrapped, in, out); return 1; }
    printf("ok %u writes across the wrap\n", out);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "fifo.c"
            src.write_text(driver)
            exe = Path(d) / "fifo"
            cc = shlex.split(os.environ.get("CC", "cc"))
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MG), str(src), "-o", str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertTrue(run.stdout.startswith("ok "), run.stdout)
        ble = (MG / "mg_ble.c").read_text()
        self.assertNotIn("% SMP_FIFO", ble)
        self.assertIn("mg_fifo_push(&f->idx, f->ring, SMP_FIFO, msg, n)", ble)
        self.assertIn("mg_fifo_pop(&f->idx, f->ring, SMP_FIFO, out, cap)", ble)

    def test_held_to_clip_rechecks_after_its_allocation(self) -> None:
        # held_to_clip allocates the clip unlocked; a held turn that went live meanwhile stays live whole (no clip of
        # its suffix, no live part left without its stop_mic), and one taken meanwhile ends.
        voice = (MG / "mg_voice.c").read_text()
        f = voice[voice.index("static bool held_to_clip(void)\n{"):]
        f = f[:f.index("\n}\n")]
        alloc = f.index("clip_alloc(")
        check = f.index("bool still = mg_proto_held_turn(mg_proto_get());")
        self.assertLess(alloc, check)
        self.assertLess(check, f.index("s_clip_len = mg_proto_held_turn_take("))
        self.assertIn("s_route = MG_ROUTE_BLE;", f)
        self.assertIn("released();", f)

    def test_video_ring_size_is_checked_at_build_time(self) -> None:
        # The ring needs a power of two (muse_vstream_init asserts it at startup); Kconfig's range can't say so, so a
        # menu-valid 98,304 fails the build instead of the boot.
        ble = (MG / "mg_ble.c").read_text()
        i = ble.index("_Static_assert((CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER & (CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER - 1)) == 0")
        self.assertLess(i, ble.index("muse_vstream_init(&s_vstream, ring, CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER"))
        for v, ok in ((65536, True), (98304, False), (8192, True), (262144, True), (12288, False)):
            self.assertEqual(v & (v - 1) == 0, ok, v)
        self.assertIn("A power of two (8192, 16384, ... 262144)", (MG / "Kconfig").read_text())

    def test_ble_audio_without_the_boot_guard_declares_what_app_c_calls(self) -> None:
        # app.c uses mg_ble_proof_clear_done in every BLE audio build: mg_ble.h comes with BLE audio, not with
        # the boot guard (a BLE audio build with MUSE_BOOTGUARD=n didn't compile).
        app = (ROOT / "main" / "app.c").read_text()
        inc = app.index('#include "mg_ble.h"')
        cond = app[app.rindex("#if ", 0, inc):inc]
        self.assertIn("CONFIG_MUSE_GADGET_BLE_AUDIO", cond)
        self.assertNotIn("CONFIG_MUSE_BOOTGUARD", cond)
        call = app.index("mg_ble_proof_clear_done, false);")
        self.assertEqual(app[app.rindex("#if ", 0, call):call].split("\n")[0], "#if CONFIG_MUSE_GADGET_BLE_AUDIO")
        # The sealed clear's order: erase, answer (the erase's own result), wait for it to go out (bounded), restart.
        run = app[app.index("static void gadget_clear_run(unsigned setup_gen) {"):]
        run = run[:run.index("\n}\n")]
        self.assertLess(run.index("mg_ble_proof_clear_done, false);"),
                        run.index("while (mg_ble_proof_clear_owed() && esp_timer_get_time() < until) {"))
        self.assertLess(run.index("while (mg_ble_proof_clear_owed()"), run.index("restart_after_setup_reset();"))
        self.assertIn("#define CLEAR_ANSWER_WAIT_MS 5000", app)
        checked = app[app.index("static bool reset_setup_checked("):]
        checked = checked[:checked.index("\n}\n")]
        self.assertLess(checked.index("if (answer) answer(true);"), checked.index("if (restart) restart_after_setup_reset();"))


if __name__ == "__main__":
    unittest.main()
