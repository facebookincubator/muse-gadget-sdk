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

"""Bench test AV60: video holds its rate while replies play.

The Mac is the phone's side of a Live call (as `mg_ble_client.py call` is) and
sends the load the Muse app does:
  - the call: connecting, then live (or, with --wait-call, the answer to the
    gadget's own start);
  - H.264 at the board's size, fps and kb/s (BOARDS) from a test clip (this
    repository's test streams, ffmpeg's testsrc2 and a UI clock, transcoded
    with ffmpeg; never a person's face), in two bands where the gadget decodes
    two (ESP-Mosaico);
  - reply audio interleaved with the video on Data, switched with
    change_data_type about 25 times a second, as the app does: two spoken
    replies (LC3 at the highest rate the gadget plays, 24 kHz where listed,
    32 kb/s; a sweep and a tone at a moderate level) and one muted reply
    (subtitle state 1 and words over the video, no audio).
The clip is transcoded once and cached before connecting; nothing is encoded during the call. One timed
sender (40 ms slots) writes it at its frames' due times, only when the Mac's Bluetooth has room for a
write (CoreBluetooth silently drops one it has no room for) and the gadget's credits allow, and logs each
second what it actually sent (frames, bytes) and what it waited on: `write-wait` (the Mac's side) or
`credit-wait` (the gadget's buffer_update left no room).

It reads the gadget's once-a-second counters (`av video:` on muse_video, `av play:` on mg.play) from the
Nordic UART log mirror, and from a USB console too with --console PORT (opened read-only, DTR and RTS
untouched, 115200 baud, on its own thread), and judges, in this order:
  - the sender (the harness rule): frames sent may fall short of the frames due by at most
    5% in any 5 s window. A miss caused by credit-wait with the sender on schedule is the gadget's: FAIL.
    Any other miss (write-wait, or catching up after one) makes the run INVALID: rerun;
  - the link: every byte written must reach the gadget by its own counts, else INVALID;
  - video held through replies on the gadget, per window: shown fps >= 90% of the target over every 5 s window and no 1 s
    window under 75%, during replies as well as between them; 0 decode errors; 0 audio underruns after
    a reply's first 200 ms; no memory failure (the firmware's own "no memory" / "low memory" lines),
    with the internal-RAM low point reported; and the three replies happened.
The camera clip of a spoken reply over video is a separate step.

It can't hang: every phase (scan, clip, connect, status, capabilities, the call, video start, each reply,
finish) is bounded, the sender and a once-a-second watchdog end the run when nothing moves (no credits,
no write room, bytes the gadget never counts, the gadget stopping the video or the call, a lost link),
and the cleanup (video stopped, the call ended) is bounded too. Each second it prints a progress line
(phase, frames sent, credits, waits, the gadget's latest counters).

  python3 -u tools/av60.py --name MuseGadget-M5SW-ABCDEF [--secs 60] [--console PORT] [--json out.json]
  python3 tools/av60.py --dry [--secs 10]              # the whole flow against a gadget in this process
  python3 tools/av60.py --evaluate av60_log.jsonl      # re-check a saved run (whole or cut short)

The jsonl (--log, default av60_<time>.jsonl, mode 0600, masked by tools/redact.py) is written as the run
goes: the gadget's lines, the sender's per-second counts and the run's notes, so even a killed run leaves
one. Prints a per-second table (with the sender's frames, kB and wait beside the gadget's counters), PASS /
FAIL / INVALID per rule and a JSON summary. Exit 0 PASS, 1 FAIL, 2 INCOMPLETE (the run couldn't finish or
be judged; the phase is named), 3 INVALID (rerun).
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import termios
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mg_ble_client as C  # noqa: E402
import redact  # noqa: E402

P = C.P
LIVE_CALL = P["mg_command_live_call"]
SUBTITLE = P["mg_command_subtitle"]
SV = P["mg_command_stream_video"]
SA = P["mg_command_stream_audio"]
CDT = P["mg_command_change_data_type"]
H264 = P["mg_data_type_video_h264"]
LC3 = P["mg_data_type_audio_lc3"]
SHOW_NOW = 0xFFFFFFFF

# The call video table, by the product code in the
# advertised name: board, suggested width, height, fps, kb/s.
BOARDS = {
    # Round panels suggest enough for the 2x picture to cover the circle.
    "M5SW": ("StopWatch", 240, 240, 30, 450),
    "SCW1": ("SenseCAP Watcher", 208, 208, 30, 450),
    "EMO1": ("ESP-Mosaico", 480, 480, 30, 600),
    "W175": ("Waveshare S3 1.75C", 240, 240, 30, 450),
}
# Boards with a small video buffer: the largest frame the clip may have (the apps cap a frame at the buffer / 2 - 8).
MAX_FRAME: dict[str, int] = {}
# Boards that decode two bands: (bands, the top band's share in percent; the gadget says so too).
BANDS = {"EMO1": (2, 60)}

# The pass rules.
FPS_5S = 0.90
FPS_1S = 0.75
UNDERRUN_GRACE_MS = 200
SENDER_SLACK = 0.05        # the harness rule: the sender may miss at most 5% of its frames in any 5 s

# The default test clip: this repository's H.264 test streams (ffmpeg's testsrc2, a UI clock).
TEST_CLIPS = [C.ESP32 / "tests/vectors/h264/testsrc2_208.264", C.ESP32 / "tests/vectors/h264/ui_clock_466.264"]
# Bumped whenever _encode's settings change (the cache key).
CLIP_VERSION = "x264 baseline, one slice, repeat headers"
CACHE = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "mg_ble_client" / "av60"

WORDS = ("This reply is muted, so the gadget shows its words over the call's video instead of playing "
         "them. Each word comes in step with the picture, two or three a second, the way the app sends "
         "them while the speaker is off. The picture should keep its rate the whole time.").split(" ")


# ---- the gadget's lines ---------------------------------------------------------

TICK_RE = re.compile(r"\((\d+)\) (muse_video|mg\.play): av (video|play)( end)?: (.*?)\s*$")
KV_RE = re.compile(r"(\w+)=(-?\d+)")
# The firmware's own memory-failure lines (muse_video, mg.play, muse_audio, the call's "Video unavailable").
MEMFAIL_RE = re.compile(r"no memory|low memory|Video unavailable|out of memory|alloc(?:ation)? failed", re.I)
BAND_RE = re.compile(r"mg\.subs: band (on|off) \(([^)]*)\)")
PLAY_START_RE = re.compile(r"mg\.play: stream start: (\w+) (\d+) Hz")
NOT_DECODED_RE = re.compile(r"muse_video: frame \d+ not decoded")


def parse_line(line: str) -> dict | None:
    """One `av video:` / `av play:` line: {"kind", "end", "dev_ms", and its key=value counters}, else None."""
    m = TICK_RE.search(line)
    if not m:
        return None
    out = {"kind": m.group(3), "end": bool(m.group(4)), "dev_ms": int(m.group(1))}
    for k, v in KV_RE.findall(m.group(5)):
        out[k] = int(v)
    need = ("t", "span", "shown", "drop", "decerr", "fps") if out["kind"] == "video" else ("t", "underruns", "new", "last")
    return out if all(k in out for k in need) else None


@dataclass
class Reply:
    kind: str            # spoken | muted
    start: float         # host time (time.monotonic())
    end: float = 0.0
    detail: dict = field(default_factory=dict)


def _dedupe(lines: list[tuple[float, str]]) -> list[tuple[float, dict]]:
    """Tick lines from every source (the mirror and the console carry the same ones), once each, by device time."""
    seen, out = set(), []
    for at, text in sorted(lines, key=lambda x: x[0]):
        t = parse_line(text)
        if not t:
            continue
        key = (t["kind"], t["end"], t["dev_ms"], t["t"])
        if key in seen:
            continue
        seen.add(key)
        out.append((at, t))
    return sorted(out, key=lambda x: x[1]["dev_ms"])


def sender_check(sender: list[dict], fps: int, video_end: float | None = None) -> dict:
    """The harness rule: the Mac's sender must keep its schedule. Over every 5 s of
    the sender's per-second counts (its first second and anything past video_end left out), frames written
    may fall short of the frames due by at most 5%. Each window's cause is the wait that dominated it, as
    the harness reads it: `write-wait` (the Mac's own Bluetooth writes: the host stack had no room),
    `credit-wait` (the gadget's buffer_update left no room: its ring was full) with the sender on schedule
    as the window opened, or `catch-up` (credit-wait while catching up after a host stall: the ring limits
    a burst, not evidence against the gadget). A miss over 5% caused by credit-wait is the gadget's: FAIL.
    Any other miss makes the run INVALID (rerun), never a gadget FAIL."""
    bins = sorted((b for b in sender if b.get("sec", 0) >= 1 and (video_end is None or b["t"] + 1 <= video_end)),
                  key=lambda b: b["sec"])
    if not bins:
        return {"result": "INCOMPLETE", "detail": "no per-second send counts", "windows": []}
    cum_due, cum_sent = [0], [0]
    for b in bins:
        cum_due.append(cum_due[-1] + b["due"])
        cum_sent.append(cum_sent[-1] + b["frames"])
    windows = []
    for i in range(len(bins) - 4):
        w = bins[i:i + 5]
        if w[-1]["sec"] - w[0]["sec"] != 4:
            continue   # a gap in the counts: not a window
        due = sum(b["due"] for b in w)
        sent = sum(b["frames"] for b in w)
        ww = sum(b.get("room_ms", 0) for b in w)
        cw = sum(b.get("credit_ms", 0) for b in w)
        backlog = cum_due[i] - cum_sent[i]   # frames the sender was behind as the window opened
        if not ww and not cw:
            cause = "-"
        elif ww >= cw:
            cause = "write-wait"
        else:
            cause = "credit-wait" if backlog <= 1 else "catch-up"
        miss = max(0.0, 1 - sent / due) if due else 0.0
        windows.append({"from_s": w[0]["sec"], "to_s": w[-1]["sec"] + 1, "due": due, "sent": sent,
                        "miss": round(miss, 3), "write_wait_ms": ww, "credit_wait_ms": cw,
                        "backlog_frames": backlog, "cause": cause, "missed": miss > SENDER_SLACK})
    missed = [w for w in windows if w["missed"]]
    gadget = [w for w in missed if w["cause"] == "credit-wait"]
    host = [w for w in missed if w["cause"] != "credit-wait"]

    def say(ws):
        return "; ".join(f"seconds {w['from_s']}-{w['to_s']}: {w['sent']} of {w['due']} frames "
                         f"({100 * (1 - w['miss']):.0f}%), {w['cause']} (write-wait {w['write_wait_ms']} ms, "
                         f"credit-wait {w['credit_wait_ms']} ms)" for w in ws[:3]) + (
            f" (+{len(ws) - 3} more)" if len(ws) > 3 else "")

    if gadget:
        return {"result": "FAIL", "windows": windows,
                "detail": "the gadget's credits held the sender back (a gadget FAIL): " + say(gadget)}
    if host:
        return {"result": "INVALID", "windows": windows,
                "detail": "the sender missed its schedule on the Mac's side (rerun): " + say(host)}
    return {"result": "PASS", "windows": windows,
            "detail": f"{cum_sent[-1]} of {cum_due[-1]} frames sent over {len(bins)} s; every 5 s window "
                      f">= {100 * (1 - SENDER_SLACK):.0f}% ({len(windows)} windows; worst "
                      f"{100 * (1 - max((w['miss'] for w in windows), default=0)):.0f}%)"}


def second_cause(b: dict) -> str:
    """What the sender waited on in one second: write-wait (the Mac's Bluetooth), credit-wait (the gadget), or -."""
    ww, cw = b.get("room_ms", 0), b.get("credit_ms", 0)
    return "-" if not ww and not cw else "write-wait" if ww >= cw else "credit-wait"


def evaluate(lines: list[tuple[float, str]], target_fps: int, replies: list[Reply] | None = None,
             video_end: float | None = None, need_spoken: int = 2, need_muted: int = 1,
             sender: list[dict] | None = None) -> dict:
    """The video-through-replies rules over the gadget's lines (host time, text). Returns windows, rule verdicts and a summary.

    A line's host time is when it arrived; windows are placed on the gadget's
    own clock (the log's "(ms)"), mapped to host time by the smallest
    arrival-minus-device offset (the line that came fastest). With the
    sender's per-second counts, the sender's schedule is checked first
    (sender_check): a run whose sender missed it is INVALID whatever the
    gadget did."""
    replies = replies or []
    ticks = _dedupe(lines)
    texts = [t for _, t in lines]
    offs = [at * 1000 - t["dev_ms"] for at, t in ticks]
    offset = min(offs) if offs else 0.0

    def host(dev_ms: float) -> float:
        return (dev_ms + offset) / 1000

    def label(start_dev: float, end_dev: float) -> str:
        names = []
        for i, r in enumerate(replies):
            if r.start < host(end_dev) and (r.end or 1e18) > host(start_dev):
                names.append(f"{r.kind}{i + 1}")
        return "+".join(names) or "-"

    video = [t for _, t in ticks if t["kind"] == "video"]
    play = [t for _, t in ticks if t["kind"] == "play"]
    fails: list[str] = []
    gaps: list[str] = []

    # Video: the measurement starts with the first full second after the first picture, and ends where the client stopped.
    streams: list[list[dict]] = []
    for t in video:
        if not streams or t["t"] <= streams[-1][-1]["t"]:
            streams.append([])
        streams[-1].append(t)
    vs = max(streams, key=len) if streams else []
    first = next((i for i, t in enumerate(vs) if t["shown"] > 0), None)
    measured = vs[first + 1:] if first is not None else []
    if video_end is not None:
        measured = [t for t in measured if host(t["dev_ms"]) <= video_end - 0.2]
    windows = []
    prev_t = None
    for t in measured:
        w = {"dev_ms": t["dev_ms"], "t": t["t"], "span": t["span"], "shown": t["shown"], "drop": t["drop"],
             "skip": t.get("skip", 0), "decerr": t["decerr"], "rx": t.get("rx", 0), "low": t.get("low"),
             "int": t.get("int"), "fps1": t["shown"] * 1000 / max(1, t["span"]), "lost_before": False,
             "reply": label(t["dev_ms"] - t["span"], t["dev_ms"])}
        if prev_t is not None and t["t"] - t["span"] > prev_t + 250:
            w["lost_before"] = True   # a line (or more) never arrived: those seconds are unknown
            gaps.append(f"no line for {prev_t / 1000:.1f}-{(t['t'] - t['span']) / 1000:.1f} s of the stream")
        prev_t = t["t"]
        windows.append(w)
    min1 = FPS_1S * target_fps
    min5 = FPS_5S * target_fps
    for i, w in enumerate(windows):
        w["ok1"] = w["fps1"] >= min1 and w["span"] <= 1500
        if not w["ok1"]:
            why = f"stalled: {w['span']} ms between lines" if w["span"] > 1500 else f"{w['fps1']:.1f} fps"
            fails.append(f"1 s window ending {w['t'] / 1000:.1f} s ({w['reply']}): {why} < {min1:.1f}")
        # The 5 s window ending here: this line and the ones before it, unbroken, covering >= 5 s.
        shown, span, j = 0, 0, i
        while j >= 0 and span < 4950:
            if j < i and windows[j + 1]["lost_before"]:
                break
            shown += windows[j]["shown"]
            span += windows[j]["span"]
            j -= 1
        w["fps5"] = shown * 1000 / span if span >= 4950 else None
        w["ok5"] = w["fps5"] is None or w["fps5"] >= min5
        if not w["ok5"]:
            fails.append(f"5 s window ending {w['t'] / 1000:.1f} s ({w['reply']}): {w['fps5']:.1f} fps < {min5:.1f}")
    decerr = sum(t["decerr"] for t in vs)
    not_decoded = sum(1 for x in texts if NOT_DECODED_RE.search(x))

    # Audio: per stream, any underrun after its first 200 ms.
    pstreams: list[list[dict]] = []
    for t in play:
        if not pstreams or t["t"] < pstreams[-1][-1]["t"] or pstreams[-1][-1]["end"]:
            pstreams.append([])
        pstreams[-1].append(t)
    late, early, audio_rows = [], 0, []
    for k, s in enumerate(pstreams):
        last_line = s[-1]
        underruns = last_line["underruns"]
        late_here = [t for t in s if t["new"] > 0 and t["last"] >= UNDERRUN_GRACE_MS]
        # An end line can carry underruns no earlier line showed (the last part second).
        if late_here:
            late.append(f"stream {k + 1}: underrun at {late_here[-1]['last']} ms of the stream")
        elif underruns:
            early += underruns
        audio_rows.append({"stream": k + 1, "played_ms": last_line["t"], "underruns": underruns,
                           "late": len(late_here), "ended": last_line["end"], "lines": len(s),
                           "reply": label(s[0]["dev_ms"] - s[0]["t"], last_line["dev_ms"])})
    lows = [t["low"] for t in ticks_values(ticks) if t.get("low") is not None]
    memfails = [redact.redact_text(x) for x in texts if MEMFAIL_RE.search(x)]

    bands_on = [(at, x) for at, x in lines if (m := BAND_RE.search(x)) and m.group(1) == "on"]
    spoken = [r for r in replies if r.kind == "spoken"]
    muted = [r for r in replies if r.kind == "muted"]
    muted_seen = sum(1 for r in muted if any(r.start - 0.5 <= at <= (r.end or 1e18) + 2 for at, _ in bands_on))
    spoken_played = sum(1 for a in audio_rows if a["played_ms"] >= 1000)

    rules = {
        "fps": {"result": "PASS" if windows and all(w["ok1"] and w["ok5"] for w in windows) else "FAIL",
                "detail": (f"{len(windows)} s measured; worst 1 s {min(w['fps1'] for w in windows):.1f} fps, worst 5 s "
                           f"{min((w['fps5'] for w in windows if w['fps5'] is not None), default=0):.1f} fps "
                           f"(target {target_fps}: 1 s >= {min1:.1f}, 5 s >= {min5:.1f})") if windows else
                          "no video lines after the first picture"},
        "decode_errors": {"result": "PASS" if decerr == 0 and not_decoded == 0 and vs else "FAIL",
                          "detail": f"{decerr} counted, {not_decoded} 'not decoded' lines"},
        "underruns": {"result": "PASS" if not late and pstreams else "FAIL",
                      "detail": ("; ".join(late) if late else
                                 f"{len(pstreams)} streams, {early} underrun(s) inside the first {UNDERRUN_GRACE_MS} ms")
                      if pstreams else "no playback lines"},
        "memory": {"result": "PASS" if not memfails else "FAIL",
                   "detail": (f"internal low point {min(lows)} B" if lows else "no low point logged")
                   + (f"; {len(memfails)} failure line(s): {memfails[0][:120]}" if memfails else "")},
        "replies": {"result": "PASS" if spoken_played >= need_spoken and muted_seen >= need_muted else "FAIL",
                    "detail": f"{spoken_played} spoken played (>= 1 s each), {muted_seen} muted with the band on"},
    }
    for name in ("decode_errors", "underruns", "memory", "replies"):
        if rules[name]["result"] == "FAIL":
            fails.append(f"{name}: {rules[name]['detail']}")
    verdict = "FAIL" if fails else "INCOMPLETE" if gaps or not windows else "PASS"
    sender_windows = []
    if sender is not None:
        sc = sender_check(sender, target_fps, video_end)
        sender_windows = sc["windows"]
        rules = {"sender": {"result": sc["result"], "detail": sc["detail"]}, **rules}
        if sc["result"] == "FAIL":
            fails.insert(0, f"sender: {sc['detail']}")
            verdict = "FAIL"
        elif sc["result"] == "INVALID":
            verdict = "INVALID"   # judged before the gadget's rules: rerun
        elif sc["result"] == "INCOMPLETE" and verdict != "FAIL":
            verdict = "INCOMPLETE"
        by_sec = {b["sec"]: b for b in sender}
        t0 = min((b["t"] - b["sec"] for b in sender), default=0.0)
        for w in windows:
            b = by_sec.get(int(host(w["dev_ms"]) - t0) - 1)
            w["sent"] = b["frames"] if b else None
            w["sent_kb"] = round(b["bytes"] / 1000, 1) if b else None
            w["wait"] = second_cause(b) if b else None
    return {"verdict": verdict, "target_fps": target_fps, "rules": rules, "fails": fails, "gaps": gaps,
            "sender_windows": sender_windows,
            "windows": windows, "audio": audio_rows, "low_point": min(lows) if lows else None,
            "replies": [{"kind": r.kind, "start": r.start, "end": r.end, **r.detail} for r in replies]}


def ticks_values(ticks: list[tuple[float, dict]]) -> list[dict]:
    return [t for _, t in ticks]


def table(result: dict) -> str:
    rows = [f"{'t (s)':>6} {'fps 1s':>6} {'fps 5s':>6} {'shown':>5} {'drop':>4} {'skip':>4} {'decerr':>6} "
            f"{'int low':>7} {'sent':>4} {'sent kB':>7} {'wait':>11}  reply"]
    for w in result["windows"]:
        mark = "" if w["ok1"] and w["ok5"] else "  <-- FAIL"
        fps5 = f"{w['fps5']:.1f}" if w["fps5"] is not None else "-"
        sent = "-" if w.get("sent") is None else str(w["sent"])
        skb = "-" if w.get("sent_kb") is None else f"{w['sent_kb']:.1f}"
        rows.append(f"{w['t'] / 1000:6.1f} {w['fps1']:6.1f} {fps5:>6} {w['shown']:5d} {w['drop']:4d} {w['skip']:4d} "
                    f"{w['decerr']:6d} {w['low'] if w['low'] is not None else '-':>7} {sent:>4} {skb:>7} "
                    f"{w.get('wait') or '-':>11}  {w['reply']}"
                    + (" (lines lost before)" if w["lost_before"] else "") + mark)
    missed = [w for w in result.get("sender_windows", []) if w["missed"]]
    for w in missed:
        rows.append(f"sender seconds {w['from_s']}-{w['to_s']}: {w['sent']} of {w['due']} frames, {w['cause']} "
                    f"(write-wait {w['write_wait_ms']} ms, credit-wait {w['credit_wait_ms']} ms, "
                    f"{w['backlog_frames']} frames behind as it opened)")
    for a in result["audio"]:
        rows.append(f"audio stream {a['stream']} ({a['reply']}): {a['played_ms']} ms played, {a['underruns']} underruns, "
                    f"{a['late']} after {UNDERRUN_GRACE_MS} ms" + ("" if a["ended"] else ", no end line"))
    rows.append("")
    for name, r in result["rules"].items():
        rows.append(f"{r['result']:<5} {name:<14} {r['detail']}")
    for g in result["gaps"]:
        rows.append(f"NOTE  {g}")
    rows.append(f"AV60  {result['verdict']}")
    return "\n".join(rows)


# ---- the test clip ------------------------------------------------------------------

def band_rows(height: int, split_pct: int | None) -> tuple[int, int]:
    """Two bands: the top band's rows (the device's share, rounded to 16) and the bottom's."""
    top = int(round(height * (split_pct or 50) / 100 / 16)) * 16
    top = min(max(16, top), height - 16)
    return top, height - top


def _encode(src: list[Path], w: int, h: int, fps: int, kbps: int, gop: int, crop: str | None, out: Path,
            max_frame: int | None = None) -> None:
    """ffmpeg: the sources joined, at fps, scaled to cover w x h and cropped to it (then to a band), baseline H.264.
    max_frame: the VBV buffer no larger than that many bytes, so no frame is (the gadget's frame limit)."""
    inputs, chains, labels = [], [], []
    for i, s in enumerate(src):
        inputs += ["-i", str(s)]
        chains.append(f"[{i}:v]fps={fps},scale={w}:{h}:force_original_aspect_ratio=increase:flags=lanczos,"
                      f"crop={w}:{h},setsar=1,format=yuv420p[s{i}]")
        labels.append(f"[s{i}]")
    chain = ";".join(chains) + f";{''.join(labels)}concat=n={len(src)}:v=1:a=0[c];[c]{crop or 'null'}[o]"
    cmd = ["ffmpeg", "-v", "error", "-y", *inputs, "-filter_complex", chain, "-map", "[o]", "-an",
           "-c:v", "libx264", "-profile:v", "baseline", "-preset", "medium", "-tune", "zerolatency",
           "-b:v", f"{kbps}k", "-maxrate", f"{kbps}k",
           "-bufsize", f"{max(1, min(kbps // 2, max_frame * 8 // 1000) if max_frame else kbps // 2)}k",
           "-g", str(gop), "-keyint_min", str(gop), "-sc_threshold", "0", "-bf", "0", "-refs", "1",
           "-threads", "1", "-x264-params", "repeat-headers=1", "-f", "h264", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("ffmpeg failed:\n" + r.stderr[-2000:])


def write_records(path: Path, records: list[bytes]) -> None:
    path.write_bytes(b"".join(struct.pack("<I", len(r)) + r for r in records))


def make_clip(w: int, h: int, fps: int, kbps: int, tiles: int = 1, split_pct: int | None = None,
              gop_s: float = 2.0, source: list[Path] | None = None, max_frame: int | None = None) -> list[bytes]:
    """The test clip's frames (one access unit each, or the two-band payload), made once and cached."""
    src = source or [p for p in TEST_CLIPS if p.exists()]
    if not src:
        raise SystemExit(f"no source clip: pass --clip (Annex-B .264 at the stream's size, or any video)")
    gop = max(1, int(round(gop_s * fps)))
    key = hashlib.sha256(json.dumps([CLIP_VERSION, w, h, fps, kbps, tiles, split_pct, gop, [str(s) for s in src],
                                     [s.stat().st_size for s in src]] + ([max_frame] if max_frame else []))
                         .encode()).hexdigest()[:12]
    CACHE.mkdir(parents=True, exist_ok=True)
    out = CACHE / f"clip_{w}x{h}_{fps}_{kbps}_{tiles}_{key}.au"
    if out.exists():
        return C.read_records(out)
    if tiles == 1:
        tmp = out.with_suffix(".264")
        _encode(src, w, h, fps, kbps, gop, None, tmp, max_frame)
        frames = C.split_access_units(tmp.read_bytes())
        tmp.unlink()
    else:
        top, bottom = band_rows(h, split_pct)
        parts = []
        for name, rows, y, share in (("top", top, 0, top / h), ("bottom", bottom, top, bottom / h)):
            tmp = out.with_suffix(f".{name}.264")
            # Scale to the whole picture, then crop this band out of it.
            _encode(src, w, h, fps, max(1, int(kbps * share)), gop, f"crop={w}:{rows}:0:{y}", tmp)
            parts.append(C.split_access_units(tmp.read_bytes()))
            tmp.unlink()
        n = min(len(parts[0]), len(parts[1]))
        frames = [struct.pack("<I", len(parts[0][i])) + parts[0][i] + parts[1][i] for i in range(n)]
    if not frames or not C.is_keyframe(frames[0], tiles):
        raise SystemExit("the encoded clip doesn't start with a keyframe")
    write_records(out, frames)
    return frames


def clip_from_file(path: Path, w: int, h: int, fps: int, kbps: int, tiles: int, split_pct: int | None) -> list[bytes]:
    if path.suffix in (".264", ".h264") and tiles == 1:
        return C.split_access_units(path.read_bytes())
    if path.suffix == ".au":
        return C.read_records(path)
    return make_clip(w, h, fps, kbps, tiles, split_pct, source=[path])


# ---- reply audio --------------------------------------------------------------------

def reply_pcm(kind: str, secs: float, rate: int, dbfs: float) -> list[int]:
    """A sweep (first reply) or a tone (second): moderate level, no speech."""
    if kind == "sweep":
        return C.sweep_pcm(200.0, min(7000.0, rate / 2 - 500), secs, rate, dbfs)
    return C.tone_pcm(660.0, secs, rate, dbfs)


def audio_start(rate: int, frame_bytes: int, live_ms: int = 0) -> bytes:
    """start_streaming: LC3 at rate, the frame size its bitrate takes, and, as the Muse apps
    do for a call's reply stream on a gadget that lists live_depth, a live depth (ids in increasing order)."""
    live = bytes([P["mg_stream_audio_parameter_live_depth_ms"], *live_ms.to_bytes(2, "little")]) if live_ms else b""
    return bytes([SA, P["mg_stream_audio_start_streaming"], 5 if live_ms else 4,
                  P["mg_stream_audio_parameter_codec"], LC3,
                  P["mg_stream_audio_parameter_sample_rate"], *rate.to_bytes(4, "little"),
                  P["mg_stream_audio_parameter_bitrate"], *(frame_bytes * 8 // 10).to_bytes(2, "little"),
                  P["mg_stream_audio_parameter_frame_size"], *frame_bytes.to_bytes(2, "little")]) + live


def subtitle_state(active: bool, timing: int) -> bytes:
    return bytes([SUBTITLE, P["mg_subtitle_state"], 1 if active else 0, timing])


def subtitle_words(reply_id: int, cues: list[tuple[int, str]], offset: int, flags: int = 0) -> bytes:
    out = struct.pack("<HBHB", reply_id, flags, offset, len(cues))
    for at, w in cues:
        b = w.encode("utf-8")
        out += struct.pack("<IB", at, len(b)) + b
    return bytes([SUBTITLE, P["mg_subtitle_words"]]) + out


def subtitle_reply_end(reply_id: int, at: int) -> bytes:
    return bytes([SUBTITLE, P["mg_subtitle_reply_end"]]) + struct.pack("<HI", reply_id, at)


def pack_words(words: list[str], first_frame: int, frames_per_word: int, max_bytes: int) -> list[tuple[int, list]]:
    """Cues for words (frame timing), packed into words messages of at most max_bytes from the reply id on:
    [(offset, [(frame, word), ...]), ...]."""
    msgs, cur, size, offset, pos = [], [], 6, 0, 0
    for i, w in enumerate(words):
        text = w + (" " if i < len(words) - 1 else "")
        need = 5 + len(text.encode())
        if cur and (size + need > max_bytes or len(cur) == 255):
            msgs.append((offset, cur))
            cur, size, offset = [], 6, pos
        cur.append((first_frame + i * frames_per_word, text))
        size += need
        pos += len(text.encode())
    if cur:
        msgs.append((offset, cur))
    return msgs


# ---- the log: written as it comes ------------------------------------------------------

_CONTROL_RE = re.compile(r"[\x00-\x08\x0b-\x1f\x7f-\x9f]")


def printable(text: str) -> str:
    """A gadget's line as plain text: C0/C1 control characters (terminal escapes among them) shown as \\xNN, so a
    line from the peripheral can't drive the operator's terminal or fake the report around it."""
    return _CONTROL_RE.sub(lambda m: f"\\x{ord(m.group()):02x}", text)


class Sink:
    """Everything the run sees, written to the jsonl as it arrives (masked by tools/redact.py), so a run
    that dies still leaves its log: the gadget's lines (the mirror, the console), the sender's per-second
    counts and the run's own notes. Keeps the gadget's latest counters for the progress line."""

    def __init__(self, path: Path | None = None):
        self.lines: list[tuple[float, str, str]] = []
        self.sender: list[dict] = []
        self.lock = threading.Lock()
        self.last: dict = {"video": None, "play": None, "band": None}
        self.f = None
        if path:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            self.f = os.fdopen(fd, "w", buffering=1)

    def _write(self, rec: dict) -> None:
        if self.f:
            self.f.write(json.dumps(rec) + "\n")

    def append(self, item: tuple[float, str, str]) -> None:
        at, src, text = item
        text = redact.redact_text(printable(text))
        with self.lock:
            self.lines.append((at, src, text))
            self._write({"t": at, "src": src, "line": text})
            tick = parse_line(text)
            if tick:
                self.last[tick["kind"]] = tick
            m = BAND_RE.search(text)
            if m:
                self.last["band"] = m.group(1)

    def add_sender(self, rec: dict) -> None:
        with self.lock:
            self.sender.append(rec)
            self._write({"t": rec["t"], "src": "sender", "rec": rec})

    def note(self, at: float, text: str) -> None:
        with self.lock:
            self._write({"t": at, "src": "run", "line": text})

    def meta(self, **kw) -> None:
        with self.lock:
            self._write({"meta": kw})

    def gadget_lines(self) -> list[tuple[float, str]]:
        with self.lock:
            return [(at, text) for at, src, text in self.lines if src in ("nus", "console")]

    def close(self) -> None:
        with self.lock:
            if self.f:
                self.f.close()
                self.f = None


def load_log(path: Path) -> tuple[dict, list[tuple[float, str]], list[dict]]:
    """A saved run (whole or cut short): its meta (later records win), the gadget's lines and the sender's counts."""
    meta, lines, sender = {}, [], []
    for raw in path.read_text().splitlines():
        try:
            rec = json.loads(raw)
        except ValueError:
            continue   # a line cut by a kill
        if "meta" in rec:
            meta.update(rec["meta"])
        elif rec.get("src") == "sender":
            sender.append(rec["rec"])
        elif rec.get("src") in ("nus", "console"):
            lines.append((rec["t"], printable(str(rec["line"]))))   # as Sink.append() wrote it, or an edited log
    return meta, lines, sender


# ---- the console (read-only) ----------------------------------------------------------

class Console(threading.Thread):
    """Lines from a USB console with their host time: read-only, no DTR/RTS change, 115200 baud. Its own
    thread, never the event loop's; it reopens the port if the gadget restarts."""

    def __init__(self, port: str, sink: Sink):
        super().__init__(daemon=True)
        self.port, self.sink, self.stop = port, sink, threading.Event()
        self.opened = 0
        self.lines = 0

    def run(self) -> None:
        fd = None
        buf = b""
        while not self.stop.is_set():
            if fd is None:
                try:
                    fd = os.open(self.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
                    a = termios.tcgetattr(fd)
                    a[2] = (a[2] & ~termios.HUPCL) | termios.CLOCAL | termios.CREAD
                    a[3] &= ~(termios.ICANON | termios.ECHO)
                    a[4] = a[5] = termios.B115200
                    termios.tcsetattr(fd, termios.TCSANOW, a)
                    self.opened += 1
                except (OSError, termios.error):
                    if fd is not None:
                        os.close(fd)
                    fd = None
                    self.stop.wait(0.5)
                    continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                self.stop.wait(0.02)
                continue
            except OSError:
                os.close(fd)
                fd = None
                continue
            if not chunk:
                self.stop.wait(0.02)
                continue
            buf += chunk
            *done, buf = buf.split(b"\n")
            now = time.monotonic()
            for ln in done:
                text = ln.decode("utf-8", "replace").strip()
                if text:
                    self.lines += 1
                    self.sink.append((now, "console", text))
        if fd is not None:
            os.close(fd)


# ---- the run ------------------------------------------------------------------------

class Abort(Exception):
    """The run can't go on: which phase, and why."""

    def __init__(self, phase: str, why: str):
        super().__init__(f"{phase}: {why}")
        self.phase, self.why = phase, why


def peripheral_of(client):
    """CoreBluetooth's CBPeripheral behind a bleak client (None elsewhere, or for the fake gadget without one)."""
    p = getattr(getattr(client, "_backend", None), "_peripheral", None)
    return p if p is not None and hasattr(p, "canSendWriteWithoutResponse") else None


async def write_room(client, until: float) -> bool:
    """True once the host stack takes another Write Without Response, False if not by `until`. CoreBluetooth
    drops a write it has no room for, without a word: a dropped Data write loses bytes the gadget never
    counts, so the credits never come back. Never write without room."""
    p = peripheral_of(client)
    if p is None:
        return True
    while not p.canSendWriteWithoutResponse():
        if time.monotonic() >= until:
            return False
        await asyncio.sleep(0.002)
    return True


class AvGadget(C.Gadget):
    """mg_ble_client's Gadget, also keeping whole log lines (timed), keyframe and call requests, the gadget's
    own video stop, and Control writes paced on the host stack's room."""

    def __init__(self, client, sink: Sink):
        super().__init__(client, verbose=False)
        self.sink = sink
        self._buf = ""
        self.keyframe_asks = 0
        self.call_requests: list[int] = []
        self.sub_caps: dict = {}
        self.video_stopped: int | None = None
        self.disconnected = False

    def _on_control(self, char, payload: bytearray) -> None:
        p = bytes(payload)
        if p[:2] == bytes([SV, P["mg_stream_video_request_keyframe"]]):
            self.keyframe_asks += 1
            return
        if p[:2] == bytes([SV, P["mg_stream_video_stop_streaming"]]):
            self.video_stopped = p[2] if len(p) > 2 else 0
        if len(p) >= 2 and p[0] == LIVE_CALL:
            self.call_requests.append(p[1])
        if len(p) >= 8 and p[:2] == bytes([SUBTITLE, P["mg_subtitle_capabilities"]]):
            self.sub_caps = {"version": p[2], "flags": p[3], "lines": p[4], "chars": p[5],
                             "max_words_bytes": int.from_bytes(p[6:8], "little")}
            return
        super()._on_control(char, payload)

    def _on_nus(self, char, payload: bytearray) -> None:
        super()._on_nus(char, payload)
        self._buf += bytes(payload).decode("utf-8", "replace")
        *done, self._buf = self._buf.split("\n")
        now = time.monotonic()
        for ln in done:
            ln = ln.strip()
            if ln:
                self.sink.append((now, "nus", ln))

    async def send(self, *payload: int) -> None:
        if not await write_room(self.client, time.monotonic() + 3.0):
            raise Abort("link", "the Mac's Bluetooth took no write for 3 s (a Control message)")
        await self.client.write_gatt_char(P["MG_CONTROL_UUID"], bytes(payload), response=False)


@dataclass
class AudioJob:
    frames: list[bytes]
    per_write: int
    t0: float
    lead: float
    sent_frames: int = 0
    sent_bytes: int = 0
    progress_t: float = 0.0


class SenderError(Exception):
    pass


class Pump:
    """The one sender on Data, on a 40 ms timer: the pre-encoded clip's frames at their due times and,
    during a spoken reply, its audio, alternating with change_data_type (about 25 times a second, as the
    Muse app does). Nothing is encoded here. It writes only when the host stack has room and the gadget's
    credits allow, and counts each second what it actually wrote (frames whose last byte went, bytes)
    against what was due, and why it waited (the gadget's credits, or the Mac's Bluetooth)."""

    SLOT = 0.04
    LOST_S = 3.0     # credits held this long with bytes the gadget never counted: lost on the link

    def __init__(self, g: AvGadget, frames: list[bytes], fps: int, tiles: int, chunk: int, sink: Sink | None = None,
                 codec: int = H264):
        self.g, self.frames, self.fps, self.tiles, self.chunk, self.sink = g, frames, fps, tiles, chunk, sink
        self.codec = codec          # the stream's codec: its Data type (change_data_type)
        self.audio: AudioJob | None = None
        self.stop = asyncio.Event()
        self.k = 0                  # frames queued (the next frame number)
        self.clip_i = 0
        self.pending = bytearray()  # video bytes queued, not yet written
        self.pending_ends: list[tuple[int, float]] = []   # (bytes written once a frame's last byte goes, its due time)
        self.vsent = 0
        self.selected = codec       # what Data carries now (as the device sees it: the start's codec)
        self.switches = 0
        self.max_lag = 0.0
        self.late_frames = 0
        self.frames_done = 0
        self.t0 = 0.0
        self.last_ask_v = 0.0
        self.last_ask_a = 0.0
        self.kf_seen = 0
        self.video_progress_t = 0.0
        self.blocked = ""           # why the last slot left bytes behind: "", "credits" or "room"
        self.stuck_v: tuple[float, int] | None = None   # (since, the gadget's received then) while credit-held
        self.stuck_a: tuple[float, int] | None = None
        self.bin_i = 0
        self.bin = self._new_bin(0)

    # -- per-second counts

    def _new_bin(self, i: int) -> dict:
        return {"sec": i, "t": self.t0 + i, "frames": 0, "due": 0, "bytes": 0, "audio_bytes": 0,
                "credit_ms": 0, "room_ms": 0, "switches": 0, "max_lag_ms": 0}

    def roll(self, now: float) -> None:
        i = int(now - self.t0)
        while i > self.bin_i:
            if self.sink:
                self.sink.add_sender(self.bin)
            self.bin_i += 1
            self.bin = self._new_bin(self.bin_i)

    # -- writing

    async def select(self, kind: int, until: float) -> bool:
        if self.selected == kind:
            return True
        if not await write_room(self.g.client, until):
            return False
        await self.g.client.write_gatt_char(P["MG_CONTROL_UUID"], bytes([CDT, kind]), response=False)
        self.selected = kind
        self.switches += 1
        self.bin["switches"] += 1
        return True

    def queue_due(self, now: float) -> None:
        if self.g.keyframe_asks > self.kf_seen:
            self.kf_seen = self.g.keyframe_asks
            self.clip_i = 0            # the clip's first frame is a keyframe (SPS and PPS with it)
        while self.t0 + self.k / self.fps <= now:
            au = self.frames[self.clip_i % len(self.frames)]
            key = self.clip_i % len(self.frames) == 0 or C.is_keyframe(au, self.tiles)
            due = self.t0 + self.k / self.fps
            self.pending += C.video_frame(self.k, key, au)
            self.pending_ends.append((self.vsent + len(self.pending), due))
            if int(due - self.t0) == self.bin_i:
                self.bin["due"] += 1
            self.k += 1
            self.clip_i += 1

    def _check_lost(self, now: float, what: str, inflight: int, received: int, stuck):
        """Credits held with bytes in flight the gadget's count hasn't moved past for LOST_S: they were lost."""
        if inflight <= 0:
            return None
        if stuck is None or stuck[1] != received:
            return (now, received)
        if now - stuck[0] >= self.LOST_S:
            raise SenderError(f"{what}: the gadget's count of bytes received stayed {received} for "
                              f"{now - stuck[0]:.0f} s with {inflight} bytes written past it and no credit left: "
                              f"lost on the link, or the gadget stopped reporting")
        return stuck

    async def video_step(self, now: float, until: float) -> None:
        g = self.g
        while self.pending and not self.stop.is_set():
            w = bytes(self.pending[:self.chunk])
            received = g.vbuf.get("received", 0)
            inflight = self.vsent - received
            if len(w) > g.vbuf.get("avail", 0) - inflight:
                self.blocked = "credits"
                self.stuck_v = self._check_lost(now, "video", inflight, received, self.stuck_v)
                if now - self.last_ask_v > 0.1:
                    self.last_ask_v = now
                    if await write_room(g.client, until):
                        await g.client.write_gatt_char(P["MG_CONTROL_UUID"],
                                                       bytes([SV, P["mg_stream_video_request_buffer_update"]]),
                                                       response=False)
                return
            self.stuck_v = None
            if not await self.select(self.codec, until):
                self.blocked = "room"
                return
            if not await write_room(g.client, until):
                self.blocked = "room"
                return
            await g.client.write_gatt_char(P["MG_DATA_UUID"], w, response=False)
            del self.pending[:len(w)]
            self.vsent += len(w)
            self.bin["bytes"] += len(w)
            self.video_progress_t = time.monotonic()
            while self.pending_ends and self.pending_ends[0][0] <= self.vsent:
                _, due = self.pending_ends.pop(0)
                lag = time.monotonic() - due
                self.frames_done += 1
                self.bin["frames"] += 1
                self.max_lag = max(self.max_lag, lag)
                self.bin["max_lag_ms"] = max(self.bin["max_lag_ms"], int(lag * 1000))
                if lag > 1.0 / self.fps:
                    self.late_frames += 1
            if time.monotonic() >= until:
                return
        self.blocked = ""

    async def audio_step(self, now: float, until: float) -> None:
        a, g = self.audio, self.g
        if a is None or a.sent_frames >= len(a.frames):
            return
        due = min(len(a.frames), int((now - a.t0 + a.lead) * 100))
        while a.sent_frames < due and not self.stop.is_set():
            n = min(a.per_write, due - a.sent_frames)
            inflight = a.sent_bytes - g.buf_received
            fb = len(a.frames[a.sent_frames])
            if fb and inflight + n * fb > g.buf_avail:
                # A live-depth stream's credits can be smaller than one full write (70 ms = 7 frames on the
                # W175): write what fits, never wait for room a whole write would need.
                n = max(1, min(n, (g.buf_avail - inflight) // fb))
            w = b"".join(a.frames[a.sent_frames:a.sent_frames + n])
            if inflight + len(w) > g.buf_avail:
                self.stuck_a = self._check_lost(now, "reply audio", inflight, g.buf_received, self.stuck_a)
                if now - self.last_ask_a > 0.1:
                    self.last_ask_a = now
                    if await write_room(g.client, until):
                        await g.client.write_gatt_char(P["MG_CONTROL_UUID"],
                                                       bytes([SA, P["mg_stream_audio_request_buffer_update"]]),
                                                       response=False)
                return
            self.stuck_a = None
            if not await self.select(LC3, until) or not await write_room(g.client, until):
                return
            await g.client.write_gatt_char(P["MG_DATA_UUID"], w, response=False)
            a.sent_frames += n
            a.sent_bytes += len(w)
            a.progress_t = time.monotonic()
            self.bin["audio_bytes"] += len(w)

    async def run(self) -> None:
        self.t0 = time.monotonic()
        self.bin = self._new_bin(0)
        self.video_progress_t = self.t0
        while not self.stop.is_set():
            now = time.monotonic()
            until = now + self.SLOT
            self.roll(now)
            self.queue_due(now)
            await self.audio_step(now, now + self.SLOT / 2)
            await self.video_step(now, until)
            if self.pending and self.blocked:
                self.bin["credit_ms" if self.blocked == "credits" else "room_ms"] += int(self.SLOT * 1000)
            await asyncio.sleep(max(0.0, until - time.monotonic()))
        self.roll(time.monotonic())


@dataclass
class Plan:
    """When each reply runs, in seconds from the first video frame."""
    spoken1: tuple[float, float]
    muted: tuple[float, float]
    spoken2: tuple[float, float]


def plan_for(secs: float) -> Plan:
    return Plan(spoken1=(0.10 * secs, 0.20 * secs), muted=(0.37 * secs, 0.17 * secs), spoken2=(0.60 * secs, 0.20 * secs))


def board_for(name: str) -> tuple[str, int, int, int, int] | None:
    m = re.match(r"(?:MuseGadget|SAFE)-([A-Za-z0-9]+)-", name or "")
    return BOARDS.get(m.group(1).upper()) if m else None


def bands_for(name: str) -> tuple[int, int | None]:
    m = re.match(r"(?:MuseGadget|SAFE)-([A-Za-z0-9]+)-", name or "")
    return BANDS.get(m.group(1).upper(), (1, None)) if m else (1, None)


class Runner:
    """One AV60 run, in phases, each bounded: it can't hang. A phase that runs out of time, a gadget that
    goes quiet, a sender that stops making progress or dies, a lost link: each ends the run with the phase
    named (exit 2), after a bounded cleanup (video stopped, the call ended)."""

    def __init__(self, args, sink: Sink, out=print, connect=None, scan=None):
        self.args, self.sink, self.out = args, sink, out
        self.connect_fn = connect
        self.scan_fn = scan
        self.phase = "start"
        self.t_start = time.monotonic()
        self.g: AvGadget | None = None
        self.pump: Pump | None = None
        self.replies: list[Reply] = []
        self.video_end: float | None = None
        self.audio_on = False
        self.in_call = False
        self.run_vol = None   # run_volume's state while --volume holds
        self.video_on = False
        self.live_ms = 0   # start_streaming's live depth, when the gadget lists it

    def el(self) -> float:
        return time.monotonic() - self.t_start

    def say(self, text: str) -> None:
        self.out(f"[{self.el():6.1f}s] {text}")
        self.sink.note(time.monotonic(), text)

    def enter(self, phase: str) -> None:
        self.phase = phase
        self.say(f"phase: {phase}")

    async def step(self, phase: str, coro, timeout: float):
        """One phase, bounded: its own error, or a timeout, ends the run with the phase named."""
        self.enter(phase)
        task = asyncio.ensure_future(coro)
        done, _ = await asyncio.wait({task}, timeout=timeout)
        if not done:
            task.cancel()
            raise Abort(phase, f"not done within {timeout:.0f} s")
        e = task.exception()
        if e is None:
            return task.result()
        if isinstance(e, Abort):
            raise e
        raise Abort(phase, f"{type(e).__name__}: {e}" if str(e) else
                    f"{type(e).__name__} (no answer from the gadget)" if isinstance(e, TimeoutError) else type(e).__name__) from e

    # -- the parts

    async def scan(self):
        if self.scan_fn:
            return await self.scan_fn(self.args)
        return await C.pick(self.args)

    def stream_for(self, name: str) -> dict:
        board = board_for(name)
        a = self.args
        w, h = a.size or ((board[1], board[2]) if board else (None, None))
        fps = a.fps or (board[3] if board else None)
        kbps = a.kbps or (board[4] if board else None)
        if not all((w, h, fps, kbps)):
            raise Abort("stream", f"{name} isn't in the board table: pass --size, --fps and --kbps")
        tiles, split = bands_for(name)
        m = re.match(r"(?:MuseGadget|SAFE)-([A-Za-z0-9]+)-", name or "")
        code = m.group(1).upper() if m else ""
        return {"board": board[0] if board else None, "w": w, "h": h, "fps": fps, "kbps": kbps,
                "tiles": a.tiles or tiles, "split": split,
                "max_frame": getattr(a, "max_frame", None) or MAX_FRAME.get(code)}

    def clip(self, s: dict) -> list[bytes]:
        if self.args.clip:
            return clip_from_file(Path(self.args.clip), s["w"], s["h"], s["fps"], s["kbps"], s["tiles"], s["split"])
        return make_clip(s["w"], s["h"], s["fps"], s["kbps"], s["tiles"], s["split"], self.args.gop,
                         max_frame=s.get("max_frame"))

    async def watch(self) -> None:
        """Once a second: the progress line, and the checks that end a stuck run."""
        g, pump = self.g, self.pump
        last_frames = 0
        while True:
            await asyncio.sleep(1.0)
            b = pump.bin
            v, pl = self.sink.last["video"], self.sink.last["play"]
            gad = (f"gadget shown {v['shown']}/{v['span']} ms drop {v['drop']} decerr {v['decerr']} low {v.get('low')}"
                   if v else "gadget: no av video line yet")
            aud = ""
            if pump.audio is not None:
                aud = f" | reply audio {pump.audio.sent_frames}/{len(pump.audio.frames)} frames"
            if pl:
                aud += f" | play t={pl['t']} underruns {pl['underruns']}"
            sent = pump.frames_done - last_frames
            last_frames = pump.frames_done
            self.out(f"[{self.el():6.1f}s] {self.phase} | sent {sent} frames ({pump.k} due so far), "
                     f"credits {g.vbuf.get('avail', 0) - (pump.vsent - g.vbuf.get('received', 0))} B, "
                     f"waits: credits {b['credit_ms']} ms, Bluetooth {b['room_ms']} ms | {gad}{aud}")
            if g.disconnected:
                raise Abort("link", "the gadget disconnected")
            if g.video_stopped is not None:
                raise Abort("video", f"the gadget stopped the video stream itself (stop_streaming {g.video_stopped})")
            if P["mg_live_call_request_end"] in g.call_requests:
                raise Abort("call", "the gadget ended the call")
            stalled = time.monotonic() - pump.video_progress_t
            if pump.pending and stalled > 8:
                why = ("the gadget's credits stayed full" if pump.blocked == "credits" else
                       "the Mac's Bluetooth took no write" if pump.blocked == "room" else "the sender stopped")
                raise Abort("sender", f"no video written for {stalled:.0f} s: {why}")

    async def spoken_reply(self, n: int, kind: str, secs: float) -> Reply:
        g, pump, a = self.g, self.pump, self.args
        label = f"spoken reply {n}"
        r = Reply("spoken", time.monotonic())
        frames = self.dec.lc3_encode(reply_pcm(kind, secs, self.rate, a.level), self.rate, a.frame_bytes)
        await g.send(P["mg_command_assistant_state"], C.assistant_state("responding"))
        g.buf_event.clear()
        g.stream_errors.clear()
        g.stops.clear()
        await g.send(*audio_start(self.rate, a.frame_bytes, self.live_ms))
        try:
            await asyncio.wait_for(g.buf_event.wait(), 3)
        except asyncio.TimeoutError:
            raise Abort(label, "start_streaming: " + ("; ".join(g.stream_errors) or "no buffer_update in 3 s")) from None
        self.audio_on = True
        if self.anchors and pump.frames_done:
            # The picture this reply's first sample belongs with: the one just written (the bench's speech
            # starts with the picture it's sent beside), extended to 32 bits as subtitles count frames.
            frame = pump.frames_done - 1
            await g.send(SA, P["mg_stream_audio_video_anchor"], *frame.to_bytes(4, "little"))
            self.say(f"{label}: A/V anchor at picture {frame}")
        per_write = max(1, (min(512, self.mtu - 3)) // a.frame_bytes)
        job = AudioJob(frames, per_write, time.monotonic(), a.lead, progress_t=time.monotonic())
        pump.audio = job
        self.say(f"{label}: LC3 {self.rate} Hz, {a.frame_bytes} B frames ({a.frame_bytes * 8 // 10} kb/s), "
                 f"{len(frames) / 100:.1f} s {kind} at {a.level:.0f} dBFS")
        while job.sent_frames < len(frames):
            if time.monotonic() - job.progress_t > 5:
                raise Abort(label, f"no audio written for 5 s ({job.sent_frames} of {len(frames)} frames; the gadget's "
                                   f"audio credits: {g.buf_avail} B free, {g.buf_received} received of "
                                   f"{job.sent_bytes} sent)")
            if g.stops:
                break   # the gadget ended it
            await asyncio.sleep(0.02)
        await g.send(SA, P["mg_stream_audio_stop_streaming"], P["mg_stream_audio_buffer_keep"])
        deadline = time.monotonic() + a.lead + 5
        while not g.stops and time.monotonic() < deadline:
            await asyncio.sleep(0.05)
        pump.audio = None
        self.audio_on = False
        lost = max(0, job.sent_bytes - g.buf_received) if g.stops else 0   # the gadget's last count, once it ended
        if lost:
            self.say(f"{label}: {lost} audio bytes written never reached the gadget")
        await g.send(P["mg_command_assistant_state"], C.assistant_state("done"))
        r.end = time.monotonic()
        r.detail = {"audio": kind, "rate": self.rate, "frame_bytes": a.frame_bytes, "frames": len(frames), "lost": lost,
                    "stop": ("keep" if g.stops[0][1] == P["mg_stream_audio_buffer_keep"] else "drop") if g.stops else None,
                    "errors": list(g.stream_errors)}
        if not g.stops:
            self.say(f"{label}: no stop_streaming from the gadget after the audio played out")
        return r

    async def muted_reply(self, secs: float, reply_id: int = 0x6060) -> Reply:
        g, pump, fps = self.g, self.pump, self.stream["fps"]
        r = Reply("muted", time.monotonic())
        timing = P["mg_subtitle_timing_frame"]
        await g.send(*subtitle_state(True, timing))
        await g.send(P["mg_command_assistant_state"], C.assistant_state("responding"))
        max_bytes = min(255, (g.sub_caps.get("max_words_bytes") or 60), self.mtu - 5)
        words = (WORDS * 4)[:max(4, int(secs * 2.5))]
        per_word = max(1, int(round(secs * fps / len(words))))
        start = pump.k + int(0.5 * fps)          # the first cue half a second ahead
        msgs = pack_words(words, start, per_word, max_bytes)
        sent = 0
        for i, (off, cues) in enumerate(msgs):
            # Each cue at least ~300 ms (and here up to 1.5 s) before its frame.
            while cues[0][0] - pump.k > int(1.5 * fps):
                await asyncio.sleep(0.05)
            flags = (P["mg_subtitle_words_replace"] if i == 0 else 0) | (P["mg_subtitle_words_final"] if i == len(msgs) - 1 else 0)
            await g.send(*subtitle_words(reply_id, cues, off, flags))
            sent += len(cues)
        last = msgs[-1][1][-1][0] if msgs else pump.k
        await g.send(*subtitle_reply_end(reply_id, last + per_word))
        while pump.k < last + per_word:
            await asyncio.sleep(0.05)
        await g.send(P["mg_command_assistant_state"], C.assistant_state("done"))
        await asyncio.sleep(1.0)
        await g.send(*subtitle_state(False, timing))
        r.end = time.monotonic()
        r.detail = {"words": sent, "messages": len(msgs), "max_words_bytes": max_bytes}
        self.say(f"muted reply: {sent} words in {len(msgs)} messages over {secs:.1f} s (frame timing), "
                 f"band {self.sink.last['band'] or 'not seen'}")
        return r

    async def video_lost(self) -> int:
        """After the sender stopped: the gadget's own count of video bytes received against what was written."""
        g, pump = self.g, self.pump
        await asyncio.sleep(0.5)
        g.vbuf_event.clear()
        await g.send(SV, P["mg_stream_video_request_buffer_update"])
        try:
            await asyncio.wait_for(g.vbuf_event.wait(), 2.0)
        except asyncio.TimeoutError:
            self.say("link check: no buffer_update from the gadget")
            return 0
        lost = pump.vsent - g.vbuf.get("received", 0)
        if lost:
            self.say(f"link check: {lost} video bytes written never reached the gadget")
        return max(0, lost)

    async def sequence(self) -> None:
        plan, secs = plan_for(self.args.secs), self.args.secs
        t0 = self.pump.t0

        async def until(sec: float) -> None:
            await asyncio.sleep(max(0.0, t0 + sec - time.monotonic()))

        no_audio = getattr(self.args, "no_audio", False)
        await self.step("video before the first reply", until(plan.spoken1[0]), plan.spoken1[0] + 5)
        if not no_audio:
            self.replies.append(await self.step("spoken reply 1", self.spoken_reply(1, "sweep", plan.spoken1[1]),
                                                plan.spoken1[1] + self.args.lead + 20))
        await self.step("video before the muted reply", until(plan.muted[0]), secs)
        if not getattr(self.args, "no_subtitles", False):
            self.replies.append(await self.step("muted reply", self.muted_reply(plan.muted[1]), plan.muted[1] + 20))
        await self.step("video before the second reply", until(plan.spoken2[0]), secs)
        if not no_audio:
            self.replies.append(await self.step("spoken reply 2", self.spoken_reply(2, "tone", plan.spoken2[1]),
                                                plan.spoken2[1] + self.args.lead + 20))
        await self.step("video to the end", until(secs), secs)

    async def cleanup(self) -> None:
        """Bounded, best effort: whatever hangs here is given up after a few seconds."""
        g, pump = self.g, self.pump

        async def best(what, coro, timeout=3.0):
            try:
                await asyncio.wait_for(coro, timeout)
            except Exception as e:
                self.say(f"cleanup: {what}: {type(e).__name__} {e}")

        if pump is not None:
            pump.stop.set()
        if self.pump_task is not None and not self.pump_task.done():
            try:
                await asyncio.wait_for(asyncio.shield(self.pump_task), 2.0)
            except Exception:
                self.pump_task.cancel()
        if g is None:
            return
        if self.audio_on:
            await best("stopping the reply audio", g.send(SA, P["mg_stream_audio_stop_streaming"],
                                                           P["mg_stream_audio_buffer_drop"]))
        if self.video_on:
            await best("stopping the video", g.send(SV, P["mg_stream_video_stop_streaming"],
                                                     P["mg_stream_audio_buffer_drop"]))
        if self.run_vol is not None:
            await best("the speaker volume back", C.end_run_volume(g, self.run_vol))
            self.run_vol = None
        if self.in_call:
            await best("subtitles off", g.send(*subtitle_state(False, P["mg_subtitle_timing_frame"])))
            await best("ending the call", g.send(LIVE_CALL, C.live_call_state("ended")))
            await best("idle", g.send(P["mg_command_assistant_state"], C.assistant_state("idle")))

    async def go(self) -> dict:
        a = self.args
        self.pump_task = None
        name = None
        result_extra: dict = {}
        try:
            addr, name = await self.step("scan", self.scan(), a.scan_secs + 15)
            self.stream = s = self.stream_for(name)
            self.enter("clip")
            if not a.clip:
                self.say("the test clip: transcoded once with ffmpeg and cached, before connecting")
            frames = await asyncio.wait_for(asyncio.to_thread(self.clip, s), 600)
            clip_kbps = sum(map(len, frames)) * 8 * s["fps"] / len(frames) / 1000
            self.say(f"video: {s['w']}x{s['h']} {s['fps']} fps {s['kbps']} kb/s"
                     + (f", two bands {band_rows(s['h'], s['split'])[0]}+{band_rows(s['h'], s['split'])[1]} rows"
                        if s["tiles"] == 2 else "")
                     + f"; pre-encoded clip {len(frames)} frames, {clip_kbps:.0f} kb/s, largest {max(map(len, frames))} B")
            self.sink.meta(gadget=name, fps=s["fps"], stream=s)
            result_extra = {"stream": {**s, "clip_kbps": round(clip_kbps)}}
            self.dec = C.Decoder()
            client = self.connect_fn(addr, self)
            await self.step("connect", client.connect(), 30)
            try:
                return await self.session(client, name, frames, result_extra, addr)
            finally:
                await self.cleanup()
                try:
                    await asyncio.wait_for(client.disconnect(), 10)
                except Exception:
                    pass
        except Abort as e:
            self.say(f"ABORT in {e.phase}: {e.why}")
            result = evaluate(self.sink.gadget_lines(), (getattr(self, "stream", None) or {}).get("fps") or 30,
                              self.replies, self.video_end, sender=self.sink.sender or None)
            result.update(verdict="INCOMPLETE", error=f"{e.phase}: {e.why}", phase=e.phase, gadget=name,
                          video_end=self.video_end, **result_extra)
            return result

    async def session(self, client, name: str, frames: list[bytes], extra: dict, addr: str = "") -> dict:
        a, s = self.args, self.stream
        g = self.g = AvGadget(client, self.sink)
        await self.step("subscribe", g.start(), 15)
        st = await self.step("status", g.status(), 10)
        # A set-up gadget takes streams and kept settings only from a connection whose token proof matched:
        # prove with the key `mg_ble_client.py setup` saved on this Mac, as mg_ble_client's own commands do.
        if P["mg_command_token_proof"] in st["commands"]:
            proven = await self.step("proof", C.prove_if_kept(g, addr), 15)
            if proven is None:
                self.say("NOTE  no saved proof key for this gadget: a set-up gadget refuses the streams "
                         "(mg_ble_client.py setup saves one)")
            elif proven != "match":
                raise Abort("proof", f"the token proof {proven}: the saved key isn't this gadget's")
        need = [("live_call", LIVE_CALL), ("stream_video", SV), ("stream_audio", SA)]
        if not getattr(a, "no_subtitles", False):
            need.append(("subtitle", SUBTITLE))
        missing = [n for n, c in need if c not in st["commands"]]
        if missing:
            raise Abort("status", f"the gadget doesn't list {', '.join(missing)}")

        async def caps():
            await g.send(SV, P["mg_stream_video_request_capabilities"])
            v = C.parse_video_caps(await g.expect(SV, sub=P["mg_stream_video_report_capabilities"]))
            await g.send(SA, P["mg_stream_audio_request_capabilities"])
            au = C.parse_capabilities(await g.expect(SA, sub=P["mg_stream_audio_report_capabilities"]))
            if not getattr(a, "no_subtitles", False):
                await g.send(SUBTITLE, P["mg_subtitle_request_capabilities"])
            return v, au

        vcaps, acaps = await self.step("capabilities", caps(), 10)
        sug = vcaps.get("suggested") or {}
        self.say(f"gadget: {name} ({s['board'] or 'not in the board table'}); suggests {sug.get('w')}x{sug.get('h')} "
                 f"{sug.get('fps')} fps {sug.get('kbps')} kb/s; max {vcaps.get('max_size')}; bands {vcaps.get('tiles', 1)}")
        if sug and (sug.get("w"), sug.get("h"), sug.get("fps"), sug.get("kbps")) != (s["w"], s["h"], s["fps"], s["kbps"]):
            self.say(f"NOTE  the gadget suggests other than the stream ({s['w']}x{s['h']} {s['fps']} fps {s['kbps']} kb/s)")
        if LC3 not in acaps.get("codecs", []):
            raise Abort("capabilities", "the gadget doesn't play LC3")
        self.rate = max([r for r in acaps.get("sample_rates", []) if r in (16000, 24000)] or [16000])
        self.live_ms = 100 if "live_depth" in acaps else 0   # the apps' live depth for a call (mgcommands.h)
        # As the apps do, each reply's first audio write follows an A/V anchor naming its picture.
        self.anchors = bool(acaps.get("video_sync")) and not self.args.no_anchor
        if a.volume is not None:
            # For this run only (bench volumes are RAM-only): the gadget's vol= word over the Nordic
            # UART, or set_settings on an older gadget; cleanup() puts it back either way.
            self.run_vol = await self.step("volume", C.run_volume(g, a.volume), 5)
        self.mtu = getattr(client, "mtu_size", 247)
        chunk = min(512, self.mtu - 3)

        # The call.
        if a.wait_call:
            async def wait_start():
                while P["mg_live_call_request_start"] not in g.call_requests:
                    await asyncio.sleep(0.1)
            await self.step("waiting for the gadget to start a call", wait_start(), a.wait_call)

        async def call():
            await g.send(LIVE_CALL, C.live_call_state("connecting"))
            self.in_call = True
            await asyncio.sleep(1.0)
            await g.send(LIVE_CALL, C.live_call_state("live"))
            await asyncio.sleep(0.5)
        await self.step("call: connecting, live", call(), 10)

        async def video():
            for _ in range(8):
                g.vbuf_event.clear()
                g.video_errors.clear()
                await g.send(*C.video_start_payload(s["w"], s["h"], s["fps"], s["kbps"], s["tiles"]))
                try:
                    await asyncio.wait_for(g.vbuf_event.wait(), 1.0)
                    return
                except asyncio.TimeoutError:
                    if not any("busy" in e for e in g.video_errors):
                        break
            raise Abort("video start", "start_streaming refused: " + ("; ".join(g.video_errors) or "no buffer_update"))
        await self.step("video start", video(), 15)
        self.video_on = True

        pump = self.pump = Pump(g, frames, s["fps"], s["tiles"], chunk, self.sink)
        self.pump_task = asyncio.create_task(pump.run())
        watch = asyncio.create_task(self.watch())
        seq = asyncio.create_task(self.sequence())
        self.enter("call running")
        try:
            done, _ = await asyncio.wait({self.pump_task, watch, seq}, timeout=a.secs + 120,
                                         return_when=asyncio.FIRST_COMPLETED)
            if not done:
                raise Abort(self.phase, f"the call ran past {a.secs + 120:.0f} s")
            if self.pump_task in done:
                e = self.pump_task.exception()
                raise Abort("sender", f"{type(e).__name__}: {e}" if e else "it stopped by itself")
            if watch in done:
                raise watch.exception() or Abort(self.phase, "the watchdog stopped")
            e = seq.exception()
            if e:
                raise e if isinstance(e, Abort) else Abort(self.phase, f"{type(e).__name__}: {e}")
        finally:
            for t in (watch, seq):
                t.cancel()
            self.video_end = time.monotonic()
            pump.stop.set()
        self.enter("finish")
        try:
            await asyncio.wait_for(self.pump_task, 3.0)
        except Exception:
            self.pump_task.cancel()
        lost = await self.step("link check", self.video_lost(), 5)
        await self.cleanup()
        self.video_on = self.in_call = False
        await asyncio.sleep(2.0)   # the last lines on their way
        host = {"frames_sent": pump.frames_done, "frames_due": pump.k, "max_lag_ms": round(pump.max_lag * 1000),
                "late_frames": pump.late_frames, "data_type_switches": pump.switches,
                "switches_per_s": round(pump.switches / max(1e-6, self.video_end - pump.t0), 1),
                "keyframe_requests": g.keyframe_asks, "mtu": self.mtu}
        self.say(f"host: {pump.frames_done} of {pump.k} frames sent, {pump.switches} change_data_type "
                 f"({host['switches_per_s']}/s), max lag {host['max_lag_ms']} ms, {g.keyframe_asks} keyframe requests")
        result = evaluate(self.sink.gadget_lines(), s["fps"], self.replies, self.video_end, sender=self.sink.sender,
                          need_spoken=0 if getattr(a, "no_audio", False) else 2,
                          need_muted=0 if getattr(a, "no_subtitles", False) else 1)
        audio_lost = sum(r.detail.get("lost", 0) for r in self.replies)
        link_ok = not lost and not audio_lost
        result["rules"] = {"link": {"result": "PASS" if link_ok else "INVALID",
                                    "detail": "every byte written reached the gadget (its own counts)" if link_ok else
                                    f"{lost} video and {audio_lost} audio bytes written never reached the gadget "
                                    f"(lost on the Mac's side: rerun)"}, **result["rules"]}
        if not link_ok:
            result["verdict"] = "INVALID"   # the gadget never got the stream it's judged on
        result.update(gadget=name, board=s["board"], audio_format={"codec": "lc3", "rate": self.rate,
                                                                    "frame_bytes": a.frame_bytes},
                      host=host, video_end=self.video_end, **extra)
        return result


def ble_connect(addr: str, runner: Runner):
    from bleak import BleakClient

    def gone(_client):
        if runner.g is not None:
            runner.g.disconnected = True

    return BleakClient(addr, timeout=20.0, disconnected_callback=gone)


# ---- the dry run: a gadget in this process ---------------------------------------------

class FakeGadget:
    """A gadget in-process, for --dry and the host tests: enough of bleak's client and of the firmware's
    protocol (status, capabilities, live_call, video and audio streams with their credits, subtitles),
    logging the firmware's `av video:` / `av play:` lines on the Nordic UART mirror and, given one, the
    master side of a pty (the console). `faults`: "audio_stall" (no audio credits after the first
    update), "data_raises" (the 200th Data write fails), "write_hangs" (Data writes from the 200th never
    return), "no_status" (request_status unanswered), "lists_proof" (lists the token proof, as a set-up gadget does),
    "drop_writes" (Data writes lost while the link is busy, as CoreBluetooth drops them), and link_kbps
    (the host stack's room: a slow link the sender has to wait on)."""

    def __init__(self, name: str = "MuseGadget-SCW1-D7D7D7", sug=(208, 208, 30, 450), console_fd: int | None = None,
                 faults: tuple = (), link_kbps: float | None = None, rates=(16000, 24000), tiles: int = 1,
                 video_ring: int = 262144, decode_fps: float | None = None):
        self.name, self.sug, self.console_fd, self.faults = name, sug, console_fd, set(faults)
        self.rates, self.tiles = rates, tiles
        self.cbs: dict = {}
        self.mtu_size = 247
        self.t0 = time.monotonic()
        self.connected = False
        self.rx_type = H264
        self.v = {"on": False, "received": 0, "buf": bytearray(), "shown": 0, "dropped": 0, "consumed": 0,
                  "queue": [], "upd_t": 0.0}
        self.video_ring, self.decode_fps = video_ring, decode_fps
        self.au = {"on": False, "received": 0, "buffered": 0, "frame_bytes": 40, "clock": 0, "underruns": 0,
                   "last": -1, "playing": False, "keep": False, "updates": 0}
        self.calls: list[int] = []
        self.control_writes: list[bytes] = []
        self.subs_on = False
        self.band = False
        self.data_writes = 0
        self.tasks: list[asyncio.Task] = []
        self.link_kbps = link_kbps
        self.link_t = time.monotonic()
        self.link_bytes = 0.0
        if link_kbps:
            self._backend = type("B", (), {})()
            self._backend._peripheral = self   # canSendWriteWithoutResponse below

    # -- bleak's client

    async def connect(self):
        self.connected = True
        self.tasks = [asyncio.create_task(self._ticks()), asyncio.create_task(self._player()),
                      asyncio.create_task(self._decoder())]

    async def disconnect(self):
        self.connected = False
        for t in self.tasks:
            t.cancel()

    async def start_notify(self, uuid, cb):
        self.cbs[uuid] = cb

    def canSendWriteWithoutResponse(self) -> bool:   # noqa: N802 (CoreBluetooth's name)
        now = time.monotonic()
        self.link_bytes = max(0.0, self.link_bytes - (now - self.link_t) * self.link_kbps * 125)
        self.link_t = now
        return self.link_bytes < 2000

    async def write_gatt_char(self, uuid, data, response=False):
        data = bytes(data)
        if uuid == P["MG_CONTROL_UUID"]:
            self.control_writes.append(data)
        if self.link_kbps:
            self.link_bytes += len(data)
        if uuid == P["MG_DATA_UUID"]:
            self.data_writes += 1
            if "data_raises" in self.faults and self.data_writes == 200:
                raise OSError("the link went away")
            if "write_hangs" in self.faults and self.data_writes >= 200:
                await asyncio.Event().wait()   # a write that never returns
            if "drop_writes" in self.faults and self.data_writes % 50 == 0:
                return   # lost, as CoreBluetooth drops a write it had no room for
            self._data(data)
        elif uuid == P["MG_CONTROL_UUID"]:
            self._control(data)
        elif uuid == P["MG_NUS_RX_UUID"]:
            pass

    # -- the gadget's side

    def dev_ms(self) -> int:
        return int((time.monotonic() - self.t0) * 1000) + 5000

    def notify(self, uuid, payload: bytes) -> None:
        cb = self.cbs.get(uuid)
        if cb:
            cb(None, bytearray(payload))

    def log(self, tag: str, text: str) -> None:
        line = f"I ({self.dev_ms()}) {tag}: {text}\n"
        raw = line.encode()
        for i in range(0, len(raw), self.mtu_size - 3):
            self.notify(P["MG_NUS_TX_UUID"], raw[i:i + self.mtu_size - 3])
        if self.console_fd is not None:
            try:
                os.write(self.console_fd, raw.replace(b"\n", b"\r\n"))
            except OSError:
                pass

    def _video_update(self) -> None:
        v = self.v
        v["upd_t"] = time.monotonic()
        self.notify(P["MG_CONTROL_UUID"], bytes([SV, P["mg_stream_video_buffer_update"]])
                    + struct.pack("<II", self.video_ring - (v["received"] - v["consumed"]), v["received"]))

    def _audio_update(self) -> None:
        stall = "audio_stall" in self.faults
        if stall and self.au["updates"] >= 1:
            return
        self.au["updates"] += 1
        room = 2000 if stall else 16384 - self.au["buffered"]
        self.notify(P["MG_CONTROL_UUID"], bytes([SA, P["mg_stream_audio_buffer_update"]])
                    + struct.pack("<II", room, self.au["received"]))

    def _control(self, p: bytes) -> None:
        cmd = p[0]
        if cmd == P["mg_command_request_status"]:
            if "no_status" in self.faults:
                return
            self.notify(P["MG_CONTROL_UUID"], bytes([CDT, P["mg_data_type_audio_sbc"]]))
            proof = [P["mg_command_token_proof"]] if "lists_proof" in self.faults else []
            self.notify(P["MG_CONTROL_UUID"], bytes([P["mg_command_supported_features"], LIVE_CALL, SV, SA, SUBTITLE,
                                                     P["mg_command_assistant_state"], P["mg_command_set_settings"],
                                                     *proof]))
        elif cmd == SV:
            sub = p[1]
            if sub == P["mg_stream_video_request_capabilities"]:
                w, h, fps, kbps = self.sug
                caps = [bytes([P["mg_stream_video_capability_codecs"], 1, H264]),
                        bytes([P["mg_stream_video_capability_max_size"]]) + struct.pack("<HH", 240, 240),
                        bytes([P["mg_stream_video_capability_suggested"]]) + struct.pack("<HHBH", w, h, fps, kbps)]
                if self.tiles > 1:
                    caps.append(bytes([P["mg_stream_video_capability_tiles"], self.tiles]))
                self.notify(P["MG_CONTROL_UUID"], bytes([SV, P["mg_stream_video_report_capabilities"], len(caps)])
                            + b"".join(caps))
            elif sub == P["mg_stream_video_start_streaming"]:
                self.v.update(on=True, received=0, buf=bytearray(), shown=0, dropped=0, consumed=0, queue=[])
                self.rx_type = H264 if not self.au["on"] else self.rx_type
                self.v_t0 = time.monotonic()
                self.log("muse_video", "decoder open: H.264")
                self._video_update()
            elif sub == P["mg_stream_video_request_buffer_update"]:
                self._video_update()
            elif sub == P["mg_stream_video_stop_streaming"]:
                self.v["on"] = False
        elif cmd == SA:
            sub = p[1]
            if sub == P["mg_stream_audio_request_capabilities"]:
                caps = (bytes([P["mg_stream_audio_capability_codecs"], 2, P["mg_data_type_audio_sbc"], LC3])
                        + bytes([P["mg_stream_audio_capability_sample_rates"], len(self.rates)])
                        + b"".join(struct.pack("<I", r) for r in self.rates))
                self.notify(P["MG_CONTROL_UUID"], bytes([SA, P["mg_stream_audio_report_capabilities"], 2]) + caps)
            elif sub == P["mg_stream_audio_start_streaming"]:
                fb = 40
                i, n = 3, p[2]
                for _ in range(n):
                    param = p[i]
                    w = 1 if param in (P["mg_stream_audio_parameter_codec"], P["mg_stream_audio_parameter_channels"]) \
                        else 4 if param == P["mg_stream_audio_parameter_sample_rate"] else 2
                    if param == P["mg_stream_audio_parameter_frame_size"]:
                        fb = int.from_bytes(p[i + 1:i + 3], "little")
                    i += 1 + w
                self.au.update(on=True, received=0, buffered=0, frame_bytes=fb, clock=0, underruns=0, last=-1,
                               playing=False, keep=False, updates=0)
                self.log("mg.play", "stream start: lc3 24000 Hz, resampled to 16 kHz")
                self.play_tick = time.monotonic()
                self.play_prev = 0
                self._audio_update()
            elif sub == P["mg_stream_audio_request_buffer_update"]:
                if self.au["on"]:
                    self._audio_update()
            elif sub == P["mg_stream_audio_stop_streaming"]:
                if len(p) > 2 and p[2] == P["mg_stream_audio_buffer_keep"]:
                    self.au["keep"] = True
                else:
                    self.au["on"] = False
        elif cmd == CDT:
            self.rx_type = p[1]
        elif cmd == LIVE_CALL:
            self.calls.append(p[1])
            self.log("mg.ble", f"live call: {p[1]}")
        elif cmd == SUBTITLE:
            sub = p[1]
            if sub == P["mg_subtitle_request_capabilities"]:
                self.notify(P["MG_CONTROL_UUID"], bytes([SUBTITLE, P["mg_subtitle_capabilities"], 1, 0x14, 2, 20])
                            + struct.pack("<H", 120))
            elif sub == P["mg_subtitle_state"]:
                self.subs_on = p[2] == 1
                if not self.subs_on and self.band:
                    self.band = False
                    self.log("mg.subs", "band off (state 0)")
            elif sub == P["mg_subtitle_words"] and self.subs_on and not self.band:
                self.band = True
                self.log("mg.subs", "band on (client)")

    def _data(self, d: bytes) -> None:
        if self.au["on"] and self.rx_type == LC3:
            self.au["received"] += len(d)
            self.au["buffered"] += len(d)
            return
        v = self.v
        if not v["on"]:
            return
        v["received"] += len(d)
        b = v["buf"]
        b += d
        while len(b) >= 8:
            magic, _flags, _num, n = struct.unpack_from("<BBHI", b, 0)
            if magic != P["MG_VIDEO_FRAME_MAGIC"]:
                v["dropped"] += 1        # a write went missing: the stream lost its frame boundary
                v["consumed"] += len(b)
                del b[:]
                break
            if len(b) < 8 + n:
                break
            del b[:8 + n]
            v["queue"].append(8 + n)
        if self.decode_fps is None:
            self._decode(len(v["queue"]))
        elif time.monotonic() - v["upd_t"] > 0.05:
            self._video_update()

    def _decode(self, n: int) -> None:
        v = self.v
        for _ in range(min(n, len(v["queue"]))):
            v["consumed"] += v["queue"].pop(0)
            v["shown"] += 1
        if n and time.monotonic() - v["upd_t"] > 0.02:
            self._video_update()

    async def _decoder(self) -> None:
        """A decoder slower than the stream (decode_fps): its ring fills and its credits hold the sender."""
        while True:
            await asyncio.sleep(1.0 / self.decode_fps if self.decode_fps else 0.5)
            if self.decode_fps and self.v["on"]:
                self._decode(1)

    async def _player(self) -> None:
        """20 ms chunks of the reply audio, as mg_play does: a 100 ms prebuffer, underruns, a kept stream played out."""
        while True:
            await asyncio.sleep(0.02)
            a = self.au
            if not a["on"]:
                continue
            chunk = a["frame_bytes"] * 2
            a["clock"] += 20
            if not a["playing"]:
                if a["buffered"] >= a["frame_bytes"] * 10:
                    a["playing"] = True
                continue
            if a["buffered"] >= chunk:
                a["buffered"] -= chunk
                if a["received"] % 400 == 0 or a["buffered"] < 4096:
                    self._audio_update()
            elif a["keep"]:
                a["on"] = False
                self._play_line(end=True)
                self.notify(P["MG_CONTROL_UUID"], bytes([SA, P["mg_stream_audio_stop_streaming"],
                                                         P["mg_stream_audio_buffer_keep"]]))
            else:
                a["underruns"] += 1
                a["last"] = a["clock"] - 20
                a["playing"] = False

    def _play_line(self, end: bool = False) -> None:
        a = self.au
        self.log("mg.play", f"av play{' end' if end else ''}: t={a['clock']} span=1000 underruns={a['underruns']} "
                            f"new={a['underruns'] - self.play_prev} last={a['last']} buf={a['buffered'] // 4} "
                            f"int=60000 low=52000")
        self.play_prev = a["underruns"]

    async def _ticks(self) -> None:
        last_shown = last_drop = 0
        t_last = None
        while True:
            await asyncio.sleep(1.0)
            if self.v["on"]:
                now = time.monotonic()
                t_last = t_last or self.v_t0
                shown, drop = self.v["shown"] - last_shown, self.v["dropped"] - last_drop
                last_shown, last_drop = self.v["shown"], self.v["dropped"]
                self.log("muse_video", f"av video: t={int((now - self.v_t0) * 1000)} span={int((now - t_last) * 1000)} "
                                       f"rx={shown + drop} shown={shown} drop={drop} skip=0 decerr=0 "
                                       f"fps={self.sug[2]} int=60000 low=52000")
                t_last = now
            else:
                t_last = None
            if self.au["on"]:
                self._play_line()


def dry_connect(fake: FakeGadget):
    def connect(addr, runner):
        def gone():
            if runner.g is not None:
                runner.g.disconnected = True
        fake.on_gone = gone
        return fake
    return connect


def dry_scan(fake: FakeGadget):
    async def scan(args):
        return "fake-address", fake.name
    return scan


def summary_json(result: dict) -> dict:
    """The JSON summary: everything but the per-second rows' bulk (kept as compact lists)."""
    out = {k: v for k, v in result.items() if k != "windows"}
    out["seconds"] = [{"t": round(w["t"] / 1000, 1), "fps1": round(w["fps1"], 1),
                       "fps5": None if w["fps5"] is None else round(w["fps5"], 1), "shown": w["shown"],
                       "drop": w["drop"], "decerr": w["decerr"], "low": w["low"], "reply": w["reply"],
                       "sent": w.get("sent"), "sent_kb": w.get("sent_kb")}
                      for w in result.get("windows", [])]
    return out


EXIT = {"PASS": 0, "FAIL": 1, "INCOMPLETE": 2, "INVALID": 3}


def run_live(args, sink: Sink, out=print, fake: FakeGadget | None = None, console_path: str | None = None) -> dict:
    """One run (on the gadget, or with `fake` in-process), the console reader beside it."""
    console = Console(console_path, sink) if console_path else None
    if console:
        console.start()
    try:
        runner = Runner(args, sink, out, connect=dry_connect(fake) if fake else ble_connect,
                        scan=dry_scan(fake) if fake else None)
        async def bounded():
            try:
                return await asyncio.wait_for(runner.go(), args.secs + 900)
            except asyncio.TimeoutError:
                return {"verdict": "INCOMPLETE", "phase": runner.phase,
                        "error": f"{runner.phase}: the whole run passed {args.secs + 900:.0f} s"}
        result = asyncio.run(bounded())
        if console:
            result["console"] = {"opened": console.opened, "lines": console.lines}
        return result
    finally:
        if console:
            console.stop.set()
            console.join(2.0)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", help="advertised name (or its start), e.g. MuseGadget-M5SW-ABCDEF")
    ap.add_argument("--address", help="BLE address (macOS: the CoreBluetooth UUID)")
    ap.add_argument("--scan-secs", type=float, default=6.0)
    ap.add_argument("--secs", type=float, default=60.0, help="the call's video, in seconds (>= 30; >= 6 with --dry)")
    ap.add_argument("--size", type=lambda s: tuple(int(v) for v in s.lower().split("x")), metavar="WxH",
                    help="stream size (default: the board's suggestion)")
    ap.add_argument("--fps", type=int, help="stream fps (default: the board's suggestion)")
    ap.add_argument("--kbps", type=int, help="stream kb/s (default: the board's suggestion)")
    ap.add_argument("--tiles", type=int, choices=(1, 2), help="bands (default: two on the ESP-Mosaico)")
    ap.add_argument("--clip", help="the video: Annex-B .264 at the stream's size, a cached clip's .au, "
                                   "or any video file to transcode (default: tests/vectors/h264's testsrc2_208 and ui_clock_466)")
    ap.add_argument("--gop", type=float, default=2.0, help="seconds between keyframes in the generated clip")
    ap.add_argument("--no-audio", action="store_true",
                    help="no reply audio at all (no stream_audio): video and the muted reply's subtitles only, for a "
                         "run that must stay silent; the replies rule then needs no spoken reply (the run isn't a "
                         "full AV60)")
    ap.add_argument("--no-subtitles", action="store_true",
                    help="no muted reply (subtitles): for a gadget that doesn't list subtitles (not a full AV60)")
    ap.add_argument("--max-frame", type=int, metavar="BYTES",
                    help="cap the generated H.264 clip's frames at BYTES (default: the board's, none today)")
    ap.add_argument("--frame-bytes", type=int, default=40, help="LC3 frame bytes (40 = 32 kb/s, the app's)")
    ap.add_argument("--level", type=float, default=-18.0, help="reply audio level in dBFS (moderate)")
    ap.add_argument("--lead", type=float, default=1.0, help="how far ahead of playback the reply audio is sent (s)")
    ap.add_argument("--no-anchor", action="store_true",
                    help="send no A/V anchor with replies even when the gadget lists video_sync (lip sync A/B)")
    ap.add_argument("--volume", type=int, choices=range(0, 101), metavar="0-100",
                    help="the gadget's speaker_volume for the run (0: silent; playback still runs); RAM only "
                         "through the gadget's vol= word, put back at the end")
    ap.add_argument("--wait-call", type=float, default=0.0, metavar="SECS",
                    help="wait up to SECS for the gadget to start the call, then answer it (default: start it)")
    ap.add_argument("--console", metavar="PORT", help="also read the gadget's USB console (read-only, 115200)")
    ap.add_argument("--log", help="the jsonl, written as the run goes (default av60_<time>.jsonl)")
    ap.add_argument("--json", help="also write the JSON summary here")
    ap.add_argument("--evaluate", metavar="LOG", help="evaluate a saved log (whole or cut short) instead of running")
    ap.add_argument("--dry", action="store_true",
                    help="run the whole flow against a gadget in this process, its console on a pty (no Bluetooth)")
    return ap


def main(argv: list[str] | None = None) -> int:
    try:
        sys.stdout.reconfigure(line_buffering=True)   # progress shows as it happens, piped or not
    except (AttributeError, ValueError):
        pass
    ap = build_parser()
    args = ap.parse_args(argv)

    if args.evaluate:
        meta, lines, sender = load_log(Path(args.evaluate))
        replies = [Reply(r["kind"], r["start"], r["end"]) for r in meta.get("replies", [])]
        result = evaluate(lines, meta.get("fps") or args.fps or 30, replies, meta.get("video_end"),
                          sender=sender or None)
    else:
        if args.secs < (6 if args.dry else 30):
            ap.error("--secs: at least 30 (three replies and their gaps)")
        log = Path(args.log or f"av60_{time.strftime('%Y%m%d-%H%M%S')}.jsonl")
        sink = Sink(log)
        sink.meta(args={k: v for k, v in vars(args).items() if k not in ("evaluate",)}, started=time.time())
        print(f"log: {log} (written as the run goes)")
        fake, console_path, pty_fds = None, args.console, ()
        if args.dry:
            master, slave = os.openpty()
            pty_fds = (master, slave)
            console_path = console_path or os.ttyname(slave)
            fake = FakeGadget(console_fd=master)
        result: dict = {"verdict": "INCOMPLETE", "error": "interrupted"}
        try:
            result = run_live(args, sink, fake=fake, console_path=console_path)
        finally:
            # The end of the log: what --evaluate needs (a killed run leaves the lines without this).
            sink.meta(replies=[{"kind": r["kind"], "start": r["start"], "end": r["end"]}
                               for r in result.get("replies", [])],
                      video_end=result.get("video_end"), verdict=result.get("verdict"))
            sink.close()
            for fd in pty_fds:
                os.close(fd)
    # A saved log (--evaluate) is a file anyone may have written: what it names is escaped like a live line.
    if "windows" in result:
        print(printable(table(result)))
    if result.get("error"):
        print(printable(f"AV60  {result['verdict']}: {result['error']}"))
    text = json.dumps(summary_json(result), indent=1, default=str)
    print(text)
    if args.json:
        Path(args.json).write_text(text)
    return EXIT.get(result.get("verdict"), 2)


if __name__ == "__main__":
    sys.exit(main())
