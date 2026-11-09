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

# Seeed reSpeaker XVF3800 with XIAO ESP32-S3 (experimental)

This Link-only profile adds push-to-talk voice notes. Hold XIAO BOOT to
record and release to send. Replies appear as text in the Muse app unless the
existing optional TTS integration is configured. The vendor-controlled LED
ring is not a Muse status indicator in this milestone. Use app prompts and the
serial log for pairing and connection state.

## Hardware and XMOS firmware

Use only the reSpeaker XVF3800 SKU with a pre-soldered XIAO ESP32-S3, 8 MB
flash and 8 MB octal PSRAM. Versions without XIAO are not supported. Attach
the XIAO antenna and use 2.4 GHz Wi-Fi.

Install Seeed's `respeaker_xvf3800_i2s_slave_v1.0.9_16k.bin` baseline image,
or a documented compatible newer same-major 16 kHz I2S-slave image. USB-audio,
48 kHz and I2S-master images are out of scope. Keep the stock two-channel
output routing. The ESP32 accepts firmware with the same major version and a
minimum version of 1.0.9, but VERSION alone does not establish the interface,
channel routing or image family.

The ESP32 supplies the I2S clocks at 16 kHz with stereo 32-bit Philips slots.
Capture uses the right ASR slot. The driver does not reset XMOS, rewrite
output routes, save XMOS configuration or claim verified echo cancellation.
Flashing the XIAO does not update XMOS.

References: [vendor source and firmware](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY),
[XIAO setup](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_getting_started/),
[I2S](https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_i2s/) and
[I2C control](https://wiki.seeedstudio.com/respeaker_xvf_3800_i2c_list/).

## Build and flash

Use ESP-IDF v6.0.3. Connect USB to the small XIAO board, which exposes the
ESP32 USB Serial/JTAG console (`303a:1001`). From `esp32/`:

```sh
tools/board.sh seeed-respeaker-xvf3800 build
idf.py -B build-seeed-respeaker-xvf3800 menuconfig
```

Set your [SDK token](https://gadgets.muse.ai/settings/sdk-tokens) under ESP32
Device SDK, then rebuild and flash:

```sh
tools/board.sh seeed-respeaker-xvf3800 build
tools/board.sh seeed-respeaker-xvf3800 flash /dev/cu.usbmodemXXXX
```

Use the actual port. Linux normally exposes `/dev/ttyACM0`. See the
[flashing guide](../AGENTS.md#flash) before replacing firmware.

## Pairing and controls

In Muse, enable Settings > Devices > Developer mode and add
`MuseGadget-xvf3800-XXXXXX`. Press XIAO BOOT when prompted and finish Wi-Fi
setup. With the XIAO silver label facing you and USB-C at the top, BOOT is the
small button marked **B** to the right of USB-C. **R** resets the ESP32.

- Hold XIAO BOOT to record and release to send.
- The larger board's hardware mute is read through X0D30. Muted or unreadable
  audio is dropped after every capture chunk as well as before a turn.
- While hardware mute is active, XIAO BOOT keeps its setup role. A five-second
  hold forgets pairing and Wi-Fi.
- X0D31 controls the amplifier, active low. Initialization turns it off.
- Volume uses only the AIC3104's digital mute and attenuation registers. It
  does not enable analog boost or rewrite output routes.
- Calls remain half duplex because this profile does not claim a verified AEC
  playback-reference route.

## Validation limits

No physical-board validation is claimed. Host tests cover control framing,
version policy, pin configuration, PCM conversion, mute and amplifier policy,
volume and failure behavior. Clean builds do not establish audible channel
content or timing. Hardware checks still needed include pairing, Wi-Fi,
push-to-talk, right-slot intelligibility, playback, volume zero, mute during
capture, amplifier sequencing and reconnect. The added mute read's effect on
capture cadence also needs measurement on hardware.
