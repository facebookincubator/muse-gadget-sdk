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
# Muse voice gadget

This device is a Muse Gadget for voice: a Home Assistant Voice Preview Edition
or a Seeed reSpeaker Lite running the Muse Gadget SDK firmware. It pairs with
the Muse app over Bluetooth, joins Wi-Fi and keeps an encrypted Link session
with Muse. Muse uses it only through the commands it lists; each command's
description says what it does and returns.

## Status light

The LED ring (Voice PE) or RGB LED (reSpeaker Lite) shows its state:

| Colour | Meaning |
|---|---|
| Orange, breathing | Waiting for setup in the Muse app |
| Blue, breathing | Pairing needs a press on the device |
| Blue | Joining Wi-Fi or connecting to Muse |
| Green | Connected |
| Yellow, blinking | Reconnecting |
| Purple | Not paired |
| Red, blinking | Error |

## Controls

- The talk button is the centre button on the Voice PE and the XIAO's BOOT
  button on the reSpeaker Lite. Once it's connected, holding it records a
  voice note for Muse, sent on release. Muse answers in the Muse app.
- With the mute switch on, the talk button does setup instead: a short press
  confirms a pending pairing, and a 5-second hold forgets the pairing and
  Wi-Fi so it can be set up again. Before it's set up, it does setup anyway.
- On the Voice PE, the dial sets the speaker volume, shown on the ring.

## Using its commands

- If it lists `voice.configure`, the volume it sets is kept across restarts.
- If it lists `device.discover`, a scan takes up to 90 seconds: work from the
  last result rather than starting another.
- If it lists `ble.*` commands, run them one at a time, and connect only to
  devices the user means.

## Limits

- It has no screen, and no shell or files Muse can reach: it does only what
  its commands offer.
- Commands work only while it's connected (green).
