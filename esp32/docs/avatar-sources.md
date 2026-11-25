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

# Optional Muse avatar sync

**Experimental.** `CONFIG_MUSE_AVATAR_SYNC` adds a selectable home-avatar
source that follows the paired Muse's current still image. Build support is off
by default. The default user setting remains Built-in, including after an
upgrade. Initial hardware validation covers the Waveshare 1.75C.

Avatar updates are data. A daily scheduled avatar change requires no firmware
rebuild and no change to the scheduled prompt. This implementation uses the
paired daemon directly; it does not send chat messages to discover or convert
an avatar.

## Select an avatar source

With support compiled in, open **Settings → Avatar**.

| Setting | Behavior |
| --- | --- |
| Source: Built-in | Use the build's existing renderer. This includes the custom C avatar selected by `tools/muse/avatar.py`. |
| Source: My Muse | Use the latest valid image from the paired Muse, refreshing automatically while awake and online. |
| Refresh now | Check the current identity; enabled for My Muse. Voice, sleep, and foreground display work defer the refresh. |
| Clear saved avatar | Remove the private cache. With My Muse selected, fetch again when conditions allow. |

Switching changes the next rendered home frame. It does not restart the device
or replace the active conversation. The source setting persists in NVS. The
worker writes it from a flash-safe task; the UI does not write flash in an
avatar event handler.

While the first image loads, Built-in remains visible. Failed or incomplete
updates retain the last valid personal image. Offline operation uses the saved
image when the cache matches the pairing and VM. Settings shows Updating,
Up to date, Using saved avatar, or an availability error. Sync status does not
replace chat captions.

The personal still stays selected through listening, speaking, thinking,
taps, and reconnects. Existing captions, status and level indicators, petting,
volume swipes, settings, reply review, option selection, and sleep/wake controls
remain in the same home view. Petting and playback level can add a small scale
pulse. A still does not reproduce the procedural character's mouth or hand
animation.

## Enable a development build

Use ESP-IDF v6.0.3 and the SDK's normal token and pairing setup. For the 1.75C,
from `esp32/`, create a local defaults fragment and build a separate directory:

```sh
printf 'CONFIG_MUSE_AVATAR_SYNC=y\n' > /tmp/muse-avatar-sdkconfig
idf.py -B build-avatar-sync -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-avatar-sync/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175c;/tmp/muse-avatar-sdkconfig" \
  build
```

The option requires the full LVGL UI, PSRAM, and daemon requests. Headless and
FreeInk configurations exclude it. Other boards need memory and display
validation before their defaults enable it. Do not move partition offsets,
change pairing, or enable Apps merely to enable avatar sync.

`MUSE_AVATAR_DEPS` is an internal string used by the component manager. It
remains defined when the feature is disabled, so dependency discovery also
works on headless builds. The WebP source is fetched only for enabled builds,
pinned to libwebp v1.6.0 and an archive SHA-256. Its decoder uses scalar code,
without the encoder, tools, or worker threads. See the
[libwebp notices](../licenses/libwebp.txt).

## Data and refresh behavior

Avatar sync reads the VM's Gadget API on `ServiceType::Gadget`
(`noise_ctrl_req_open_gadget`): `GET /api/gadget/identity`, which needs the
credential's `gadget.identity.read` scope. It uses `result.avatar.image` (the
still's `light` URL, else its `dark` one) and `result.updated_ms`, which
changes whenever the identity or any avatar file does, as the revision. The
parser accepts unknown fields and bounds the body to 8 KiB and the URL to 511
bytes, and takes only URLs under `/api/gadget/identity/avatar?`. Invalid
metadata leaves the current image intact. No `avatar`, or no `image` in it, can
describe a pending asset; it does not establish that the person chose the
gadget's Built-in source.

The worker refreshes after connection, when a wake finds the previous check
stale, and every 15 minutes while awake. It coalesces manual requests and backs
off network failures from one minute to one hour with jitter. A voice or sleep
interruption defers the check instead of losing a manual refresh. It never
wakes the display or continues a download solely for polling.

