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

`muse_font_cjk_noto_16.c` is the anti-aliased alternative, built in instead
with `CONFIG_MUSE_CJK_FONT_NOTO`: Noto Sans SC Medium at 16 px, 4 bpp, in the
same cell, for CJK punctuation, kana, the 6763 hanzi of GB2312 and the
fullwidth forms. `tools/muse/gen_cjk_noto_font.sh` regenerates it.

Noto Sans SC is by Adobe and Google (https://github.com/notofonts/noto-cjk),
licensed under the SIL Open Font License, version 1.1. This file is a
conversion of an unaltered subset of the font.
