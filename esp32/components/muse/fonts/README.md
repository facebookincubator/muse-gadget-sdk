# Fonts

`muse_font_cjk_16.c` is the CJK fallback for the caption font, built in only
with `CONFIG_MUSE_CJK_FONT`. It holds GNU Unifont 16.0.04's 16x16 bitmaps,
the same cell as unscii-16, for CJK punctuation, kana, every CJK Unified
Ideograph (U+4E00 to U+9FFF) and the fullwidth forms: about 850 KB of flash.
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
Montserrat is by Julieta Ulanovsky and the Montserrat Project Authors, under
the SIL Open Font License, version 1.1.

GNU Unifont is by Roman Czyborra, Paul Hardy and contributors
(https://unifoundry.com/unifont/). Its compiled fonts are licensed under the
SIL Open Font License, version 1.1 (https://openfontlicense.org), and under
the GNU GPL version 2 or later with the GNU font embedding exception. This
file and `muse_font_latin_16.c` are conversions of unaltered subsets of the
font.
