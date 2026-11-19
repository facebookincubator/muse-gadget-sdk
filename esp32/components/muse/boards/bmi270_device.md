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

# BMI270 capability adapter

`bmi270_device.c` exposes `accel`, `gyro` and `steps` to the capability
registry. Espressif's pinned **`bmi270_sensor` 0.4.1** owns the low-level
Bosch driver and Base firmware; the SDK no longer carries a configuration
blob or programs feature-page registers itself.

## Optional dependency

`MUSE_BMI270_DRIVER` is an always-defined component-manager selector:
`"y"` only for `MUSE_BOARD_M5STACK_STOPWATCH && CAPABILITIES_REGISTRY`,
otherwise `"n"`. The manifest fetches the driver only when this selector is
on. The adapter's sources are likewise in the StopWatch CMake block behind
`CONFIG_CAPABILITIES_REGISTRY`.

Thus a StopWatch apps-off build and apps builds for other boards do not
fetch or link the BMI270 library or `i2c_bus` solely for this driver.
Shared dependencies such as `cmake_utilities` can still be fetched when
another component needs them.
This is build-time isolation, not installing a driver at runtime. Use the
capability registry's `dev_*` API; no sensor-specific WASM import is added.

The package contains precompiled static libraries per ESP-IDF major/minor
and chip. Its `6.0/esp32s3/libbmi270_sensor.a` supports the SDK's ESP-IDF
6.0.3 build. Keep the pin and verify an available matching library before
changing ESP-IDF. The adapter also pins `i2c_bus` 1.5.2 and
`cmake_utilities` 0.5.3 under the same gate; use the current master backend, not
`CONFIG_I2C_BUS_BACKWARD_CONFIG`.

## Runtime ownership and behavior

- The board supplies its existing synchronous native I2C master bus and
  axis/sign map. The library uses 400 kHz transfers with a 200 ms timeout;
  it owns only its sensor device, never the caller's shared bus.
- Boot does an ID-only probe. Vendor initialization/configuration upload
  occurs on the adapter worker at first use, never in an app callback.
- Base firmware provides steps. Circle/Toy firmware is not used: those
  feature sets cannot be combined with Base. Existing tap/double-tap/shake
  and face-up/down events remain software-derived while accel is open.
- The worker configures 100 Hz sampling, +/-8 g and +/-2000 deg/s, converts
  raw readings to units/screen axes, and pushes at the requested rate.
- Closing all devices disables the sensors and step feature, restores
  advanced power save and retains the loaded handle for the next open.
  A configuration/power transition failure stops the adapter until reboot;
  it attempts to restore rest and release the vendor device. Failed deletion
  retains the cleanup-only handle and is retried by the worker.

Although only Base runs, the package's runtime firmware selection links
all three 8 KB firmware images and some Circle/Toy support code. This is
a flash-size cost, not enabling Circle/Toy on the chip. Check image size
when updating the package; unused sensor firmware remains in the image.

The package exports `bmi270_sensor_disable` but omits its declaration from
`bmi270_api.h`. The adapter uses the exact prototype from Bosch's public
BMI270_SensorAPI v2.113.0 `bmi270.h`, verified against the pinned library.
This disables feature sensors such as the step counter; `bmi2_sensor_disable`
alone accepts only physical sensors.

## Verification

Run `tests.test_bmi270_device`, `tests.test_apps_dev` and the focused apps
tests. The adapter harness exercises lazy start, units/axes, steps,
suspend/resume and initialization/configuration failures with a fake
vendor API. Build real apps-enabled firmware to validate the actual
headers/binary linkage. Configure apps-off and a different board with apps
from an empty managed-components directory and check that neither resolves
`bmi270_sensor`; compare apps-off ELF sections to main.

Hardware must be rechecked after switching drivers, including sensor
rates, step counting, gestures, shared-bus touch responsiveness and idle
power. A successful build and fake-API tests do not establish those.

### StopWatch hardware check (2026-11-16)

Installed the apps-enabled ESP-IDF 6.0.3 build with the recommended
PSRAM-statics setting. Boot was clean, control registration acknowledged,
and all five previously installed apps survived. Run apps had to be enabled
on the device before running the test; the off state correctly refused it.

A temporary USB-loaded app opened accel/gyro at 50 Hz and steps at 1 Hz,
closed all three after six seconds, then reopened after a two-second pause.
The log confirmed suspension and subsequent operation. Both active phases
returned valid accel/gyro/step-counter data: 27 successful reads of each,
432 queued samples collected for each motion sensor. Acceleration was
approximately 1 g at rest and gyro readings responded to motion. The
collector drained only every 500 ms, so these sample totals do not qualify
exact streaming-rate accuracy. The app used 114 KB peak and exited normally.
A 150 ms buzz action returned success, with no reported I2C/motor transition
failure. The device was left online and idle with its full 2 MB app budget.

Walking-step accuracy (the mostly stationary test returned zero), physical
vibration strength, software gesture accuracy, axis orientation, touch under
bus load and battery idle power still require dedicated hardware checks.
A successful command return is not confirmation that a pulse was felt.

## Upstream references

- [Espressif package](https://components.espressif.com/components/espressif/bmi270_sensor/versions/0.4.1):
  `README.md`, `include/bmi270_api.h`, `include/bmi2.h`, `include/bmi2_defs.h`,
  `CMakeLists.txt` and `examples/raw_data/` in the published package.
- [Bosch BMI270 SensorAPI](https://github.com/boschsensortec/BMI270_SensorAPI/tree/v2.113.0):
  `bmi270.h` for the disable API; `bmi270.c` and `bmi2.c` for the
  distinction between feature and physical sensors.

The binary package ships its own Apache-2.0 and Bosch BSD-3 license notices.
Keep the upstream notices intact when distributing managed dependencies.
