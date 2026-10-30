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

# Seeed reSpeaker Lite with XIAO ESP32-S3 (experimental)

This profile adds an RGB status LED and push-to-talk voice notes. Hold the
XIAO BOOT button, speak, then release; replies appear as text in the Muse app.
It uses the XMOS-processed microphone channel over 16 kHz I2S.

## Before flashing

Use the reSpeaker Lite with an attached XIAO ESP32-S3, 8 MB flash and 8 MB
octal PSRAM. Attach the XIAO's external antenna and use 2.4 GHz Wi-Fi.

The XMOS must run Seeed's **16 kHz I2S firmware v1.0.9**. The USB audio and
48 kHz I2S images do not match this profile. If needed, install the I2S image
through the larger board's USB port using [Seeed's firmware instructions](https://wiki.seeedstudio.com/reSpeaker_usb_v3/).
The ESP32 flash command below does not update XMOS.

Hardware references: [Seeed guide](https://wiki.seeedstudio.com/xiao_respeaker/),
[pinout, I2C examples and XMOS images](https://github.com/respeaker/reSpeaker_Lite),
[I2S recording example](https://wiki.seeedstudio.com/respeaker_record_and_play/).

## Build and pair

Use ESP-IDF v6.0.1. Connect USB to the **small XIAO board**, which exposes the
ESP32's USB Serial/JTAG console (`303a:1001`). From `esp32/`:

```sh
tools/board.sh seeed-respeaker-lite build
idf.py -B build-seeed-respeaker-lite menuconfig
```

In menuconfig, set your [SDK token](https://gadgets.muse.ai/settings/sdk-tokens)
under ESP32 Device SDK. Then rebuild and flash:

```sh
tools/board.sh seeed-respeaker-lite build
tools/board.sh seeed-respeaker-lite flash /dev/cu.usbmodemXXXX
```

Use the actual serial port; Linux normally exposes `/dev/ttyACM0`.
See [the flashing guide](../AGENTS.md#flash) for identification and backup steps.

In Muse, enable Settings > Devices > Developer mode and add
`MuseGadget-respeaker-XXXXXX`. Press XIAO BOOT when prompted, then finish
Wi-Fi setup. With the XIAO's silver label facing you and USB-C at the top,
BOOT is the tiny button to the right of USB-C, marked **B**; **R** is RESET.

## Controls and limitations

- Hold XIAO BOOT to record, release to send. Replies are text in the phone app;
  spoken replies require a separate TTS integration.
- The larger board's Mute button retains its hardware role. When muted,
  XIAO BOOT has its setup role: a five-second hold forgets pairing and Wi-Fi.
- The larger board's Usr button is not handled by this profile.
- XMOS initializes the codec and owns amplifier/headphone routing. The
  speaker path converts the player's 48 kHz stereo stream to 16 kHz.

Tested on hardware with XMOS 1.0.9: boot, 8 MB PSRAM, microphone samples,
physical pairing confirmation, Wi-Fi, Muse connection, push-to-talk and
replies in the phone app. Speaker playback, the physical mute toggle and
setup reset have not been verified on hardware. OTA is disabled by default.
