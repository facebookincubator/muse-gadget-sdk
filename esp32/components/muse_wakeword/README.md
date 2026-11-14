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

Hands-free activation: saying "Hey Muse" (or "Ok Muse") starts the
same voice note as holding the talk button.

## Status: ExecuTorch runner implemented; feature still off by default

`ww_runner_et.cpp` drives the embedded int8 `models/hey_muse.pte`
through the ExecuTorch registry component (kernel bundle generated at
build time). `MUSE_WAKEWORD_ENABLE` defaults to `n` and no board
overlay sets it, so shipped builds are unchanged.

Verified by `tests/test_muse_wakeword.py`:

```sh
python3 -m unittest tests.test_muse_wakeword -v
```

## Trying it on a BOX-3

For the real runner, build with the committed overlay
`devices/sdkconfig.muse-wakeword` (token setup as usual):

```sh
EXECUTORCH_PYTHON=<python with executorch> MUSE_WAKEWORD=1 tools/muse/board.sh build box3
MUSE_WAKEWORD=1 tools/muse/board.sh flash box3
```

Then run `>wake.on` and say "Hey Muse". The model's false-alarm rate
has not been measured on a device yet, so expect false triggers; `@wake`
and the `muse_wake`/`ww_et` logs show the flow.

The overlay already trims for size and speed as follows. ExecuTorch
runtime logging is compiled out (`CONFIG_ET_ENABLE_LOGGING` unset).
Measured on BOX-3 it saves ~91 KB with no latency cost (inference
still ~15 ms/frame), and `et_model`/runner error logs still reach
the console. Program verification is already off
(`CONFIG_ET_PROGRAM_VERIFICATION` unset; the runtime falls back to
minimal header checks), and the build-time kernel bundle already
registers only the kernels the model uses, so there is nothing
further to prune there. `CONFIG_ET_OPTIMIZE_SIZE` defaults to `y` in
the registry component; the overlay unsets it.
A build with it (plus an Error log floor) measured 67 KB smaller but
slowed inference to ~23 ms/frame, over the 20 ms budget with tap-frame
drops, so it was rejected.

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
