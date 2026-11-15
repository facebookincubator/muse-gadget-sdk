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

A speaker-and-microphone device for talking to Muse, with no screen: a Home
Assistant Voice Preview Edition, a Seeed reSpeaker Lite or a Seeed reSpeaker
XVF3800 running the Muse Gadget SDK firmware. Muse uses it through the commands it lists.

## What it can do

- People talk to Muse from it: holding the talk button (the centre button on
  the Voice PE, the XIAO's BOOT button on the reSpeakers) records a voice
  note, sent on release. Muse answers in the Muse app.
- The dial sets the speaker volume on the Voice PE. If it lists
  `voice.configure`, the volume it sets is kept across restarts.
- If it lists `device.discover`, it's a bridge to the home network. The scan
  runs from the gadget and takes up to about 90 seconds: work from the last
  result rather than starting another. Muse can reach the devices it finds
  through the gadget's network tunnel.
- If it lists `ble.*` commands, it can scan for and talk to nearby Bluetooth
  LE devices: one at a time, and only devices the user means.

## Status light

The LED ring (Voice PE) or RGB LED (reSpeaker Lite). The reSpeaker XVF3800
shows no state: its ring isn't a status light, so check the Muse app.

| Colour | Meaning |
|---|---|
| Green | Connected |
| Yellow, blinking | Reconnecting |
| Red, blinking | Error |
| Any other colour | It needs setup in the Muse app |

## Recovery

- With mute on, holding the talk button for 5 seconds forgets its
  pairing and Wi-Fi; set it up again in the Muse app.

## Limits

- It has no screen, and no shell or files Muse can reach: it does only what
  its commands offer.
- Commands work only while it's connected (green, on boards with a light).
