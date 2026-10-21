# E-ink UI fonts

The fonts the e-ink UI (`components/muse/eink/`, `CONFIG_MUSE_UI_FREEINK`)
draws with, packed into the `fonts` partition by `tools/muse/pack_eink_fonts.py`
when the app builds. `tools/muse/gen_eink_fonts.sh` regenerates them: they're
subsets of the files LVGL v9.5.0 builds its own fonts from
(`scripts/built_in_font/`), so the e-ink UI's text and symbols match the LVGL
UI's, and of GNU Unifont, the LVGL UI's CJK font.

| File | Font | License |
|---|---|---|
| `montserrat.ttf` | Montserrat Medium (The Montserrat Project Authors): ASCII, accented Latin, Cyrillic | SIL Open Font License 1.1 |
| `unscii.ttf` | unscii-8 (Viznut): ASCII and Latin-1, drawn at 8 and 16 px | Public domain |
| `symbols.ttf` | Font Awesome 5 Free (Fonticons, Inc.): LVGL's `LV_SYMBOL_*` glyphs | SIL Open Font License 1.1 (icons CC BY 4.0) |
| `unifont.otf` | GNU Unifont 16.0.04 (Roman Czyborra, Paul Hardy and contributors): Greek, Cyrillic, Hebrew, Arabic, kana, Hangul, CJK | SIL Open Font License 1.1, or GNU GPL 2 or later with the GNU font embedding exception |

Each file is an unaltered subset of its font.
