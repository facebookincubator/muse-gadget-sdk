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

# OSTB EchoEar-2ST (experimental)

This profile targets the measured **OSTB EchoEar-2ST** unit: ESP32-S3
revision 0.2, **16 MB quad flash in DIO mode**, and **8 MB octal PSRAM at
80 MHz**. It runs the round LVGL UI with touch controls and ES8311/ES7210
audio. The observed memory differs from the generic N32R8 box label; check
the actual hardware before using this profile. It does not claim compatibility
with all EchoEar variants or stock ESP-VoCat hardware.

References: [official product](https://docs.yishierniao.cn/details/ai/echoear-2st/product.html)
and [user guide](https://docs.yishierniao.cn/details/ai/echoear-2st/user_guide.html).
The [public ESP-VoCat source](https://github.com/78/xiaozhi-esp32/blob/0d576d3d4c049c6f55eaf879725dc23e516511b4/main/boards/espressif/esp-vocat/esp_vocat.cc)
supplies the 184-command ST77916 initialization table, whose functional
sequence matches the original device. Its generic GPIO map does not match
this unit. The table in
[`ostb_echoear_display_init.h`](../components/muse/boards/ostb_echoear_display_init.h)
retains the MIT notice of Shenzhen Xinzhi Future Technology Co., Ltd. and
Project Contributors; it is not Apache-licensed.

## Hardware map

Pins below are specific to the measured unit and the board driver
[`board_ostb_echoear_2st.c`](../components/muse/boards/board_ostb_echoear_2st.c).
I2C addresses are 7-bit.

| Function | Wiring / configuration |
|---|---|
| Display | 1.85-inch round ST77916 LCD, 360×360, QSPI at 40 MHz, RGB565 |
| Display bus | CS GPIO3, CLK GPIO8, D0/D1/D2/D3 GPIO4/5/6/7 |
| Display reset | GPIO9, active low |
| Backlight | GPIO41, noninverted PWM, 20 kHz |
| Shared I2C | SCL GPIO11, SDA GPIO12, 400 kHz device transfers |
| Touch | Address `0x15`; single-finger register-2 packets, polled by LVGL |
| Touch interrupt | GPIO42; not used by this driver |
| Speaker codec | ES8311 at `0x18` |
| Microphone ADC | ES7210 at `0x40`; MIC1/MIC2 stereo capture, mixed by Muse |
| Audio I2S | MCLK GPIO10, BCLK GPIO15, WS GPIO16, DOUT GPIO14, DIN GPIO13 |
| Speaker amplifier | GPIO18 |
| Console | Native USB Serial/JTAG, VID:PID `303A:1001` |
| Power | Physical bottom button; no verified software latch |

The touch decoder rejects multi-touch and out-of-range coordinates. Touch
release or loss releases push-to-talk. The overlay's GPIO0 setting is not a
physical talk-button mapping: this board's `poll_buttons`
returns no GPIO button events. Battery telemetry, IMU and SD wiring/support
are unverified and are not enabled by this port.

## Build

Activate **ESP-IDF v6.0.1** and run from `esp32/`:

```sh
tools/muse/board.sh build echoear
```

The alias `echoear` selects profile `ostb-echoear-2st`, target `esp32s3`, and
build directory `build-muse-ostb-echoear-2st`. It loads these files in order:

1. `sdkconfig.defaults`
2. `devices/sdkconfig.muse`
3. `devices/sdkconfig.muse-ostb-echoear-2st`

The equivalent manual build is:

```sh
idf.py -B build-muse-ostb-echoear-2st -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-ostb-echoear-2st/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-ostb-echoear-2st" build
```

The profile uses the 16 MB `partitions_muse.csv` layout. Follow the
[SDK setup instructions](../README.md#2-build) for local configuration before
pairing. Boards share `managed_components/` and `dependencies.lock`; do not
build different profiles concurrently.

The USB ID is shared by other Espressif boards and cannot identify an EchoEar
on its own. The Muse tools register `OSTB EchoEar-2ST` as alias `echoear` for
port selection and avatar setup. Identify the actual target before flashing;
when several native USB boards are attached, pass the intended port explicitly.

## Controls

- In Muse's setup flow, pick `MuseGadget-XXXXXX`. Enter any displayed pairing
  code on the phone. When the app asks for device confirmation, explicitly
  press the screen; confirmation goes through the existing input handler.
- Once paired, hold the screen microphone to record, then release it to send.
  Settings use the touch screen. There is no physical talk or menu button.
- Use the physical bottom button for power. Firmware `power_off` returns
  `ESP_ERR_NOT_SUPPORTED`; use the bottom button to power off.
- The GPIO button's five-second setup-reset gesture does not apply here.

## Recovery preparation

Keep a full original flash backup outside Git before replacing firmware.
These commands access hardware; the first enters the ROM loader and resets
the device. Replace `PORT` and use a private backup path:

```sh
python -m esptool --chip esp32s3 -p PORT --after no-reset read-flash 0 0x1000000 echoear-original.bin
python -m esptool --chip esp32s3 -p PORT --before no-reset --after no-reset verify-flash 0 echoear-original.bin
```

Keep the device in the loader between reading and verifying: do not power-cycle,
reset or boot the app, which could change NVS and invalidate the comparison.
Verify the backup against the unchanged device before flashing. A full flash
backup contains private device state; keep it and any checksums out of the
contribution. To restore the original layout, use the full backup at offset
zero rather than restoring only the app. For a Muse flash, use
`tools/muse/board.sh flash echoear PORT`, which writes the build's bootloader,
partition table and app at their generated offsets.

## Validation

The EchoEar build passed with ESP-IDF v6.0.1. Its binary is
`0x211000` bytes within a `0x400000`-byte app slot, leaving approximately 48%
free.

Repository checks passed with ESP-IDF v6.0.1: the EchoEar profile, the existing
AIPI full-UI board and the default ESP32-C5 build. All EchoEar overlay settings
were applied. The host suite passed 223 tests without skips; the desktop UI
simulator built with warnings treated as errors and passed its headless CTest.
The focused host test exercises touch packet bounds, press/release/lost-touch
callbacks, rapid taps, and pairing confirmation without recording.

Earlier prototype hardware checks confirmed the panel UI, touch pairing,
input recording, codec initialization and local speaker playback. The final
cleaned port has not been flashed and awaits hardware validation. Prototype
checks do not establish full validation of the final port.

Before treating the port as hardware-validated, verify its boot and memory
configuration; display colors and corner alignment; explicit touch pairing;
recording and local playback with volume/gain changes; screen timeout and
touch wake; and network reconnect. The home-network tunnel, image delivery
and OTA remain configured capabilities without final-branch hardware proof.
The simplified two-microphone stereo capture path has no four-slot TDM or
acoustic echo cancellation integration. No battery, IMU, SD, or software
power-latch behavior is claimed.
