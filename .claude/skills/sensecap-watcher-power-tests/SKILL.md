---
name: sensecap-watcher-power-tests
description: Run USB-free Nordic PPK2 power profiling on a Seeed SenseCAP Watcher: flash the opt-in power-test image, arm a peripheral/state sweep while USB and PPK power overlap safely, prompt unplug/replug, retrieve timestamped results, compare fixes, and document measured versus skipped states.
---

# SenseCAP Watcher power tests

Use this for Watcher power characterization or power-fix validation. Read
`esp32/docs/power/sensecap-watcher.md` first; it is the setup, wiring,
measurement-boundary and CLI reference. For PPK source ownership, also read
`.claude/skills/ppk2-power-profile/SKILL.md`.

## Safety gates

- Hardware access/flash needs the operator's authorization. Never infer it
  from this skill, flash another connected board, or change security eFuses.
- Confirm the actual fixture with the operator: PPK VOUT to the verified
  battery input, GND common and internal battery disconnected. Establish that
  brief USB/source overlap is suitable for that actual charger, battery-sense
  and blocking arrangement; install protection or isolate the charger when
  required by the power path. Record the evidence basis and do not pretend
  an external isolator exists. Battery removal alone is not proof, and these
  tests characterize USB-free loads, not the charger's behavior.
- 3872 mV is a battery-input example, not USB or a direct MCU rail. Record the
  PPK setpoint and independently measured DUT voltage under load; never
  compensate a diode drop by guessing a higher voltage.
- Keep Watcher USB attached until the single PPK owner is powered and
  fault-free. Keep that owner alive until Watcher USB is reconnected and
  confirmed. Do not kill a keeper to start measurement.
- Both Watcher USB sockets can power it: all Watcher USB power must be removed
  for measurement. Leave PPK USB and source wiring connected.

## Procedure

Paths below are relative to `esp32/`. Use a fresh run directory and explicit
ports, with no credentials in commands or committed results.

1. Identify the CH342 Watcher and its ESP32 console (second serial interface).
   The first is the Himax coprocessor; do not flash it. Verify the running
   firmware's board name using `tools/muse/chat.py --status` when available.
2. Build `build-watcher-power-test` using ESP-IDF v6.0.1 and the defaults chain
   `sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-sensecap-watcher;devices/sdkconfig.muse-watcher-power-test`.
   Run host tests and check partition sizes. Preserve NVS and existing factory
   backups. This isolated image needs no account/SDK token.
3. Flash over USB with **`tools/muse/paced_esptool.py`**, 115200 baud, using
   that build's `@flash_args`; never plain `idf.py flash` for the CH342. Let it
   finish. Verify the diagnostic ready reply and absence of panic/reset loop.
4. Start `tools/power/ppk2_profile.py hold` in a persistent terminal or managed
   background task, with the explicit port/voltage, actual wiring
   acknowledgments, `--output RUN/ppk.jsonl --control-dir RUN/control`.
   Its READY and a fault-free `control ... status` prove software ownership,
   not correct electrical wiring.
5. Run `tools/muse/watcher_power.py arm --port DUT_PORT --ppk-control-dir RUN/control --output RUN/acquisition.json`.
   It begins recording via the existing owner, bounds the UART clock mapping,
   and requires the Watcher's actual VBUS-present acknowledgment. Do not
   re-arm blindly after a timeout: command outcome may be unknown.
6. Only then prompt: **"Unplug all Watcher USB cables now. Leave PPK USB and
   source wiring connected."** The device starts its sweep on actual VBUS
   absence. No BLE connection is required for radio-off states.
7. Hold the task for the bounded duration reported by `arm`, measured from
   unplugging. Do not call it successful just because time elapsed. Prompt
   the operator to reconnect Watcher USB when the bound has elapsed.
8. Run `watcher_power.py collect --port DUT_PORT --ppk-control-dir RUN/control --acquisition RUN/acquisition.json --output RUN/results.json --usb-reconnected`.
   It verifies live USB, run/boot identity, and retrieves the retained result
   journal before allowing the owner to release source power.
9. Run `watcher_power.py analyze --results RUN/results.json --samples RUN/ppk.jsonl --output RUN/profile.json`.
   Preserve raw acquisition locally; publish only reviewed, scrubbed results.

On failure, reconnect Watcher USB safely before explicit PPK finish. Never
unplug PPK or terminate its process while it is the only source. Do not
restart source mode, auto-reset hardware, or erase NVS as generic recovery.

## What counts as evidence

The arm reply lists the actual state plan. The device journal records actual
capture timestamps, status, readback, sleep/frame/scan counts and resets.
Differentiate planned, applied, functionally observed, skipped and rejected.
An empty capture is not zero current. A failed peripheral transition stops the
sweep rather than being averaged as a low-power success.

The minimum isolated sweep covers CPU idle policies, automatic light sleep,
LCD/touch rail and brightness, shared audio codecs/I2S/PA, mic draining,
bounded sine playback, battery divider/conversions and unassociated Wi-Fi
scan. Unsupported radio/connected/SSCMA/SD/Grove/deep-sleep states stay
explicitly skipped. Do not relabel rail power as proven camera operation.

This is **isolated BSP characterization, not the production application**.
Measure the normal application separately for advertising, connected idle,
animation, screen-off transition, Wi-Fi-napped rest, wake/reconnect and voice.
Report OAT changes as board-input marginal costs, including regulator loss.

Report mean/min/max, repeats, sample coverage, voltage basis, timestamps,
settling/guard and timing limitations. The Python PPK stream lacks exact lost
frame positions; accepted guarded receive-window estimates are not
sample-perfect synchronization. Do not recombine per-window reservoir
percentiles or extrapolate rejected/aborted profiles into battery life.

For fixes, compare A/B/A under the same voltage, stimulus and radio settings,
and validate wake/display/audio recovery. Document repeat variation. A code
change is not a power improvement until measured. Draft the PR only with
truthful setup, findings and limitations; do not land or alter another PR
without authorization.