An unchanged path and nonempty revision skip the image download. Without a
revision, the worker revalidates the bytes. A changed revision may require a
validation download even if only animation assets or other identity fields,
such as the name, changed. Identical decoded
pixels avoid an image rewrite; a small metadata checkpoint preserves the newer
reference across reboot.

Files are the URLs the identity hands out, fetched as given with Range
requests on the Gadget API over the existing Noise session. Each range is at
most 4 KiB and respects the board's response limit.
The loader accepts static WebP, at most 512 KiB encoded and 2048 pixels on either
source edge. It scales directly into bounded output rather than allocating a
full source-sized RGBA image. Opaque output is at most 466 × 466 RGB565. Images
with alpha use at most 320 pixels per edge and composite onto black. Animated
WebP and other input formats are outside this release.

Phone animation assets need a separate conversion and playback contract. The
examined phone MP4 uses 720 × 720 H.264 High profile. The existing small video
path expects a much smaller constrained-baseline stream; enabling Phone camera
does not make those assets usable as a home avatar. See
[the video protocol](../../protocols/VIDEO.md).

## Rendering and resource ownership

The renderer adapts the existing home canvas. It receives the same mode, audio
level, and pet reaction as the bundled character. It reads immutable RGB565
pixels through the existing strip decoder and contains the image inside the
current canvas bounds. A still image has gentle breathing while idle. Each tap
restarts a short bounce on the same personal image. Invalidation follows changes
to the image or visible output geometry.

Every open strip decoder holds a reference to its image until close. The worker
can publish a replacement or switch sources while those reads finish. Selection
and pairing generations reject stale responses. A cancelled HTTP request owns
its callback context until the transport sends its terminal callback, even if
the worker has stopped waiting.

Network reads, decoding, and flash writes run outside the render loop. The
worker defers to voice states, video, an open app, and temporary image downloads.
Large allocations use PSRAM exclusively. The tracked image/decoder budget is
1.5 MiB less encoded input and a 16 KiB allowance; prior live images remain
counted. Admission also leaves 1 MiB of free PSRAM and requires at least 48 KiB
of internal byte-addressable RAM, preserving the existing Noise DMA checks.
A failed admission or decode keeps the last valid image.

Selecting Built-in releases the personal pixel buffer once pending strip reads
finish. The background worker then exits. It retains no polling timer or
personal image in that mode. Feature-off builds omit the setting, loader,
decoder, filesystem dependency, and avatar requests.

## Persistent cache

Avatar sync and Apps use one owner for the existing `files` LittleFS mount when
both features are compiled in. Avatar sync does not depend on WASM. A mount
failure never formats existing data; the mount owner checks the complete
partition for erased bytes before initializing a blank one. Missing or unsafe
storage degrades to RAM-only operation and reports that the avatar was not saved.

The private `.muse-avatar-v1` directory contains two bounded image slots and a
small metadata checkpoint. These files stay below 1 MiB in total. Apps' file
names and listing exclude the private directory. Avatar sync never evicts Apps
files or moves a partition. The current Apps cleanup may remove this directory
on unpair; cache readers validate every file rather than relying on a manifest
that could outlive its data.

Each image slot records a version, dimensions, sequence, paired-VM binding,
identity metadata, and header/pixel checksums. The worker writes the inactive
slot and flushes it before selecting it. Interrupted candidates fail validation;
the previous complete slot remains available. Metadata-only checkpoints apply
only to the matching image generation and checksum. A damaged checkpoint leaves
the image slot usable.

The binding includes the resolved VM, endpoint/settings, and a digest of pairing
credentials. The last resolved VM ID is saved for offline startup. Credential
renewal can invalidate a cache. Unpairing clears the new VM key and queues cache
removal; pairing changes hide the previous image immediately. Raw credentials
are never stored in the avatar cache or printed by its diagnostics.

