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

# E-ink UI fonts

The fonts the e-ink UI (`components/muse/eink/`, `CONFIG_MUSE_UI_FREEINK`)
draws with, embedded in the app. `tools/muse/gen_eink_fonts.sh` regenerates
them: they're subsets of the files LVGL v9.5.0 builds its own fonts from
(`scripts/built_in_font/`), so the e-ink UI's text and symbols match the LVGL
UI's. CJK comes from the LVGL UI's own `../../fonts/muse_font_cjk_16.c`.

| File | Font | License |
|---|---|---|
| `montserrat.ttf` | Montserrat Medium (The Montserrat Project Authors): ASCII, accented Latin, Cyrillic | SIL Open Font License 1.1 |
| `unscii.ttf` | unscii-8 (Viznut): ASCII and Latin-1, drawn at 8 and 16 px | Public domain |
| `symbols.ttf` | Font Awesome 5 Free (Fonticons, Inc.): LVGL's `LV_SYMBOL_*` glyphs | SIL Open Font License 1.1 (icons CC BY 4.0) |

Each file is an unaltered subset of its font.
