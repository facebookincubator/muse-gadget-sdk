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

"""Bench test AV60 on the host:
tools/av60.py's evaluator against synthetic gadget logs (a pass, a 1 s dip, a
5 s sag, an underrun inside and after a reply's first 200 ms, a decode error,
a memory failure, lost and stalled lines, missing replies), the lines the
firmware's own formatters print (components/muse/muse_video_tick.c,
components/muse_gadget_ble/mg_play.c), and the sender: video and reply audio
interleaved on Data with change_data_type, the credits kept, and the
messages it builds (start_streaming for LC3, subtitle words)."""

from __future__ import annotations

import asyncio
import contextlib
import io
import json
import os
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import av60  # noqa: E402
import mg_ble_client as C  # noqa: E402

P = C.P


# ---- synthetic gadget logs ---------------------------------------------------------

def vline(dev_ms: int, t: int, shown: int, span: int = 1000, drop: int = 0, decerr: int = 0, low: int = 21000,
          fps: int = 30) -> str:
    return (f"I ({dev_ms}) muse_video: av video: t={t} span={span} rx={shown + drop} shown={shown} drop={drop} "
            f"skip=0 decerr={decerr} fps={fps} int=24000 low={low}")


def pline(dev_ms: int, t: int, underruns: int, new: int, last: int, end: bool = False) -> str:
    return (f"I ({dev_ms}) mg.play: av play{' end' if end else ''}: t={t} span=1000 underruns={underruns} new={new} "
            f"last={last} buf=900 int=24000 low=21000")


class Log:
    """A 62 s call: video from device time 10 000 ms, host time = device time + 0.15 s."""

    def __init__(self, secs: int = 62, fps: int = 30):
        self.lines: list[tuple[float, str]] = []
        self.fps = fps
        self.secs = secs
        self.shown = {s: fps for s in range(1, secs + 1)}
        self.spans: dict[int, int] = {}
        self.extra: dict[int, dict] = {}
        self.skip: set[int] = set()

    def add(self, dev_ms: int, text: str) -> None:
        self.lines.append((dev_ms / 1000 + 0.15, text))

    def build(self, replies: bool = True, streams: tuple = ((6, 12, None), (36, 12, None))) -> list[tuple[float, str]]:
        t = 0
        for s in range(1, self.secs + 1):
            span = self.spans.get(s, 1000)
            t += span
            if s in self.skip:
                continue
            self.add(10000 + t, vline(10000 + t, t, self.shown[s], span=span, **self.extra.get(s, {})))
        if replies:
            for start, length, underrun in streams:
                base = 10000 + start * 1000
                self.add(base, f"I ({base}) mg.play: stream start: lc3 24000 Hz, resampled to 16 kHz")
                total = 0
                for k in range(1, length + 1):
                    new = 0
                    last = -1 if not total else underrun
                    if underrun is not None and k == 1 + underrun // 1000:
                        total, new, last = total + 1, 1, underrun
                    self.add(base + k * 1000, pline(base + k * 1000, k * 1000, total, new, last))
                self.add(base + length * 1000 + 200, pline(base + length * 1000 + 200, length * 1000 + 200, total, 0,
                                                            last if total else -1, end=True))
            self.add(10000 + 23000, "I (33000) mg.subs: band on (client)")
            self.add(10000 + 33000, "I (43000) mg.subs: band off (state 0)")
        return sorted(self.lines)

    def replies(self) -> list[av60.Reply]:
        h = lambda sec: (10000 + sec * 1000) / 1000 + 0.15   # noqa: E731
        return [av60.Reply("spoken", h(6), h(18)), av60.Reply("muted", h(22), h(32)), av60.Reply("spoken", h(36), h(48))]


