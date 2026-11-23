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

# Muse Gadgets

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/images/muse-gadgets-dark.png">
    <img src=".github/images/muse-gadgets-light.png" width="900" alt="Muse gadgets: a Waveshare round AMOLED, an M5Stack StickS3, Muse Home Link, a Raspberry Pi and a Seeed reTerminal e-ink display">
  </picture>
</p>

Muse gadgets are open source devices you build yourself. Program an
off-the-shelf ESP32 board or set up a Raspberry Pi with our device SDKs, then
connect Muse to your displays, buttons, sensors, actuators, and whatever else
you've got lying on your workbench.

We open sourced the SDKs and firmware here. It's built by hackers, for hackers,
just for fun. Side effects of tinkering may include bricked boards, voided
warranties, brownouts, or bankruptcies. Proceed at your own risk!

| | |
|---|---|
| [**ESP32 Device SDK**](esp32) | Connect your ESP32 board to Muse through our open source SDK. Throw in a screen to show images, add audio in and out, or wire up other sensors. |
| [**Linux Device SDK**](linux) | Turn that spare Raspberry Pi or Linux box into a Muse gadget. Hack in your own commands to let Muse handle sysadmin chores or your Home Assistant setup. |
| [**Muse Gadgets Test**](android/musegadgetstest) ([iPhone](ios/musegadgetstest)) | Stream your phone's camera, or a clip, to an ESP32 gadget's screen over Wi-Fi: H.264 or VP8 decoded on the board, 30 fps on the round 466 px boards. [`protocols/VIDEO.md`](protocols/VIDEO.md) explains how. |

