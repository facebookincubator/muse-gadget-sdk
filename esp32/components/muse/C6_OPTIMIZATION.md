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

# C6 rendering optimization

## What changed

- Default-avatar dirty detection reads the 4096 undimmed palette cells directly,
  instead of scaling 64 full-width screen rows merely to sample them. It omits
  the 1024-byte scratch row. A customized avatar can omit the new accessor and
  retain the strip-scaling fallback; canvases smaller than 64 pixels use whole
  image invalidation, since those sizes skip grid cells.
- The default renderer reuses fixed palette conversions and face-column fields,
  skips silhouette hashes outside the edge band, rejects limbs by row, limits
  outline traversal to the avatar bounds, and caches exact dot-ring trig results.
  Cached results use the original arithmetic, not approximated animation.
- Bezel clipping uses exact integer roots. The fixed C6 radius/hole bounds use
  355 bytes of flash tables, with no RAM cache. Different geometry falls back
  to integer computation. LVGL still draws the ring and its progress indicator.
- The avatar's two trig slots use 1096 bytes of BSS. Removing the default dirty
  scratch row offsets 1024 bytes of that allocation. Compiler stack usage also
  grows (standalone RV32IMAC compilation added 320 bytes to the render frame);
  the C6 adapter's default task stack is 8 KiB. After the exercised device UI
  workloads, read-only TCB canary scans found 2272 untouched bytes for `lvgl`
  and 6796 for `swdraw`. These are workload-specific headroom samples, not
  guarantees for unexercised audio paths; check them again when changing fields.
- Normal C6 builds use 16-line buffers. BLE performance builds default to 48.
  `CONFIG_MUSE_C6_DRAW_BUF_LINES` permits rebuilding at other heights. Each extra
  line costs 1472 bytes across the two internal-RAM buffers, directly competing
  with offline audio. Larger buffers are a benchmark candidate, not a measured
  production recommendation. Prefer even heights: LVGL safely rounds odd
  full-width heights down, leaving one allocated row unused.
- The flush-start hook drains a stale completion token before the next DMA is
  submitted. An early adapter interrupt can clear LVGL's `flushing` flag, causing
  LVGL to skip the wait callback; that unconsumed semaphore token must not release
  a later transfer. The adapter notification is retained for its bookkeeping.
- OLED brightness initialization finishes before the C6 adapter worker starts;
  later brightness commands share its recursive LVGL mutex. Brightness is a
  QSPI parameter write, not independent PWM. A live debugger snapshot of a
  build without that serialization showed both `muse_boot` (brightness initialization) and
  `lvgl` (submitting the second 48-line strip) blocked on the same SPI-device
  semaphore while BLE heartbeats continued. The bus lock arbitrates devices,
  not competing tasks using one device; serialize those callers explicitly.
- Live `bufs=` replacement is disabled: freeing LVGL's buffers leaves adapter
  DMA bookkeeping referencing its original storage. Rebuild to sweep heights.
  `cf=` rejects invalid values rather than silently changing byte order.

A flash-backed A8 ring-image cache was prototyped and pixel-tested, then rejected:
its host refresh benchmark was slower than the existing slab-clipped LVGL arc.
It is not part of the production code.

## Repeatable checks

From `esp32/`:

```sh
# One direct IDF build first retains managed cJSON for the complete host suite.
IDF_PATH=~/.espressif/esp-idf-v6.0.3 \
  python3 -m unittest discover -s tests -p 'test_*.py'

cmake -S simulator -B simulator/build -G Ninja \
  -DMUSE_SIM_WARNINGS_AS_ERRORS=ON
cmake --build simulator/build --parallel
ctest --test-dir simulator/build --output-on-failure

# An isolated host timing report, not C6 FPS:
python3 tests/test_muse_pixel_perf.py --benchmark
```

The frozen renderer oracle checks 5980 deterministic frames: palette indices,
mask, RGB565 palette, dim palette, and accessor. Scaling covers all sizes 1–512
in all seven modes, plus guarded random rectangles. For every size 64–512,
the accessor is checked against the first screen pixel used by dirty detection.
The suite includes ASan/UBSan and optional-accessor link-present/link-absent probes.

The C6 flush regression compiles the actual board callback and registration
snippets against deterministic semaphore/LVGL/adapter fakes. It exercises 3616
early, delayed, synchronous and check-to-wait DMA schedules, including repeated
28-strip frames, with ASan/UBSan. Bypassing the drain reproduces the premature
release; the test includes that negative control. It also checks actual startup
ordering and compiles the brightness setter against a recursive-lock fake for
external callers, already-owned callers and failed acquisition, under normal
and sanitizer builds. The original unlocked writer is another negative control.
These are scheduling/serialization models, not physical DMA tests.

The ring harness compares complete framebuffers with the previous float clipping
across 360 combinations of translations, angles, strip heights 4/16/32/48/64,
and native/swapped RGB565. It also exhausts integer geometry up to radius 1024.
The UI simulator uses a native host CPU and placeholder settings; it does not
measure MCU performance, settings-list FPS, BLE load, heap pressure, or power.

## Measured C6 BLE results

Measured on an ESP-IDF v6.0.1 build: a C6 V2 ran the default avatar with `-O2`,
48-line buffers, swapped RGB565 and a 16-entry LVGL circle cache, with a BLE
client connected.
These are real panel transfers with injected UI gestures, not host timings.

Profiler-off gesture medians below include the approximately 300 ms settle
window in their effective FPS: do not confuse these with instantaneous render
FPS. Each row has five repetitions, except thinking/listening (three each).

| Mode / gesture | Frame average median (ms) | Effective gesture FPS median |
| --- | ---: | ---: |
| Idle, face to settings | 51.1 | 13.3 |
| Idle, settings to face | 42.4 | 18.2 |
| Settings drag up | 25.1 | 25.3 |
| Settings drag down | 27.9 | 21.6 |
| Thinking, face to settings | 40.3 | 16.3 |
| Thinking, settings to face | 30.8 | 23.9 |
| Listening, face to settings | 52.0 | 13.0 |
| Listening, settings to face | 44.6 | 17.8 |

Four complete five-second windows per state maintained approximately 20 FPS.
Per-window median avatar tick times were 8.45 ms idle, 12.2 ms thinking and
13.4 ms listening; median frame averages were 11.6, 9.75 and 17.15 ms respectively.
The 42 periodic windows reported 87–89 KiB free internal heap, a 77 KiB minimum
and a 68 KiB largest block. Those are UI-workload values, not audio-load limits.

All 34 profiler-off gesture destinations matched, including two navigation
transitions. All 214 list-drag frames were under 33 ms, but one inter-flush gap
reached 105 ms; the worst profiler-off frame across all gestures was 71.9 ms.
There were no gesture caps, timeouts, unexpected reboots, panics or freezes.
Two profiler-on gestures were slower (10.2/15.1 effective FPS) and are excluded.
Both a cold boot and a reset after debugging reached UI ready,
`mg.perf`, and explicit console marker acknowledgements.

The 48-line buffers alone use 46 KiB more than the normal 16-line buffers.

## Validation limits and future checks

Normal C6/C6 BLE builds retain 16-line buffers for audio headroom; their FPS
was not separately measured. Offline capture, push-to-talk and playback under
BLE load, physical colour correctness and real touch-sensor behaviour remain
unverified. Larger buffers are not a production recommendation until
those workloads' minimum heap and stack margins have been checked. Additional
buffer heights require separate builds: `bufs=` is refused and doesn't change
allocated buffers. Native host timing is not MCU FPS.
