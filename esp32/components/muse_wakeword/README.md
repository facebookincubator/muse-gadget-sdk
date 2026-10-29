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

Hands-free activation: a clap (or a serial bench trigger) starts the
same voice note as holding the talk button.

## What this is

- `include/ww_runner.h` - the interface contract with the wake
  word runner. No ESP-IDF types, so the host tests link the same
  code.
- `ww_runner_bench.c` - the runner linked here. It fires on serial
  bench triggers (`wake=1`, `wake.test=1`) or a loud-then-quiet
  energy burst (a clap). **It detects no speech and no wake word**;
  the Settings row's small print says the model is not implemented
  yet, and the boot log names it `runner=bench`, so it cannot be
  mistaken for detection.
- `include/muse_wakeword.h` + `muse_wakeword_core.c` - the
  session state machine, the integer energy fallback, and the
  caller-owned frame ring behind the handoff queue and pre-roll.
- `Kconfig` - the options. `MUSE_WAKEWORD_ENABLE` defaults to
  `n` and no board overlay sets it, so shipped builds are unchanged.

The firmware glue in the `muse` component taps `muse_voice` audio,
runs the wake task, posts synthetic push-to-talk turns (with the
trigger audio as pre-roll), and persists a runtime opt-in. Turning
listening on or off is one shared setter, driven by the Sound
settings toggle, the quick-menu row, `>wake.on` / `>wake.off` over
serial (status: `>wake`), and the Muse-side `enable_wakeword`
control.

Run the host tests with:

```sh
python3 -m unittest tests.test_muse_wakeword -v
```
