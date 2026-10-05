# Fonts

`muse_font_cjk_16.c` is the CJK fallback for the caption font, built in only
with `CONFIG_MUSE_CJK_FONT`. It holds GNU Unifont 16.0.04's 16x16 bitmaps,
the same cell as unscii-16, for CJK punctuation, kana, every CJK Unified
Ideograph (U+4E00 to U+9FFF) and the fullwidth forms: about 850 KB of flash.
`tools/muse/gen_cjk_font.sh` regenerates it.

GNU Unifont is by Roman Czyborra, Paul Hardy and contributors
(https://unifoundry.com/unifont/). Its compiled fonts are licensed under the
SIL Open Font License, version 1.1 (https://openfontlicense.org), and under
the GNU GPL version 2 or later with the GNU font embedding exception. This
file is a conversion of an unaltered subset of the font.

`zh_font_12.c` and `zh_font_24.c` are the pixel fonts behind the Chinese UI
(`components/muse/i18n.c`): Fusion Pixel Font (zh_hans, proportional) at 12 px
for the labels and at 24 px for the enlarged captions, both 1 bit per pixel,
both with LVGL's Montserrat 14/28 as the fallback so `LV_SYMBOL_*` icons still
draw. `zh_font_12.c` covers ASCII, General Punctuation, CJK Symbols and
Punctuation, the fullwidth forms and every CJK Unified Ideograph (U+4E00 to
U+9FFF); `zh_font_24.c` carries the same ASCII and punctuation with only the
ideographs the UI's own strings use. They are built in for every board, since
the translation table is compiled in regardless of the board.

Fusion Pixel Font is by TakWolf (https://github.com/TakWolf/fusion-pixel-font)
and licensed under the SIL Open Font License, version 1.1:
[`LICENSE-OFL.txt`](LICENSE-OFL.txt) in this directory is that license, and
these two files are conversions of an unaltered subset of the font. Both were
generated with `lv_font_conv` 1.5.3; the exact options are in each file's
header.
