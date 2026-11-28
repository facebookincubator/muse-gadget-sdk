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

# Vendored: the haptics core

`mg_haptics.c`, `mg_haptics.h`, `tests/test_mg_haptics.py` and
`tests/mg-haptic-v1.json` are the Muse Gadgets haptics core (mgcommands.h,
Haptics: `haptic_play` / `haptic_stop` / setting 15), vendored whole:
change them only together, with the vectors and the core's own suite
passing. `mg_haptics.c` and `mg_haptics.h` carry this repo's license
header. The build points `MGH_MGCOMMANDS_H` at this repo's
`protocols/mgcommands.h`.

`tests/test_mg_haptics.py` (the core's own suite) and
`tests/mg-haptic-v1.json` (the core's test vectors) replay every vector
scenario through this copy under ASan/UBSan;
`esp32/tests/test_mg_haptics_core.py` runs them with this repo's paths.
The board backend (the motor, the gate states, the timer) is in `mg_ble.c`.

`mg_haptic_test.c` and `mg_haptic_test_core.h` beside them are this repo's,
not part of the core: the vibration tests (Developer > Tests > Vibration and
the console's `vib` words) built on the core's public API without changing
it; `tests/test_mg_haptic_test.py` runs them.
