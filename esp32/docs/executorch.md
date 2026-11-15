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

# ExecuTorch runtime (registry component)

The [ExecuTorch](https://github.com/pytorch/executorch) runtime comes
from the ESP Component Registry, pinned exact in the wake word
component's
[`idf_component.yml`](../components/muse_wakeword/idf_component.yml),
next to the microfrontend the model's features come from:

```yaml
pytorch/executorch:
  version: "==1.6.0-dev.20261112"
  rules:
    - if: "$CONFIG{MUSE_WAKEWORD_EXECUTORCH} == \"y\""
espressif/esp-nn:
  version: "==1.5.0"
  rules:
    - if: "$CONFIG{MUSE_WAKEWORD_EXECUTORCH} == \"y\""
esphome/esp-micro-speech-features:
  version: "==1.2.3"
  rules:
    - if: "$CONFIG{MUSE_WAKEWORD_EXECUTORCH} == \"y\""
```

The rules keep all three out of every build without `MUSE_WAKEWORD_ENABLE`:
those builds don't download them, list them in `dependencies.lock`, or
compile any of them. `MUSE_WAKEWORD_EXECUTORCH` is a string that is
always defined (`"y"` with the feature, `"n"` without), because the
component manager can't evaluate a bool whose dependencies are unmet.
ExecuTorch itself asks for `espressif/esp-nn` `^1.4`; the pin holds the
delegate's kernels at the release the model was checked with.

There is no vendored tree. The component manager downloads the
components into `managed_components/` at build time, checking the
publisher's checksums (`managed_components/` and `dependencies.lock` are
gitignored build artifacts; `tools/muse/board.sh` deletes them around
every build). The component's own `README.md` covers using the runtime;
this page covers how the wake word uses it and updating the pin.

## How the wake word uses it

`models/hey_muse.pte` runs entirely in the ESP-NN delegate: the program
is one delegate call with no CPU operators. So the wake word component
declares only the backend, `executorch_add_backend(EspNNBackend)`: the
build links the runtime and the delegate, no kernel library and no prim
ops, generates no kernel bundle, and needs no executorch Python. A model
that needed CPU kernels would need a kernel bundle (the component's
`tools/gen_kernels.py`).

`ww_runner_espnn.cpp` drives ExecuTorch's `Program`/`Method` API
directly rather than the component's `et_model` C API, so it can bind
the model's inputs and outputs to its own buffers once and run each
step as a single `execute()` (the file's header explains). The method
arena is sized exactly when the runner starts.

The overlay `devices/sdkconfig.muse-wakeword` sets the component's
options for the wake word: `CONFIG_ET_OPTIMIZE_SIZE` off (the default
`-Os` makes inference about 40% slower), `CONFIG_ET_ENABLE_LOGGING` off,
and `CONFIG_ESPNN_DELEGATE_CONSTANTS_TO_PSRAM` off (weights are read
from flash).

The delegate in this release reads the clock around every operator and
converts the model's streaming state between layouts on every step, even
with profiling off, and keeps about 17 KB of tables that a newer
delegate can read in place from the model. A change proposed upstream
removes that work: about 160 µs per inference (a quarter of the ESP-NN
time) and 17 KB of RAM on an ESP32-S3. It needs no SDK change beyond a
version bump and, for the stateful form, a re-export.

The host tests build the same runner, delegate and microfrontend from the
same components, with small host stand-ins for the ESP-IDF headers the
delegate includes in `components/muse_wakeword/host/`. So does the Linux
client's native library (`linux/wakeword/build.sh`), from the same registry
archives.

## Updating the components

Bump the `==` pins in
[`components/muse_wakeword/idf_component.yml`](../components/muse_wakeword/idf_component.yml),
and the matching archive names and SHA-256 sums in
`linux/wakeword/build.sh`. Re-export the model only if the new release needs it, and keep `models/hey_muse.json` in step. Then:

- build the wake-word firmware for an ESP32-S3 board
  (`MUSE_WAKEWORD=1 tools/muse/board.sh build box3`) and a normal build,
  which must still pass without downloading either component;
- run the host tests, whose `WakeWordRunner` class builds the runner
  against the downloaded components and checks the model's output bytes;
- with a board attached, flash the wake-word firmware and check the
  boot log's `Hey Muse runner ready` line, `>wake.on`, and the timing
  line the runner logs after 15 s of listening.

Note the binary size change in the PR. Rollback is reverting the PR.

Models (`.pte` files) are not part of the component; they ship with the
application component (`components/muse_wakeword/models/`).
