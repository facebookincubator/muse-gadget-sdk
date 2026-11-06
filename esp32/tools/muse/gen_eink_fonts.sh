#!/bin/sh
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

# Regenerates components/muse/eink/fonts/, the fonts the e-ink UI draws with
# (CONFIG_MUSE_UI_FREEINK): the files LVGL v9.5.0 builds its own fonts from,
# so the e-ink UI's text and symbols match the LVGL UI's. (CJK comes from the
# LVGL UI's own fonts/muse_font_cjk_16.c.) Needs curl and fontTools
# (pip install fonttools brotli).
set -eu

LVGL=https://raw.githubusercontent.com/lvgl/lvgl/v9.5.0/scripts/built_in_font
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/eink/fonts"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

curl -fsSL -o "$TMP/Montserrat-Medium.ttf" "$LVGL/Montserrat-Medium.ttf"
curl -fsSL -o "$TMP/unscii-8.ttf" "$LVGL/unscii-8.ttf"
curl -fsSL -o "$TMP/fa.woff" "$LVGL/FontAwesome5-Solid+Brands+Regular.woff"

# Montserrat: LVGL's ASCII, plus accented Latin and Cyrillic for the keyboards.
pyftsubset "$TMP/Montserrat-Medium.ttf" --unicodes=U+0020-007E,U+00A0-024F,U+0400-04FF,U+2022,U+2026 \
    --layout-features='kern' --output-file="$OUT/montserrat.ttf"
# unscii-8: LVGL's unscii_8 and unscii_16 (the same file at 8 and 16 px).
pyftsubset "$TMP/unscii-8.ttf" --unicodes=U+0020-007E,U+00A0-00FF --output-file="$OUT/unscii.ttf"
# FontAwesome: the LV_SYMBOL_* glyphs LVGL builds into Montserrat (lv_symbol_def.h).
pyftsubset "$TMP/fa.woff" --flavor= --output-file="$OUT/symbols.ttf" \
    --unicodes=U+F001,U+F008,U+F00B,U+F00C,U+F00D,U+F011,U+F013,U+F015,U+F019,U+F01C,U+F021,U+F026,U+F027,U+F028,U+F03E,U+F043,U+F048,U+F04B,U+F04C,U+F04D,U+F051,U+F052,U+F053,U+F054,U+F067,U+F068,U+F06E,U+F070,U+F071,U+F074,U+F077,U+F078,U+F079,U+F07B,U+F093,U+F095,U+F0C4,U+F0C5,U+F0C7,U+F0C9,U+F0E0,U+F0E7,U+F0EA,U+F0F3,U+F11C,U+F124,U+F158,U+F15B,U+F1EB,U+F240,U+F241,U+F242,U+F243,U+F244,U+F287,U+F293,U+F2ED,U+F304,U+F55A,U+F7C2,U+F8A2
ls -l "$OUT"
