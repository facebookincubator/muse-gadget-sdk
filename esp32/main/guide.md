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
# Muse Gadget

An ESP32 board running the Muse Gadget SDK firmware, connected to Muse over
Wi-Fi. Muse uses it through the commands it lists.

## What it can do

- If it lists `device.discover`, it's a bridge to the home network. The scan
  runs from the gadget and takes up to about 90 seconds: work from the last
  result rather than starting another. Muse can reach the devices it finds
  through the gadget's network tunnel.
- If it lists `display.*` commands, it has a screen Muse can draw on. An image
  stays up until `display.show_animation`; bring the status screen back when
  the user is done.
- If it lists `sensors.read`, it reports the room's air readings.
- If it lists `ble.*` commands, it can scan for and talk to nearby Bluetooth
  LE devices: one at a time, and only devices the user means.
- `device.health` covers its battery, power and network.

## Status

The status light shows the state as a colour. Boards with a status screen show
the colour or, on e-paper and small text screens, the words:

| Light | Words | Meaning |
|---|---|---|
| Green | Connected | Connected |
| Yellow, blinking | Reconnecting | Reconnecting |
| Red, blinking | Error | Error |
| Any other colour | Any other words | Starting up, or it needs setup in the Muse app |

## Recovery

- If it stops responding, unplug it and plug it back in.
- Holding the button for 5 seconds forgets its pairing and Wi-Fi; set it up
  again in the Muse app. If the button also records voice notes, a hold
  records instead while voice is connected: switch the board's mute on first,
  or, on a board without one, hold it while voice isn't connected.

## Limits

- It has no shell or files Muse can reach: it does only what its commands
  offer.
- Commands work only while it's connected (green, or "Connected").
