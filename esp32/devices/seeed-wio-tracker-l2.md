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

# Seeed Wio Tracker L2 with the IPS display

This experimental Muse profile targets the base Wio Tracker L2 fitted with the
3.2-inch IPS touchscreen. The board has 16 MB of flash, 8 MB of octal PSRAM
and a speaker connector. Connect a speaker to hear playback.

<img src="https://files.seeedstudio.com/wiki/SenseCAP/Wio_Tracker_L2/3-100026809-Wio-Tracker-L2.jpg" width="480" alt="Base Wio Tracker L2 with its landscape screen, exposed mounting holes and display flex on the right. The photo shows Meshtastic firmware.">

With the display upright and USB-C at the bottom, the right edge carries the
power switch, U/B (User/Boot), microSD slot and RST in that order from the
top. The rear PCB drawing shows this edge on the left. Seeed gives **87.5 ×
69 × 26.6 mm** for the assembled device with its antenna folded. It does
not give separate dimensions for the bare board shown above.
[Seeed specifications and board diagrams][seeed]

## Build and controls

From `esp32/`, activate ESP-IDF v6.0.3 and build the profile:

```sh
tools/muse/board.sh build l2
```

To install the built firmware over native USB Serial/JTAG (`303a:1001`),
use `tools/muse/board.sh flash l2`. Installing Muse replaces the firmware
currently on the board. Before pairing, provision your Muse SDK token over
the USB console as described in the [SDK build guide](../README.md#2-build).
Seeed's [getting started guide][restore] links the Meshtastic flasher for
returning to the mesh firmware.

- Muse uses its standard BLE setup and Wi-Fi connection. Press U/B to
  confirm a pending pairing request. During normal use, hold U/B to
  record a voice note and release to send.
- Use the touchscreen for settings. WAKE controls screen sleep and wake.
  The physical slide switch controls power. RST restarts the board.

## Board wiring

| Peripheral | Connection |
| --- | --- |
| NV3031B IPS | SPI3, 40 MHz QSPI. Clock GPIO42, D0–D3 GPIO41/40/39/38, CS GPIO46. The panel scans in 320 × 240 landscape. |
| Board control | I²C0, SDA GPIO47 and SCL GPIO48. PCA9555-compatible expander at 0x21. P0.4 is kept high, P0.5 supplies the display, P0.6 resets it. |
| Backlight | LP5814 at 0x2c. Four PWM channels drive the backlight. |
| GT911 touch | Address 0x5d. P0.3 selects the address when P1.0 releases reset. Muse polls for touch events. |
| Audio | ES8311 DAC at 0x18 and ES7243E microphone ADC at 0x14. I2S0 MCLK/BCLK/WS on GPIO10/11/12, data to DAC on GPIO16 and from ADC on GPIO15. P1.4 enables the speaker amplifier. |
| Keys | U/B on GPIO0 and WAKE on P0.0, both active low. |

The port follows the [L2 pin assignments][variant] and [expander reset
sequence][init]. It uses the existing ESP LCD SH8601 transport for the
NV3031B's matching command and pixel framing, with the [NV3031B
initialization table][panel] ([license](licenses/LovyanGFX.txt)).
The [L2 IPS touch mapping][touch] and the [community L2 Pro hardware
notes][audio] provide additional references for orientation, backlight and
the microphone slot.

## Validation

The firmware has been compiled for ESP32-S3. On-board checks are pending.
To verify a unit, cold-boot it, check the avatar and all four touch corners,
change brightness, confirm a BLE pairing request with U/B, then send a voice
note over Wi-Fi and listen to the reply through the attached speaker. Repeat
the audio check after letting the display sleep and waking it with WAKE.

The e-paper assembly requires a separate display driver. This profile does
not expose the SX1262, GNSS, microSD or battery gauge to Muse.

[seeed]: https://wiki.seeedstudio.com/meshtastic_wio_tracker_l2_intro/
[restore]: https://wiki.seeedstudio.com/get_started_with_meshtastic_wio_tracker_l2/
[variant]: https://github.com/meshtastic/firmware/blob/2ab8abdebb006e9bd4ef4fea8bdf95a258548583/variants/esp32s3/seeed_wio_tracker_L2/variant.h
[init]: https://github.com/meshtastic/firmware/blob/2ab8abdebb006e9bd4ef4fea8bdf95a258548583/src/platform/extra_variants/seeed_wio_tracker_l2/variant.cpp
[panel]: https://github.com/lovyan03/LovyanGFX/blob/dd228d15a03900ce658fd62e120dc7cc489915dd/src/lgfx/v1/panel/Panel_NV3031B.hpp
[touch]: https://github.com/recrof/QuakeESP32S3/blob/c8178cc6b81f/QuakeESP32S3/main/board_config.h
[audio]: https://github.com/spangap/hw-seeed-l2pro/blob/4fbe5f49eefeae9c90b63381ad7dfde01b9b8b5e/INTERNALS.md
