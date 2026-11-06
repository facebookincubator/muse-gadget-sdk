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

# ExecuTorch component (`components/executorch`)

[`components/executorch`](../components/executorch) is the
[ExecuTorch](https://github.com/pytorch/executorch) runtime vendored
as a source ESP-IDF component, for the wake word work. **It is
generated. Never hand-edit anything in that directory** — fixes land
upstream in ExecuTorch and the tree is regenerated. Its own
`README.md` covers using the runtime; this page covers regenerating
and updating it.

Today no shipped board uses it: `sdkconfig.defaults` sets
`CONFIG_ET_ENABLE=n`, which makes the component register empty, so
default firmware is unchanged (this matters because ESP-IDF links
every component under `components/` and ExecuTorch's kernel
registration is deliberately GC-proof). The wake word runner in
`components/muse_wakeword` selects `ET_ENABLE` when the feature is
compiled in with the ExecuTorch runner choice.

## Prerequisites

Generation happens on your computer, from an ExecuTorch checkout.
Consumers of this SDK need none of it — only ESP-IDF.

1. A checkout of `pytorch/executorch`, at the commit you want to
   vendor (the current one is recorded in
   [`components/executorch/VERSION`](../components/executorch/VERSION)).
   The generator is `examples/espressif/build_idf_component.sh` in
   that repo, added with this component — older ExecuTorch releases
   do not have it.
2. Two submodules initialized in that checkout:
   `git submodule update --init third-party/flatcc third-party/flatbuffers`
3. A Python with ExecuTorch and torchgen importable, installed *from
   that checkout* (`./install_executorch.sh` there). Kernel codegen
   needs torchgen, and the exporter/runtime versions must match or
   models fail later with unhelpful errors; the generator warns when
   the installed package was built from a different commit. Point at
   the right interpreter with `PYTHON=...` if `python3` is not it.
4. `cmake`. If no `flatc` is on `PATH` (or `FLATC=...`), the
   generator builds one from `third-party/flatbuffers` to produce
   the schema headers. Consumers never need flatc.

## Regenerate

From the ExecuTorch checkout:

```sh
PYTHON=<executorch-python> \
  examples/espressif/build_idf_component.sh \
  --profiles wakeword_int8 \
  --out <muse-gadget-sdk>/esp32/components/executorch
```

`--profiles` takes a comma-separated list; each name is a file in
ExecuTorch's `examples/espressif/idf_profiles/<name>.ops` (one root
op per line). The shipped profile is `wakeword_int8` (the int8 Hey
Muse model: `cadence::` quantized ops on the Cadence backend's
generic/portable kernels, plus the aten glue ops). A portable-fp32
variant measured ~69 ms per 20 ms frame on ESP32-S3 — not real
time — and was removed. A profile may mix `aten::` and `cadence::`
root ops; the generator codegen's each namespace separately and
vendors the kernel sources both need. Each profile gets its own
generated kernel registration in the component, and boards pick one
with `CONFIG_ET_OP_PROFILE_<NAME>=y` in their
`devices/sdkconfig.<board>` file — switching profiles never needs
regeneration. The first profile in the list is the Kconfig default.

The generator refuses to overwrite a directory that is not a previous
output; `--out <dir> --clean` removes a generated tree.

## Check the update before opening a PR

Review the result as generated code: read the `VERSION` diff
(ExecuTorch commit, per-profile op-set SHA-256, kernel counts) and
file/size deltas. Do not line-review vendored sources. Then build
the consumers: this SDK's wake-word firmware for one Xtensa and one
RISC-V board (the builds that actually link the component), plus a
normal board build, which must still pass and stay ET-free while
`CONFIG_ET_ENABLE=n`:

```sh
tools/muse/board.sh build box3
```

With a board attached, flash the wake-word firmware and confirm the
boot log's runner-ready line and `@wake` status over serial. Note
the binary size change in the PR: when enabled, `wakeword_int8`
costs roughly 0.2 MB of flash text (plus the ~80 KB embedded model
in the application component). Rollback is
reverting the PR — component and any models version together.

Models (`.pte` files) are not part of the component and are never
regenerated here; they ship with the application component (today:
`components/muse_wakeword/models/`), exported against the op-set
hash in `VERSION` by that component's `tools/export_hey_muse.py`.

## Using the component

- `#include <et_model.h>` — small C inference API (model bytes in
  flash, arenas via heap_caps with internal/PSRAM placement).
  Application pipelines built on it (the wake word frontend and
  detector in `components/muse_wakeword`) live with the
  application, not in this runtime component.
- `#include <executorch/...>` — full C++ runtime. ESP-IDF has no
  mmap, so `MmapDataLoader` exists but always fails with
  `NotSupported`; use `BufferDataLoader`. Runtime logs go to ESP_LOG
  under the `executorch` tag (`CONFIG_ET_ENABLE_LOGGING`,
  `CONFIG_ET_LOG_LEVEL_*`).
