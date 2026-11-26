<!-- Copyright (c) Meta Platforms, Inc. and affiliates. SPDX-License-Identifier: Apache-2.0 -->
# Reusable pixel-art avatars

The experimental `CONFIG_MUSE_PIXEL_AVATAR` path uses one shared local renderer for many avatar types. The converter in [`../../avatar_pixel/`](../../avatar_pixel/README.md) produces a small indexed sprite, palette and independent alpha mask. Its output is data, not custom C, WASM or an inferred face rig. A new asset therefore needs no firmware rebuild after the renderer is installed.

The first release is deterministic image processing: contain-fit framing, palette reduction, optional dithering, and nearest-neighbor enlargement. It preserves the original image/background rather than inventing anatomy or removing a background. Generic whole-sprite breathing, bobbing, tap feedback and subtle state tint run on the ESP32. A plant, animal, robot or abstract logo can use the same runtime. Individual eye/mouth/limb rigging and full phone-character parity are not part of v1.

## Build

Use ESP-IDF v6.0.3 and your board's existing hardware/SDK-token setup. Enable these options in a local fragment outside version control:

```text
CONFIG_MUSE_AVATAR_SYNC=y
CONFIG_MUSE_PIXEL_AVATAR=y
```

For the Waveshare ESP32-P4 3.5, build in a separate directory using `sdkconfig.defaults`, `devices/sdkconfig.muse`, `devices/sdkconfig.muse-waveshare-p4-lcd35` and that fragment. Keep silicon-revision settings as described in [the P4 setup guide](../devices/waveshare-p4-lcd35.md). The new feature defaults off and works independently of `CONFIG_MUSE_AVATAR_ANIMATIONS`; original still images and H.264 clips remain separate fallback sources. Do not enable Apps, camera or live voice merely to use a pixel sprite.

The native renderer/parser is `components/muse/muse_pixel_art.c`; the owner-bound cache/USB installer is `main/pixel_avatar.c`. The UI pins a model and its pose per render strip, so a concurrent asset/source change cannot free its palette or mix transforms halfway through a strip.

## Compile the current personal avatar

Use Python with Pillow and pyserial, from `esp32/`:

```sh
python tools/muse/pixel_avatar.py compile-current --port PORT \
  --output /private/path/avatar.mspix --preview /private/path/avatar.png \
  --profile 64 --colors 24 --dither
python tools/muse/pixel_avatar.py install --port PORT /private/path/avatar.mspix
```

Output files are new-only private files. The helper exports the canonical personal still over the existing USB console, checks dimensions/CRC/generation, calls the same `avatar_pixel.compile_image` library as the HTTP API, then installs the validated data pack. No token, prompt, public image upload, avatar regeneration or firmware rebuild is needed. A source change during export/install rejects the candidate rather than mixing avatar generations.

For independent images or other consumers, use the root CLI or self-hostable API:

```sh
python -m avatar_pixel compile --input avatar.png --output avatar.mspix \
  --profile 96 --colors 24 --preview preview.png
python -m avatar_pixel serve --port 8080
```

See the [API/security instructions](../../avatar_pixel/README.md) and [v1 format contract](../../protocols/PIXEL_AVATAR.md). The service binds loopback by default. Internet deployment is a separate operator action with TLS, rate limits and process/decoder isolation; this code does not deploy a public endpoint or publish personal images.

## Device controls and diagnostics

These are serial maintenance commands, not unauthenticated network endpoints:

```text
>pixel
>pixel.on
>pixel.off
>pixel.assets
>pixel.image
>pixel.begin=SIZE,SOURCE_KEY
>pixel.chunk=OFFSET:BASE64
>pixel.commit
>pixel.abort
```

`pixel` reports numeric pack/cache/upload counters and matching state, never pairing credentials. `pixel.on/off` persists the local appearance selection with readback. Select My Muse as the avatar source for the pixel pack; Built-in still selects the existing local procedural character. The speaker/mute button and existing captions, touch controls and volume gestures are unchanged.

The installer bounds the entire pack to 64 KiB and every chunk to 600 bytes. It validates the paired owner, exact current avatar generation, palette/indices/alpha, dimensions, capability mask and CRCs before atomic file replacement. Incomplete/invalid candidates leave the previous good pack intact. Staging expires after 30 seconds without progress and abort releases its memory.

## Offline restoration and boundaries

A saved model is read from the existing private avatar directory on the `files` LittleFS volume. It is bound to the stored pairing and explicit endpoint/VM settings, not to transient connection success or a newly resolved VM bearer. A cloud HTTP 502 does not itself invalidate a matching pixel pack. Temporary storage/allocation failures are retried with bounded backoff instead of marking a failed load complete.

An online identity mismatch hides an outdated pixel sprite and uses the current personal still until that generation is compiled. Pairing changes, unpairing, and explicit endpoint/VM changes must not expose the old owner's assets. Credential rotation can change the stored owner fingerprint and requires revalidation/reinstallation; this release does not promise cache continuity across every credential rotation. The saved pack is private and is never exposed through the public compiler service.

No failed mount formats existing files data. The `files` volume must already be usable for installation; otherwise report the storage failure and obtain explicit authorization before repairing or formatting it. Interrupted cache updates keep the prior valid file. A corrupted/mismatched pack is rejected, not run as code or trusted because of its hash.

## Validation

Run the converter/API tests from the SDK root and the native parser/backend tests from `esp32/`:

```sh
python -m unittest discover -s avatar_pixel/tests -p 'test_*.py'
python -m unittest discover -s tests -p test_pixel_art.py
python -m unittest discover -s tests -p test_pixel_backend.py
```

The C harnesses use ASan/UBSan and test hostile bounds, alpha/opaque black,
immutable strips, owner/generation changes, temporary mount/allocation retries,
interrupted candidates and pinned-model lifetime. The compiler-to-C integration
test feeds synthetic robot, animal, plant and logo packs at both supported
profiles into the production parser and renderer.

Snapshot validation builds additionally accept `pixel.test_wifi=0/1`. The
command persists a private test flag with readback and disables or restores the
existing Wi-Fi setting. During the isolated test, the flag suppresses Link's
initial boot scan/join/VM request as well as the keeper's autojoin; merely
disconnecting after startup is not adequate offline evidence. Restore the
original Wi-Fi-on setting and clear that flag in a `finally` block. Builds
without snapshots omit both the command and boot override.

Native P4/S3 builds and on-device cold boot, offline restore, rendering, state
transitions, settings preservation and memory measurements remain required for
a deployment. Generic visual feedback does not claim anatomical expressions,
audible TTS or precise speech lip-sync.
