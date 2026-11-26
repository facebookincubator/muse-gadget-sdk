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
"""Regenerates the v1.0 UI's fonts and icon masks.

The Muse Gadgets reference screens size every text by its capitals (the time
by its digits) as a share of the panel's short side S. LVGL's built-in Montserrat comes in fixed
sizes, so each panel size gets its own set, converted from Montserrat (Medium,
as LVGL's built-ins; SemiBold for titles) at the size whose glyphs measure what
the table below says:

    S    time  date  title  conn  row  label  value  small  digit  (capital height, px)
    466   46    28    17     20    15   17     13     19     32    StopWatch
    480   47    29    20     21    15   20     15     20     28    ESP-Mosaico
    412   41    25    16     18    13   16     14     17     29    SenseCAP Watcher
    368   36    22    19     16    12   19     15     15     27    Waveshare C6 1.8

(small: the not-set-up home's "Open Muse app"; title, label and value: the
menu grown to its minimum physical sizes, the titles, Status's too, at the
labels' size; digit: the passcode keypad's digits, digits only)

-> components/muse/fonts/type/muse_font_<role>_<S>.c (muse_type.c picks the
set; a board compiles only its own, components/muse/CMakeLists.txt).

Icons: components/muse/muse_icons_gen.h, the standard Material Icons glyphs
(ICON_PATHS below) as 8-bit alpha masks at the sizes the UI draws (buttons, menu rows, the
StopWatch's rim hints), plus the top row's Wi-Fi, Bluetooth and muted
speaker cropped to their ink at the battery's height.

    python3 tools/muse/gen_ui_assets.py [--fonts] [--icons]

Needs Pillow and ImageMagick (`magick`, the icons' SVG rasterizer), npx (Node.js)
for lv_font_conv, LVGL's Montserrat-Medium.ttf (LVGL_DIR, else the component
manager's copy) and Montserrat-SemiBold.ttf (MONTSERRAT_DIR, else
/Library/Fonts or ~/Library/Fonts). Montserrat is under the SIL Open Font
License 1.1.
"""
import argparse
import glob
import io
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ESP32 = os.path.dirname(os.path.dirname(HERE))
MUSE = os.path.join(ESP32, "components", "muse")
FONT_OUT = os.path.join(MUSE, "fonts", "type")

# role -> caps per S (home time digits/date caps, status conn/row,
# menu title/label/value)
SETS = {
    466: {"time": 46, "date": 28, "title": 17, "conn": 20, "row": 15, "label": 17, "value": 13, "small": 19, "digit": 32},
    480: {"time": 47, "date": 29, "title": 20, "conn": 21, "row": 15, "label": 20, "value": 15, "small": 20, "digit": 28},
    412: {"time": 41, "date": 25, "title": 16, "conn": 18, "row": 13, "label": 16, "value": 14, "small": 17, "digit": 29},
    368: {"time": 36, "date": 22, "title": 19, "conn": 16, "row": 12, "label": 19, "value": 15, "small": 15, "digit": 27},
}
TEXT_RANGE = ["0x20-0x7E", "0xB0", "0xB7", "0x2014", "0x2019", "0x2026"]
RANGES = {
    "time": ["0x2D", "0x30-0x3A"],          # digits, ':' and '-'
    "title": ["0x20-0x7E", "0x2026"],
    "digit": ["0x30-0x39"],                  # the passcode keypad's digits
}
SEMIBOLD = {"title"}
MEASURE = {"time": 0x30, "digit": 0x30}     # '0'; capitals ('H') otherwise


def lvgl_ttf():
    d = os.environ.get("LVGL_DIR")
    cands = [d] if d else []
    cands += glob.glob(os.path.join(ESP32, "managed_components", "lvgl__lvgl"))
    cands += glob.glob(os.path.expanduser("~/Library/Caches/Espressif/ComponentManager/*/lvgl__lvgl_9.5.0_*"))
    cands += glob.glob(os.path.expanduser("~/.cache/Espressif/ComponentManager/*/lvgl__lvgl_9.5.0_*"))
    for c in cands:
        p = os.path.join(c, "scripts", "built_in_font", "Montserrat-Medium.ttf")
        if os.path.isfile(p):
            return p
    sys.exit("no Montserrat-Medium.ttf in LVGL (set LVGL_DIR)")


