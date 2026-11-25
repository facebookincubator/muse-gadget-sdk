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
"""Regenerate xplat/liblc3's src/tables.c and src/ltpf.c from upstream for
the configurations the SDKs build (10 ms frames at the given sample rates):
every table entry another configuration uses becomes NULL, so the linker
drops those tables. lc3.c's LC3_DT_MASK / LC3_SR_MASK must match.

    python3 tools/trim_tables.py UPSTREAM_SRC_DIR --rates 16k,24k

writes src/tables.c, src/ltpf.c and src/tables_mask.h here. tables_mask.h
holds the masks of the configurations the tables carry, lc3.c's defaults, and
a _Static_assert that stops a build whose LC3_DT_MASK / LC3_SR_MASK lets in
another (its tables are NULL). The FFT and MDCT tables a frame
size needs: ns = rate * 10 ms, the MDCT of 2*ns (mdct_rot_<2*ns>) and the
FFT of ns/2 = 5 * 3^n3 * 2^n2 points (twiddles 5*3^n3*2^k for k in 1..n2,
and 15 or 45 for the radix-3 stages).
"""

import argparse
import re
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
ALL_RATES = ["8k", "16k", "24k", "32k", "48k"]   # 10 ms frames; no high-resolution modes
SRATE_BIT = {"8k": 0, "16k": 1, "24k": 2, "32k": 3, "48k": 4}   # enum lc3_srate
DT_10M_BIT = 3                                                   # enum lc3_dt: LC3_DT_10M
SRATE_ENUM = {"8K": "8k", "16K": "16k", "24K": "24k", "32K": "32k", "48K": "48k",
              "48K_HR": "48k_hr", "96K_HR": "96k_hr"}

NOTE = (" *  Modified by Meta Platforms, Inc.: generated for {cfg} only:\n"
        " *  tables of other configurations are replaced by NULL\n"
        " *  (tools/trim_tables.py). See README.md in this directory.\n")


def needed_sizes(rates):
    fft, rot = set(), set()
    for r in rates:
        ns = int(r.rstrip("k")) * 10   # samples per 10 ms frame
        rot.add(2 * ns)
        n = ns // 2
        assert n % 5 == 0
        m, n3 = n // 5, 0
        while m % 3 == 0:
            m //= 3
            n3 += 1
        if n3:
            fft.add(15 if n3 == 1 else 45)
            fft.update({15, 45} if n3 == 2 else set())
        base = 5 * 3 ** n3
        k = 1
        while base * 2 ** k <= n:
            fft.add(base * 2 ** k)
            k += 1
    return fft, rot


def keep_name(name, rates, fft, rot):
    m = re.fullmatch(r"fft_twiddles_(\d+)", name)
    if m:
        return int(m.group(1)) in fft
    m = re.fullmatch(r"mdct_rot_(\d+)", name)
    if m:
        return int(m.group(1)) in rot
    m = re.fullmatch(r"(?:mdct_win|band_lim)_(2m5|5m|7m5|10m)_(\w+)", name)
    if m:
        return m.group(1) == "10m" and m.group(2) in rates
    m = re.fullmatch(r"resample_(\w+?)_12k8", name)
    if m:
        return m.group(1) in rates
    return True


TABLE_TOKEN = re.compile(r"&?\b((?:fft_twiddles|mdct_rot)_\d+|(?:mdct_win|band_lim)_(?:2m5|5m|7m5|10m)_\w+?(?:_hr)?|"
                         r"resample_(?:8k|16k|24k|32k|48k|96k)_12k8)\b(?!\s*[\[(])")
DEFINITION = re.compile(r"^\s*(?:LC3_HOT\s+)?(?:static\s+)?(?:const\s+|inline\s+)*(?:float|int|struct|void|int16_t)\b")


