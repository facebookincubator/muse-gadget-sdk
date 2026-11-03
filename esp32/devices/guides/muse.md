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

This device is a Muse Gadget running the Muse Gadget SDK firmware with its
on-screen interface. It pairs with the Muse app over Bluetooth, joins Wi-Fi
and keeps an encrypted Link session with Muse. Muse uses it only through the
commands it lists; each command's description says what it does and returns.

## Screen and controls

- The screen shows the connection state: orange while waiting for setup, blue
  while connecting, green when connected, yellow when reconnecting, red on an
  error. Until it's paired, it also shows the name to pick in the Muse app.
- A pending pairing is confirmed on the device: the talk button, the Enter key
  on boards with a keyboard, or a tap on the screen.
- On boards with a microphone, holding the talk button (or the on-screen
  microphone on touch-only boards) records a voice note for Muse, sent on
  release. Replies show on the screen.
- The settings menu has the device's options, including Reset pairing, which
  forgets Wi-Fi and the pairing so it can be set up again.

## Using its commands

- If it lists `device.discover`, a scan takes up to 90 seconds: work from the
  last result rather than starting another.
- If it lists `display.*` commands, bring the usual screen back with
  `display.show_animation` when the user is done; a press on the device also
  brings it back.
- If it lists `ble.*` commands, run them one at a time, and connect only to
  devices the user means.

## Limits

- It's a microcontroller with no shell or files Muse can reach: it does only
  what its commands offer.
- Commands work only while it's connected. While it's reconnecting or not
  paired, wait or ask the user to check its Wi-Fi or set it up again.