class EvaluatorTest(unittest.TestCase):
    def check(self, log: Log, **kw) -> dict:
        lines = log.build(**kw)
        return av60.evaluate(lines, log.fps, log.replies(), video_end=(10000 + log.secs * 1000) / 1000 + 0.15 + 0.5)

    def test_pass(self) -> None:
        r = self.check(Log())
        self.assertEqual(r["verdict"], "PASS", r["fails"])
        self.assertEqual(len(r["windows"]), 61)   # the first second (the first picture's) isn't measured
        self.assertEqual(r["low_point"], 21000)
        self.assertTrue(all(rule["result"] == "PASS" for rule in r["rules"].values()))
        # Windows during replies are labeled.
        self.assertIn("spoken1", {w["reply"] for w in r["windows"]})
        self.assertIn("muted2", {w["reply"] for w in r["windows"]})
        self.assertIn("AV60  PASS", av60.table(r))

    def test_one_second_dip(self) -> None:
        log = Log()
        log.shown[10] = 22   # under 75% of 30 (22.5), during the first spoken reply
        r = self.check(log)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("1 s window ending" in f and "spoken1" in f for f in r["fails"]), r["fails"])
        # 23 frames in one second passes the 1 s rule, and the 5 s windows around it hold 90%.
        log = Log()
        log.shown[10] = 23
        self.assertEqual(self.check(log)["verdict"], "PASS")

    def test_five_second_sag(self) -> None:
        log = Log()
        for s in range(40, 46):
            log.shown[s] = 26   # every second over 75%, five of them under 90% (27)
        r = self.check(log)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any(f.startswith("5 s window") for f in r["fails"]), r["fails"])
        self.assertFalse(any(f.startswith("1 s window") for f in r["fails"]), r["fails"])

    def test_underrun_inside_and_after_200_ms(self) -> None:
        r = self.check(Log(), streams=((6, 12, 140), (36, 12, None)))
        self.assertEqual(r["verdict"], "PASS", r["fails"])
        self.assertIn("inside the first 200 ms", r["rules"]["underruns"]["detail"])
        r = self.check(Log(), streams=((6, 12, None), (36, 12, 220)))
        self.assertEqual(r["verdict"], "FAIL")
        self.assertEqual(r["rules"]["underruns"]["result"], "FAIL")
        self.assertIn("underrun at 220 ms", r["rules"]["underruns"]["detail"])
        # One late underrun in the middle of a reply.
        r = self.check(Log(), streams=((6, 12, 5300), (36, 12, None)))
        self.assertEqual(r["rules"]["underruns"]["result"], "FAIL")

    def test_decode_error(self) -> None:
        log = Log()
        log.extra[30] = {"decerr": 1, "drop": 1}
        r = self.check(log)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertEqual(r["rules"]["decode_errors"]["result"], "FAIL")
        # A decode error before the measured seconds still counts.
        log = Log()
        log.extra[1] = {"decerr": 2}
        self.assertEqual(self.check(log)["rules"]["decode_errors"]["result"], "FAIL")

    def test_memory_failure(self) -> None:
        log = Log()
        log.extra[20] = {"low": 1800}
        lines = log.build() + [(25.0, "E (24850) muse_video: no memory for the decoder")]
        r = av60.evaluate(lines, 30, log.replies())
        self.assertEqual(r["rules"]["memory"]["result"], "FAIL")
        self.assertIn("internal low point 1800 B", r["rules"]["memory"]["detail"])
        self.assertEqual(r["verdict"], "FAIL")

    def test_lost_and_stalled_lines(self) -> None:
        log = Log()
        log.skip = {30}   # its line never arrived: unknown, not a pass
        r = self.check(log)
        self.assertEqual(r["verdict"], "INCOMPLETE", r["fails"])
        self.assertTrue(r["gaps"])
        log = Log()
        log.spans[30] = 2600   # the decode task didn't log for 2.6 s: a stall
        log.shown[30] = 78
        r = self.check(log)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("stalled" in f for f in r["fails"]), r["fails"])

    def test_replies_must_happen(self) -> None:
        r = self.check(Log(), replies=False)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertEqual(r["rules"]["replies"]["result"], "FAIL")
        self.assertEqual(r["rules"]["underruns"]["result"], "FAIL")   # no playback lines at all

    def test_console_and_mirror_lines_count_once(self) -> None:
        log = Log()
        lines = log.build()
        doubled = lines + [(at + 0.4, text) for at, text in lines]
        r = av60.evaluate(doubled, 30, log.replies())
        self.assertEqual(r["verdict"], "PASS", r["fails"])
        self.assertEqual(len(r["windows"]), 61)

    def test_video_end_trims_the_tail(self) -> None:
        log = Log()
        log.shown[62] = 5   # the client stopped sending mid-second: past the measured end
        lines = log.build()
        r = av60.evaluate(lines, 30, log.replies(), video_end=(10000 + 61500) / 1000 + 0.15)
        self.assertEqual(r["verdict"], "PASS", r["fails"])

    def test_saved_log_round_trip(self) -> None:
        log = Log()
        lines = log.build()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "run.jsonl")
            sink = av60.Sink(path)
            sink.meta(fps=30)
            for at, text in lines:
                sink.append((at, "nus", text))
            sink.meta(replies=[{"kind": r.kind, "start": r.start, "end": r.end} for r in log.replies()], video_end=None)
            sink.close()
            self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
            meta, back, sender = av60.load_log(path)
            self.assertEqual(back, lines)
            self.assertEqual((meta["fps"], len(meta["replies"]), sender), (30, 3, []))
            # A log cut short by a kill (its last line half written) still loads.
            with open(path, "a") as f:
                f.write('{"t": 99.0, "src": "nus", "li')
            self.assertEqual(av60.load_log(path)[1], lines)
            out = Path(tmp, "out.json")
            with contextlib.redirect_stdout(io.StringIO()):
                rc = av60.main(["--evaluate", str(path), "--json", str(out)])
            self.assertEqual(rc, 0)
            self.assertEqual(json.loads(out.read_text())["verdict"], "PASS")


