<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Fonts

`muse_font_cjk_16.c` is the CJK fallback for the caption font, built in only
with `CONFIG_MUSE_CJK_FONT`. It holds GNU Unifont 16.0.04's 16x16 bitmaps,
the same cell as unscii-16, for CJK punctuation, kana, every CJK Unified
Ideograph (U+4E00 to U+9FFF), fullwidth forms, Hangul jamo (U+1100 to
U+11FF), Hangul compatibility jamo (U+3130 to U+318F) and Hangul syllables
(U+AC00 to U+D7AF): about 1.2 MB of read-only font data, 33,024 glyphs.
`tools/muse/gen_cjk_font.sh` regenerates it.

`muse_font_latin_16.c` is the caption font for accented Latin letters, built
in only with `CONFIG_MUSE_LATIN_FONT`: GNU Unifont 16.0.04's 8x16 bitmaps for
ASCII, Latin-1's letters, Latin Extended-A, the horned O and U, and the
Vietnamese letters U+1EA0 to U+1EF9, about 10 KB of flash. A caption with
accented letters in it is drawn wholly in it. The same script regenerates it.

`muse_font_vi_12.c`, `_14.c`, `_20.c` and `_28.c` are Montserrat Medium's 134
Vietnamese letters at the sizes LVGL builds Montserrat in, 4 bpp like LVGL's,
built in only with `CONFIG_MUSE_UI_LANG_VI`. `muse_lang_montserrat()` makes
each the fallback of LVGL's font of its size, for the menu and the pairing
card. They come from the `Montserrat-Medium.ttf` LVGL generates its own from
(`scripts/built_in_font/` in the lvgl component), with the same script.

`type/muse_font_<role>_<S>.c` are the home, Settings and passcode screens'
type at four panel sizes S (368, 412, 466 and 480 px), nine roles each, 4 bpp:
Montserrat Medium from that same file, and Montserrat SemiBold for the titles.
The time has its digits, `:` and `-`, the passcode keypad's digits only the
digits, the titles printable ASCII and the ellipsis, the other roles printable
ASCII plus the degree sign, middle dot, em dash, right single quote and
ellipsis. `tools/muse/gen_ui_assets.py --fonts` regenerates them. A board
builds only its own set, with `CONFIG_MUSE_UI_PANEL_TYPE` (off by default; on
in the overlays of the Waveshare C6 1.8, StopWatch and ESP-Mosaico and the
SenseCAP Watcher's gadget profile). A set takes about 105 KB of flash at
368 px, 107 KB at 412, 120 KB at 466 and 134 KB at 480. Other boards draw the
home's time in LVGL's Montserrat 28, from the sizes `devices/sdkconfig.muse`
builds in; a board's own overlay may turn on `CONFIG_LV_FONT_MONTSERRAT_48`
for a larger time, about 94 KB of flash (the apps overlay does).

Montserrat is by Julieta Ulanovsky and the Montserrat Project Authors, under
the SIL Open Font License, version 1.1.

`muse_font_pt_12.c`, `_14.c`, `_16.c`, `_20.c` and `_28.c` are Montserrat
Medium's 26 Portuguese letters (À-Ã, Ç, É, Ê, Í, Ó-Õ, Ú, Ü and their lower
case) at the same sizes plus 16 px, which the touch settings pages use, built
in only with `CONFIG_MUSE_UI_LANG_PT_BR`. They come from the same
`Montserrat-Medium.ttf` with lv_font_conv 1.5.3:
`lv_font_conv --font Montserrat-Medium.ttf --size <px> --bpp 4 --format lvgl
--lv-font-name muse_font_pt_<px> --no-compress -r
0xC0-0xC3,0xC7,0xC9-0xCA,0xCD,0xD3-0xD5,0xDA,0xDC,0xE0-0xE3,0xE7,0xE9-0xEA,0xED,0xF3-0xF5,0xFA,0xFC`,
with the include changed to `lvgl.h` as in the Vietnamese ones.

GNU Unifont is by Roman Czyborra, Paul Hardy and contributors
(https://unifoundry.com/unifont/). Its compiled fonts are licensed under the
SIL Open Font License, version 1.1 (https://openfontlicense.org), and under
the GNU GPL version 2 or later with the GNU font embedding exception. This
file and `muse_font_latin_16.c` are conversions of unaltered subsets of the
font.

`muse_font_cjk_noto_16.c` is the anti-aliased alternative, built in instead
with `CONFIG_MUSE_CJK_FONT_NOTO`: Noto Sans SC Medium at 16 px, 4 bpp, in the
same cell, for CJK punctuation, kana, the 6763 hanzi of GB2312 and the
fullwidth forms. It has no Hangul, so Korean shows as boxes with it.
`tools/muse/gen_cjk_noto_font.sh` regenerates it.

Noto Sans SC is by Adobe and Google (https://github.com/notofonts/noto-cjk),
licensed under the SIL Open Font License, version 1.1. This file is a
conversion of an unaltered subset of the font.

`muse_font_emoji_16.c` is the caption font's colour emoji, built in only with
`CONFIG_MUSE_EMOJI`: 190 of the emoji most common in chat replies, as 16x16
RGB565A8 images, the height of unscii-16 and two of its cells wide, about
150 KB of flash. `muse_font_emoji_16` is an LVGL font whose glyphs are those
images. The caption font falls back to it last, and `muse_text.c` keeps in
captions only the emoji in `muse_emoji.h`, its list, so the others still go
rather than show as boxes. To change the set, edit `EMOJI` in
`tools/muse/gen_emoji_font.py` and run it with Pillow installed:
`python3 tools/muse/gen_emoji_font.py`. It downloads the pinned Twemoji
release, checks its SHA-256 and writes both files; pass a checkout of the
release instead to work offline.

The emoji are Twemoji v16.0.1's 72x72 graphics (https://github.com/jdecked/twemoji),
copyright Twitter, Inc and other contributors, and Jason Sofonia, Justine De
Caires and other contributors, licensed under CC-BY 4.0
(https://creativecommons.org/licenses/by/4.0/; the full text is in
`LICENSE-twemoji.txt`). They are changed: scaled to 16x16 and converted to
RGB565A8.
`muse_font_ellipsis_20.c` and `muse_font_ellipsis_28.c` hold one glyph, the
ellipsis (U+2026), as fallbacks for LVGL's `lv_font_montserrat_20` and `_28`,
which stop at ASCII and their symbols: the Live call's "Calling…" and
"Waiting for phone…" (`muse_call.c`). They're converted from
the `Montserrat-Medium.ttf` LVGL ships for its own Montserrat builds, with the
same options; `tools/muse/gen_ellipsis_font.sh` regenerates them. Montserrat
is by Julieta Ulanovsky and contributors, under the SIL Open Font License,
version 1.1, as LVGL's built-in Montserrat fonts are.
