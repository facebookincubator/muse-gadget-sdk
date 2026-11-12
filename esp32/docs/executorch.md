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
from the ESP Component Registry, pinned exact in
[`main/idf_component.yml`](../main/idf_component.yml):

```yaml
pytorch/executorch:
  version: "==1.6.0-dev.20261112"
```

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
`CMakeLists.txt`, gated on the feature and the ExecuTorch runner
choice, so default firmware is unchanged. A project that declares
nothing and calls `et_model` fails to link instead of failing every
model load.

## Kernel bundle

The component registers only the kernels in the project's kernel
bundle. Nothing is committed: the wake word component generates the
bundle at configure time, into the build directory, with the committed
helper [`components/muse_wakeword/tools/make_kernel_bundle.py`](../components/muse_wakeword/tools/make_kernel_bundle.py),
which loads the component's `tools/gen_kernels.py` with its SHA gate
dropped (see that script for why, and what provenance it records).
The build then declares the generated bundle, and refuses one whose
component version, commit, or model hash does not match the declared
model and component.

Wakeword builds need a Python with executorch (any nightly whose
codegen matches the component) and PyYAML installed. Point the build
at it with `-DEXECUTORCH_PYTHON=<python>` or the `EXECUTORCH_PYTHON`
environment variable; without one, configuration fails with a message
saying so. Default builds need nothing: the bundle step only runs for
the ExecuTorch runner. Do not commit a generated bundle directory: a
component update needs no SDK change beyond the version pin, and the
bundle regenerates from the declared model on the next configure.

## Updating the component

Bump the `==` pin in [`main/idf_component.yml`](../main/idf_component.yml),
and re-export the model only if the new component needs it (the
exporter is `components/muse_wakeword/tools/export_hey_muse.py`; its
sidecar records the export, and the build-time bundle check is the
gate that the model's ops are covered). Nothing else in the SDK
changes: the bundle regenerates at configure time, so no bundle commit
is needed, and no SDK update is needed at all until someone chooses
to bump the pin.

Review the update before opening a PR: build the wake-word firmware
with an executorch Python available (see above) and check the
configure log says the bundle generated, plus the usual file/size
deltas. Do not line-review component sources (they are downloaded,
not vendored). Then build the consumers: this SDK's
wake-word firmware for one Xtensa and one RISC-V board (the builds
that actually link the component), plus a normal board build, which
must still pass and link no ExecuTorch code:

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
