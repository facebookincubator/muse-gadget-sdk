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

# livebench: latency and quality of the live camera path

Measures what a viewer of the gadget gets from Muse Gadgets Test's live path on
repeatable, high-motion input, instead of whatever the camera happens to
see:

- **Input:** a raw clip played through the app's own camera path
  (`--es source file:<clip>.yuv`, 640x480 I420 at 30 fps, looped): the same
  crop and scale, encoder, frame skipping, buffering and link as the camera.
  The xiph 4CIF sequences (`soccer`, `crew`, `city`, `ice`; 60 fps, every
  other frame, area-scaled to 640x480) are good stand-ins for a phone
  pointed at something moving.
- **Latency, capture to shown:** the gadget echoes in its buffer messages
  the number of the last frame it drew and how long ago (`last_frame`,
  `last_age_ms`, `../../../../protocols/VIDEO.md`); the app
  (`LatencyMeter.java`) subtracts that frame's capture time. With
  `--ez latlog true` it logs one `lf <frame> <total> <capture->encoded>
  <encoded->sent> <sent->shown>` line per frame (ms).
- **Quality, at the panel:** with `--ez benchdump true` the app writes every
  access unit it sends (`files/bench/enc.bin`, u32 length + u32 source frame
  + AU). `score.py` decodes them with the gadget's own decoder (host build of
  tinyh264: run `./build.sh` here once), scales each frame up to the panel
  (bilinear; `PANEL`, default two times the picture) and compares it inside
  the circle with the source area-scaled to the panel, at every source frame: a skipped frame
  counts as the previous one still on screen. So runs at different encode
  sizes, filters or encoders compare directly. It reports PSNR (from the
  mean MSE), SSIM, the steady-state MSE (from 1 s in), the kb/s actually
  sent and the frames skipped.

```sh
adb push soccer_640x480.yuv /sdcard/Android/data/com.musegadgets.test/files/soccer.yuv
tools/livebench/build.sh
CLIPS=~/clips tools/livebench/run.sh x264 soccer 232 --ez adaptive false --ei kbps 1000
CLIPS=~/clips tools/livebench/run.sh hw soccer 232 --ez adaptive false --ei kbps 1000 --es sw hw
```

Fix the bitrate (`--ez adaptive false --ei kbps N`) when comparing encoders
or filters, or the link decides the bitrate. Keep the phone and gadget in
the same place between runs; run each case twice (`LOOPS`).

Not modeled: the gadget decodes but doesn't draw a frame when a newer one is
already waiting (late frames, live streams only): such a frame is scored as
shown.
