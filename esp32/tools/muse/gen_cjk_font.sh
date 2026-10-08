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

# Regenerates components/muse/fonts/muse_font_cjk_16.c, the CJK fallback for
# the caption font (CONFIG_MUSE_CJK_FONT): GNU Unifont's 16x16 bitmaps, the
# same cell as unscii-16, for CJK punctuation, kana, every CJK Unified
# Ideograph and the fullwidth forms. Needs curl and npx (Node.js).
set -eu

VERSION=16.0.04
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_cjk_16.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

SHA256=0e3981ab552231b5a2a870f2b61741903a4bf25c23ef5aeb05fdced1b3c7af4d
curl -fsSL -o "$TMP/unifont.otf" \
    "https://unifoundry.com/pub/unifont/unifont-$VERSION/font-builds/unifont-$VERSION.otf"
echo "$SHA256  $TMP/unifont.otf" | shasum -a 256 -c - >/dev/null
npx -y lv_font_conv@1.5.3 --font "$TMP/unifont.otf" --size 16 --bpp 1 \
    --format lvgl --lv-font-name muse_font_cjk_16 --no-compress \
    -r 0x3000-0x30FF -r 0x4E00-0x9FFF -r 0xFF00-0xFFEF -o "$TMP/font.c"
# The project includes LVGL as "lvgl.h"; the header names no temp paths.
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" > "$OUT"
echo "wrote $OUT"