def semibold_ttf():
    for d in [os.environ.get("MONTSERRAT_DIR", ""), "/Library/Fonts", os.path.expanduser("~/Library/Fonts"),
              "/usr/share/fonts/truetype/montserrat"]:
        p = os.path.join(d, "Montserrat-SemiBold.ttf")
        if d and os.path.isfile(p):
            return p
    sys.exit("no Montserrat-SemiBold.ttf (set MONTSERRAT_DIR)")


def conv(ttf, size, ranges, name, out):
    cmd = ["npx", "-y", "lv_font_conv@1.5.3", "--font", ttf, "--size", str(size), "--bpp", "4",
           "--no-compress", "--no-prefilter", "--force-fast-kern-format", "--format", "lvgl",
           "--lv-font-name", name, "-o", out]
    for r in ranges:
        cmd += ["-r", r]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def codepoints(ranges):
    cps = set()
    for r in ranges:
        lo, _, hi = r.partition("-")
        cps.update(range(int(lo, 16), int(hi or lo, 16) + 1))
    return sorted(cps)


def box_h(path, code, ranges):
    """The converted glyph's bitmap height (its ink, as LVGL draws it): glyph
    ids follow the code points in order, after the reserved id 0."""
    gid = codepoints(ranges).index(code) + 1
    rows = re.findall(r"\{\.bitmap_index = \d+, \.adv_w = \d+, \.box_w = \d+, \.box_h = (\d+),", open(path).read())
    return int(rows[gid])


def fit(ttf, target, code, tmp):
    """The pixel size whose glyph `code` is `target` px tall."""
    size = max(6, round(target / 0.7))
    seen = {}
    for _ in range(12):
        if size not in seen:
            out = os.path.join(tmp, "probe.c")
            conv(ttf, size, ["0x%X" % code], "probe", out)
            seen[size] = box_h(out, code, ["0x%X" % code])
        h = seen[size]
        if h == target:
            return size
        nxt = size + (1 if h < target else -1)
        if nxt in seen and (seen[nxt] > target) != (h > target):
            # between two sizes (bitmap heights step by 2): the closer one, the
            # larger on a tie
            return size if abs(h - target) < abs(seen[nxt] - target) or (abs(h - target) == abs(seen[nxt] - target) and size > nxt) else nxt
        size = nxt
    return size


def probe_h(ttf, size, code, tmp):
    out = os.path.join(tmp, "probe.c")
    conv(ttf, size, ["0x%X" % code], "probe", out)
    return box_h(out, code, ["0x%X" % code])


def date_size(ttf, size, time_px, tmp):
    """Bitmap heights step by 2, so the date can miss its capitals by one: of
    the neighbors, the one whose time : date ratio is inside 1.6-2 and nearest the reference home's 1.64."""
    best = None
    for s_ in (size - 1, size, size + 1):
        r = time_px / float(probe_h(ttf, s_, 0x48, tmp))
        score = abs(r - 1.64) + (0 if 1.6 <= r <= 2.0 else 10)
        if best is None or score < best[0]:
            best = (score, s_)
    return best[1]


