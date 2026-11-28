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

# S3 2.06 avatar fixes and validation

Hardware validation on the Waveshare ESP32-S3-Touch-AMOLED-2.06 (32 MiB flash, 8 MiB PSRAM) exposed two memory limits that prevented reliable optional avatar operation. The fixes retain the existing pairing protocol, image formats, cache ownership and safety reserves. Avatar features and pixel style remain opt-in.

## Internal/DMA headroom

The board profile now includes the same four placement settings used during the hardware run:

```text
CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM=y
CONFIG_MUSE_STACKS_IN_PSRAM=y
CONFIG_HOMEHUB_SETUP_TASKS_IN_PSRAM=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024
```

CPU-only avatar/audio/UI buffers and eligible setup/session stacks move to PSRAM through existing SDK mechanisms. The board already runs code and read-only data from PSRAM. Flash-mapping, power, input, console and actual DMA paths remain internal; stack sizes and watchdogs are unchanged. Generated configurations must retain these settings, not silently drop them through unmet dependencies.

The diagnostic profile originally had about 16 KiB internal memory free with a 4 KiB largest block and an audio-DMA admission failure. Static-buffer placement enabled the codec self-test; the complete placement profile and release configuration later held about 70 KiB free with a 56 KiB largest block. These are observed results for this board, not guaranteed heap values on every S3.

## Bounded image admission

The old preflight required the entire 4 MiB image/matte maximum plus the 1 MiB reserve before downloading any still. Only about 3 MiB of PSRAM remained after code placement and connection setup, so even a small valid image was refused.

Admission now derives an effective per-attempt ceiling from free PSRAM, retained image allocations, the encoded size and the existing workspace allowance. Decoder and matte buffers share that limit; cache and matte retries obtain fresh limits. The existing 4 MiB/1.5 MiB caps, 1 MiB reserve, 48 KiB/internal-DMA gates and dimensions are unchanged. Allocation pressure rejects the candidate rather than replacing valid display/cache content. Native regressions cover 3 MiB-free admission, reserve refusal, held-image accounting, concurrent release sampling and allocation/replacement recovery.

## Hardware evidence

The runtime run used commit `3c2e31e496c947547b598a2bdd90bff8c10b3680` with the four settings above, on ESP-IDF v6.0.3. That commit's exact-head verifier passed 2,422 tests with no failures, errors or skips. The board-specific release app was 2,560,000 bytes in a 4 MiB slot. Later documentation/configuration commits do not constitute another hardware run.

The personal still loaded at 466×466 with derived A8 transparency and was saved to its private cache. A complete USB export passed bounds, CRC and generation checks. The library and loopback API produced identical pixel packs; style-preserving installation, mode feedback and restoration of normal style passed. Interrupted/corrupt serial transfers were rejected, not padded. No personal images or generated packs accompany this public report.

Software H.264 playback was exercised through the real home-avatar decoder and UI using a synthetic 160×160, 12 fps constrained-baseline clip. Approximately 11.94 decoded/UI frames per second, changing frame CRC/revision and zero decoder errors were observed. Working-preview, voice-state suspension/resume and sleep/wake passed. The fixture was removed and the personal still restored. This verifies local software playback, not phone-animation parity or video alpha.

Network-isolated USB-reset boots restored the normal alpha still and optional pixel pack with no IP address or avatar identity/file reads. Wi-Fi, normal style and unrelated settings were restored afterward. A release boot, real chat round trip and 65-second observation passed without panic or reboot; the saved pixel option also loaded after the release flash and was switched off again.

Physical power cuts, brownouts, battery runtime, acoustic quality and end-to-end spoken replies were not measured. The release image omits snapshots, performance/profiler instrumentation and bench/test hooks. See the [avatar overview](avatar-rendering.md), [pixel controls](pixel-avatars.md) and [matting limits](avatar-background.md).
