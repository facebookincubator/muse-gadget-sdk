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

# Seeed reSpeaker Flex with XIAO ESP32-S3 (experimental)

This Link-only profile supports the circular and linear reSpeaker Flex
geometries for push-to-talk voice notes. Hold XIAO BOOT to record and release
to send. Replies appear as text in the Muse app unless the existing optional
TTS integration is configured. The green power LED remains power-only. Use
app prompts and the serial log for pairing and connection state.

## Hardware and XMOS firmware

Use only a SKU with a pre-soldered XIAO ESP32-S3, 8 MB flash and 8 MB octal
PSRAM. These are `-C4-1` for circular and `-L4-1` for linear. The `-C4-0` and
`-L4-0` SKUs without XIAO are not supported. Attach the XIAO antenna and use
2.4 GHz Wi-Fi.

Install the geometry-matched 16 kHz, two-channel I2S-slave image with stock
output routing. The v1.0.4 baselines are:

- Circular: `respeaker_flex_i2s_c16k2ch_v1.0.4.bin`.
- Linear: `respeaker_flex_i2s_l16k2ch_v1.0.4.bin`.

A documented compatible newer same-major image with the same interface,
geometry, sample rate and channel count may also work. USB-audio, 48 kHz and
mismatched-geometry images are out of scope. The ESP32 accepts the same major
version at or above 1.0.4, but VERSION alone does not establish the image
family, geometry or output routing.

The ESP32 supplies the I2S clocks at 16 kHz with stereo 32-bit Philips slots.
Capture uses the left processed slot. The driver does not reset XMOS, rewrite
output routes, save XMOS configuration or claim verified echo cancellation.
Flashing the XIAO does not update XMOS.

References: [vendor source and firmware](https://github.com/respeaker/reSpeaker_Flex),
[XIAO setup](https://wiki.seeedstudio.com/respeaker_flex_xiao_introduction/),
[I2S](https://wiki.seeedstudio.com/respeaker_flex_xiao_i2s/) and
[GPIO ownership](https://wiki.seeedstudio.com/respeaker_flex_xiao_gpio_control/).

## Build and flash

Use ESP-IDF v6.0.3. Connect USB to the small XIAO board, which exposes the
ESP32 USB Serial/JTAG console (`303a:1001`). From `esp32/`:

```sh
tools/board.sh seeed-respeaker-flex build
idf.py -B build-seeed-respeaker-flex menuconfig
```

Set your [SDK token](https://gadgets.muse.ai/settings/sdk-tokens) under ESP32
Device SDK, then rebuild and flash:

```sh
tools/board.sh seeed-respeaker-flex build
tools/board.sh seeed-respeaker-flex flash /dev/cu.usbmodemXXXX
```

Use the actual port. Linux normally exposes `/dev/ttyACM0`. See the
[flashing guide](../AGENTS.md#flash) before replacing firmware.

## Pairing and controls

In Muse, enable Settings > Devices > Developer mode and add
`MuseGadget-flex-XXXXXX`. Press XIAO BOOT when prompted and finish Wi-Fi
setup. With the XIAO silver label facing you and USB-C at the top, BOOT is the
small button marked **B** to the right of USB-C. **R** resets the ESP32.

- Hold XIAO BOOT to record and release to send.
- No equivalent hardware mute circuit is documented. This profile does not
  read X0D30 as mute and always reports the microphones unmuted.
- XMOS owns the amplifier. The ESP32 amplifier hook is a no-op because the
  Flex PA polarity is not established for this integration.
- Volume uses only the AIC3104's digital mute and attenuation registers. It
  does not enable analog boost or rewrite output routes.
- Calls remain half duplex because this profile does not claim a verified AEC
  playback-reference route.
- The Flex **SafeMode (Boot)** button is attached to XMOS and is for firmware
  recovery. It is not the XIAO BOOT button used by Muse.
- Once voice is ready, XIAO BOOT is the talk control. This profile does not
  claim a ready-state five-second BOOT reset gesture. Unpair through the Muse
  app instead.

## Validation limits

No physical-board validation is claimed. Host tests cover control framing,
version policy, pin configuration, PCM conversion, the absence of mute and PA
traffic, volume and failure behavior. Clean builds do not establish audible
channel content or XMOS-owned PA behavior. Hardware checks still needed
include pairing, Wi-Fi, push-to-talk, left-slot intelligibility on both
geometries, playback, volume zero and reconnect.
