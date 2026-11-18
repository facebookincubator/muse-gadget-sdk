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

# muse_wakeword

Hands-free activation: saying "Hey Muse" starts the same voice note as
holding the talk button.

## Status: off by default

`ww_runner_espnn.cpp` runs the embedded int8 `models/hey_muse.pte`
(model card in [`models/README.md`](models/README.md)) through
ExecuTorch's ESP-NN delegate, on TensorFlow Lite Micro's microfrontend
features (`ww_frontend.c`), with microWakeWord's detector
(`ww_detector.c`). `MUSE_WAKEWORD_ENABLE` defaults to `n` and no board
overlay sets it, so shipped builds are unchanged: without it the
component compiles nothing and the registry components aren't
downloaded.

While listening is on, the microphone and codecs stay powered: a board
on battery doesn't rest them when its screen sleeps, so battery life is
much shorter with the wake word on.

## Cost

Measured on a SenseCAP Watcher (ESP32-S3, 240 MHz) running the full
firmware (UI, BLE), listening with the screen on:

| | This runner | The earlier ExecuTorch runner |
|---|---|---|
| Inference | 1.2 ms per 30 ms (0.9 ms with the screen off) | 17.7 ms per 20 ms |
| Front end + inference, CPU time | 2.5 ms per 30 ms (8 to 9% of one core) | 20 ms per 20 ms (the audio tap overflowed) |
| Internal RAM | 14 KB (front end tables and small delegate tables) | not measured |
| PSRAM | 29 KB for the runner, plus the 133 KB of audio buffers | not measured |
| Model | 23 KB | 74 KB |

Run back to back, the same runner takes 0.7 ms per inference on the
Watcher with its buffers in PSRAM, close to the 0.6 ms of a test image
without the UI that has everything in internal RAM. Listening is slower because it runs one
inference every 30 ms, and in between the UI, radios and audio push the
model's code and weights out of the chip's caches (32 KB instructions,
64 KB data), so each inference reloads them from flash. Measured as wall
time, the wake task also shares its core and priority with the UI's
rendering task, which adds another 2 to 4% of a core: the runner's own
log reports that (3.2 to 3.8 ms per 30 ms).

The runner and the delegate put their larger buffers in internal RAM
only when 48 KB of it would stay free, which on the Watcher with its UI
it doesn't; that costs little (above).

Verified by `tests/test_muse_wakeword.py`, whose `WakeWordRunner` class
builds this runner for the host against the same registry components
(they're in `managed_components/` after a wake-word configure, or set
`MUSE_WAKEWORD_DEPS`):

```sh
python3 -m unittest tests.test_muse_wakeword -v
```

## Trying it

Build with the committed overlay `devices/sdkconfig.muse-wakeword`
(token setup as usual), on a board with PSRAM and the LVGL UI:

```sh
MUSE_WAKEWORD=1 tools/muse/board.sh build watcher
```

The overlay builds ExecuTorch at the project's `-O2`
(`CONFIG_ET_OPTIMIZE_SIZE` unset), turns its logging off and leaves the
model's weights in flash. Then run `>wake.on` and say "Hey Muse". `@wake`
and the `muse_wake`/`ww` logs show the flow; after 15 s of listening the
runner logs its timing and heap.

Over serial:

```text
>wake.on       listening on (persisted; >wake.off turns it off)
>wake          prints @wake status (also what the commands return)
```

On the device, the same persisted opt-in is the **Sound > Wake word** settings switch
(on button boards, the quick menu's **Wake word** row). A submit only
reaches Muse once the gadget is paired; unpaired, the turn stops
at the setup caption, and the `muse_wake` log lines plus `@wake`
counters show the flow.

Muse can only turn listening off (`wakeword.disable`, the same as
`wake.off`); there is no remote command to turn it on, so only someone
at the device can start the gadget listening.
