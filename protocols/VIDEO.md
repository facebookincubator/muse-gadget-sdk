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

# Video streaming to a gadget: how it works and why

A phone streams H.264 (or VP8, below) to a gadget with a display; the
gadget decodes it and draws it. This is the design note for anyone changing that path: the phone
side (Muse Gadgets Camera, for Android in `../android/camstream` and for iPhone in
`../ios/camstream`: the same app, class for class), the wire (the frame format and,
for Wi-Fi, the messages below) and the ESP32 side
(`../esp32/components/tinyh264`, `components/muse/muse_video.c` and its
transports). It says what each choice buys, how it was measured, and how to
check a change.

| Where | What |
|---|---|
| this file, Wi-Fi | the Wi-Fi protocol: discovery, messages, credits |
| `../android/camstream/README.md` | the Android app (Android Studio or Gradle), its extras and flow control |
| `../ios/camstream/README.md` | the iPhone app (Xcode), its launch arguments |
| `../esp32/components/tinyh264/README.md` | the decoder and its bit-exact optimizations |
| `../esp32/components/libvpx_vp8/README.md` | the VP8 decoder, the same |
| `../esp32/components/muse_vstream` | the receive ring and frame format every transport shares |
| `../esp32/components/muse/muse_video.c` | decode, pacing and drawing on the ESP32 |
| `../esp32/components/muse/muse_video_wifi.c` | the Wi-Fi transport (TCP and mDNS) |

## The pieces

```
phone: camera -> crop, scale, mask -> H.264 encoder -> frames --+
                                                                 | transport (Wi-Fi: TCP)
gadget: ring (muse_vstream) <- transport's receive task <--------+
        -> decode task (tinyh264, one band per core) -> two picture slots
        -> show task: YUV to RGB565, scaled, straight to the panel
```

The decoder and the drawing don't know what carried the frames. A
transport owns a ring (`muse_vstream_t`), writes what it receives into it,
and gives `muse_video` a `muse_video_transport_t`: how to report room
(credits) after each frame, ask for a keyframe, end a stream, and ask its
client to start or stop when the user opens or closes the view. Wi-Fi is
the transport here (`muse_video_wifi.c`); another adds its own ring and callbacks and calls `muse_video_begin()` the
same way. One stream plays at a time; a second transport asking while one
streams hears busy.

## The budget

- **The decoder has 20-30 ms a frame.** A software H.264 decoder on a
  240-320 MHz MCU, with the pictures in PSRAM. Over Wi-Fi this, not the
  link, sets the bitrate and the picture size: the link carries several
  times what the decoder can use.
- **Small pictures.** 232x232 on the round 466 px panels (Waveshare S3
  1.75C, M5Stack StopWatch), shown two times upscaled; up to 480x480 on the
  ESP-Mosaico (ESP32-S31, two cores at 320 MHz).

So the stream is H.264 constrained baseline (the only profile the small
decoders take), 24-30 fps, with every byte that doesn't reach the viewer's
eye removed before it is encoded, and every frame's worth of buffering
counted as latency.

## Frames