def bins(secs: int = 12, fps: int = 30, sent: dict | None = None, room: dict | None = None,
         credit: dict | None = None) -> list[dict]:
    """The sender's per-second counts as the Pump logs them: on schedule unless told otherwise."""
    sent, room, credit = sent or {}, room or {}, credit or {}
    return [{"sec": i, "t": 100.0 + i, "frames": sent.get(i, fps), "due": fps, "bytes": 1900 * fps,
             "audio_bytes": 0, "credit_ms": credit.get(i, 0), "room_ms": room.get(i, 0), "switches": 0,
             "max_lag_ms": 0} for i in range(secs)]


class SenderRuleTest(unittest.TestCase):
    """The harness rule: a sender that misses its schedule by > 5% in a 5 s window makes the run INVALID
    when the Mac's side made it wait (write-wait), and is the gadget's FAIL when its credits did (credit-wait)."""

    def test_on_schedule(self) -> None:
        r = av60.sender_check(bins(), 30)
        self.assertEqual(r["result"], "PASS", r)
        self.assertTrue(r["windows"] and all(w["cause"] == "-" and not w["missed"] for w in r["windows"]))
        # 5% short in one window still passes (142.5 of 150 frames), 6% doesn't.
        self.assertEqual(av60.sender_check(bins(sent={4: 23}, room={4: 300}), 30)["result"], "PASS")
        self.assertEqual(av60.sender_check(bins(sent={4: 21}, room={4: 300}), 30)["result"], "INVALID")

    def test_late_sender_is_invalid(self) -> None:
        # The Mac's Bluetooth took no write for a while: the sender fell behind on its own side.
        r = av60.sender_check(bins(sent={5: 20, 6: 18}, room={5: 400, 6: 500}), 30)
        self.assertEqual(r["result"], "INVALID", r)
        self.assertIn("write-wait", r["detail"])
        # A sender late with no wait at all (its own loop) is the Mac's too.
        self.assertEqual(av60.sender_check(bins(sent={5: 15}), 30)["result"], "INVALID")

    def test_credit_starved_is_a_gadget_fail(self) -> None:
        # The gadget's ring stayed full from the start of the window: its credits held a sender on schedule.
        r = av60.sender_check(bins(sent={5: 20, 6: 20}, credit={5: 600, 6: 700}), 30)
        self.assertEqual(r["result"], "FAIL", r)
        self.assertIn("credit-wait", r["detail"])

    def test_credit_wait_while_catching_up_is_invalid(self) -> None:
        # A host stall (write-wait) left the sender behind; the gadget's small ring then limits the catch-up burst.
        r = av60.sender_check(bins(sent={3: 0, 4: 26, 5: 26}, room={3: 1000}, credit={4: 400, 5: 400}), 30)
        self.assertEqual(r["result"], "INVALID", r)
        self.assertTrue(any(w["cause"] == "catch-up" for w in r["windows"]), r["windows"])

    def test_evaluate_judges_the_sender_first(self) -> None:
        log = Log(secs=12)
        lines = log.build()
        t0 = (10000 + 1000) / 1000 + 0.15 - 1   # the sender's second 0 a second before the first video line
        late = [dict(b, t=t0 + b["sec"]) for b in bins(secs=12, sent={5: 18, 6: 18}, room={5: 600, 6: 600})]
        r = av60.evaluate(lines, 30, log.replies(), sender=late)
        self.assertEqual(r["verdict"], "INVALID", r["rules"]["sender"])
        self.assertEqual(list(r["rules"])[0], "sender")
        starved = [dict(b, t=t0 + b["sec"]) for b in bins(secs=12, sent={5: 18, 6: 18}, credit={5: 600, 6: 600})]
        r = av60.evaluate(lines, 30, log.replies(), sender=starved)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(r["fails"][0].startswith("sender:"))
        ok = [dict(b, t=t0 + b["sec"]) for b in bins(secs=12)]
        r = av60.evaluate(lines, 30, log.replies(), sender=ok)
        self.assertEqual(r["verdict"], "PASS", r["fails"])
        self.assertTrue(any(w.get("sent") == 30 for w in r["windows"]))
        self.assertIn("sent kB", av60.table(r))


