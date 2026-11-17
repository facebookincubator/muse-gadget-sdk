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
# M5Stack CoreMatrix

A Muse gadget with a 16 by 16 white LED matrix that shows the link's state
as a picture: a face while it waits for setup, a flashing exclamation mark
when a pairing needs confirming, a spinner while it connects, a check mark
when it is connected, and a face with crossed-out eyes on an error.

It can also show a 16 by 16 picture with each pixel either lit or dark,
which replaces the status until display.show_animation. Lit pixels are
white LEDs: icons, symbols and simple shapes work, but words, photos and
replies don't fit.

BtnA, labelled KEY1, is the setup button: it confirms a pending pairing,
otherwise a tap retries the connection, a double tap rescans Wi-Fi, and a
five-second hold resets setup. The separate power button turns it on or
resets it with a click, turns it off with a double click, and a long hold
starts download mode for flashing. BtnB and BtnC do nothing.

It has no microphone or speaker for voice, and its motion sensor, buzzer
and microSD slot aren't used.
