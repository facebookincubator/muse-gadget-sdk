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

# muse_restart

Restarts the firmware means to make, told apart from the ones it doesn't, so a
boot's reset reason can be trusted: a watchdog reset is a real one.

- `muse_restart(why)` for every planned restart: the console's `>restart`, an
  OTA done, pairing done, a setup reset, a new SDK token. It leaves a marker in
  RTC memory (survives every reset but a power loss; checked against its
  complement, so a power-on's leftovers never pass), tells ESP-Iris the
  restart is planned on the ESP-Mosaico, then restarts. A reboot out of a
  failure (a stuck panel, a BLE stack that won't stop) stays a plain
  `esp_restart()`, so a loop of them still counts as one.
- `esp_reset_reason()` is wrapped (`-Wl,--wrap`) for the whole image: an RTC
  watchdog reset right after a marked restart reads as a software restart;
  any other watchdog reset, and task or interrupt watchdogs and panics always,
  stay what they are. ESP-Iris's crash report and crash-loop counter, the
  battery log and bug reports all see that.
- The boot log says how each boot began, now and again 15 s later (for a log
  capture that starts late): `link.main: boot: intentional restart (console,
  from core 1); chip: software, raw 0x03/0x03`, or `boot: panic; raw ...`.
  On the ESP-Mosaico, `tools/muse/iris_bench.py boot` asks for it over ESP-Iris.

## The ESP32-S31 (ESP-Mosaico)

With ESP-IDF master (7b9cc1ac), `esp_restart()` on the S31 came back as a
watchdog reset: raw reason 0x10 (`RESET_REASON_SYS_RWDT`), `ESP_RST_WDT`.
`esp_restart_noos()` arms the RTC watchdog for 1 s (stage 0: system reset,
flash-boot protection) and then resets the CPUs (`hpcore0_sw_reset`); that CPU
reset didn't get the chip booting, so the watchdog's system reset did, a second
later. Measured on the board, each through a marked restart:

| Restart | Raw reason | Reported before |
|---|---|---|
| console, in the RPC handler (core 0) | 0x10 RTC watchdog | watchdog |
| a task on core 0, after the RPC answered | 0x10 | watchdog |
| a task on core 1 | 0x10 | watchdog |
| ESP-Iris's own restart after a system update | 0x0c CPU0 software | software |
| the ROM's system reset (`esp_rom_software_reset_system()`) | 0x03 core software | software |

So it isn't intended behavior: the watchdog is the restart's safety net, and a
working CPU reset reads 0x0c (as after ESP-Iris's update restart, with the app
quiet). The S31's `esp_system_reset_modules_on_exit()` is still a TODO in that
IDF (IDF-14841); on the P4, the same function stops the DMAs and resets MSPI,
timers and UARTs before the CPU reset. With the app running, something left
active keeps the CPU reset from booting. The S3 (StopWatch, ESP-IDF v6.0.1)
restarts with `RTC_SW_CPU_RST` (software) as it should.

The cost wasn't only the label: ESP-Iris's crash-loop counter counts
`ESP_RST_WDT` as a failed boot, and three within 30 s boot the recovery
firmware. So on the S31 `muse_restart()` runs `esp_restart()`'s shutdown
handlers itself and ends in the ROM's whole-system software reset, which resets
every module and reads 0x03 (software); the marker survives it. After the
change every console restart, from either core, read
`intentional restart (console, from core N); chip: software, raw 0x03/0x03`,
and ESP-Iris's crash count stayed 0. A panic's restart still goes through
`esp_restart_noos()` and may read as a watchdog reset (0x10); the panic's own
record says what happened.

Tests: `tests/test_muse_restart.py` (the marker, the reason with and without
it, and the wiring: the wrap, planned restarts marked, failure restarts plain).