def gen_fonts():
    medium, semibold = lvgl_ttf(), semibold_ttf()
    time_px = {}
    os.makedirs(FONT_OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for S, roles in SETS.items():
            for role, caps in roles.items():
                ttf = semibold if role in SEMIBOLD else medium
                code = MEASURE.get(role, 0x48)
                size = fit(ttf, caps, code, tmp)
                if role == "date":
                    size = date_size(ttf, size, time_px[S], tmp)
                name = "muse_font_%s_%d" % (role, S)
                out = os.path.join(tmp, "font.c")
                conv(ttf, size, RANGES.get(role, TEXT_RANGE), name, out)
                got = box_h(out, code, RANGES.get(role, TEXT_RANGE))
                if role == "time":
                    time_px[S] = got
                src = open(out).read()
                src = src.replace('#include "lvgl/lvgl.h"', '#include "lvgl.h"')
                src = src.replace(ttf, os.path.basename(ttf)).replace(tmp + "/", "")
                head = ("/* Muse v1.0 UI font (tools/muse/gen_ui_assets.py): Montserrat %s %d px for the\n"
                        " * %s text on %d px panels, %s %d px tall (spec %d). Montserrat: SIL Open Font\n"
                        " * License 1.1. Don't edit: regenerate. */\n"
                        % ("SemiBold" if role in SEMIBOLD else "Medium", size, role, S,
                           "digits" if role in MEASURE else "capitals", got, caps))
                with open(os.path.join(FONT_OUT, name + ".c"), "w") as f:
                    f.write(head + src)
                print("%s: size %d, %s %d px (spec %d)" % (name, size, "digits" if role in MEASURE else "caps", got, caps))


# ---------------------------------------------------------------------- icons

# Material Icons (filled, 24x24 viewBox; https://github.com/google/material-design-icons,
# Apache License 2.0): the standard glyphs every Muse Gadget draws.
ICON_PATHS = {
    "add": "M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z",
    "battery": (
        "M15.67 4H14V2h-4v2H8.33C7.6 4 7 4.6 7 5.33v15.33C7 21.4 7.6 22 8.33 22h7.33c.74 0 "
        "1.34-.6 1.34-1.33V5.33C17 4.6 16.4 4 15.67 4z"),
    "bluetooth": (
        "M17.71 7.71L12 2h-1v7.59L6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 11 "
        "14.41V22h1l5.71-5.71-4.3-4.29 4.3-4.29zM13 5.83l1.88 1.88L13 9.59V5.83zm1.88 10.46L13 "
        "18.17v-3.76l1.88 1.88z"),
    "call": (
        "M6.62 10.79c1.44 2.83 3.76 5.14 6.59 6.59l2.2-2.2c.27-.27.67-.36 1.02-.24 1.12.37 "
        "2.33.57 3.57.57.55 0 1 .45 1 1V20c0 .55-.45 1-1 1-9.39 0-17-7.61-17-17 0-.55.45-1 "
        "1-1h3.5c.55 0 1 .45 1 1 0 1.25.2 2.45.57 3.57.11.35.03.74-.25 1.02l-2.2 2.2z"),
    "call_end": (
        "M12 9c-1.6 0-3.15.25-4.6.72v3.1c0 .39-.23.74-.56.9-.98.49-1.87 1.12-2.66 "
        "1.85-.18.18-.43.28-.7.28-.28 0-.53-.11-.71-.29L.29 13.08c-.18-.17-.29-.42-.29-.7 "
        "0-.28.11-.53.29-.71C3.34 8.78 7.46 7 12 7s8.66 1.78 11.71 4.67c.18.18.29.43.29.71 0 "
        ".28-.11.53-.29.71l-2.48 2.48c-.18.18-.43.29-.71.29-.27 "
        "0-.52-.11-.7-.28-.79-.74-1.69-1.36-2.67-1.85-.33-.16-.56-.5-.56-.9v-3.1C15.15 9.25 13.6 "
        "9 12 9z"),
    "charging": (
        "M11 21h-1l1-7H7.5c-.58 0-.57-.32-.38-.66.19-.34.05-.08.07-.12C8.48 10.94 10.42 7.54 13 "
        "3h1l-1 7h3.5c.49 0 .56.33.47.51l-.07.15C12.96 17.55 11 21 11 21z"),
    "developer": (
        "M9.4 16.6L4.8 12l4.6-4.6L8 6l-6 6 6 6 1.4-1.4zm5.2 0l4.6-4.6-4.6-4.6L16 6l6 6-6 "
        "6-1.4-1.4z"),
    "display": (
        "M12 7c-2.76 0-5 2.24-5 5s2.24 5 5 5 5-2.24 5-5-2.24-5-5-5zM2 13h2c.55 0 1-.45 "
        "1-1s-.45-1-1-1H2c-.55 0-1 .45-1 1s.45 1 1 1zm18 0h2c.55 0 1-.45 1-1s-.45-1-1-1h-2c-.55 "
        "0-1 .45-1 1s.45 1 1 1zM11 2v2c0 .55.45 1 1 1s1-.45 1-1V2c0-.55-.45-1-1-1s-1 .45-1 1zm0 "
        "18v2c0 .55.45 1 1 1s1-.45 1-1v-2c0-.55-.45-1-1-1s-1 .45-1 1zM5.99 "
        "4.58c-.39-.39-1.03-.39-1.41 0-.39.39-.39 1.03 0 1.41l1.06 1.06c.39.39 1.03.39 1.41 "
        "0s.39-1.03 0-1.41L5.99 4.58zm12.37 12.37c-.39-.39-1.03-.39-1.41 0-.39.39-.39 1.03 0 "
        "1.41l1.06 1.06c.39.39 1.03.39 1.41 0 .39-.39.39-1.03 "
        "0-1.41l-1.06-1.06zm1.06-10.96c.39-.39.39-1.03 0-1.41-.39-.39-1.03-.39-1.41 0l-1.06 "
        "1.06c-.39.39-.39 1.03 0 1.41s1.03.39 1.41 0l1.06-1.06zM7.05 18.36c.39-.39.39-1.03 "
        "0-1.41-.39-.39-1.03-.39-1.41 0l-1.06 1.06c-.39.39-.39 1.03 0 1.41s1.03.39 1.41 "
        "0l1.06-1.06z"),
    "info": (
        "M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 "
        "15h-2v-6h2v6zm0-8h-2V7h2v2z"),
    "mic": (
        "M12 14c1.66 0 2.99-1.34 2.99-3L15 5c0-1.66-1.34-3-3-3S9 3.34 9 5v6c0 1.66 1.34 3 3 "
        "3zm5.3-3c0 3-2.54 5.1-5.3 5.1S6.7 14 6.7 11H5c0 3.41 2.72 6.23 6 "
        "6.72V21h2v-3.28c3.28-.48 6-3.3 6-6.72h-1.7z"),
    "power": (
        "M13 3h-2v10h2V3zm4.83 2.17l-1.42 1.42C17.99 7.86 19 9.81 19 12c0 3.87-3.13 7-7 "
        "7s-7-3.13-7-7c0-2.19 1.01-4.14 2.58-5.42L6.17 5.17C4.23 6.82 3 9.26 3 12c0 4.97 4.03 9 9 "
        "9s9-4.03 9-9c0-2.74-1.23-5.18-3.17-6.83z"),
    "remove": "M19 13H5v-2h14v2z",
    "touch": (
        "M9,11.24V7.5C9,6.12,10.12,5,11.5,5S14,6.12,14,7.5v3.74c1.21-0.81,2-2.18,2-3.74C16,5.01,13.99,3,11.5,3S7,5.01,7,7.5 "
        "C7,9.06,7.79,10.43,9,11.24z "
        "M18.84,15.87l-4.54-2.26c-0.17-0.07-0.35-0.11-0.54-0.11H13v-6C13,6.67,12.33,6,11.5,6 "
        "S10,6.67,10,7.5v10.74c-3.6-0.76-3.54-0.75-3.67-0.75c-0.31,0-0.59,0.13-0.79,0.33l-0.79,0.8l4.94,4.94 "
        "C9.96,23.83,10.34,24,10.75,24h6.79c0.75,0,1.33-0.55,1.44-1.28l0.75-5.27c0.01-0.07,0.02-0.14,0.02-0.2 "
        "C19.75,16.63,19.37,16.09,18.84,15.87z"),
    "videocam": (
        "M17 10.5V7c0-.55-.45-1-1-1H4c-.55 0-1 .45-1 1v10c0 .55.45 1 1 1h12c.55 0 1-.45 "
        "1-1v-3.5l4 4v-11l-4 4z"),
    "volume_off": (
        "M16.5 12c0-1.77-1.02-3.29-2.5-4.03v2.21l2.45 2.45c.03-.2.05-.41.05-.63zm2.5 0c0 .94-.2 "
        "1.82-.54 2.64l1.51 1.51C20.63 14.91 21 13.5 21 12c0-4.28-2.99-7.86-7-8.77v2.06c2.89.86 5 "
        "3.54 5 6.71zM4.27 3L3 4.27 7.73 9H3v6h4l5 5v-6.73l4.25 4.25c-.67.52-1.42.93-2.25 "
        "1.18v2.06c1.38-.31 2.63-.95 3.69-1.81L19.73 21 21 19.73l-9-9L4.27 3zM12 4L9.91 6.09 12 "
        "8.18V4z"),
    "volume_up": (
        "M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 "
        "2.5-4.02zM14 3.23v2.06c2.89.86 5 3.54 5 6.71s-2.11 5.85-5 6.71v2.06c4.01-.91 7-4.49 "
        "7-8.77s-2.99-7.86-7-8.77z"),
    "wifi": (
        "M1 9l2 2c4.97-4.97 13.03-4.97 18 0l2-2C16.93 2.93 7.08 2.93 1 9zm8 8l3 3 "
        "3-3c-1.65-1.66-4.34-1.66-6 0zm-4-4l2 2c2.76-2.76 7.24-2.76 10 0l2-2C15.14 9.14 8.87 9.14 "
        "5 13z"),
}
HOME_PATH = "M10 20v-6h4v6h5v-8h3L12 3 2 12h3v8z"   # Material `home` (the Muse row)
# Material `lock` and `backspace`, for the passcode lock
EXTRA_PATHS = {
    "lock": ("M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 "
             "2-.9 2-2V10c0-1.1-.9-2-2-2zm-6 9c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2zm3.1-9H8.9V6"
             "c0-1.71 1.39-3.1 3.1-3.1 1.71 0 3.1 1.39 3.1 3.1v2z"),
    "backspace": ("M22 3H7c-.69 0-1.23.35-1.59.88L0 12l5.41 8.11c.36.53.9.89 1.59.89h15c1.1 0 2-.9 "
                  "2-2V5c0-1.1-.9-2-2-2zm-3 12.59L17.59 17 14 13.41 10.41 17 9 15.59 12.59 12 9 8.41 "
                  "10.41 7 14 10.59 17.59 7 19 8.41 15.41 12 19 15.59z"),
}
SQUARE = [   # (px, names)
    (36, ["call", "call_end", "volume_up", "volume_off"]),   # 52 % of a 72 px button
    (42, ["call", "call_end", "volume_up", "volume_off"]),   # 81 px
    (48, ["call", "call_end", "volume_up", "volume_off"]),   # 92-95 px
    (28, ["call_end", "add", "remove"]),                     # call controls
    (32, ["mic", "volume_up", "volume_off", "power"]),       # StopWatch rim hints
    # The charging bolt, an 18 px box at 360 (5 % of S)
    (18, ["charging"]), (21, ["charging"]), (23, ["charging"]), (24, ["charging"]),
    # The passcode lock: the locked face's lock
    # 6.2 % of S (23/26/29/30), the wait screen's 11 % (40/45/51/53), the
    # keypad's delete (26/27 on the ring, 32/33 on the grid)
    (23, ["lock"]), (26, ["lock", "backspace"]), (29, ["lock"]), (30, ["lock"]), (40, ["lock"]),
    (45, ["lock"]), (51, ["lock"]), (53, ["lock"]), (27, ["backspace"]), (32, ["backspace"]),
    (33, ["backspace"]),
]
# The Live call's own icons: behind MUSE_ICON_CALLS too (muse_icons.c), so a build without Live call carries none.
CALL_ICONS = {"call", "call_end", "add", "remove"}
MENU = ["call", "volume_up", "display", "mic", "battery", "bluetooth", "wifi", "home", "info", "power",
        "developer", "videocam", "add", "remove", "lock", "touch"]
# 24 s on the other boards (MUSE_ICON_MENU in muse_icons.c: 20, 21, 24); the four
# catalog boards' icon box grown to its minimum physical size: 29 StopWatch, 28 Watcher, 35 ESP-Mosaico, 32 C6 1.8
MENU_PX = [20, 21, 24, 28, 29, 32, 35]
# The top row's Wi-Fi, Bluetooth and the muted speaker, also on the Sound page.
INK = [(n, h) for n in ("wifi", "bluetooth", "volume_off") for h in (14, 16, 18)]


def raster(svg_text, px):
    """Rasterizes SVG text to an RGBA PIL image of px x px (4x supersampled)."""
    from PIL import Image   # noqa: E402

    out = subprocess.run(
        ["magick", "-background", "none", "-density", "384", "svg:-",
         "-resize", "%dx%d" % (px, px), "png:-"],
        input=svg_text.encode(), capture_output=True, check=True).stdout
    return Image.open(io.BytesIO(out)).convert("RGBA")


def gen_icons():
    def svg(name, color="#FFFFFF"):
        path = HOME_PATH if name == "home" else EXTRA_PATHS[name] if name in EXTRA_PATHS else ICON_PATHS[name]
        return ('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">'
                '<path fill="%s" d="%s"/></svg>' % (color, path))

    def mask(name, px):
        return raster(svg(name), px).split()[3]

    out = ["/*",
           " * Muse Gadgets standard icons, generated from Material Icons",
           " * by tools/muse/gen_ui_assets.py --icons at the drawn sizes:",
           " * button glyphs 36/42/48 px (52 % of a 72/81/92 px button), call controls 28 px,",
           " * menu-row glyphs %s px, the StopWatch rim hints 32 px, and the top" % "/".join(map(str, MENU_PX)),
           " * row's Wi-Fi, Bluetooth and muted speaker cropped to their ink at 14/16/18 px tall, each behind",
           " * MUSE_ICON_PX (buttons, call controls, hints), MUSE_ICON_MENU (menu rows) or",
           " * MUSE_ICON_INK (muse_icons.c: a board compiles its own), the Live call's",
           " * own (call, call_end, add, remove) behind MUSE_ICON_CALLS too. Material Icons",
           " * (https://github.com/google/material-design-icons), Apache License 2.0;",
           " * `home` is Material's, redrawn for the Muse row; `lock` and `backspace`",
           " * Material's, redrawn for the passcode lock. Don't edit: regenerate.",
           " */", "#pragma once", "#include <stdint.h>", ""]
    entries = []
    sq = {}
    why = {}   # (name, px) -> the guards that want it
    for px, names in SQUARE:
        for n in names:
            sq.setdefault(n, set()).add(px)
            why.setdefault((n, px), []).append("MUSE_ICON_PX(%d)" % px)
    for n in MENU:
        sq.setdefault(n, set()).update(MENU_PX)
        for px in MENU_PX:
            why.setdefault((n, px), []).append("MUSE_ICON_MENU(%d)" % px)
    # Each mask behind its size's guard: muse_icons.c says which sizes a
    # board draws (MUSE_ICON_PX, MUSE_ICON_MENU for the menu rows,
    # MUSE_ICON_INK), so a board compiles only its own (the ESP-Mosaico's
    # app slot is tight).
    guards = {}
    for n in sorted(sq):
        for px in sorted(sq[n]):
            data = list(mask(n, px).tobytes())
            guards[(n, px)] = " || ".join(why[(n, px)])
            if n in CALL_ICONS:
                guards[(n, px)] = "MUSE_ICON_CALLS && (%s)" % guards[(n, px)]
            out.append("#if %s" % guards[(n, px)])
            out.append("static const uint8_t muse_icon_%s_%d[%d * %d] = {" % (n, px, px, px))
            for i in range(0, len(data), px):
                out.append("    " + ",".join(str(v) for v in data[i:i + px]) + ",")
            out.append("};")
            out.append("#endif")
            entries.append((n, px, px, px, "muse_icon_%s_%d" % (n, px)))
    out.append("")
    out.append("/* The top row: cropped to the ink, ink height = the battery's height. */")
    ink_entries = []
    for n, h in INK:
        big = mask(n, 480)
        bb = big.getbbox()
        units_h = (bb[3] - bb[1]) / 480.0 * 24.0
        box = int(round(h * 24.0 / units_h))
        m = mask(n, box)
        # crop to the ink proper: rows and columns of faint edge coverage
        # (under 1/8) only would make the icon a pixel taller than the battery
        b = m.point(lambda v: 255 if v >= 32 else 0).getbbox()
        m = m.crop(b)
        w, hh = m.size
        data = list(m.tobytes())
        nm = "muse_icon_ink_%s_%d" % (n, h)
        out.append("#if MUSE_ICON_INK(%d)" % h)
        out.append("static const uint8_t %s[%d * %d] = {" % (nm, w, hh))
        for i in range(0, len(data), w):
            out.append("    " + ",".join(str(v) for v in data[i:i + w]) + ",")
        out.append("};")
        out.append("#endif")
        ink_entries.append((n, h, w, hh, nm))
    out.append("")

    def table(name, rows, guard):
        out.append("/* Smallest first within each icon, so the lookup keeps the last that fits. */")
        out.append("static const entry_t %s[] = {" % name)
        for n, px, w, h, nm in rows:
            out.append("#if %s" % (guards[(n, px)] if guard == "MUSE_ICON_PX" else "%s(%d)" % (guard, px)))
            out.append("    { MUSE_ICON_%s, %d, DSC(%s, %d, %d) }," % (n.upper(), px, nm, w, h))
            out.append("#endif")
        out.append("};")
        out.append("")

    table("ENTRIES", entries, "MUSE_ICON_PX")
    table("INK_ENTRIES", ink_entries, "MUSE_ICON_INK")
    with open(os.path.join(MUSE, "muse_icons_gen.h"), "w") as f:
        f.write("\n".join(out))
    print("wrote muse_icons_gen.h: %d masks, %d top-row masks" % (len(entries), len(ink_entries)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--fonts", action="store_true")
    ap.add_argument("--icons", action="store_true")
    a = ap.parse_args()
    if not a.fonts and not a.icons:
        a.fonts = a.icons = True
    if a.icons:
        gen_icons()
    if a.fonts:
        gen_fonts()
