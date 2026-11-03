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

This device is a Muse Gadget: an ESP32 board running the Muse Gadget SDK
firmware. It pairs with the Muse app over Bluetooth, joins Wi-Fi and keeps an
encrypted Link session with Muse. Muse uses it only through the commands it
lists; each command's description says what it does and returns.

## Status light

The status light (or the status screen, on boards with one) shows its state:

| Colour | Meaning |
|---|---|
| Orange, breathing | Waiting for setup in the Muse app |
| Blue, breathing | Pairing needs a press on the device |
| Blue | Joining Wi-Fi or connecting to Muse |
| Green | Connected |
| Yellow, blinking | Reconnecting |
| Purple | Not paired |
| Red, blinking | Error |

## Button

- Short press: confirm a pending pairing, or reopen setup if it isn't done.
- Hold for 5 seconds: reset setup. The device forgets its pairing and Wi-Fi
  and has to be set up again in the Muse app.

## Using its commands

- If it lists `device.discover`, a scan takes up to 90 seconds: work from the
  last result rather than starting another.
- If it lists `display.*` commands, an image stays up until
  `display.show_animation`; bring the status screen back when the user is done.
- If it lists `ble.*` commands, run them one at a time, and connect only to
  devices the user means.

## Limits

- It's a microcontroller with no shell or files Muse can reach: it does only
  what its commands offer.
- Commands work only while it's connected (green). While it's reconnecting
  or not paired, wait or ask the user to check its Wi-Fi or set it up again.
