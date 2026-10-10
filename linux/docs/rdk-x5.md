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

# RDK X5 community deployment notes

The Linux SDK has been deployed on a D-Robotics RDK X5 running Ubuntu
22.04.5 on ARM64. Start with the standard [Linux installation and pairing
instructions](../README.md#install); an SDK token and access to the Muse app
are required. This is a community-tested setup, not an official board support
profile or an offline model deployment.

## Device prerequisites

- A working network connection, DNS and system clock.
- Bluetooth LE and the SDK's distro-provided BlueZ/D-Bus dependencies.
- The Linux account you want Muse commands to run as. The tested image uses
  `sunrise`; select the account deliberately when installing the SDK.

The SDK handles pairing and device commands. Installing it alone does not
provide a graphical avatar interface or a speech synthesis service.

## Optional touchscreen interface

A separate [community RDK X5 UI project](https://github.com/shockley6668/rdk-x5-muse-gadget)
adds a native LVGL/SDL frontend using the Muse pixel avatar and CJK captions,
a Python chat/audio bridge, and optional Edge TTS speech output. It reads the
existing SDK pairing without replacing the `musegadget` service. Follow that
project's prerequisites and review its installer before installing it.

The tested display setup is a 480 x 320 SPI touchscreen with a working
XFCE/Xorg session. Screen and touch drivers must already be installed. The
community installer currently assumes the default `sunrise` account, UID
1000 and display `:0`; other setups require adapting its paths and units.

Two practical display lessons from this setup:

- Start the graphical frontend from XFCE session autostart, after the window
  manager and desktop are ready. Starting it only after the display manager
  service can leave its window hidden behind the desktop.
- Use a modest refresh rate for the small SPI screen. This setup limits avatar
  and display refresh to 8 fps to keep the desktop responsive.

The community backend currently needs root access to read the SDK's pairing.
Its README documents the local caption-state permissions and intended
single-user deployment. It uses SDK internals and cloud chat endpoints, so
upstream changes may require updates. Its extra TTS dependency requires a
network connection and is not part of this SDK.

## Audio and verification scope

The community UI implements hold-to-record and release-to-send voice notes.
Check actual input/output devices in the desktop user's PulseAudio session;
the board's audio connector alone does not establish a working microphone.
A PulseAudio combine sink can route playback to connected jack and Bluetooth
outputs, but routing, latency and recording quality need hardware testing.

Recorded checks include an ARM64 native UI build, a live Muse text reply,
synthetic Mandarin voice upload and reply, TTS generation and software
playback, frontend close/reopen and minimized-window recovery, and a board
reboot with XFCE autostart. These checks do not establish physical microphone
quality or sound heard simultaneously from both outputs.

Model inference runs in Muse's cloud. The X5 provides the device client,
interface and audio plumbing; this setup does not use the board's BPU for
local LLM inference.
