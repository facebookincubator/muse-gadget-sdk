---
name: board-power-profiling
description: Plan and run Nordic PPK2 battery-input power profiling on a board or device other than the SenseCAP Watcher — choose labelled source capture, a ported ESP32 diagnostic sweep, or a protocol port to another platform. Use when asked to measure, characterize, or optimize power of a new board; never as hardware authorization.
---

# Power profiling a new board or device

The full recipe is `esp32/docs/power/README.md`; the worked example is the
SenseCAP Watcher (`esp32/docs/power/sensecap-watcher.md` and its findings).
Read the recipe before acting. This skill is the decision and safety outline.

## Gates

- Hardware access (flashing, opening serial ports, enabling PPK2 output,
  asking someone to unplug or power-cycle) needs the board owner's explicit
  authorization for that action. Prefer host tests and emulation otherwise.
- Before any PPK2 output, settle the fixture with the owner from the schematic
  (recipe section 0): battery removed, battery-input pinout and polarity,
  battery-detect/NTC expectations, charger VBUS→VBAT path and whether USB
  overlap needs blocking, explicit source millivolts, peak current under ~1 A,
  power latch, reset-surviving rails, USB-detect signal, console type. Record
  the basis of each answer; never claim protection that isn't fitted.
- Use one long-lived PPK2 owner from before USB removal until USB is back
  (`ppk2-power-profile` skill). Never stop, reopen or retune it while it is
  the only supply.

## Choose the level

1. **Labelled source capture, any device, no firmware change.** `ppk2_profile.py
   hold` plus `control begin/end` labels; drive states over BLE, buttons or a
   device-side timer. Good for a first look and for non-ESP32 hardware.
2. **Autonomous sweep on another ESP32 board in this repo.** The shared core
   is already board-neutral: `muse_power_test.{c,h}`, `CONFIG_MUSE_POWER_TEST`
   and `boards/muse_power_test_board.h` are the porting surface. Implement
   `muse_ptest_board_*` hooks (including `name` and board-owned `matrix` tables
   with A/B/A references), extend the supported-board Kconfig list, add an
   overlay and host-test fakes. Keep the Watcher conformance tests green.
   Dry-run on USB before any PPK2 run.
3. **Autonomous sweep on another platform.** Port the core's `fw_*` platform
   boundary or speak the PTEST line protocol natively; the host-compiled C core
   is the conformance reference for `power_sweep.py`.

## Run and report

- Pause other heavy work on the capture host; use `--receiver-max-bytes
  16777216`; require idle and statistics preflights (99–101% coverage, zero
  loss, backlog plus consumer delay < 0.5 s) before USB removal; FIFO-only
  health checks while USB is out.
- Power-cycle before any "cold" state; a dry run consumes cold state.
- Analyze with `esp32/tools/power/power_sweep.py analyze --guard-s 1.0
  --clock-drift-ppm 250` and `compare`. Firmware's `board` names the report;
  `--device "<board>"` overrides it (Watcher is the fallback for legacy bundles
  only). New captures use `power-sweep`; legacy `watcher-sweep` is accepted.
  Report conditional receive-window
  estimates, A/B/A deltas with reference drift, one-repeat and
  source-setpoint caveats.
- Publish only scrubbed CSV/findings: no serial numbers, boot/run IDs, host
  paths, network names or credentials. Change production defaults only on
  resolved A/B/A evidence.
