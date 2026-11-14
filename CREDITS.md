<!--
Copyright (c) 2026 ledienbien-ai

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

# Credits and licenses

This repository is a community fork of
[facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
that adds ESP32-S3 boards: so far the Waveshare ESP32-S3-Touch-LCD-1.85C, the
OSTB-3ST and the LCDWIKI 2.8inch ESP32-S3 Display. It is not affiliated with
or endorsed by Meta, Waveshare, Espressif, LCDWIKI, the xiaozhi-esp32 project
or the OSTB-3ST's maker.

This file records where the code came from and under which terms. It is a
plain-language record, not legal advice.

## 1. The Muse Gadget SDK (upstream)

| | |
|---|---|
| Source | <https://github.com/facebookincubator/muse-gadget-sdk> |
| Copyright | Meta Platforms, Inc. and affiliates |
| License | Apache License, Version 2.0, in [`LICENSE`](LICENSE) |
| Forked from | commit `b9008abba7dc4109c66212b9b82e459d08b98b85` |

Everything in this repository that is not listed in sections 2 to 4 is the
upstream SDK, unchanged.

Upstream keeps these third-party files under their own licenses:

| Path | Upstream | License |
|---|---|---|
| [`esp32/components/minimp3/include/minimp3.h`](esp32/components/minimp3) | [lieff/minimp3](https://github.com/lieff/minimp3) | CC0-1.0, see [`LICENSE`](esp32/components/minimp3/LICENSE) |
| [`esp32/main/pixel_font.c`](esp32/main/pixel_font.c) | Adafruit GFX `glcdfont.c` | BSD-2-Clause, in the file header |

**The Apache License does not cover the [Jollybot avatar](esp32/avatar).** Its
files carry only a Meta copyright line. Firmware built from this repository
includes that avatar unless you replace it with your own
(`esp32/tools/muse/AVATAR_RECIPE.md`).

## 2. Changes made in this fork

Copyright (c) 2026 ledienbien-ai. Licensed under the Apache License,
Version 2.0, the same license as upstream.

Files added:

| Path | What it is |
|---|---|
| [`esp32/components/muse/boards/board_waveshare_s3_185c.c`](esp32/components/muse/boards/board_waveshare_s3_185c.c) | The 1.85C: display, touch, both audio versions, button, battery, power |
| [`esp32/components/muse/boards/waveshare_s3_185c_lcd_init.h`](esp32/components/muse/boards/waveshare_s3_185c_lcd_init.h) | ST77916 register table (see section 3) |
| [`esp32/devices/sdkconfig.muse-waveshare-s3-185c`](esp32/devices/sdkconfig.muse-waveshare-s3-185c) | Build settings for the 1.85C |
| [`esp32/components/muse/boards/board_ostb_3st.c`](esp32/components/muse/boards/board_ostb_3st.c) | The OSTB-3ST: NV3023 display and its setup table (see section 3), touch, audio, keys, battery, power |
| [`esp32/devices/sdkconfig.muse-ostb-3st`](esp32/devices/sdkconfig.muse-ostb-3st) | Build settings for the OSTB-3ST |
| [`esp32/components/muse/boards/board_lcdwiki_s3_28.c`](esp32/components/muse/boards/board_lcdwiki_s3_28.c) | The LCDWIKI 2.8inch ESP32-S3 Display: display, touch, audio, button, battery, power |
| [`esp32/devices/sdkconfig.muse-lcdwiki-s3-28`](esp32/devices/sdkconfig.muse-lcdwiki-s3-28) | Build settings for the LCDWIKI board |
| [`esp32/components/muse/boards/board_echoear.c`](esp32/components/muse/boards/board_echoear.c) | The Espressif EchoEar, v1.0 and v1.2: display, touch, audio, button, touch pads, battery gauge, power |
| [`esp32/devices/sdkconfig.muse-echoear`](esp32/devices/sdkconfig.muse-echoear) | Build settings for the EchoEar |
| [`esp32/components/muse/boards/echoear_lcd_init.h`](esp32/components/muse/boards/echoear_lcd_init.h) | The EchoEar panel's ST77916 register table. Not ledienbien-ai's: it is Espressif's file, under its own copyright line (section 3) |
| [`esp32/components/muse/logo/logo.c`](esp32/components/muse/logo) | The DB_ROBOT startup logo, as LVGL's image converter exported it. Copyright (c) 2026 ledienbien-ai, free to use under the Apache License like the rest of this section |
| `doc/image/` | Pictures for the READMEs: the UI at each added board's screen size, drawn by the simulator. They show the default Jollybot avatar, which the Apache License does not cover (section 1) |
| [`esp32/components/muse/muse_lang.c`](esp32/components/muse/muse_lang.c), `muse_lang.h` | The screen's language, and its texts in Vietnamese |
| [`esp32/components/muse/muse_fonts.c`](esp32/components/muse/muse_fonts.c), `muse_fonts.h` | The fonts each language uses |
| [`esp32/components/muse/fonts/README.md`](esp32/components/muse/fonts/README.md) | How the Vietnamese font files were made. The font files beside it are not under the Apache License: see the table below |
| [`esp32/components/muse/muse_tts.c`](esp32/components/muse/muse_tts.c), `muse_tts_text.c`, `muse_tts.h` | Replies read aloud through Google Translate's text-to-speech (section 5) |
| `esp32/tests/test_muse_lang.py`, `muse_lang_harness.c` | Host tests for the two above |
| `CREDITS.md`, `README.vi.md`, `README.zh-CN.md`, `README.ja.md`, `README.ko.md` | This file and the translated READMEs |

`doc/image/db-robot-logo.png` is the same logo as a picture, for the READMEs.
`doc/image/` also holds pictures of the boards themselves.
`waveshare-s3-185c-muse.jpg` is ledienbien-ai's own photo.
`waveshare-s3-185c-device.jpg`, `ostb-3st-device.jpg`, `lcdwiki-s3-28-device.jpg`
and `lcdwiki-s3-28-back.jpg` were supplied by this
fork's owner and look to be the makers' or sellers' product pictures: they are
here to show which hardware is meant, belong to their owners, and are not
under the Apache License.
`echoear-muse.jpg` and `echoear-device.jpg` were made by this fork's owner from
pictures of the EchoEar, the simulator's Muse screen and the DB_ROBOT logo.
`echoear-boards-front.webp` and `echoear-boards-back.jpg`, supplied by this
fork's owner, look to be Espressif's labelled pictures of the boards from its
EchoEar user guide. The pictures of the device and its boards belong to their
owner and are not under the Apache License either.

Fonts added, under their own license, for the Vietnamese letters that LVGL's
built-in fonts lack. Each file is a subset of the font converted to LVGL's
format with [lv_font_conv](https://github.com/lvgl/lv_font_conv), and names
its source and license at the top:

| Path | Font | License |
|---|---|---|
| `esp32/components/muse/fonts/muse_font_montserrat_vi_14.c`, `_16.c`, `_20.c` | [Montserrat](https://github.com/JulietaUla/Montserrat) Medium, Copyright 2011 The Montserrat Project Authors | SIL Open Font License 1.1, in [`OFL-Montserrat.txt`](esp32/components/muse/fonts/OFL-Montserrat.txt) |
| `esp32/components/muse/fonts/muse_font_mono_vi_16.c`, `_20.c` | [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) ExtraBold, Copyright 2020 The JetBrains Mono Project Authors | SIL Open Font License 1.1, in [`OFL-JetBrainsMono.txt`](esp32/components/muse/fonts/OFL-JetBrainsMono.txt) |

Upstream files modified, each marked "Modified by ledienbien-ai" under its
license header, as section 4(b) of the Apache License asks:

| Path | Change |
|---|---|
| `README.md` | Describes this fork; links the translations and this file |
| `esp32/README.md`, `esp32/AGENTS.md`, `esp32/devices/README.md`, `esp32/devices/AGENTS.md` | List the new boards; `AGENTS.md` also says how on-screen text gets its Vietnamese |
| `esp32/components/muse/Kconfig`, `CMakeLists.txt`, `idf_component.yml` | Register the boards, the 1.85C's `esp_lcd_st77916` dependency (the EchoEar's too) and the LCDWIKI board's `esp_lcd_ili9341`; the startup logo's options; the starting language and spoken replies; ESP-IDF's touch sensor driver, for the EchoEar's pads |
| `esp32/components/muse/muse_ui.c` | On round screens smaller than 412 px, the speaker button sits under the state word. On a rectangular screen under 300 px tall (the OSTB-3ST's 296×240), a smaller Muse and a bar in place of the ring. The startup logo. The screen in Vietnamese: its texts, its fonts and the room their taller lines need |
| `esp32/components/muse/muse_settings_ui.c` | Settings pages that fit a screen that short; a "Muse AI by DB-robot" line under the settings list; the pages in Vietnamese and a Language page |
| `esp32/components/muse/muse_settings.c`, `muse_settings.h`, `muse_app.c`, `muse_ble.c` | The language setting: kept in NVS, applied at startup, set with the `lang` setup command |
| `esp32/components/muse/muse_text.c`, `muse_text.h`, `muse_chat_text.c` | Vietnamese letters kept in Vietnamese and made plain in English; the transcript's tail counted in characters |
| `esp32/components/muse/muse_state.c`, `muse_state.h`, `muse_input.c`, `muse_voice.c`, `muse_keypad.c` | Captions and keys in the screen's language |
| `esp32/components/muse/muse_board.h`, `muse_input.c` | A talk press can be marked as a hand on a touch pad (`MUSE_BTN_TALK_TOUCH`, the EchoEar's pads): it talks but doesn't confirm a pairing |
| `esp32/components/muse/muse_chat_session.cpp` | Sends each reply to `muse_tts.c` and plays the speech it fetches |
| `esp32/simulator/CMakeLists.txt`, `src/main.c`, `README.md` | Build the Vietnamese texts and fonts; a `--lang` option |
| `esp32/tools/muse/ble_setup.html` | A Language setting |
| `esp32/tools/muse/board.sh`, `ports.py`, `avatar.py` | Add the `s3lcd`, `ostb`, `lcd28` and `echoear` board aliases |

## 3. Sources used for the board ports

### Waveshare ESP32-S3-Touch-LCD-1.85C

| Source | Used for | License |
|---|---|---|
| [Waveshare documentation](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85C) and [wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85C) | Hardware facts: chip, pins, the V1 and V2 audio versions | Reference only; no text or code copied |
| [waveshareteam/ESP32-S3-Touch-LCD-1.85C](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C) (`ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test`) | The ST77916 register table in `waveshare_s3_185c_lcd_init.h`, copied; panel ID check, reset lines, touch registers, battery divider and V2 codec pins, as reference | Apache-2.0 |
| [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (`main/boards/waveshare/esp32-s3-touch-lcd-1.85c`) | The same ST77916 register table; V1 and V2 audio pin maps and the V1 mic format, as reference | MIT (notice below) |
| [waveshareteam/Waveshare-ESP32-components](https://github.com/waveshareteam/Waveshare-ESP32-components) (`bsp/esp32_s3_touch_amoled_1_75c`) | How the ES8311 and ES7210 pair is set up, as reference for V2 | Apache-2.0 |

### OSTB-3ST

| Source | Used for | License |
|---|---|---|
| The board's xiaozhi-esp32 board directory, `ostb-xiaozhi-3st` (`config.h`, `ostb-xiaozhi-3st.cc`, `power_manager.h`), supplied by this fork's owner. It is not in the xiaozhi-esp32 repository. | The NV3023 setup table and the battery's ADC table in `board_ostb_3st.c`, copied as data; pins, panel orientation, touch and power-off handling, as reference | The files carry no copyright or license notice. They are written against [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32), which is MIT (notice below). If the board's maker publishes them under other terms, record those here. |

### LCDWIKI 2.8inch ESP32-S3 Display

| Source | Used for | License |
|---|---|---|
| [LCDWIKI's page for the board](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display) | Hardware facts: chip, memory, pins, the two models, the amp enable's polarity | Reference only; no text or code copied |
| The board's xiaozhi-esp32 board directory, `xiaozhi-ai-iot-vietnam-es3n28p-lcd-2.8` (`config.h`, the board's `.cc`, `power_manager.h`), supplied by this fork's owner | Pins, the panel's orientation and colour settings, as reference; the two ends of the battery's ADC range, copied as numbers | The files carry no copyright or license notice. They are written against [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32), which is MIT (notice below) |
| [jvduuren/esphome-es3c28p-light-panel](https://github.com/jvduuren/esphome-es3c28p-light-panel) | Two things measured on the board: the touch panel's orientation and the 40 MHz SPI limit | Reference only; no text or code copied |

### Espressif EchoEar (ESP-VoCat)

| Source | Used for | License |
|---|---|---|
| [espressif/esp-bsp](https://github.com/espressif/esp-bsp) (`bsp/esp_vocat`), Espressif's board support package for the v1.2 board | The ST77916 register table, copied whole as [`echoear_lcd_init.h`](esp32/components/muse/boards/echoear_lcd_init.h) with its copyright line; the v1.2 pins, which way each supply switch and reset line works, the touch and codec setup, and the touch pads' sensor settings and 1.5 % threshold (`bsp_button.c`), as reference | Apache-2.0. Copyright 2026 Espressif Systems (Shanghai) CO LTD |
| Espressif's user guides for the [EchoEar v1.2](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/echoear/user_guide_v1.2.html) and [v1.0](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/echoear/user_guide_v1.0.html) | Hardware facts: the two modules and their memory, the parts on the board, the pins v1.2 moved, one touch pad on v1.0 and two on v1.2 | Reference only; no text or code copied |
| ESP-IDF's [`touch_sens_sleep`](https://github.com/espressif/esp-idf/tree/v6.0.1/examples/peripherals/touch_sensor/touch_sens_sleep) example | The order the touch sensor is brought up in, with waking from light sleep, as reference | Unlicense OR CC0-1.0 |
| The board's xiaozhi-esp32 files (`config.h`, `EchoEar.cc` and others), supplied by this fork's owner; the board is in [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) | How v1.0 is told from v1.2 at boot and v1.0's pins (its touch pad's among them), as reference; the fuel gauge's address and registers | MIT (notice below) |

### Notice for xiaozhi-esp32

As its license requires:

```
MIT License

Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
Copyright (c) 2025 Project Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 4. Components fetched at build time

The firmware links components that the ESP-IDF Component Manager downloads
into `esp32/managed_components/`. They are not stored in this repository and
each keeps its own license. For these boards they include:

| Component | License |
|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) v6.0.1 | Apache-2.0 |
| `espressif/esp_lcd_st77916` (1.85C and EchoEar), `esp_lcd_ili9341` (LCDWIKI board only), `esp_lvgl_adapter`, `esp_lcd_touch`, `esp_codec_dev`, `esp_websocket_client`, `led_strip`, `button`, `knob`, `esp_lv_fs`, `esp_lv_decoder`, `esp_mmap_assets`, `cmake_utilities` | Apache-2.0 |
| `lvgl/lvgl` 9.5.0 | MIT |
| `espressif/cjson` (cJSON) | MIT |
| `espressif/esp_new_jpeg` | Espressif MIT |
| `espressif/freetype` | FreeType License |
| `espressif/libpng` | PNG Reference Library License |
| `espressif/zlib` | zlib License |

The simulator's LVGL and SDL are listed in
[`esp32/simulator/THIRD_PARTY.md`](esp32/simulator/THIRD_PARTY.md).

## 5. What the licenses do not give you

- **Names and logos.** The Apache License grants no right to the Meta, Muse,
  Waveshare or Espressif names and marks beyond describing where the work came
  from.
- **The Muse service.** Pairing a gadget needs an SDK token and is governed by
  the [Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms), not by this
  repository's license.
- **Google's text-to-speech.** Spoken replies come from the address behind
  Google Translate's "listen" button (`translate.google.com/translate_tts`).
  It is not a published API and nothing here gives a right to use it: Google's
  terms apply, Google can change or block it at any time, and the text of each
  spoken reply is sent to Google. `CONFIG_MUSE_TTS_GOOGLE=n` builds without it.
- **Warranty.** Everything here is provided "as is". Flashing firmware can
  brick a board.

## 6. Keeping this in order in later versions

- Keep `LICENSE` and this file in the repository and in any source archive.
- Give each new file an Apache header with your own copyright line. Leave the
  headers of upstream and third-party files as they are.
- When you change an upstream file, add or extend the "Modified by" line under
  its header and the table in section 2.
- When you copy code or data from another project, check its license first and
  add a row to section 3, with the notice text if the license asks for one.
- When you publish a firmware `.bin`, link this file and `LICENSE` next to the
  download: a binary is a distribution too. A build made with your SDK token
  carries that token.
