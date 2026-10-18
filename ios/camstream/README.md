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

# Muse Gadgets Camera for iPhone

The iPhone version of [`../../android/camstream`](../../android/camstream):
it streams the camera, colour bars or a sample clip to a Muse gadget's
display over Wi-Fi ([`../../protocols/VIDEO.md`](../../protocols/VIDEO.md)),
with the same buttons, settings, flow control and bitrate adaptation. Each
Swift file is the Android class of the same name: `WifiLink` (Bonjour
`_musevideo._tcp` and TCP, frames inside the gadget's credits),
`CameraStreamer` (AVFoundation frames, cropped, scaled for the panel by
`PanelScaler`, masked to the round display's 4x4 blocks), `ClipStreamer`
(sample clips as keyed and steady loops, a bitrate ladder),
`LatencyMeter`, and `MainViewController` (Android's `MainActivity`).

Swift and a little C (`native/`). You need a Mac with Xcode 16 or later, an
iPhone on iOS 17 or later with Developer Mode on, and an Apple ID: a free one
(Xcode's "Personal Team") is enough.

## In Xcode

1. Open `MuseGadgetsCamera.xcodeproj`.
2. Settings > Accounts: add your Apple ID if it isn't there.
3. Pick your team and a bundle ID of your own (bundle IDs are unique across
   all teams, so with a free team `com.musegadgets.camstream` is likely
   taken): either in the target's Signing & Capabilities, or, to keep them
   out of the project file, in `Local.xcconfig` next to `Config.xcconfig`
   (git-ignored):

   ```
   DEVELOPMENT_TEAM = ABCDE12345
   PRODUCT_BUNDLE_IDENTIFIER = com.yourname.musegadgetscamera
   ```
4. Choose your iPhone and Run. The first launch may need Settings > General >
   VPN & Device Management > your Apple ID > Trust on the phone. A free
   team's app runs for 7 days; Run again to renew it.

The app asks for Local Network and Camera access on first use. The phone and
the gadget must be on the same Wi-Fi network. The scheme's launch arguments
(Product > Scheme > Edit Scheme > Arguments: `-auto YES`, `-target`, `-codec
vp8`, off until ticked) take the options under Using it. The Simulator runs
it too, without a camera: Bars and the sample clips work.

Every Swift file in `Sources/` is in the target (a synchronized folder):
add a file there and it builds. Anything in a `Resources/` folder next to
the project (absent here) is copied into the app as is.

## From the command line

`build.sh` builds the same project with `xcodebuild`, unsigned, then signs
it with your own development certificate and a profile Xcode has already
downloaded for it (`signing.py`: one whose app ID covers the bundle ID and
that lists your iPhone, with its certificate in your keychain; if there's
none, it says how to get one), and installs it:

```sh
xcrun devicectl list devices                 # your iPhone's name or UDID
DEVICE=<udid> BUNDLE_ID=com.yourname.musegadgetscamera ./build.sh install
xcrun devicectl device process launch --device <udid> com.yourname.musegadgetscamera -- -auto YES
```

`BUNDLE_ID` defaults to `Local.xcconfig`'s, then `Config.xcconfig`'s.
`PROFILE` and `IDENTITY` choose a profile and certificate by hand; `SIM=1
./build.sh install` builds for the booted Simulator.

## Encoders

- **The phone's H.264 encoder** (VideoToolbox) is the default: constrained
  baseline, low-latency rate control, no frame reordering, keyframes only
  when the gadget asks. Like Android's hardware path it always deblocks, so
  the gadget's decoder does about 30% more work than with x264, and it has
  no intra refresh.
- **x264** (GPL-2.0, not in this repository; an app that includes it is
  under the GPL, so keep such builds for testing): `native/build_x264.sh`
  builds your checkout for iPhone with `../../android/camstream/jni/x264_mvcheap.py`
  and `native/mgx264.c` into `libmgx264.a`. Settings as on Android: `fast`,
  zerolatency, baseline, one reference, no deblocking, intra refresh.
- **VP8** (libvpx, BSD-3-Clause with a patent grant; not in this repository
  either): `native/build_vpx.sh` builds your libvpx checkout with
  `../../android/camstream/jni/libvpx_mg.py` and `native/mgvpx.c` into
  `libmgvpx.a`. The **H.264 / VP8** button (or `-codec vp8`) picks it for
  gadgets that list it.

```sh
git clone https://code.videolan.org/videolan/x264.git ~/x264
git clone -b v1.16.0 https://chromium.googlesource.com/webm/libvpx ~/libvpx
X264_SRC=~/x264 LIB_DIR=~/mgcam-ios-lib ./native/build_x264.sh
VPX_SRC=~/libvpx LIB_DIR=~/mgcam-ios-lib ./native/build_vpx.sh
DEVICE=<udid> LIB_DIR=~/mgcam-ios-lib ./build.sh install
```

The build scripts write `lib/codecs.xcconfig` (git-ignored), which the
Xcode project includes: device builds link whichever of the two libraries
exist and stub the other (`native/codecs_xcconfig.sh` rewrites it after you
move them). The Simulator always gets the stubs.

## Using it

The buttons are Android's: Camera, Bars, Bunny, Sintel (8 s of the Blender
Foundation's open movies, CC BY 3.0, played from the Internet Archive when
first picked; nothing is stored in the app), Device (gadgets found over
Bonjour; the choice is remembered), Rate (fixed rates for clips and camera,
picture size and frame rate on gadgets past 240 px), H.264 / VP8, Stop. The
yellow line names what streams, the top lines show the link, the encoder
and the gadget's reports each second.

Launch arguments take Android's intent extras (`-name value`; only Xcode
or `devicectl` can pass them): `-target` (part of a gadget's name), `-host`
(an address, no Bonjour), `-auto YES` (stream as soon as the gadget's Phone
camera view is open, without a button; the gadget takes a stream only
then), `-front YES`, `-fps`, `-kbps`, `-maxkbps`,
`-adaptive NO`, `-size`, `-source camera|pattern|clip:<name>`, `-clip`,
`-clipkbps`, `-playout`, `-codec h264|vp8`, `-sw hw|x264`, `-swpreset`,
`-swparams`, `-pf 0`, `-live 0`, `-ahead`, `-behind`, `-latlog YES`,
`-rateclip`, `-ratec`. In Debug builds, `musegadgetscamera://pick/<button>`
presses a button on the running app (`camera`, `pattern`, `clip:bunny`,
`codec`, `stop`; for `xcrun devicectl device process launch --payload-url`);
Release builds ignore it, since any app or web page can open a URL. The log
(as Android's `MuseCam` tag) goes to Xcode's console, or to stdout with
`xcrun devicectl device process launch --console ...`.

Not ported: Android's raw-file test source and its bench dumps
(`tools/livebench` drives the Android app).
