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

# Optional avatar rendering

These experimental paths extend existing avatar sync with shared animation, reusable pixel packs and local white-background matting. All new feature flags default off. Shipped board profiles, the existing Built-in/My Muse source selection, pairing and home controls are unchanged.

## Data flow

The authenticated Muse identity and canonical still feed avatar sync and its private cache. Optional mode clips use the shared software H.264 decoder. A host compiler can convert the still into a data-only pixel pack, installed through the USB console and rendered locally. Matting derives alpha without editing the phone or VM avatar.

## Implementation highlights

### Shared software H.264 on S3 and P4

`CONFIG_MUSE_AVATAR_ANIMATIONS` enables constrained-stream playback on suitable PSRAM-equipped S3 and P4 UI builds. The local decoder archive/linker mapping avoids a P4 dependency collision; idle clips must loop, and absent or invalid mode clips retain the still fallback. This is software decoding, not a P4 hardware-decoder implementation or support for arbitrary phone MP4s. See [avatar sources and animation constraints](avatar-sources.md).

### Pixel compiler, API and common renderer

[`avatar_pixel`](../../avatar_pixel/README.md) provides a Python library, CLI and self-hostable API. It contain-fits PNG/JPEG/WebP images into 64- or 96-pixel profiles, reduces the palette, optionally dithers, and emits an indexed sprite, RGB565 palette and A8 alpha. The [v1 pack contract](../../protocols/PIXEL_AVATAR.md) bounds and checks the data; there is no executable renderer or inferred face rig in a pack.

The shared ESP32 renderer supplies breathing, bobbing, tap feedback and subtle state tint. A new validated pack needs no firmware rebuild once feature support is installed. Normal rendering remains the default style: `pixel.on` selects pixels, `pixel.off` restores normal rendering, and `install --no-enable` updates a pack without changing the selection. No phone Settings UI is added. See [pixel build, installation and controls](pixel-avatars.md).

### Conservative white-background removal

`CONFIG_MUSE_AVATAR_WHITE_MATTE` derives an immutable display image and A8 mask from a uniformly white canonical still, preserving the original RGB565/cache bytes. It removes background-connected white, retains enclosed white details, softens the edge and composites over the canvas. Analysis runs outside frame rendering.

The compiler/API offers the same opt-in treatment with `--remove-white-background` or `remove_white_background: true`; existing nonopaque RGBA is preserved. USB export can carry the pinned still's derived mask into pixel compilation. This is uniform-white matting, not general segmentation. H.264 clips remain opaque. See [background behavior, alpha export and memory limits](avatar-background.md).

## Configuration and recovery

Use the normal ESP-IDF v6.0.3 board/token setup. `devices/sdkconfig.muse-avatar-rendering` is an optional combined fragment for a suitable S3/P4 UI board; it enables sync, animations, pixels and matting without selecting pixel style. Features can also be enabled separately. Speaker/mute, captions, touch controls and volume behavior stay unchanged.

Private pixel caches bind to the stored owner and validated generation. HTTP 502s and temporary allocation/storage failures do not themselves invalidate a matching pack. An identity mismatch or unreadable identity blocks outdated pixels; a late still-cache load cannot re-authorize them. Pairing/token journals fence provisional ownership, rollback preserves the prior durable pack without erasing a validated mismatch, and real owner changes revoke it. Committed credential rotation can still require revalidation/reinstallation. Invalid/interrupted installations keep the prior valid file; mount failures never format existing data.

## Validation and limits

Before a push, run `tools/verify_commit.sh <exact-commit>` from `esp32/` with the host packages installed. It builds the simulator, prepares pinned registry test dependencies in a clean worktree, and rejects failures or skips. Run compiler/API tests and lint from the SDK root, and build the optional P4/S3 profiles. Native sanitizer harnesses cover bounds/CRCs, alpha, strip lifetimes, cache/journal recovery and synthetic compiler-to-C packs. Hardware deployment separately requires cold-boot/offline restoration, interaction, settings and memory evidence on the target.

The API binds loopback by default and accepts uploaded bytes, not remote image URLs. Public hosting and automatic conversion of future avatars are separate work. Anatomical rigs, lip-sync, per-frame video alpha and audio/caption fixes are outside this contribution.
