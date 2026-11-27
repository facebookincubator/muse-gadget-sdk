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
"""Bit-exact checks for xplat/tinyh264: decode a stream corpus to one MD5 per
picture (tests/h264_tool.c) and compare decoders, builds or options.

    python3 xplat/tests/h264_corpus.py gen OUT_DIR
        a corpus made with ffmpeg + libx264 (needs both): synthetic sources,
        96 to 480 px, intra-only and I/P, 1 to 4 references, deblocking on,
        off and offset, one and several slices, low and high QP, partitions
        down to 4x4, plus damaged copies. Deterministic for one ffmpeg build
        (one thread). The streams are not checked in.
    python3 xplat/tests/h264_corpus.py hash -o OUT.txt [--src DIR] [--cflags="..."]
            [--sanitize] [--crop] STREAM_OR_DIR...
        builds the tool with the decoder in DIR (default xplat/tinyh264/src)
        and the given flags, decodes every .264 and .au (directories
        recursively), and writes one line per picture or error.
    python3 xplat/tests/h264_corpus.py ffmpeg -o OUT.txt STREAM_OR_DIR...
        the same lines from ffmpeg's framemd5, for comparing with
        `hash --crop` on undamaged streams.
    python3 xplat/tests/h264_corpus.py compare A.txt B.txt
        streams, pictures and errors in each, and every stream that differs;
        exit 1 if any does, if either run has no streams, or if a stream in
        either did not finish or failed (no `end` line, a nonzero exit or a
        timeout, even after `end`).

A reference is a `hash` of the decoder before a change (`git archive` its
src/ into a directory and pass --src); a bit-exact change gives the same file.
CC picks the compiler.
"""

import argparse
import os
import random
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
XPLAT = HERE.parent
TIMEOUT = 120   # per stream; the largest decode in well under a second


def streams(paths):
    out = []
    for p in map(Path, paths):
        if p.is_dir():
            out += sorted(q for q in p.rglob("*") if q.suffix in (".264", ".au") and q.is_file())
        else:
            out.append(p)
    return out


def name_of(p, roots):
    for r in map(Path, roots):
        if r.is_dir() and r in p.parents:
            return p.relative_to(r).as_posix()
    return p.name


