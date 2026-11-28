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

# Regenerates components/muse/fonts/muse_font_ellipsis_{20,28}.c: the one
# glyph LVGL's Montserrat builds lack that the calling screen needs, the
# ellipsis (U+2026), as a fallback for lv_font_montserrat_20 / _28
# ("Calling…"). Converted from the Montserrat-Medium.ttf LVGL ships with
# the same options LVGL's own builds use. Needs npx (Node.js) and LVGL's source
# (LVGL_DIR, else the ESP-IDF component manager's copy).
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
if [ -z "${LVGL_DIR:-}" ]; then
    LVGL_DIR=$(ls -d "$HERE"/managed_components/lvgl__lvgl \
        "$HOME"/Library/Caches/Espressif/ComponentManager/*/lvgl__lvgl_9.5.0_* \
        "$HOME"/.cache/Espressif/ComponentManager/*/lvgl__lvgl_9.5.0_* 2>/dev/null | head -1)
fi
TTF="$LVGL_DIR/scripts/built_in_font/Montserrat-Medium.ttf"
[ -f "$TTF" ] || { echo "no Montserrat-Medium.ttf under LVGL_DIR=$LVGL_DIR" >&2; exit 1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
for size in 20 28; do
    OUT="$HERE/components/muse/fonts/muse_font_ellipsis_$size.c"
    npx -y lv_font_conv@1.5.3 --font "$TTF" --size "$size" --bpp 4 \
        --no-compress --no-prefilter --force-fast-kern-format \
        --format lvgl --lv-font-name "muse_font_ellipsis_$size" -r 0x2026 -o "$TMP/font.c"
    {
        printf '/* Muse fallback font (tools/muse/gen_ellipsis_font.sh): Montserrat Medium %s px, the\n' "$size"
        printf ' * ellipsis (U+2026) only, for lv_font_montserrat_%s. Montserrat: SIL Open Font\n' "$size"
        printf " * License 1.1. Don't edit: regenerate. */\n"
        sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TTF|Montserrat-Medium.ttf|g" \
            -e "s|$TMP/||g" "$TMP/font.c"
    } > "$OUT"
    echo "wrote $OUT"
done
