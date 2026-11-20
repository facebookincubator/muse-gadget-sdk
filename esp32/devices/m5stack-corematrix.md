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
# M5Stack CoreMatrix

[CoreMatrix](https://docs.m5stack.com/en/core/CoreMatrix) (C158) is an
ESP32-C61HR8 with 16 MB of flash, 8 MB of quad PSRAM and native USB, behind a
16x16 white LED matrix. This profile shows Link's status on the matrix,
draws Muse's images on it (`display.draw_url` and `display.draw_file`,
16x16, a pixel lit where the image is brighter than mid-gray), and uses BtnA
(KEY1) as the setup button: tap to confirm a pairing (or reopen setup
before it's finished), hold for 5 s to reset setup. It advertises as
`MuseGadget-XXXXXX`.

```sh
tools/board.sh m5stack-corematrix build
tools/board.sh m5stack-corematrix flash
```

| State | Matrix |
|---|---|
| Setup, advertising | face |
| Pairing needs a press | flashing `!` |
| Connecting | spinner |
| Connected | check mark |
| Unpaired, error | face with X eyes |

## Code

- `components/m5stack_corematrix`: the board. Its I2C bus, the power chip's
  buttons, and the matrix as an `esp_lcd` panel taking one bit per pixel
  (`tm1680.c`). It builds nothing on other boards.
- `main/matrix_status.c`: Link's status pictures and Muse's images on a 16x16
  one-bit `esp_lcd` panel, in place of `led_status.c`. Only its start-up
  names the board: it makes the panel and gives `button.c` BtnA through
  `button_set_reader()`.
- `sdkconfig.defaults.esp32c61`: settings ESP-IDF loads for any C61 build.

## Hardware

Everything is on one I2C bus, SDA GPIO0 / SCL GPIO1, shared with the Grove
port: the TM1680 LED driver (0x72), the M5IOE1 expander (0x4f), the M5PM1
power chip (0x6e) and the BMI270 IMU (0x68). IOE1's IO4 powers the matrix;
its other pins set the charge current and power the buzzer, SD card and Grove
port, so the firmware changes only IO4. BtnA-C are the PM1's G0-G2, read over
I2C. The TM1680 protocol and frame layout are M5GFX's
[`Panel_TM1680`](https://github.com/m5stack/M5GFX/tree/28d47949bdc84f30881623476f555398e2194313/src/lgfx/v1/panel);
the power chips are in
[M5Unified](https://github.com/m5stack/M5Unified/tree/96576a14247ddc935149145aa6b51943464120f1/src/utility).

The power button is the PM1's own: click to turn on or reset, double-click to
turn off, long-press for download mode if a flash goes wrong.

Not used yet: the IMU, buzzer, microSD, BtnB and BtnC.
