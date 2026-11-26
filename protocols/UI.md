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

# UI streaming: a phone's picture on a gadget, and the gadget's touch back

The phone shows a square of its screen, or a web page it renders offscreen,
on a gadget's round panel at the panel's own resolution, and the gadget's
touchscreen drives the phone back: as touch events the app maps into its own
views, and as a Bluetooth HID device the phone's OS takes directly. The wire
contract is in [`mgcommands.h`](mgcommands.h) (UI streaming, UI input); video
in general is [`VIDEO.md`](VIDEO.md). This page is the design and the numbers.

## Trust

A UI stream rides the gadget's Muse link, as video over BLE does
([`VIDEO.md`](VIDEO.md), Trust): the link is neither bonded nor encrypted.
On a set-up gadget a UI stream needs the connection's token proof
(`mgcommands.h`, Token proof), which a relay in range can forward (its
known limit); a gadget that isn't set up takes one from any phone that
connects. A nearby sniffer can read the streamed screen. Touch events go only to the
connection that turned them on (`touch_enable`), unencrypted like the
frames; HID reports need an encrypted, bonded link (HID, below). Stream a
screen only in a trusted environment.

## The picture

UI content is mostly static (text, controls, a web page) and must be sharp,
where camera video moves all the time and can be soft. So a UI stream:

- **is the panel's own size**, shown one to one: 466x466 on the M5Stack
  StopWatch, 480x480 on the ESP-Mosaico (camera video is smaller, 240 px on
  the StopWatch, scaled up). H.264 codes whole macroblocks, so 466 is coded
  as 480 (and a 360 px panel as 368), and the SPS crops the rest.
- **sends a frame only when the picture changes.** A still picture costs
  nothing on the link and nothing on the gadget (no decoding, the panel
  keeps its contents), so a static screen is 0 kb/s.
- **sharpens after a change** (progressive refinement): the change goes out
  at once at a coarse quantizer so it shows quickly, then, once the picture
  has held still briefly, the same picture is sent again as P frames at
  finer quantizers. Each refinement codes only the difference to what the
  gadget shows, so it is small, and text ends close to lossless.
- **is H.264 Baseline with one reference frame**, decoded by the same
  decoders as camera video: no new codec on the gadgets. One keyframe at
  the start and on request_keyframe; no intra refresh (Data is reliable).