def trim(text, rates, fft, rot):
    out = []
    lines = text.split("\n")
    i = 0
    dt_row = None   # inside a [LC3_DT_X] = { ... } row of a per-configuration table
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^\s*\[LC3_DT_(\w+?)\s*\]\s*=\s*\{", line)
        if m:
            dt_row = m.group(1)
        elif dt_row and re.match(r"^\s*\},?\s*$", line):
            dt_row = None
        # A [LC3_SRATE_X] = { ... } block of compound literals (the LTPF
        # filters): NULLs for a rate we don't build.
        m = re.match(r"^(\s*)\[LC3_SRATE_(\w+?)\s*\]\s*=\s*\{\s*$", line)
        if m and SRATE_ENUM.get(m.group(2)) not in rates:
            depth, j = 0, i
            body = []
            while True:
                depth += lines[j].count("{") - lines[j].count("}")
                body.append(lines[j])
                if depth == 0:
                    break
                j += 1
            count = "\n".join(body).count("(const ")
            if count:
                ind = m.group(1)
                out.append(line)
                out.append(",\n".join(ind + "    NULL" for _ in range(count)))
                out.append(ind + "},")
                i = j + 1
                continue
        # [LC3_SRATE_X] = resample_x_12k8, (ltpf.c's function table)
        m = re.match(r"^(\s*\[LC3_SRATE_(\w+?)\s*\]\s*=\s*)(resample_\w+_12k8)(,?)\s*$", line)
        if m and SRATE_ENUM.get(m.group(2)) not in rates:
            out.append(m.group(1) + "0" + m.group(4))
            i += 1
            continue
        # Preprocessor lines and sizeof-style macros (__LC3_NUM_BANDS) link nothing.
        preproc = line.lstrip().startswith("#") or "defined(" in line or "__LC3_" in line
        if not DEFINITION.match(line) and not preproc:
            other_dt = dt_row is not None and dt_row != "10M"
            line = TABLE_TOKEN.sub(lambda t: t.group(0) if not other_dt and keep_name(t.group(1), rates, fft, rot) else "NULL", line)
            while True:   # nested LC3_IF_PLUS(LC3_IF_PLUS_HR(NULL, NULL), NULL)
                new = re.sub(r"LC3_IF_PLUS(?:_HR)?\(\s*NULL\s*,\s*NULL\s*\)", "NULL", line)
                if new == line:
                    break
                line = new
        out.append(line)
        i += 1
    return "\n".join(out)


def add_note(text, cfg):
    # After the upstream license block's first " *" paragraph end.
    k = text.index(" */")
    return text[:k] + " *\n" + NOTE.format(cfg=cfg) + text[k:]


MASK_H = """\
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Generated by tools/trim_tables.py --rates {rates}.
 * tables.c and ltpf.c carry {cfg} only: the tables of every other
 * configuration are NULL. lc3.c rejects the others at setup, so a build whose
 * LC3_DT_MASK or LC3_SR_MASK lets one in would read a NULL table: it stops
 * here instead. Regenerate the three files for other rates. */

#ifndef __LC3_TABLES_MASK_H
#define __LC3_TABLES_MASK_H

#define LC3_TABLES_DT_MASK 0x{dt:x}
#define LC3_TABLES_SR_MASK 0x{sr:x}

#ifndef LC3_DT_MASK
#define LC3_DT_MASK LC3_TABLES_DT_MASK
#endif
#ifndef LC3_SR_MASK
#define LC3_SR_MASK LC3_TABLES_SR_MASK
#endif

_Static_assert(((LC3_DT_MASK) & ~LC3_TABLES_DT_MASK) == 0,
    "LC3_DT_MASK lets in a frame duration the trimmed tables don't carry: run tools/trim_tables.py");
_Static_assert(((LC3_SR_MASK) & ~LC3_TABLES_SR_MASK) == 0,
    "LC3_SR_MASK lets in a sample rate the trimmed tables don't carry: run tools/trim_tables.py");

#endif /* __LC3_TABLES_MASK_H */
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("upstream_src")
    ap.add_argument("--rates", default="16k,24k")
    ap.add_argument("--out", default=str(HERE / "src"))
    a = ap.parse_args()
    rates = list(dict.fromkeys(r.strip() for r in a.rates.split(",")))   # a repeated rate counts once
    assert all(r in ALL_RATES for r in rates), rates
    fft, rot = needed_sizes(rates)
    cfg = " / ".join(r.replace("k", " kHz") for r in rates) + ", 10 ms"
    for name in ("tables.c", "ltpf.c"):
        src = (Path(a.upstream_src) / name).read_text()
        (Path(a.out) / name).write_text(add_note(trim(src, rates, fft, rot), cfg))
    sr = 0
    for r in rates:
        sr |= 1 << SRATE_BIT[r]
    (Path(a.out) / "tables_mask.h").write_text(MASK_H.format(rates=",".join(rates), cfg=cfg, dt=1 << DT_10M_BIT, sr=sr))
    print(f"rates {rates}: fft twiddles {sorted(fft)}, mdct_rot {sorted(rot)}")


if __name__ == "__main__":
    main()