class FormatTest(unittest.TestCase):
    """The lines the firmware formats parse as the evaluator expects."""

    def test_video_tick_lines(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        driver = r'''
#include <stdio.h>
#include "muse_video_tick.h"
int main(void) {
    muse_vtick_t t = { 0 };
    muse_vcounts_t c = { .rx = 0, .shown = 65520, .dropped = 0, .skipped = 0, .decode_errors = 0 };
    char line[160];
    muse_vtick_start(&t, 1000000, &c);
    for (int s = 1; s <= 3; s++) {
        c.rx += 30; c.shown = (unsigned short)(c.shown + 29); c.dropped += 1; c.decode_errors += s == 2;
        int64_t now = 1000000 + s * 1000500LL;
        if (!muse_vtick_due(&t, now)) return 1;
        muse_vtick_line(&t, now, &c, 30, 23456, 21000 - s, line, sizeof(line));
        printf("I (%lld) muse_video: %s\n", (long long)(now / 1000), line);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "d.c").write_text(driver)
            exe = Path(tmp, "d")
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'components/muse'}",
                                "-o", str(exe), str(Path(tmp, "d.c")), str(ROOT / "components/muse/muse_video_tick.c")],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
        ticks = [av60.parse_line(ln) for ln in out.splitlines()]
        self.assertTrue(all(ticks), out)
        self.assertEqual([t["shown"] for t in ticks], [29, 29, 29])   # across the 16-bit wrap
        self.assertEqual([t["decerr"] for t in ticks], [0, 1, 0])
        self.assertEqual([t["span"] for t in ticks], [1000, 1000, 1000])
        self.assertEqual([t["t"] for t in ticks], [1000, 2001, 3001])
        self.assertEqual(ticks[2]["low"], 20997)
        self.assertTrue(all(len(ln) < 150 for ln in out.splitlines()))

    def test_play_tick_lines(self) -> None:
        from test_mg_ble import MG, PROTOCOLS, codec_build, compile_and_run
        srcs, defs, incs = codec_build(True)
        out = compile_and_run(self, [ROOT / "tests/mg_play_harness.c", MG / "mg_play.c", MG / "mg_resample.c"], defs,
                              [MG, PROTOCOLS, *incs], vendor=srcs)
        samples = [ln[len("SAMPLE "):] for ln in out.splitlines() if ln.startswith("SAMPLE ") and "mg.play" in ln]
        ticks = [av60.parse_line(ln) for ln in samples]
        self.assertEqual(len(ticks), 2, out)
        self.assertEqual((ticks[0]["end"], ticks[0]["t"], ticks[0]["underruns"], ticks[0]["new"], ticks[0]["last"]),
                         (False, 260, 2, 1, 220))
        self.assertTrue(ticks[1]["end"])
        # The evaluator calls that one late (>= 200 ms into the stream).
        lines = [(i, ln) for i, ln in enumerate(samples)]
        r = av60.evaluate(lines, 30)
        self.assertEqual(r["rules"]["underruns"]["result"], "FAIL")
        # The end line's drift trim counts parse as counters too, the rest of it unchanged.
        trim = av60.parse_line("I (60050) mg.play: av play end: t=60020 span=20 underruns=0 new=0 last=0 buf=0 "
                               "queued=-1 int=23456 low=20900 dropped=120 repeated=0 repaid=1280")
        self.assertEqual((trim["end"], trim["t"], trim["dropped"], trim["repeated"], trim["repaid"]),
                         (True, 60020, 120, 0, 1280))


class MessagesTest(unittest.TestCase):
    def test_lc3_start_streaming_matches_audio_rates(self) -> None:
        # LC3 at 24 kHz, 32 kb/s, 40 B frames.
        self.assertEqual(av60.audio_start(24000, 40).hex(" ").upper(), "1D 02 04 01 13 02 C0 5D 00 00 03 20 00 05 28 00")
        # A gadget that lists live_depth gets the apps' 100 ms, last (ids in increasing order).
        self.assertEqual(av60.audio_start(24000, 40, 100).hex(" ").upper(),
                         "1D 02 05 01 13 02 C0 5D 00 00 03 20 00 05 28 00 06 64 00")

    def test_words_fit_and_follow_on(self) -> None:
        words = av60.WORDS * 2
        msgs = av60.pack_words(words, 100, 7, 60)
        self.assertGreater(len(msgs), 3)
        text = b""
        frame = 100
        for off, cues in msgs:
            self.assertEqual(off, len(text))   # each message starts where the last ended
            payload = av60.subtitle_words(0x6060, cues, off)
            self.assertLessEqual(len(payload) - 2, 60)
            for at, w in cues:
                self.assertEqual(at, frame)
                frame += 7
                text += w.encode()
        self.assertEqual(text.decode(), " ".join(words))


class FakeClient:
    """What the sender writes, in order; enough of bleak's client for it."""

    mtu_size = 247

    def __init__(self):
        self.writes: list[tuple[str, bytes]] = []

    async def write_gatt_char(self, uuid, data, response=False):
        self.writes.append((uuid, bytes(data)))


def clip_file(tmp: str) -> str:
    """A small stand-in clip (the fake gadget doesn't decode): a keyframe and 59 P frames, about 370 kb/s."""
    frames = [bytes([0, 0, 0, 1, 0x65]) + bytes(1800)] + [bytes([0, 0, 0, 1, 0x41]) + bytes(1400 + i % 7 * 50)
                                                          for i in range(59)]
    path = Path(tmp, "clip.au")
    av60.write_records(path, frames)
    return str(path)


class DryRunTest(unittest.TestCase):
    """The whole flow against the in-process fake gadget, its console on a pty: it never hangs, the jsonl is
    written as it goes, and each way a run can go wrong ends the run with its phase named."""

    def run_fake(self, tmp: str, secs: int = 6, **kw) -> tuple[dict, av60.FakeGadget, float]:
        args = av60.build_parser().parse_args(["--dry", "--secs", str(secs), "--clip", clip_file(tmp)])
        fake = av60.FakeGadget(**kw)
        t = time_now()
        r = av60.run_live(args, av60.Sink(None), out=lambda *_: None, fake=fake)
        return r, fake, time_now() - t

    def test_the_token_proof_comes_first_on_a_gadget_that_lists_it(self) -> None:
        # A set-up gadget takes streams only after this connection's proof: a wrong saved key stops
        # the run at "proof", before any stream starts; no saved key is a note, and the run goes on.
        saved = av60.C.prove_if_kept
        try:
            async def mismatch(g, addr):
                return "mismatch"
            av60.C.prove_if_kept = mismatch
            with tempfile.TemporaryDirectory() as tmp:
                r, fake, _ = self.run_fake(tmp, faults={"lists_proof"})
            self.assertEqual(r["verdict"], "INCOMPLETE")
            self.assertEqual(r.get("phase"), "proof")
            self.assertFalse(any(d[:2] == bytes([av60.SV, P["mg_stream_video_start_streaming"]]) for d in fake.control_writes))

            async def none(g, addr):
                return None
            av60.C.prove_if_kept = none
            with tempfile.TemporaryDirectory() as tmp:
                r, _, _ = self.run_fake(tmp, faults={"lists_proof"})
            self.assertEqual(r["verdict"], "PASS", r.get("fails"))
        finally:
            av60.C.prove_if_kept = saved

    def test_gadget_lines_are_printable(self) -> None:
        # A line from the peripheral can't drive the operator's terminal: C0/C1 controls are shown as \xNN in the
        # log and the report.
        self.assertEqual(av60.printable("av video: ok\x1b[2J\x1b]0;PASS\x07"), "av video: ok\\x1b[2J\\x1b]0;PASS\\x07")
        sink = av60.Sink(None)
        sink.append((0.0, "nus", "no memory\x1b[31m FAIL"))
        self.assertNotIn("\x1b", sink.lines[0][2])
        self.assertIn("\\x1b[31m", sink.lines[0][2])

def time_now() -> float:
    import time
    return time.monotonic()


if __name__ == "__main__":
    unittest.main()