Before you flash or pair a gadget, get an
[SDK token](https://gadgets.muse.ai/settings/sdk-tokens) and review the
[Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms). Every gadget needs a
token to pair.

To install supported prebuilt ESP32 firmware without setting up ESP-IDF, use
the [Muse Gadget Web Flasher](https://facebookincubator.github.io/muse-gadget-sdk/)
in Chrome or Edge on a desktop computer.

ESP32 and Linux gadgets pair with the Muse app on iOS and Android, via
Settings > Devices. Turn on Developer mode there first, then look for devices
prefixed with "MuseGadget".
Each SDK directory has a `README.md` to get started and an `AGENTS.md` for coding
agents like [Muse Code](https://developer.meta.com/ai/lp/muse-code/).
[`protocols/DEVICE.md`](protocols/DEVICE.md) says how a gadget describes itself
to Muse, and [`protocols/ERROR_REPORT.md`](protocols/ERROR_REPORT.md) what it
tells Muse after a crash.

For battery-input power profiling, see the [porting recipe](esp32/docs/power/README.md).
The opt-in diagnostic core is board-neutral; boards supply its hooks and state
matrices, and [power_sweep.py](esp32/tools/power/power_sweep.py) drives the
USB-free sweep. The isolated diagnostic does not pair or need an SDK token;
fixture safety and explicit hardware authorization still come first, as the
[PPK2 rules](esp32/docs/power/ppk2.md) describe.

## Community

Meet other hackers who are building and customizing Muse gadgets in our
community [Discord](https://discord.gg/3bhjCkZdd6). Get inspired, support each
other, and share what you make.

## License

Muse Gadgets is licensed under the Apache License, Version 2.0, found in
[`LICENSE`](LICENSE), except for these third-party files, which keep their
upstream licenses:

| Path | Upstream | License |
|---|---|---|
| [`esp32/components/minimp3/include/minimp3.h`](esp32/components/minimp3) | [lieff/minimp3](https://github.com/lieff/minimp3) | CC0-1.0, see [`LICENSE`](esp32/components/minimp3/LICENSE) |
| [`esp32/components/muse/boards/ostb_echoear_display_init.h`](esp32/components/muse/boards/ostb_echoear_display_init.h) | [78/xiaozhi-esp32 ESP-VoCat panel sequence](https://github.com/78/xiaozhi-esp32/blob/0d576d3d4c049c6f55eaf879725dc23e516511b4/main/boards/espressif/esp-vocat/esp_vocat.cc) | MIT, retained in the file header |
| ST7789 tuning table in [`board_atk_dnesp32s3_box2.c`](esp32/components/muse/boards/board_atk_dnesp32s3_box2.c) | [78/xiaozhi-esp32 ATK-DNESP32S3-BOX2 board](https://github.com/78/xiaozhi-esp32/blob/a78ca19d5bc9a12ee0853b9f509530276d36aa03/main/boards/alientek/atk-dnesp32s3-box2-wifi/atk_dnesp32s3_box2.cc) | MIT, see [`xiaozhi-esp32.txt`](esp32/devices/licenses/xiaozhi-esp32.txt) |
| ST77916 initialization sequence in [`waveshare_s3_185c/st77916_init_185c.h`](esp32/components/muse/boards/waveshare_s3_185c/st77916_init_185c.h) | [78/xiaozhi-esp32 Waveshare ESP32-S3-Touch-LCD-1.85C board](https://github.com/78/xiaozhi-esp32/blob/d91d6824309d12df5fc3db5edf8145ee829135d7/main/boards/waveshare/esp32-s3-touch-lcd-1.85c/esp32-s3-touch-lcd-1.85c.cc) | MIT, see [`xiaozhi-esp32.txt`](esp32/devices/licenses/xiaozhi-esp32.txt) |
| JD9853A initialization sequence in [`board_seeed_xiao_s3_147.c`](esp32/components/muse/boards/board_seeed_xiao_s3_147.c) | [Seeed-Studio/Seeed_GFX2](https://github.com/Seeed-Studio/Seeed_GFX2) | MIT, see [`Seeed_GFX2.txt`](esp32/devices/licenses/Seeed_GFX2.txt) |
| GC9107 initialization sequence in [`board_m5stack_atoms3.c`](esp32/components/muse/boards/board_m5stack_atoms3.c) | [M5Stack M5GFX `Panel_GC9107`](https://github.com/m5stack/M5GFX/blob/df770af3a58327cd1d4736db610a3ed8906c0952/src/lgfx/v1/panel/Panel_GC9A01.hpp), from LovyanGFX | FreeBSD, see [`LovyanGFX.txt`](esp32/devices/licenses/LovyanGFX.txt) |
| AXS15231B initialization sequence in [`board_guition_jc3248w535.c`](esp32/components/muse/boards/board_guition_jc3248w535.c) | [moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX/blob/3cc08c4e9ab6d85807e49b657d73fae10871616e/src/display/Arduino_AXS15231B.h) 1.4.9's `axs15231b_init_operations` | BSD-2-Clause, see [`Arduino_GFX.txt`](esp32/devices/licenses/Arduino_GFX.txt) |
| [`esp32/components/esp_lcd_gc9107`](esp32/components/esp_lcd_gc9107) | Espressif's `espressif/esp_lcd_gc9107` 2.0.0 from the ESP Component Registry, changed to build on ESP-IDF 6 | Apache-2.0, Espressif's copyright, see [`license.txt`](esp32/components/esp_lcd_gc9107/license.txt) |
| [`esp32/components/muse/boards/viewe_smartring_display_init.h`](esp32/components/muse/boards/viewe_smartring_display_init.h) | [VIEWESMART/smartring_plus](https://github.com/VIEWESMART/smartring_plus) panel sequence | Apache-2.0 under VIEWE's copyright, in the file header |
| [`esp32/components/muse/boards/waveshare_s3_185b_init.h`](esp32/components/muse/boards/waveshare_s3_185b_init.h) | [waveshareteam/ESP32-S3-Touch-LCD-1.85B](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/tree/7232af4c501c64d34215b73e1d8319376dfd9c19/examples/esp-idf/05_bq27220/components/esp32_s3_touch_lcd_1_85B) panel sequences | Apache-2.0, in the file header |
| CO5300 initialization sequence in [`board_esp_mosaico.c`](esp32/components/muse/boards/board_esp_mosaico.c) | [esp-mosaico/esp-mosaico-bsp](https://github.com/esp-mosaico/esp-mosaico-bsp/blob/b869aa354c3a7ca8b5391bac2b6943bd676dcc22/components/esp-mosaico-bsp/onboard/display.c) vendor init | Apache-2.0, Espressif's copyright, noted in the file header |
| ExecuTorch runtime with its ESP-NN delegate, for the opt-in wake word (registry component, not vendored; see [`docs/executorch.md`](esp32/docs/executorch.md)) | [pytorch/executorch](https://github.com/pytorch/executorch), with [espressif/esp-nn](https://github.com/espressif/esp-nn) | BSD-3-Clause AND Apache-2.0 AND Apache-2.0 WITH LLVM-exception AND NCSA AND MIT, see the component's `LICENSE`; bundled third-party notices in the component. esp-nn: Apache-2.0 |
| TensorFlow Lite Micro's microfrontend, for the opt-in wake word (registry component, not vendored) | [esphome/esp-micro-speech-features](https://github.com/esphome-libs/esp-micro-speech-features) | Apache-2.0 (TensorFlow Authors, Kevin Ahrendt); its kissfft files BSD-3-Clause (Mark Borgerding) |
| [`esp32/components/muse_wakeword/models/hey_muse.pte`](esp32/components/muse_wakeword/models) | Trained for this SDK on public-domain, CC0 and CC BY data | Training data credits, as CC BY requires, in [`ATTRIBUTION.md`](esp32/components/muse_wakeword/models/ATTRIBUTION.md) |
| [`esp32/main/pixel_font.c`](esp32/main/pixel_font.c) | Adafruit GFX `glcdfont.c` | BSD-2-Clause, in the file header |
| [`esp32/components/muse/fonts/`](esp32/components/muse/fonts) | GNU Unifont 16.0.04, Noto Sans SC, and Montserrat Medium via LVGL, subsets (see its README) | SIL Open Font License 1.1 (Unifont also GPL-2.0+ with the font embedding exception) |
| [`esp32/components/muse/fonts/muse_font_emoji_16.c`](esp32/components/muse/fonts/muse_font_emoji_16.c) | [Twemoji](https://github.com/jdecked/twemoji) v16.0.1 graphics, 190 of them scaled to 16 px (see [`README.md`](esp32/components/muse/fonts/README.md)) | CC-BY 4.0, see [`LICENSE-twemoji.txt`](esp32/components/muse/fonts/LICENSE-twemoji.txt) |
| [`esp32/main/fonts/epaper_154g_caption_font.h`](esp32/main/fonts/epaper_154g_caption_font.h) | GNU Unifont 16.0.04, subset (see [`README.md`](esp32/main/fonts/README.md)) | SIL Open Font License 1.1, or GPL-2.0+ with the font embedding exception |
| [`esp32/components/muse/eink/fonts/montserrat.ttf`](esp32/components/muse/eink/fonts) | Montserrat Medium, via LVGL v9.5.0 | SIL Open Font License 1.1 |
| [`esp32/components/muse/eink/fonts/unscii.ttf`](esp32/components/muse/eink/fonts) | unscii-8 | Public domain |
| [`esp32/components/muse/eink/fonts/symbols.ttf`](esp32/components/muse/eink/fonts) | Font Awesome 5 Free, via LVGL v9.5.0 | SIL Open Font License 1.1 (icons CC BY 4.0) |
| [`esp32/main/epd_ssd1677.c`](esp32/main/epd_ssd1677.c) | Seeed-reTerminal-E10xx-Firmware's SSD1677 refresh sequence | MIT, retained in the file header |
| [`esp32/main/epd_it8951.c`](esp32/main/epd_it8951.c) | Seeed_GFX2's IT8951 command framing and ED103TC2 one-bit mode | MIT, retained in the file header |
| SSD1681 initialization sequence and waveform LUT in [`esp32/main/epd_ssd1681.c`](esp32/main/epd_ssd1681.c) | [waveshareteam/ESP32-S3-ePaper-1.54](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54/tree/7bde32a670379d815618cbee374332c2c1a42c6c/02_Example/XiaoZhi)'s XiaoZhi V2 source, 78/xiaozhi-esp32 with Waveshare's board | MIT, see [`xiaozhi-esp32.txt`](esp32/devices/licenses/xiaozhi-esp32.txt) |
| [`esp32/components/tinyh264/src`](esp32/components/tinyh264) | the Android Open Source Project's H.264 decoder, via [udevbe/tinyh264](https://github.com/udevbe/tinyh264) | Apache-2.0, AOSP's copyright, see [`LICENSE`](esp32/components/tinyh264/LICENSE) |
| [`esp32/components/libvpx_vp8/src`](esp32/components/libvpx_vp8) | [libvpx](https://chromium.googlesource.com/webm/libvpx) v1.16.0's VP8 decoder, trimmed | BSD-3-Clause, the WebM Project authors' copyright, see [`LICENSE`](esp32/components/libvpx_vp8/LICENSE), with Google's patent grant in [`PATENTS`](esp32/components/libvpx_vp8/PATENTS) |
| libwebp v1.6.0's WebP decoder, for the opt-in avatar sync (fetched at build time from a pinned archive, not vendored; see [`avatar_webp.cmake`](esp32/cmake/avatar_webp.cmake)) | [webmproject/libwebp](https://github.com/webmproject/libwebp) | BSD-3-Clause, Google's copyright, with Google's patent grant, see [`libwebp.txt`](esp32/licenses/libwebp.txt) |

Dependencies fetched at build time are under their own licenses: ESP-IDF
components (into `esp32/managed_components/`), and the simulator's LVGL and
SDL (listed in [`esp32/simulator/THIRD_PARTY.md`](esp32/simulator/THIRD_PARTY.md)).

The Apache License does not cover the [Jollybot avatar](esp32/avatar).

Muse Gadgets Test's sample clips aren't in this repository: the app fetches a few
seconds of the Blender Foundation's open movies (CC BY 3.0) when one is
played, and shows their credits.

Muse Gadgets Test can use x264 (GPL-2.0) on the phone; this repository carries none
of its code. An APK or iPhone app you build with it is under the GPL
([`android/musegadgetstest/README.md`](android/musegadgetstest/README.md),
[`ios/musegadgetstest/README.md`](ios/musegadgetstest/README.md)).
