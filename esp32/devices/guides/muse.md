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
# Muse Gadget with a screen

An ESP32 board running the Muse Gadget SDK firmware with its own screen: it
shows Muse's avatar and, on boards with a microphone, people talk to Muse
from it with voice notes whose replies show on the screen. Muse uses it
through the commands it lists.

## What it can do

- On boards with a microphone, people talk to Muse from it: holding the talk
  button (or the on-screen microphone on touch-only boards) records a voice
  note, sent on release. On
  boards set to toggle recording, one press starts the note and the next
  sends it. Replies show on the screen.
- If it lists `display.*` commands, Muse can show images on its screen. An
  image replaces the usual screen until `display.show_animation` or a press
  on the device; bring it back when the user is done.
- If it lists `camera.capture`, it has a camera Muse can take a still photo
  with when the user asks.
- If it lists `sensors.read`, it reports the room's air readings.
- If it lists `device.discover`, it's a bridge to the home network. The scan
  runs from the gadget and takes up to about 90 seconds: work from the last
  result rather than starting another. Muse can reach the devices it finds
  through the gadget's network tunnel.
- If it lists `ble.*` commands, it can scan for and talk to nearby Bluetooth
  LE devices: one at a time, and only devices the user means.
- `device.health` reports battery and charging on boards with a battery.

## Controls

- The talk button, or the on-screen microphone on touch-only boards, where
  the board has a microphone.
- The settings menu, with the device's options.

## Recovery

- Settings > Reset pairing forgets Wi-Fi and the pairing; set it up again in
  the Muse app.
- If it's frozen, turn it off and on again.

## Limits

- It has no shell or files Muse can reach: it does only what its commands
  offer.
- Commands work only while it's connected.