Every transport carries the same stream: frames, each an 8-byte header
then one access unit in Annex B form (start codes; SPS and PPS before every
keyframe), or for VP8 one frame as libvpx writes it (what an IVF file holds,
without IVF's headers):

| Bytes | Field |
|---|---|
| 1 | magic `0x56` (`V`) |
| 1 | flags: bit 0 keyframe |
| 2 | frame number, little-endian, from 0 at the stream's start, wrapping |
| 4 | access unit length, little-endian, not counting the header |

With two **bands** (below), the access unit is the top band's `uint32` LE
length and access unit, then the bottom band's access unit to the end of
the frame. Each band is its own H.264 stream of the picture's width and
the band's height, a multiple of 16 lines, coded independently, so the
gadget decodes them at once on its two cores. A keyframe has every band an
IDR picture.

The gadget's ring (`muse_vstream.c`) takes the bytes as they come and
hands the decoder one whole frame at a time. A damaged header makes it
search for the next one (a header is believed when a start code follows it,
or for VP8 a frame tag that fits the frame and, on a keyframe, the sync
code); the frame after a gap is marked so the decoder
drops it unless it's a keyframe, and asks for one.

## Wi-Fi

The phone and the gadget are on the same network (a gadget running Home
Link has joined it already). The gadget (`CONFIG_MUSE_VIDEO_WIFI`) listens
on TCP port 7766 (`CONFIG_MUSE_VIDEO_WIFI_PORT`) and announces itself over
mDNS as `_musevideo._tcp`, instance name its BLE name
(`MuseGadget-A1B2C3`), with TXT `version=1`, `width`, `height` and
`round` (its display). One client at a time; a client that vanished is
dropped by TCP keepalive (about 11 s). See Trust below for who may
connect and when.

Each message on the connection is one of:

- **a control message:** `0x4A` (`J`), a `uint16` LE length, then that
  many bytes of a JSON object with a `"type"`;
- **a frame** (client to gadget only), starting with its `0x56` magic.

Messages, in the order they usually come:

| Type | From | Fields | Meaning |
|---|---|---|---|
| `hello` | gadget, on connect | `version` (1), `name`, `codecs` (`["h264"]`, `["h264","vp8"]` with `CONFIG_MUSE_VIDEO_VP8`), `display` {`width`, `height`, `round`}, `max` {`width`, `height`, `fps`}, `suggested` {`width`, `height`, `fps`, `kbps`}, `buffer` (ring bytes), `max_playout_ms`, `max_bands`, `band_split_pct` | what it decodes and shows, and the stream it suggests |
| `want` | gadget | `on` | the user opened (true) or left (false) the Phone camera view; sent again after `hello` while the view is open. A client that can stream answers true with `start`, false by stopping |
| `start` | client | `codec` (`"h264"` or, where hello lists it, `"vp8"`), `width`, `height` (the picture after cropping), `fps`, `kbps`, optional `playout_ms`, `bands` | starts a stream (replacing one that's on) |
| `buffer` | gadget | `free`, `received`, `shown`, `dropped`, `decode_ms`, optional `last_frame`, `last_age_ms` | credits and progress: after a start, and after each frame the decoder takes |
| `error` | gadget | `for` (`"start"`), `reason` (`closed`, `busy`, `invalid`, `unsupported`) | the start was refused. `closed`: the Phone camera view isn't open, wait for `want`; `busy` (the screen still waking, another stream on): try again in a second; `invalid`: a value out of range or past `max` |
| `keyframe` | gadget | | make the next frame a keyframe (a frame lost, damaged or too big) |
| `stop` | either | | the stream ends; the gadget stops decoding at once |
| `status` | client | | asks for a `buffer` now; the apps also send it every 4 s while not streaming, to show they're still there |

Unknown types and fields are ignored, so either side can add some.

**Credits.** `received` counts the frame bytes that have reached the ring
since the start, and `free` is the ring's room when the message was sent.
The client keeps what it has sent but the gadget hasn't received under
`free`: `sent - received + next <= free`. TCP would hold it back anyway
(the gadget stops reading when the ring is full), but bytes waiting in
socket buffers are latency. For a live source it also starts a frame only
while what the gadget hasn't decoded (`sent - received` plus
`buffer - free`) is under about 3.5 frames, and skips camera frames
otherwise.

**Latency echo.** `last_frame` and `last_age_ms` say which frame the panel
finished drawing last and how long ago, so the phone measures
capture-to-shown per frame (`LatencyMeter.java`).

**Playout delay.** With `playout_ms` (at most `max_playout_ms`) the gadget
shows frame N at the first frame's arrival + `playout_ms` + N frame
periods, so frames that arrive unevenly (a keyframe that takes several
frame times) still show evenly; a frame later than its time shows at once
and moves the schedule. For pre-recorded clips; live sources leave it 0.

**Power.** While a stream is on the gadget keeps Wi-Fi out of modem sleep
(`muse_wifi_keep_awake()`): asleep, the access point holds every packet
for the next beacon, up to 100 ms of extra latency and jitter.

### Trust

The port is not authenticated or encrypted: anything on the network can
connect, and anything on the network can answer mDNS as a gadget. Both
ends treat what they receive as untrusted and limit what a stranger gets:

- **A stream only while someone is looking.** The gadget takes `start`
  only while the Phone camera view is open on it (`error` `closed`
  otherwise), so nobody else can wake its screen, draw on it, keep its
  radio out of sleep or silence its BLE.
  `CONFIG_MUSE_VIDEO_WIFI_START_ANYTIME` lifts this for automated benches.
- **One client, kept.** A connection must send a whole message within
  5 s; a new connection is turned away while the client has sent
  something in the last 10 s (the apps' `status` every 4 s keeps their
  place), and replaces it only once it has gone quiet. A client that
  stops reading (a send gives up after 300 ms) is dropped.
- **Sizes are the stream's.** `start` values out of range, or past
  `max`, are `invalid`. A frame longer than 64 KB closes the connection.
  The decoders are told the stream's size before they see a byte, and
  refuse an H.264 sequence parameter set or a VP8 keyframe for a larger
  picture (nothing is allocated for it), and tinyh264 refuses sizes whose
  arithmetic would overflow. Drawing reads only what the decoded picture
  holds, and pictures being drawn are never freed under the show task
  (the decoder waits for both before new parameter sets or a keyframe).
  `esp32/tests/test_video_untrusted.py` checks these.
- **The apps** clamp what `hello` claims (sizes, rates, buffer) before
  sizing encoders and buffers from it, and take launch options (Android
  intent extras, iOS's `musegadgetscamera://` URLs) only in debug builds.

What that leaves: someone on the network while the view is open can show
their own video instead of the phone's, and anyone on it can see the
video, which isn't encrypted. Use it on networks you trust.

## Encoding the camera (live)

`CameraStreamer.java`, per frame:

1. **Camera2 frame, center-cropped and scaled for the panel** to the size
   the gadget suggests. A gadget that upscales the picture to its panel
   blurs it, so `PanelScaler` downscales with the least-squares best filter
   for what the panel ends up showing (a 6-tap sharpening filter), rather
   than a box: +0.2-0.7 dB at the panel for the same bits (`--ei pf 0`: the
   2x2 box).
2. **Round-display mask.** On a round display the corners are never seen.
   The phone blacks out only the 4x4 luma blocks wholly outside the circle
   plus 3 px (an upscale reads past the edge, and black there showed as
   dark specks along the rim), not the pixel-exact circle: a mask edge on
   the encoder's 4x4 transform grid costs ~10% fewer bits for the same
   picture inside the circle. Flat black blocks cost almost nothing.
3. **x264 on the phone when the build has it, else the phone's hardware
   encoder.** x264 (`X264.java`, `jni/`; GPL, so it is not in this
   repository: the app's README says how to add it) runs `fast`,
   zerolatency, constrained baseline, one reference, no B frames, no
   lookahead, deblocking off (the gadget's decoder runs ~30% fewer
   instructions; at the panel within 0.1-0.3 dB of deblocked), VBV of
   200 ms: 0.4-2 dB better than the phone's hardware encoder at the same
   bitrate, ~4 ms a frame on a Galaxy S22. The hardware path
   (`MediaCodec`): constrained baseline, CBR, `KEY_LATENCY` 1, no B
   frames, SPS/PPS before every sync frame.
4. **Intra refresh, not periodic keyframes.** A keyframe is 5-8 times a P
   frame. `KEY_INTRA_REFRESH_PERIOD` spreads a stripe of intra macroblocks
   over every frame, refreshing the whole picture every 3 s. A real
   keyframe comes only when the gadget asks for one.
5. **Adaptive bitrate.** Once a second: back off when the link or the
   decoder falls behind (camera frames skipped, the send gate holding
   frames, decode time near the frame period); creep up after calm
   seconds, to at most 5/4 of the gadget's suggestion. 30 fps and low
   latency first, quality second.
6. **Live buffering: frames are skipped, not queued.** Every frame's worth
   of video waiting anywhere is a frame period of latency. The phone keeps
   what the gadget hasn't decoded within ~3.5 frames and what waits on the
   phone within ~2 frames; past that it skips a camera frame, never an
   encoded one, so the encoder's reference chain never breaks. The gadget
   helps from its side by not drawing a frame that a newer, already decoded
   one replaces.

## Encoding clips (pre-recorded video)

A clip (Muse Gadgets Camera's samples: a few seconds of the Blender Foundation's
open movies, fetched when first played, `SampleClips.java`) is decoded
once, area-averaged to the gadget's size, masked to its circle and encoded
ahead of time (`ClipStreamer.java`): twice through one encoder, a keyed
loop (an IDR, then P frames) and a steady loop of P frames that follows on
from either, so a loop wraps without a keyframe; a keyed loop goes out
again every few loops, or when the gadget asks. Past 240 px x264 encodes
them with lookahead and macroblock-tree rate control, and a VBV that holds
any 300 ms to the clip's rate plus 2%. Four bitrates form the clips'
ladder: encoded frames can't be skipped, so a link that falls behind steps
down a level rather than playing in slow motion. Clips ask for a 200 ms
playout delay (300 for big pictures), so the IDR, a few frame times on the
link, never shows late.

## Decoding on the ESP32

`tinyh264` (AOSP h264bsd via udevbe/tinyh264), with bit-exact changes
(406 test streams match upstream and ffmpeg byte for byte): an inlined bit
reader, coefficient clears only for coded blocks, a faster start code
search, and deblocking per macroblock while the pixels are in cache
(15-37% less PSRAM traffic). `CONFIG_MUSE_VIDEO_DECODER_IRAM` puts its hot
code in IRAM: about 44 KB, which on the S3 comes out of the heap a Wi-Fi
and UI build needs, so it's off there and the decoder runs from cache.

`muse_video.c`:

- **Two picture slots and two tasks.** The decode task (first core) takes
  a frame, claims a free slot and hands it to the show task before it
  decodes; the show task (the UI core) draws picture N while N + 1
  decodes.
- **Draw while decoding.** The decoder reports rows as they become final
  (`h264bsdSetProgress`), and the show task draws them as they come: the
  top of the picture is on the panel while the bottom is still decoding
  (received-to-shown 49 -> 30 ms on the StopWatch).
- **Straight to the panel.** LVGL is paused while video plays;
  `muse_lcd_bands_frame` sets one window for the frame and sends it as
  Memory Write / Write Continue pieces, each converted from YUV
  (`muse_yuv.c`, the S3's SIMD in `muse_yuv_pie.S`, the S31's in
  `muse_yuv_pie_s31.S`) and scaled straight into a free DMA buffer while
  the one before is on the wire: two times (232 to 464), 3/2 (320 to 480),
  one to one, or any other factor.
- **Playout pacing** (clips): the decoder works up to a frame ahead and the
  show task keeps the time, so a slow frame borrows the time a quick one
  left.
- **Late live frames are skipped.** A live picture that's late, or whose
  successor is already decoded whole, isn't drawn (it's still decoded: the
  next one predicts from it).
- **Tearing effect.** On panels whose TE line is wired (the Waveshare
  1.75C and 1.75, GPIO13; the ESP-Mosaico, GPIO43) each frame starts at a
  moment its write stays clear of the panel's scan, so it shows no tear
  line (`muse_lcd_bands_set_te`). A whole 466 px frame takes 26 ms to write,
  longer than the panel's 16.8 ms refresh, so it can start only in the
  first 6 ms or so of a scan: at most 29.8 fps, and live pictures reach the
  panel 20-25 ms later than unsynchronised ones (51-61 ms from in to shown
  on the 1.75C instead of about 31; 0.35% of frames still meet the scan).
- **Bands, one per core.** On chips fast enough for big pictures (the
  S31) the bottom band decodes on the UI core while the top decodes on the
  first; the gadget asks for uneven bands (`band_split_pct` 60) because the
  UI core also converts and sends.

## VP8

VP8 is the second codec: no patent pool charges royalties for it, and
libvpx, its reference code, is BSD-3-Clause with the WebM project's patent
grant. A gadget built with `CONFIG_MUSE_VIDEO_VP8` (on with
`CONFIG_MUSE_VIDEO`) lists it in hello, and the client picks it in start
(Muse Gadgets Camera: the H.264 / VP8 button, `--es codec vp8` on Android, `-codec vp8` on iPhone). Frames, bands, credits
and keyframe requests work as with H.264; two bands are two VP8 streams.

- **Decoder:** libvpx v1.16.0's (`../esp32/components/libvpx_vp8`), C only
  and trimmed, with bit-exact speed-ups (one-pass sub-pixel prediction,
  early-out loop filters, skipped zero DC blocks, an inlined bool decoder,
  and on the S3 bilinear prediction on its SIMD) and the same row progress
  as tinyh264, so drawing starts before the frame is done. It matches stock
  libvpx byte for byte on 933 test streams. Four frame buffers in PSRAM
  (0.55 MB at 240 px, 1.8 MB at 480), about 45 KB of flash.
- **Encoder:** libvpx on the phone (`jni/build_vpx.sh`, not in this
  repository: it builds your libvpx checkout), its good-quality mode with no
  lag, version 1 (bilinear prediction: cheap to decode), no loop filter, one
  thread (two cost 4-15% more bits at 232 px). Up to 240 px cpu-used 1 and
  every motion vector, about 19 ms a frame on a Galaxy S22 core; past it
  cpu-used 2 and motion vectors with at most one fractional component
  (`jni/libvpx_mg.py`), two bands encoded side by side. Clips: the same, VBR
  inside a 300 ms buffer.
- **Against H.264** (the x264 settings above), bench clips (xiph.org's crew,
  soccer, city, ice), BD-rate at the same PSNR: at 480 px -37%, -10%, +1%,
  -20%; at 232 px -24%, -4%, +14%, -5%. Decode in QEMU (S31 base ISA, 480 px,
  600 kb/s): 6.0 M instructions a frame against tinyh264's 7.5 M; 1.8 M
  against 2.7 M at 240 px.
- **On the Waveshare S3 1.75C over Wi-Fi** (the default build: decoders
  from flash; 232 px at the suggested 1000 kb/s, fixed; the same clips
  through `tools/livebench`, two passes each, Galaxy S22):

  | | H.264 (x264) | VP8 |
  |---|---|---|
  | decode, mean of crew / soccer / city / ice | 34 / 33 / 32 / 23 ms | 24 / 24 / 25 / 20 ms |
  | capture to shown, median | 146 / 110 / 129 / 69 ms | 77 / 75 / 82 / 67 ms |
  | encoded picture (PSNR at the panel's size) | 37.18 / 35.65 / 32.25 / 39.07 dB | 37.43 / 35.61 / 32.15 / 39.08 dB |

  At the same bitrate the pictures match (within 0.25 dB), but tinyh264
  needs more than a frame period at 1000 kb/s, so frames queue and the
  latency doubles; VP8 decodes inside it. At 350 kb/s (decoders in IRAM)
  VP8 took 13-15 ms against tinyh264's 13-20.

## Sizes and bitrates

**ESP32-S3** (Waveshare S3 1.75C, 240 MHz, octal PSRAM, the decoder from
cache): 232 px, shown two times upscaled. A Galaxy S22 a few metres from the
board on the same access point, x264, the xiph 4CIF soccer and crew clips
looped through the camera path (`tools/livebench`, 20 s each, fixed
bitrate). PSNR is what the panel shows inside the circle, a skipped frame
counting as the one before it still on screen:

| kb/s | decode, ms | frames skipped | frame in to shown, median | PSNR soccer / crew |
|---|---|---|---|---|
| 600 | 21-23 | 0-4 of 600 | 59-66 ms | 34.0 / 34.8 dB |
| 800 | 24-25 | 3 | 66-71 ms | 33.6 / 35.8 dB |
| 1000 | 25-27 | 0-10 | 69-77 ms | 35.2 / 35.9 dB |
| 1500 | 31-33 | 13-31 | 99-144 ms | 33.1 / 34.3 dB |
| 2000 | 34-37 | 58-82 | 163-181 ms | 28.2 / 31.3 dB |

So the S3 suggests 1000 kb/s: about 1 dB over 600 at 30 fps; past that the
decoder falls behind, frames are skipped and the latency climbs. The camera
itself, adaptive bitrate, settles at 1000-1150 kb/s, 28-30 fps, 155 ms from
the sensor to the panel (90 of them in the phone's camera and encoder, 60 on
the link and the board). The decode task takes 75-80% of the first core and
the panel's sends about 25% of the second. With BLE advertising beside the
stream (before the radio hold above), the same 1000 kb/s run showed 22-26
fps with 110-140 ms on the link, and the camera's adaptive bitrate fell to
its 200 kb/s floor.

**ESP-Mosaico** (ESP32-S31): one decoder can't keep 480 px at 30 fps
(QEMU instruction counts: 10.3 M instructions a frame at 600 kb/s, 12-13 M
at the 95th percentile), so the picture goes as two bands, each its own
480 px wide H.264 stream from its own x264 at a share of the bitrate. With
60/40 bands both decode in 22-24 ms. Panel PSNR (RGB, against the 480 px
source; crew / soccer, 150 frames, 30 fps):

| picture | 400 kb/s | 500 kb/s | 600 kb/s |
|---|---|---|---|
| 480, one stream | 28.3 / 29.3 | 29.0 / 30.1 | 29.5 / 30.7 |
| 480, two bands | 28.1 / 29.2 | 28.8 / 30.0 | 29.3 / 30.5 |
| 400, scaled 1.2x | | | 29.6 / 30.3 |
| 320, 3/2 | | | 29.6 / 30.0 |
| 240, 2x | | | 29.0 / 29.0 |

At 600 kb/s 480 px matches 320-400 px on the crowd scene and beats them on
fast motion, and beats 240 px by 0.5-1.6 dB; at 750 kb/s it gains about
another 1 dB (crew 32.6 -> 33.6, soccer 33.0 -> 34.0). Bands cost 0.13-0.2
dB. Over Wi-Fi the S31 suggests 1000 kb/s at 480 px, a little over what it
decoded in time over BLE (750 kb/s in 24 ms); not yet measured over Wi-Fi.

**Decoder-friendly motion vectors.** Most of what's left is motion
compensation, and the positions with both components fractional cost the
most (6-tap filters both ways). Muse Gadgets Camera's x264 can keep its motion
search off them (`mv-cheap=2`, added to x264 at build time by
`../android/camstream/jni/x264_mvcheap.py`; on by default past 240 px):
25% fewer decode instructions (10.3 -> 7.8 M at 480 px, 600 kb/s) for
0.18-0.23 dB.

**Motion compensation on the S31's SIMD** (`CONFIG_MUSE_VIDEO_SIMD_MC`, off
by default): correct (it passes its first-use check on the board) but
slower than the C there, 40 ms a 480 px frame in two bands against 28, so
experimental.

## Checking a change

- **Host tests:** `esp32/tests/test_muse_video.py` (the ring and the Wi-Fi
  messages, H.264 and VP8), `test_video_untrusted.py` (hostile sizes and
  the port's guards), `test_tinyh264_pie.py` (the S31 SIMD model),
  `test_libvpx_vp8.py` (the VP8 decoder against libvpx's output).
- **Live:** Muse Gadgets Camera's screen and log (Android: `adb logcat -s
  MuseCam`; iPhone: Xcode's console, or `MuseCam:` lines in the device log) show
  each second the send rate, frames in flight and queued, camera-to-encoder
  time, the gate and stall time, the gadget's shown/dropped/decode from its
  buffer messages, and capture-to-shown latency. The gadget logs its side
  every 2 s on `muse_video` (decode time, received-to-shown, kb/s, shown,
  dropped, skipped, each core's load).
- **Repeatable input:** `../android/camstream/tools/livebench` plays a raw
  clip through the app's camera path and scores what the gadget shows.
- A change that helps the bits but costs the decoder shows up as decode
  time near the frame period and frames dropped.
