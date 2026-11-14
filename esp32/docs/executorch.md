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
[`idf_component.yml`](../components/muse_wakeword/idf_component.yml):

```yaml
pytorch/executorch:
  version: "==1.6.0-dev.20261112"
  rules:
    - if: "$CONFIG{MUSE_WAKEWORD_EXECUTORCH} == \"y\""
```

The rule keeps it out of every build without `MUSE_WAKEWORD_ENABLE`:
those builds don't download it, list it in `dependencies.lock`, or
compile any of it. `MUSE_WAKEWORD_EXECUTORCH` is a string that is
always defined (`"y"` with the feature, `"n"` without), because the
component manager can't evaluate a bool whose dependencies are unmet.

Do not loosen the pin to a range: a kernel bundle is made for one
component version, a respin included, and the build refuses a bundle
made for another version.

There is no vendored tree. The component manager downloads the
component into `managed_components/` at build time
(`managed_components/` and `dependencies.lock` are gitignored build
artifacts; `tools/muse/board.sh` deletes them around every build).
The component's own `README.md` covers using the runtime; this page
covers the pin, build-time bundle generation, and updating it.

The component links nothing into firmware until a model and a kernel
bundle are declared. The wake word component declares them in its
`CMakeLists.txt`, only with the feature on. A project that declares
nothing and calls `et_model` fails to link instead of failing every
model load.

## Kernel bundle

The component registers only the kernels in the project's kernel
bundle. Nothing is committed: the wake word component generates the
bundle at configure time, into the build directory, by running the
component's own `tools/gen_kernels.py` unmodified. The generator checks
that the installed executorch is the one the component was built from
(its version and commit) before it generates anything, so a mismatched
code generator can't produce the bundle. The build then declares the
generated bundle, and refuses one whose component version, commit, or
model hash does not match the declared model and component.

Wakeword builds need a Python with executorch and PyYAML installed.
Use the nightly the component was built from, which the component's
`VERSION` names: for `1.6.0-dev.20261112` that is
`executorch==1.6.0.dev20261112` (commit `2a00808`) from
`https://download.pytorch.org/whl/nightly/cpu`, with
`torch==2.14.0` from `https://download.pytorch.org/whl/test/cpu`.
The generator refuses any other version, the stable
`executorch==1.6.0` from PyPI included. Point the build
at it with `-DEXECUTORCH_PYTHON=<python>` or the `EXECUTORCH_PYTHON`
environment variable; without one, configuration fails with a message
saying so. Default builds need nothing: the bundle step only runs for
the ExecuTorch runner. Do not commit a generated bundle directory: a
component update needs no SDK change beyond the version pin, and the
bundle regenerates from the declared model on the next configure.

## Updating the component

Bump the `==` pin in [`components/muse_wakeword/idf_component.yml`](../components/muse_wakeword/idf_component.yml),
and re-export the model only if the new component needs it (the
exporter is `components/muse_wakeword/tools/export_hey_muse.py`; its
sidecar records the export, and the build-time bundle check is the
gate that the model's ops are covered). Nothing else in the SDK
changes: the bundle regenerates at configure time, so no bundle commit
is needed, and no SDK update is needed at all until someone chooses
to bump the pin.

Review the update before opening a PR: build the wake-word firmware
with the matching executorch Python (see above) and check the
configure log says the bundle generated, plus the usual file/size
deltas. The component isn't vendored, so a PR shows only the pin; what
the build itself verifies is the exact version, the publisher's
checksums (the component manager checks every file it downloads) and
the generator's executorch version check. Review the component's
release as you would any dependency update. Then build the consumers: this SDK's
wake-word firmware for one Xtensa and one RISC-V board (the builds
that actually link the component), plus a normal board build, which
must still pass without downloading ExecuTorch:

```sh
tools/muse/board.sh build box3
```

With a board attached, flash the wake-word firmware and confirm the
boot log's runner-ready line and `@wake` status over serial. Note
the binary size change in the PR: when enabled, the registered kernels
cost roughly 0.2 MB of flash text (plus the ~80 KB embedded model
in the application component). Rollback is reverting the PR — pin
and any models version together.

Models (`.pte` files) are not part of the component and are never
regenerated here; they ship with the application component (today:
`components/muse_wakeword/models/`).

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
  `CONFIG_ET_LOG_LEVEL_*`). `CONFIG_ET_OPTIMIZE_SIZE` defaults to `y`
  in this component; wake word builds unset it (inference time
  matters more than the flash it saves). `CONFIG_ET_PROGRAM_VERIFICATION`
  compiles in full `.pte` verification, which `et_model_config_t.verify`
  requires. `CONFIG_ET_MAX_KERNEL_NUM` sizes the kernel registry; the
  default, 0, sizes it from the bundle.