For Apps builds with external-BSS support, follow the
[Apps memory recommendation](../components/muse_apps/AGENTS.md#recommended-memory-setting-for-apps-firmware)
for `CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM`. Verify the combined configuration on
hardware and retain the existing Noise DMA reserve.

## Validation

Development validation on November 19, 2026:

| Check | Result |
| --- | --- |
| 1.75C feature off | Builds; signed image 2,232,320 bytes, matching the original image size. No WebP or LittleFS component added. |
| Default headless C5 | Builds; avatar dependencies excluded. |
| 1.75C feature on | Builds; signed image 2,363,392 bytes in the existing 4 MiB slot. |
| 1.75C Apps + avatar | Builds; signed image 2,756,608 bytes. Runtime Apps coexistence still needs device testing. |
| Real current avatar | Downloaded and rendered at 466 × 466; source changes preserve pairing and the online connection. |
| Memory | About 424 KiB retained for the opaque image; about 925 KiB tracked on the measured first load. The metric includes encoded bytes and a fixed allowance, not a whole-device heap peak. |
| Refresh/cache | Unchanged-reference checks avoid image downloads and cache writes. Saved-image restoration works across source switches and reboot. |
| UI simulator | 44 sanitizer checks pass, including source changes, pinned strip reads, petting, volume gestures, voice display states, settings geometry, and sleep/wake. |
| Host adapters | Metadata bounds, cache corruption/interrupted writes, settings failures, request cancellation/late callbacks, and strip rendering pass under ASan/UBSan. |
| WebP decoder | RGB565 ordering, alpha, scaled decoding, budget failure, truncated input, and animation rejection pass against the pinned real decoder under sanitizers. |
| Broad ESP32 host suite | 1,555 tests executed with 11 optional-dependency skips. The sole failure, a new log-tag namespace, was corrected and its module passed on rerun. The new request-owner harness also passes separately. |

Before enabling a shipping board default or calling this production-ready,
measure frame latency and whole-device heap minima with sustained audio, run
Apps and video coexistence on hardware, exercise a real scheduled daily update,
and complete overnight/battery and power-loss testing. Simulator results do not
establish ESP32 timing, physical finger behavior, or battery performance.

## USB diagnostics

On enabled builds:

```text
>avatar
>avatar.source=builtin
>avatar.source=personal
>avatar.refresh
>avatar.clear
>avatar.identity
```

`avatar` reports source, state, dimensions, tracked allocation counts, completed
refreshes, Range attempts, cache commits, and opaque asset fingerprints. It does
not reveal tokens or asset paths. `avatar.identity` returns field names and
types only. These are USB diagnostics; they do not add remote commands or grow
`link.register`.

The focused host and simulator checks are:

```sh
python3 -m unittest discover -s tests -p test_avatar_model.py
python3 -m unittest discover -s tests -p test_avatar_request.py
python3 -m unittest discover -s tests -p test_avatar_settings.py
python3 -m unittest discover -s tests -p test_avatar_render.py
cmake -S tests/avatar_webp -B build-host-avatar-webp -G Ninja
cmake --build build-host-avatar-webp
ctest --test-dir build-host-avatar-webp --output-on-failure
```

The model harness needs cJSON from an IDF configure/build. The simulator's
standard CTest run includes the avatar UI and settings tests. Follow
[the simulator guide](../simulator/README.md) and
[CONTRIBUTING.md](../../CONTRIBUTING.md) for the remaining review checks.

## Phone animation development preview

`CONFIG_MUSE_AVATAR_ANIMATIONS` is a separate, default-off build option. It
requires avatar sync and the ESP32-S3. It reuses the bounded H.264 decoder,
without enabling Phone camera, its full-screen view, a TCP listener, mDNS, or
VP8. The preview renders decoded RGB565 frames through the existing home
canvas and strip ownership protocol.

**This is not full My Muse interaction parity.** The observed identity API
supplies idle and work-family MP4s. The phone's internal manifest names more
behaviors, but those names do not supply rigs, geometry, or corresponding
video assets through `/api/gadget/identity`, whose `video_variants` name each
clip's `light` and `dark` URLs.

| Home state | Preview behavior |
| --- | --- |
| Idle | Play the phone's idle clip; use the synced still if no valid decoded frame is available. |
| Boot | Use a waking clip when supplied; otherwise use personal idle/still. |
| Working during a voice call | Play a matching working clip when supplied. |
| Listening/speaking | Keep the personal frame and existing state/level indicators; speaking adds gentle level-driven scale feedback. Suspend decoding to leave CPU for audio. |
| Tap/petting | Apply a short bounce to the same personal image/video; repeated taps restart it. Keep video playback in its current role. |
| Error/shutdown | Keep the personal image and existing state indicators. No unrelated character is substituted. |
| Hidden home/sleep | Freeze the last personal frame. Brief Settings visits retain the decoder cursor; longer pauses release the decoder/pack while keeping one RGB frame. |

My Muse does not substitute the built-in character for missing personal rigs.
The local bounce and scale feedback do not provide a mouth/face rig or claim
full phone interaction parity. The built-in source keeps its original renderer
and reactions. Native volume gestures, captions, controls, and sleep/wake retain
their owners.

### Prepare and install a pack

Enable both options in a local build fragment:

```text
CONFIG_MUSE_AVATAR_SYNC=y
CONFIG_MUSE_AVATAR_ANIMATIONS=y
```

With that development firmware on a paired board and My Muse selected, use
Python with pyserial and a local ffmpeg build that includes libx264:

```sh
python3 tools/muse/avatar_clips.py export --port PORT --directory /tmp/muse-clips --clips idle working
python3 tools/muse/avatar_clips.py convert --directory /tmp/muse-clips --output /tmp/muse-preview.mav --clips idle working
python3 tools/muse/avatar_clips.py install --port PORT /tmp/muse-preview.mav
```

The CLI defaults to 192 pixels, 20 fps, and CRF 20, based on short Waveshare
1.75C measurements. Use `--edge`, `--fps`, and `--crf` to tune other hardware or
avatars. Requested frame rate is the source timeline, not a panel-rate guarantee.

The exporter requests only paths from the current avatar's allowlisted video
variant fields. It reads authenticated 4 KiB ranges and verifies USB chunk
checksums. It exports an opaque SHA-256 identity key, not device credentials or
arbitrary VM files. Keep exported personal assets outside the source tree.

The converter creates constrained-baseline H.264, with selectable size, frame
rate (`--fps`), and quality (`--crf`, 16..32; lower is sharper),
with one reference frame, no B frames, repeated parameter sets, and AUD-delimited
access units. Each clip starts with an IDR. Because playback always starts at frame zero,
the converter uses one keyframe per bounded clip instead of inserting an
expensive keyframe every second. Choose dimensions, quality, and frame rate from
measurements on the board; higher settings consume more CPU.
Packs contain at most six named
clips, 240 frames per clip, 32 KiB per access unit, and 1 MiB total. Dimensions
are macroblock aligned and at most 240 pixels. The runtime separately limits
H.264 dimensions and reference frames before picture allocation.

A pack records the SHA-256 of the still's URL and the identity's
`updated_ms`. Packs installed before avatar sync moved to the Gadget API, and
any pack after an identity change such as a rename, report stale and need
installing again. The cache adds the
paired-VM binding. All header, payload, clip, and frame lengths and checksums
must validate before publication. A changed identity invalidates old animation
content and falls back to the current personal still when available. A failed or
incomplete install leaves the previous complete pack intact. The host installer
also checks the current asset generation before starting an upload, so a daily
avatar change reports a stale pack before transferring it. Storage admission
reserves space for a complete staged candidate; no Apps files are evicted.

Conversion and installation currently require this computer. Still-image daily
sync remains automatic and does not depend on the computer. Automatic daily
animation derivation on the Muse VM is **not implemented**. It needs a defined
conversion/manifest contract and separate validation; do not modify the daily
avatar prompt merely to hide this limitation.

### Runtime and diagnostics

Network reads, file operations, H.264 decoding, and color conversion run outside
the UI render callback. The UI consumes immutable, reference-counted RGB565
frames; the decoder receives a scratch copy because it unescapes input in place.
The source frame rate sets the playback timeline. Every predictive H.264 frame
is decoded; an overdue picture can skip RGB conversion and publication to keep
the clip at its intended speed. The worker yields between pictures.
Hidden views freeze playback. Pauses over five seconds, sleep, and sustained
voice activity release the decoder, compressed pack, and scratch buffer while
retaining one RGB565 frame (at most 115,200 bytes). It bridges the next warm-up,
so returning from Settings does not briefly show the differently framed still.
Source/binding changes and disabling playback make old frames unavailable;
pinned readers release their final references safely. Decoding runs on core 0,
matching the SDK video decoder and leaving the UI/audio core separate.

Presentation does not add a second frame interval when a decode is late. Still
photos use bilinear RGB565 interpolation; moving video uses a cheaper three-phase
interpolator to soften upscaled pixels within the board's CPU budget. The bundled
pixel-art renderer keeps its existing scaling. Each strip pins its
source and transform until the read completes. A staged pack upload freezes
decoding, then resumes after commit/abort, so USB/file work does not compete
with animation. The speaker and existing controls retain their owners.

Developer console commands:

- `avatar.motion`: numeric playback, coverage, error, and timing counters, plus
  retained-frame bytes/revision, decoded pictures, skipped RGB output, and late
  presentations;
  `parity_complete` remains false.
- `avatar.motion.preview=KIND`: preview an installed phone clip for eight
  seconds while idle. Voice and pet reactions take priority.
- `avatar.motion.frame`: checksummed RGB565 capture of one pinned decoded
  frame for visual verification. It does not capture the surrounding UI.
- `avatar.motion=0` / `avatar.motion=1`: temporarily disable/enable preview
  playback; these diagnostics do not persist a setting.
- `avatar.assets` / `avatar.asset=KIND,OFFSET`: bounded export of the current
  allowlisted phone assets.
- `avatar.motion.begin=SIZE`, `avatar.motion.chunk=OFFSET:BASE64`,
  `avatar.motion.commit`, and `avatar.motion.abort`: development installation
  protocol used by the tool. Incomplete transfer RAM is released on abort, the next begin, or after
  30 seconds without a chunk (checked every five seconds). Pairing/source
  invalidation also rejects further chunks. Install admission and commit defer
  to voice activity.

The animation cache adds `motion.dat` and its staged `motion.tmp` to the private
avatar directory. The prior still-cache layout remains unchanged. Clear saved
avatar also removes the animation pack. No partition changes are required.

### Validation and remaining release gates

On the Waveshare 1.75C, the current two-clip 192-pixel CRF 20 pack is 401,067
bytes. Two 10-second samples produced 19.97–20.04 decoded pictures/s at the
20 fps source rate, with 15.50–15.85 frames/s accepted by the UI. Neither sample
needed to skip RGB conversion. A 224-pixel CRF 24 alternative reached 14.65 UI
frames/s; 240 pixels at CRF 20 reached 11.77 UI frames/s. A 192-pixel CRF 24
profile reached 16.62–16.72 UI frames/s with more compression. The CLI favors
the sharper 192-pixel profile. These are short, awake, USB-powered measurements
of one asset generation, not panel scanout, battery tests, or a promise for
every avatar. The `decode_us` metric includes RGB conversion; the separate
`decoded_frames` counter also counts pictures whose RGB output was omitted.

Earlier preview checks covered real idle/work playback, checksummed captures of decoded
pixels, cached reboot, twenty rapid source changes, source and voice-state memory
release, sleep/wake, rejected incomplete/corrupt installs, preserved prior-cache
reload, voice-priority install refusal, and abandoned-upload reclamation. No
panic or decoder errors were observed. Voice states were injected through the
existing bench hook; this does not validate sustained microphone/audio load.

The continuity regression checks returned from Settings after 0.8, 2, 8, and
15 seconds, verified that the retained frame survived decoder shutdown, and
captured the personal avatar during three pet reactions, voice/error/off states,
and sleep/wake. Source switching released the retained frame and resumed personal
playback when selected again. The appearance preference remained unchanged.

The 53 simulator checks pass under ASan/UBSan, including animated frame pinning,
personal-identity continuity, repeated pet feedback, Settings gating, volume, and source switching. Host
checks exercise actual H.264 conversion/looping, the opt-in reference bound,
malformed packs, identity field bounds, request cancellation/EOF, and export
checksums/generation mismatches. A production-worker harness uses a virtual
clock to check short/long Settings pauses, pinned-frame lifetime, voice and
upload suspension, memory-pressure cleanup, late decoding, and skipped RGB
output without skipping predictive references. Existing untrusted-video checks
also pass.
The default 1.75C, headless C5, still-only, animation, and Apps-plus-animation
configurations build within their existing app partitions. Both extensions off
leave the 1.75C binary at 2,232,320 bytes and the C5 at 1,773,568 bytes.

Before treating this as a production personal-avatar replacement:

- Supply and validate matching personal reactions, including speech-energy and
  pause response. Local image transformations provide feedback but do not replace
  a personal face/mouth rig.
- Define and test automatic VM-side animation derivation after daily changes;
  stale packs currently fall back rather than following a new avatar automatically.
- Measure sustained audio, runtime Apps/camera coexistence, whole-device battery
  use, and an extended soak. Apps coexistence is compile-checked only.
- Exercise physical power loss during pack commit and storage pressure with
  populated Apps data. Interrupted staging files must remain harmless and
  reclaimable without deleting Apps or the previous complete pack.

## Optional appearance integration

Avatar syncing and clip playback build independently of the optional appearance
option (`CONFIG_MUSE_UI_THEME`). The adapter below is enabled only when both are on.

With both `CONFIG_MUSE_AVATAR_SYNC` and `CONFIG_MUSE_UI_THEME` enabled,
Automatic appearance uses a confident background sample from the canonical
synced still. White, dark, and colored solid backdrops can supply the home
surface color and the corresponding light or dark palette. Manual Light or
Dark always overrides the hint. The standalone [appearance feature](appearance.md)
has no dependency on avatar synchronization; this adapter is separate.

The detector reads 64 small corner samples, favors the upper corners, and
requires a dominant color cluster across at least three corners. Ambiguous
scenery, gradients, or a character filling the corners supply no hint, so
Automatic keeps its dark fallback. A manual appearance choice remains available.

Sampling happens only when the saved still revision changes. Video frames and
local pet/speech transformations do not trigger resampling, avoiding color or
brightness flicker while talking, petting, or playing a clip. Built-in uses its
original dark background in Automatic. Switching back to My Muse restores its
last detected background while the saved image reloads. This color is retained
in RAM for the current account/VM context, not written as another preference.
Clearing the image or changing the account/VM invalidates that remembered color;
an old image awaiting worker cleanup cannot restore it. Without a confident
sample, Automatic falls back to dark. Manual Light and Dark stay fixed across
source changes.

Snapshot bench builds can open `ui.settings=avatar` or `ui.settings=appearance`.
With both features enabled, `ui.settings.action=primary` clicks that page's
source/theme row and `ui.settings.action=back` clicks its back button. Actions
are queued singly, run in the LVGL task, and acknowledge with
`@ui_settings_action`. These hooks exercise the production callbacks during
cable-connected regression tests; normal firmware excludes them.

The home surface uses the selected background. Personal-image padding uses the
detected image backdrop when available, keeping its edges steady during a tap
even with a contrasting manual UI choice. Otherwise it uses the selected
background. Original files and decoded source buffers stay immutable; resampling and local reactions
write only output strips, and black details are not keyed out. The bundled
procedural renderer uses its independent backdrop hook when selected or when
no personal image is available. No network request, polling
timer, avatar regeneration, or cache-format migration is added by this adapter.