def build(src, cflags, sanitize, tmp):
    cc = shlex.split(os.environ.get("CC", "cc"))
    exe = Path(tmp) / "h264_tool"
    # The decoder shifts negative values as upstream ships it (as esp32/tests).
    san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-sanitize=shift-base", "-g"] \
        if sanitize else []
    srcs = sorted(str(p) for p in Path(src).glob("*.c"))
    cmd = [*cc, "-std=gnu99", "-O2", "-w", *san, *shlex.split(cflags), f"-I{src}",
           "-o", str(exe), str(HERE / "h264_tool.c"), *srcs, "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"build failed: {' '.join(cmd)}\n{r.stderr}")
    return exe


def cmd_hash(a):
    files = streams(a.inputs)
    if not files:
        sys.exit(f"no .264 or .au streams in {' '.join(a.inputs)}")
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(a.src, a.cflags, a.sanitize, tmp)
        lines, pics, errs, bad = [], 0, 0, 0
        for p in files:
            n = name_of(p, a.inputs)
            try:
                r = subprocess.run([str(exe), *(["--crop"] if a.crop else []), str(p)],
                                   capture_output=True, text=True, timeout=TIMEOUT)
                body = r.stdout.splitlines()
                if r.returncode:
                    body.append(f"exit {r.returncode}")
                    bad += 1
                    sys.stderr.write(f"{n}: exit {r.returncode}\n{r.stderr[-2000:]}")
            except subprocess.TimeoutExpired:
                body = ["STUCK"]
                bad += 1
            pics += sum(1 for l in body if l.startswith("f "))
            errs += sum(1 for l in body if l.startswith("e "))
            lines += [f"{n} {l}" for l in body]
    Path(a.out).write_text("\n".join(lines) + "\n")
    print(f"{len(files)} streams, {pics} pictures, {errs} errors, {bad} failed runs -> {a.out}")
    return 1 if bad else 0


def cmd_ffmpeg(a):
    files = streams(a.inputs)
    if not files:
        sys.exit(f"no .264 or .au streams in {' '.join(a.inputs)}")
    lines, pics, bad = [], 0, 0
    for p in files:
        n = name_of(p, a.inputs)
        try:
            r = subprocess.run(["ffmpeg", "-v", "error", "-threads", "1", "-i", str(p), "-f", "framemd5",
                                "-pix_fmt", "yuv420p", "-"], capture_output=True, text=True, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            lines.append(f"{n} STUCK")
            bad += 1
            continue
        k, dims = 0, "?"
        for l in r.stdout.splitlines():
            if l.startswith("#") or not l.strip():
                if l.startswith("#dimensions"):
                    dims = l.split(":")[1].strip().replace(" ", "")
                continue
            lines.append(f"{n} f {k} {dims} {l.split(',')[-1].strip()}")
            k += 1
        pics += k
        lines.append(f"{n} end {k}" if r.returncode == 0 else f"{n} exit {r.returncode}")
        if r.returncode:
            bad += 1
            sys.stderr.write(f"{n}: ffmpeg exit {r.returncode}\n{r.stderr[-2000:]}")
    Path(a.out).write_text("\n".join(lines) + "\n")
    print(f"{len(files)} streams, {pics} pictures, {bad} failed runs -> {a.out}")
    return 1 if bad else 0


# "<stream> <record>": the record is matched from the end, so a stream name
# may hold spaces
RECORD = re.compile(r"(.+) (f \d+ \S+ [0-9a-f]+|e \d+ \d+|end(?: \d+)*|exit -?\d+|STUCK)")


def load(path):
    """Lines per stream, and the streams that did not finish (no `end` line)
    or failed (a nonzero exit or a timeout, which can follow `end`)."""
    per, done, failed = {}, set(), set()
    for l in Path(path).read_text().splitlines():
        m = RECORD.fullmatch(l)
        if not m:
            sys.exit(f"{path}: not a stream record: {l!r}")
        n, rest = m.groups()
        lines = per.setdefault(n, [])
        if rest.startswith("end"):
            done.add(n)
            continue   # access unit counts: the same input either way
        if rest.startswith(("exit ", "STUCK")):
            failed.add(n)
        lines.append(rest)
    return per, sorted((set(per) - done) | failed)


def cmd_compare(a):
    (A, failA), (B, failB) = load(a.a), load(a.b)
    def count(d, kind):
        return sum(1 for v in d.values() for l in v if l.startswith(kind + " "))
    bad = 0
    for label, d, fail in (("A", A, failA), ("B", B, failB)):
        print(f"{label}: {len(d)} streams, {count(d, 'f')} pictures, {count(d, 'e')} errors")
        if not d:
            print(f"{label} has no streams")
            bad = 1
        for n in fail:
            print(f"{label}: {n} did not finish cleanly: {(d[n] or ['-'])[-1]}")
            bad = 1
    diff = sorted(n for n in set(A) | set(B) if A.get(n) != B.get(n))
    for n in diff:
        a_, b_ = A.get(n, []), B.get(n, [])
        k = next((i for i, (x, y) in enumerate(zip(a_, b_)) if x != y), min(len(a_), len(b_)))
        print(f"DIFF {n}: {len(a_)} vs {len(b_)} lines, first at {k}: "
              f"{a_[k] if k < len(a_) else '-'} | {b_[k] if k < len(b_) else '-'}")
    same = len((set(A) & set(B)) - set(diff) - set(failA) - set(failB))
    print(f"{same} streams identical, {len(diff)} differ"
          + (f", {len(failA)} + {len(failB)} did not finish cleanly" if failA or failB else ""))
    return 1 if diff or bad else 0


# ---- the corpus ---------------------------------------------------------------

SOURCES = {
    # name: lavfi graph at WxH (moving content of several kinds, a still one)
    "testsrc2": "testsrc2=size={w}x{h}:rate=30",
    "mandel": "mandelbrot=size={w}x{h}:rate=30:start_scale=3:end_scale=0.3",
    "life": "life=size={w}x{h}:rate=30:mold=10:ratio=0.2:death_color=#203040:life_color=#e0c080",
    "pan": "testsrc2=size=640x640:rate=30,scroll=h=0.0021:v=0.0013,crop={w}:{h},hue=H=t",
    "noise": "color=c=gray:size={w}x{h}:rate=30,noise=alls=60:allf=t+u",
    "still": "smptehdbars=size={w}x{h}:rate=30[a];testsrc2=size=32x32:rate=30[b];[a][b]overlay=x=16:y=16",
}
SIZES = [(96, 96), (128, 96), (176, 144), (208, 208), (232, 232), (240, 240), (320, 240), (360, 360),
         (466, 466), (480, 272), (480, 480)]
# x264 options: (tag, params, extra ffmpeg args)
PROFILES = [
    ("i1", "keyint=1:ref=1", ["-qp", "26"]),
    ("i1q4", "keyint=1:ref=1", ["-qp", "4"]),
    ("p1", "keyint=30:ref=1", ["-crf", "26"]),
    ("p2nodb", "keyint=60:ref=2:no-deblock=1", ["-crf", "30"]),
    ("p3db", "keyint=20:ref=3:deblock=-2,2:partitions=all:subme=9:me=umh", ["-crf", "22"]),
    ("p4", "keyint=60:ref=4:partitions=all:no-fast-pskip=1", ["-crf", "34"]),
    ("p4sl4", "keyint=45:ref=4:slices=4:deblock=3,-3", ["-crf", "28"]),
    ("p1hi", "keyint=15:ref=1:partitions=all:aud=1", ["-qp", "12"]),
    ("p2lo", "keyint=90:ref=2:deblock=0,0", ["-qp", "45"]),
]
FRAMES = 24


def cmd_gen(a):
    if not shutil.which("ffmpeg"):
        sys.exit("needs ffmpeg with libx264")
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    made = []
    for si, (w, h) in enumerate(SIZES):
        for pi, (tag, params, extra) in enumerate(PROFILES):
            # every source at each size, but not every profile on every source
            for ki, (src, graph) in enumerate(SOURCES.items()):
                if (si + pi + ki) % 3 and not a.full:
                    continue
                f = out / f"{src}_{w}x{h}_{tag}.264"
                cmd = ["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", graph.format(w=w, h=h),
                       "-frames:v", str(FRAMES), "-threads", "1", "-filter_threads", "1",
                       "-pix_fmt", "yuv420p", "-c:v", "libx264", "-profile:v", "baseline", "-coder", "0",
                       "-x264-params", f"{params}:threads=1:sliced-threads=0:scenecut=0", *extra,
                       "-bsf:v", "h264_mp4toannexb", "-f", "h264", str(f)]
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
                if r.returncode:
                    sys.exit(f"{f.name}: {r.stderr[-1500:]}")
                made.append(f)
    # Damaged copies: bytes flipped inside NAL units (start codes left alone),
    # a cut access unit, a dropped NAL unit, a run of noise.
    rnd = random.Random(1)
    bad = out / "damaged"
    bad.mkdir(exist_ok=True)
    ok = [f for f in made if "_i1q4" not in f.name and "noise_" not in f.name]
    picks = sorted(rnd.sample(ok, min(40, len(ok))))
    for i, f in enumerate(picks):
        d = bytearray(f.read_bytes())
        starts = [j for j in range(len(d) - 3) if d[j:j + 3] == b"\0\0\1"]
        kind = i % 4
        if kind == 0:
            for _ in range(1 + len(d) // 4000):
                j = rnd.randrange(64, len(d))
                if b"\0\0" not in d[j - 2:j + 2]:
                    d[j] ^= 1 << rnd.randrange(8)
        elif kind == 1:
            j = rnd.choice(starts[len(starts) // 3:])
            k = starts[starts.index(j) + 1] if starts.index(j) + 1 < len(starts) else len(d)
            del d[j + 4 + (k - j - 4) // 3:k]
        elif kind == 2:
            j = rnd.choice(starts[3:])
            k = starts[starts.index(j) + 1] if starts.index(j) + 1 < len(starts) else len(d)
            del d[j:k]
        else:
            j = rnd.randrange(len(d) // 3, len(d) - 40)
            d[j:j + 32] = bytes(rnd.randrange(1, 256) for _ in range(32))
        (bad / f"{f.stem}_bad{kind}.264").write_bytes(bytes(d))
    total = len(list(out.rglob("*.264")))
    print(f"{len(made)} streams + {len(picks)} damaged -> {out} ({total} files)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen")
    g.add_argument("out")
    g.add_argument("--full", action="store_true", help="every source with every profile")
    h = sub.add_parser("hash")
    h.add_argument("-o", "--out", required=True)
    h.add_argument("--src", default=str(XPLAT / "tinyh264/src"))
    h.add_argument("--cflags", default="")
    h.add_argument("--sanitize", action="store_true")
    h.add_argument("--crop", action="store_true")
    h.add_argument("inputs", nargs="+")
    f = sub.add_parser("ffmpeg")
    f.add_argument("-o", "--out", required=True)
    f.add_argument("inputs", nargs="+")
    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    a = ap.parse_args()
    return {"gen": cmd_gen, "hash": cmd_hash, "ffmpeg": cmd_ffmpeg, "compare": cmd_compare}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
