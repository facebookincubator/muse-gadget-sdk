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
# Waveshare ESP32-S3-Touch-AMOLED-1.75C

A round touch AMOLED gadget with a microphone, speaker and battery. It shows Muse's avatar; people talk to Muse from it with voice
notes, and replies show on screen and play aloud with the speaker on.

## What it can do

- Hold the top button, speak, and release to send a voice note. Replies
  show as captions on the screen.
- Swipe right from the main screen to reread the latest reply; scroll
  vertically, and swipe left to return. The saved answer clears on
  reboot.
- When Muse asks a question with options, the choices appear on the
  screen; a held touch on a choice answers it.
- If it lists `display.*` commands, Muse can show images on its screen
  until `display.show_animation` or a press on the device.
- If it lists `device.discover`, it's a bridge to the home network. The
  scan takes up to about 90 seconds: work from the last result rather
  than starting another.
- If it lists `ble.*` commands, it can talk to nearby Bluetooth LE
  devices: one at a time, and only devices the user means.
- `device.health` reports its battery level and charging state.

## Controls

- Top button (upper right side): hold to record a voice note, release to
  send. A press that only wakes the screen does not record.
- Bottom button (lower right side): a short press sleeps or wakes the
  screen, a 1.5 second hold powers off, and two quick presses toggle
  Bluetooth phone setup.
- Drag up or down on the avatar to change the volume; the setting saves
  when the finger lifts, and dragging to zero mutes the speaker.
- Swipe sideways to move between the Latest reply, Main and Settings
  screens; the three dots at the bottom show which one is up.

## Recovery

- Settings > Reset pairing forgets Wi-Fi and the pairing; set it up again
  in the Muse app.
- If it's frozen, hold the bottom button to power it off, then press the
  top button to turn it on again.

## Limits

- It has no shell or files Muse can reach: it does only what its
  commands offer.
- Commands work only while connected.