The settings each gadget uses, and the measurements behind them, are under
[Per gadget](#per-gadget).

### Square to disc (the fisheye)

A square shown on a round panel loses its corners. With the warp on, the
phone maps the whole square onto the disc instead, in a GPU shader (never on
the gadget). The mapping is the elliptical grid mapping, chosen because it
has a closed-form inverse, which the touch path needs. With `x, y` in the
square and `u, v` in the disc, both in [-1, 1]:

    disc from square:  u = x * sqrt(1 - y*y/2)
                       v = y * sqrt(1 - x*x/2)
    square from disc:  x = sqrt(2 + u*u - v*v + 2*sqrt(2)*u)/2 - sqrt(2 + u*u - v*v - 2*sqrt(2)*u)/2
                       y = sqrt(2 - u*u + v*v + 2*sqrt(2)*v)/2 - sqrt(2 - u*u + v*v - 2*sqrt(2)*v)/2

(radicands clamped at 0; a disc point outside the unit circle is first
moved onto it). The shader computes each output pixel's square position
with "square from disc" and samples there; a touch on the panel goes through
the same "square from disc" to find what was under the finger. For a panel
of `W` pixels, pixel column `px` is `u = (2 * px + 1) / W - 1` (the
inscribed circle), and the same for rows. With the warp off, the square
fills the panel and its corners are cut off by the round glass; touch maps
linearly.

The warp is for round panels only (the `round` flag of the video
capabilities' display entry, `mg_stream_video_capability_display`). On
a square panel (the ESP-Mosaico's 480x480) the phones stream the square as it
is, with no round mask, and don't offer the fisheye.

## Touch back

### Touch events (app level, both phones)

The client sends `[ui_input, touch_enable, 1]`; the gadget then notifies
`[ui_input, touch, action, x, y, time, id]` for each down, move (at most
every 16 ms, only when the point moved) and up, in panel pixels. While a UI
stream is on, the gadget's own UI ignores touches (its physical buttons still
leave the view). The app maps a touch through the inverse warp into the
square, then:

- **web page:** into the offscreen web view's coordinates, injected as its
  own touch events (Android `dispatchTouchEvent`, iOS synthesized taps and
  scrolls in the WKWebView).
- **mirrored window, Android:** into screen coordinates and injected with an
  accessibility service's `dispatchGesture` (the user turns the service on
  once). There is no app-level way into other apps on iOS: use HID.

### HID (the gadget as a Bluetooth touchscreen or pointer)

A gadget that lists the HID features also exposes a HID over GATT service
(HOGP, 0x1812) beside the musegadgets service, on the same connection. Its
reports need an encrypted, bonded link: pairing happens in the test app or
the phone's Bluetooth settings (never in the Muse app), and the phone's OS,
not the app, is then the HID host. The report map has three top-level
collections with report IDs, and the gadget sends whichever the client's
`hid_map` picked:

| Report ID | Collection | Report | Use |
|---|---|---|---|
| 1 | Digitizer / Touch Screen, one finger (hid-multitouch layout: tip switch, in range, contact id, X, Y 0..32767, contact count; feature: contact count maximum 1) | 7 bytes | Android: a touchscreen, taps and drags land where they point |
| 2 | Generic Desktop / Mouse, absolute X, Y 0..32767, buttons 1-3 | 5 bytes | hosts that take absolute pointers |
| 3 | Generic Desktop / Mouse, relative X, Y (-127..127), buttons 1-3, wheel | 4 bytes | iOS (AssistiveTouch): pointers only, relative only |

The app sends `hid_map` with the square's place on the phone's screen
(normalized to 0..65535 of its width and height) and whether the warp is on;
the gadget does the inverse warp and the mapping itself, so HID works with
the app in the background. For the relative pointer the gadget parks the
pointer in the top-left corner at each touch down (a burst of -127 moves,
which the host clamps at the edge), then moves to the target in steps of at
most 8 counts, where the host's pointer acceleration is close to linear, using
the counts-per-screen calibration from `hid_map`, then presses button 1;
moves while down drag; up releases.

iOS setup for the pointer: Settings > Accessibility > Touch > AssistiveTouch
on; the gadget, once paired, appears under AssistiveTouch > Devices. Tracking
speed changes the calibration (the app's calibration step measures it).

## The encoder policy

From an offline study of synthetic UI (text, settings, a dashboard, a clock,
a web page, scrolling) at both panel sizes, with the gadgets' own decoder
built for a host and counting instructions:

- **H.264 for changes, then a lossless still patch.** QP 18 is visually
  lossless for grey text (45 dB on glyph pixels), and P frames beat lossless
  dirty rectangles on every change (a clock tick 1.3 KB against 3.75 KB, a
  scroll frame 1.0 against 1.35 KB). But H.264 Baseline is 4:2:0: one colour
  per 2x2 pixels, whatever the QP. On coloured edges (coloured text, icons,
  badges on a coloured background) that alone limits a picture to 20-24 dB
  (PSNR over pixels where the source's colour changes sharply), against 41-46
  dB for the exact RGB565 the panel can show; QP 18 costs only 0.1-0.3 dB
  more, and a finer gadget conversion moves it by +-1 dB. So with Sharp
  quality the last refinement is a lossless patch instead (Still patches,
  below).
- **A change** goes out at the finest of QP 20, 24, 28 and 32 whose frame
  fits 0.3 s of the link (about 11 KB at 300 kb/s), never coarser than 32.
  The phone estimates the frame from how many 16x16 blocks changed times the
  bytes per block it has seen at each QP.
- **Refinement** while the picture holds still: to QP 24 at 0.3 s (skipped
  when the change was already 24 or finer), then with Sharp quality (the
  default) the still patches at 0.6 s; with Fast quality, or a gadget without
  still patches, to QP 18 at 1 s (and with Sharp on such a gadget, QP 12 at 2
  s); a frame at 20 or finer needs no QP 18. Refining a dense text page to QP 24 takes as long as one
  QP 24 keyframe would (721 against 710 ms at 360 px and 300 kb/s) and shows
  the page about 25% sooner.
- **Deblocking off:** it costs a small gadget's decoder 2.2-2.6x the decode work on
  keyframes and doesn't help text.
- **One band by default** at 466: two cut decode per core 1.5-1.9x but save
  only ~40 ms of first view (the link dominates) and cost 65% more bytes on
  scrolling.
- **Warped motion** costs 6-7x the bytes of flat scrolling, so the warp is
  best for mostly still content; while it moves the coarse QP and the credits
  keep the rate down.
- **x264 detail:** in constant-QP mode x264 pulls a forced per-frame QP back
  towards the constant, so refinements stayed coarse. The phones use CRF with
  adaptive quantization off and force the QP on each picture, with fast
  P-skip and DCT decimation off so refinements code their small residuals.
  Hardware encoders can't turn deblocking off or force a QP per frame.

## Pacing

Both phones send a change frame only when the picture changed, and:

- **from frame start to frame start:** at most every 1000/15 ms, counted
  from the previous change frame's start (not from when its encode ended);
  while the last change was small (2 KB or less) every 1000/30 ms;
- **while the previous one still goes out:** the next picture is taken and
  encoded once 2 KB or less of the previous frame is still on the phone
  (the sender's credit and backlog limits are unchanged);
- **only while the gadget keeps up:** at most one frame sent and not yet
  shown, by the gadget's buffer_update echo of the last frame it drew (no
  echo for 300 ms: nothing is held back). A faster rate so never queues
  pictures on the gadget: the rate follows its decode and draw.

Measured on the ESP-Mosaico with a running clock page, glass to glass with a
camera (phone screen to gadget screen, ~240 frames each):

| | before (15 fps from encode end, link idle) | after |
|---|---|---|
| Android (S22) | median 140 ms, p90 190; 10.9 fps | median 90 ms, p90 140; 24 fps |
| iPhone 17 Pro Max | median 100 ms, p90 150; ~13 fps | median 90 ms, p90 130; ~26 fps |
| Android tap on the gadget -> shown | median 157 ms, p90 200 | median 115 ms, p90 120 |

The old rule stays for comparisons: Android `--ez uilegacy true`, iPhone
`-uilegacy YES` (the fast rate: `--ei uifast N` / `-uifast N`, 15 or less
turns it off).

## Still patches and the gadget's frame buffer

A gadget that lists `mg_stream_video_capability_ui_still` takes frames with
`mg_video_frame_still` in a UI stream: a rectangle of exact RGB565 (big-endian,
the panels' own order), zlib-compressed. The panel shows those pixels until
the decoded picture changes there.

- **Gadget:** UI pictures go through an RGB565 frame buffer of the picture's
  size. Each decoded picture is compared with the last one, macroblock by
  macroblock; only those that changed are converted and redrawn (a clock's
  second redraws a few macroblocks, not the panel), with their unchanged
  neighbours that no patch holds: the chroma blend reaches one pixel into
  the next macroblock, so a neighbour's edge depends on what changed (left
  stale, a scrolled list measured 34.8 dB against the phone's picture instead
  of 43.2). A patch is written into
  the frame buffer and drawn, so it stays wherever later pictures don't
  change. The conversion for UI pictures is the fine one: bilinear chroma
  centred as the phones average it, BT.601 limited range with rounding, and a
  4x4 ordered dither into RGB565 (the plain video path takes one colour per
  2x2 and truncates). Rows that don't start on 16 bytes go the scalar way in
  muse_yuv.c: the S3's vector stores drop an address's low bits, so the
  vector colour conversion would store every odd row of a 466 px picture
  (932-byte rows) 2 pixels to the left and text would look doubled. A panel
  dump (`>ui.dump`) of a text page: 15.5 dB against the phone's picture with
  the vector path on those rows, 52.4 dB with the scalar one.
  The ESP32-S31's vector conversion has the same rule, and its 480 px rows
  (960 and 240 bytes) are aligned anyway; its panel matches the fine
  conversion of the decoded picture bit for bit, in one band or two.
- **Phone:** it knows the gadget's decoded picture exactly (x264's
  reconstruction, with deblocking off: what tinyh264 decodes, bit for bit). A
  macroblock is dirty when that reconstruction changed (the gadget then
  redraws it from the decoded picture) or when the source changed since its
  last patch. 0.6 s after a change (after the QP 24 refinement), the dirty
  macroblocks go as patches, one rectangle per run of rows. Patches over 120
  KB in all (photos) are skipped for the QP 18 refinement.
- **Sizes**, a whole 466 px screen as RGB565 zlib: settings 9 KB, clock 13,
  text 19, dashboard 19, web page 46-52, a photo 94-118 (skipped). The whole
  H.264 ladder for the same screens is 15-35 KB.

## The gadget's menu, panning and controls

`mg_ui_input_control` keeps a UI stream's settings in step on both sides:
fisheye, pan, quality (fast or sharp), stop, menu and edge_pan.

- **Menu:** the gadget's menu button (StopWatch: blue, the speaker button,
  during a UI stream; yellow and power still leave the view; ESP-Mosaico:
  BOOT, its aux button, while AI leaves the view) sends menu 1 or
  0. The phone draws the menu into the picture after the warp, at the
  panel's pixels, and takes taps on its rows from the touch events: Back,
  Home and Recents (screen mirror), Fisheye (round panels), Pan and
  Recenter (screen mirror), Quality, Stop and Close. While it's open no touch
  reaches the HID.
- **Back, Home, Recents:** the phone's own navigation, whatever the window
  shows (Android's nav bar sits at the screen's very bottom: on a round
  panel without the fisheye its corners are outside the circle, with it
  they're squeezed into the rim). Android does them with the touch
  service's global actions; without the service, in HID mode, the gadget's
  HID presses a consumer key (`mg_ui_input_hid_key`: AC Back 0x224, AC Home
  0x223, HID report 4). Recents is offered only with the touch service: AL
  Select Task (0x1A2) reaches Android's kernel as KEY_APPSELECT, but the
  S22's key layout maps it to nothing. iOS takes neither key. With the touch
  service off and HID on (Galaxy S22, StopWatch, fisheye on): the nav bar
  tapped through the mirror Home 10/10, Back 10/10, Recents 3/3; the menu's
  Home 10/10, Back 9/10.
- **The digitizer and repeated taps:** the kernel passes a contact's
  position only where it differs from the last one in its slot, and Android
  takes a new contact with no position as (0, 0), the status bar. So a new
  contact exactly where the last one was moves one count on each axis that
  repeats (`mg_hid_touch`); a lift reports where the contact last was.
- **Pairing for HID on Android:** a HID service the gadget showed after the
  phone's discovery (hid_map turned HID on during the connection) isn't in
  Android's GATT cache, and Service Changed doesn't reach an unbonded phone;
  Android's bond then lists the gadget without HID and its HID host never
  attaches. The app drops its GATT cache (`BluetoothGatt.refresh`) just
  before bonding, so bonding discovers afresh.
- **Panning the screen mirror:** the touch controllers on both gadgets
  report one finger only (CST820, CST815), so two-finger panning isn't
  possible. Instead a drag that starts in the panel's outer edge ring
  (beyond 88% of its radius; on a square or rectangular panel, the band
  along its edges, the outer 10% of its width or height) moves the mirror window, and Pan mode (in the
  menu) makes every drag move it. The window follows the finger (drag down
  to see what's above). Taps and drags elsewhere still go to the phone. The
  gadget sends panning gestures as touch events only, never to the HID.
- **Past the screen's edges:** the window may be panned past the phone
  screen's top and bottom by up to half its height, so the screen's edge
  (the status bar, the nav bar) can sit at the panel's centre, where the
  fisheye is nearly one to one. Across, it stays inside the screen (the
  content fits across a round panel). The app's Overscroll setting (UI
  tab, and the menu's Overscroll item, which cycles; `mg_ui_control_overscroll`)
  picks None (inside the screen), Vertical (the default) or All (past
  every edge). Beyond the screen the picture is black, with a faint
  line at the screen's edge; a touch that goes down there is ignored (one
  that moves out stays on the edge), in the app's own injection and in the
  HID (`mg_hid_map_offset`: the square's x and y as (fraction + 0.5) / 2).
  The menu's Recenter puts the window back in the screen's middle. The web
  page view doesn't overscroll: the page scrolls itself. With the S22's
  nav bar panned to the middle (fisheye on), taps hit Home 10/10, Back
  9/10, Recents 4/4.
- **Tap or pan:** a touch that goes down in the edge ring (or anywhere in
  Pan mode) is held back by the gadget until it's decided: moved more than
  12 px, it's a pan (touch events flagged `MG_TOUCH_ID_PAN`, none to the
  HID); lifted before that, or held still for 350 ms, it's an ordinary tap
  or long press for the phone and the HID, at the point it went down. On
  the StopWatch with the S22 (screen mirror at the screen's bottom, fisheye
  on), tapping the nav bar through the mirror hit Home 10/10 and Back 10/10
  (0/10 without the hold-back: the ring swallowed every tap); from the menu, Home 10/10,
  Back 9/10, Recents 3/3.

## Per gadget

| | M5Stack StopWatch (ESP32-S3) | ESP-Mosaico (ESP32-S31) |
|---|---|---|
| panel | 466x466 round AMOLED (CO5300) | 480x480 square AMOLED (CO5300 on QSPI) |
| UI picture | 466x466 (coded 480x480), one band (two offered: 272 + 208 rows) | 480x480, one band (two offered: 288 + 192 rows); still patches on (the S31 ROM's inflate) |
| max frame (ui capability) | 128 KB (the study's largest needed: 48 KB) | 128 KB |
| codec | H.264 Baseline, 1 ref, no deblocking | same |
| update policy | on change; refine at 0.3 s and 1 s | same; Sharp: lossless patches 0.6 s after a change |
| estimated first view / crisp, 300 kb/s | 0.35-0.88 s / 0.35-1.22 s | — |
| measured decode | text keyframe 87 ms (two bands 53), refinement 57-64 ms (43-45), clock tick 30 ms; on the panel ~25 ms after its decode | text change frame (16.6 KB) 34 ms (two bands 19), refinement 28 ms (16), clock tick 17-18 ms; converting a whole picture 69 ms, a tick's 12-16 ms; a whole-screen patch (21 KB) inflates in 27 ms and draws in 26 |
| measured end to end (S22) | text page: first view 0.40 s (QP 32, 18.6 KB), QP 24 at 0.61 s, QP 18 at 1.30 s; clock tick shown 80-110 ms after the page changed (11 kb/s); drag scroll ~13 fps; tap on the gadget to the new picture on it ~250 ms | text page: first view 0.45 s (QP 32, 16.6 KB), QP 24 at 0.64 s, lossless at 0.95 s (panel 53.3 dB against the phone's picture; list 54.3); clock tick shown 114-295 ms after the change (median 137), its patch (53 B-2.1 KB) ~0.7 s; drag scroll ~3.5 fps (20-23 KB frames at ~520 kb/s: the link) |
| RAM | 30.7 KB free while a one-band UI stream decodes, 24.3 KB once a two-band stream has run; flat over a 10-stream soak (lowest 19.9 KB) | 82.5 KB free when a stream starts, 78.8 KB while it decodes (largest block 39-43 KB); PSRAM 12.46 MB before, 10.74 MB during |
| still | 0 kb/s; the link drops to peripheral latency 9 after 1 s (connection events 66.7 to 6.7 per second); the first change after that shows ~70-140 ms later | 0 kb/s; peripheral latency 9 once still |
