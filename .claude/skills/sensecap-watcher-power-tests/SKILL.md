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
   re-arm blindly after a timeout: command outcome may be unknown. Omission
   of `--matrix` keeps peripheral v1 (27 states, repeats 1–2). Use
   `--matrix sleep --repeats 1` only for sleep v2 characterization from truly
   untouched codec hardware; MCU reset cannot restore cold codec registers.
   Sleep repeats >1 are explicitly rejected. Verify the emitted plan: v2 has
   26 states including final `warm_ref_9` before its two timer-deep states.
   This extra A brackets both late combined/polling B variants.
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

## USB-only diagnostic preflight

After authorized console access, run
`python3 tools/muse/watcher_power.py dryrun --port DUT_PORT --matrix sleep --output RUN/dryrun.json --timeout-s 60`
while USB remains connected and no sweep is active. It strictly applies/reads
sleep indices 0–23, skips deep entry, holds the USB awake lock, exercises no PM
or timing dwell, restores resting configuration, and emits per-state evidence
plus exactly one final summary. It uses no PPK source and leaves the RTC run
journal unchanged. Bank error rows/raw frames; false `codec_regs_expected` is
not an error, while I2C transport/transition/restore errors are. Logs are bounded
96-character safe ASCII with no UART forwarding; wildcard NONE/selective
BSP ERROR plus disconnected ROM printf sinks keep UART protocol-only. Drain
TX to actual idle before parking pads and after dry-run lines; timeout fails
without parking. Successful states clear ignored-component logs. First fatal evidence is
checkpointed before cleanup. Codec maps contain integer bytes/null and detected
ADC variant, with unavailable snapshots explicit. The host verifies fresh
nonced STATUS before/after, unchanged boot and live USB; any error returns
nonzero. **This consumes cold codec state: fully power-cycle before a subsequent
cold quantitative sweep.** Never infer hardware authorization from this skill.

## What counts as evidence

The arm reply lists the actual state plan. The device journal records actual
capture timestamps, status, readback, sleep/frame/scan counts and resets.
Differentiate planned, applied, functionally observed, skipped and rejected.
An empty capture is not zero current. A failed peripheral transition stops the
sweep rather than being averaged as a low-power success.

The minimum isolated sweep covers CPU idle policies, automatic light sleep,
LCD/touch rail and brightness, shared audio codecs/I2S/PA, mic draining,
bounded sine playback, battery divider/conversions and unassociated Wi-Fi
scan. Those unsupported peripheral-v1 radio/connected/SSCMA/SD/Grove/deep-sleep
states stay explicitly skipped. Sleep v2 instead characterizes cold,
constructor-initialized and actually opened/closed codecs, reversible narrow
GPIO/pull/isolation knobs, CPU power-down permission, slower VBUS polling and
two timer-only deep resets. Initialized means constructor register writes
(including ADC enable), not cold or powered down; suspended means checked
open/disable/close driver sequencing, not proof from register-byte equality.
Codec snapshots are evidence: only I2C transport errors fail, and
`codec_regs_expected:false` never rejects a measurement. Failed codec
init/open/disable requires ordered vendor shutdown with transport-checked
snapshots and poisoned-driver non-reuse; constructor-only success stays intact.
Independent fenced RTC markers track pad ownership before holds and sticky warm
attempts before constructors. Release owned holds before UART setup even with
an invalid run journal; preserve warm proof across subsequent retained resets,
never relabel an MCU reset as cold. Final/abort resting readback is mandatory.
On ESP-IDF 6.0.1/S3 RTC fast+slow retention is forced by rtc_sleep_init, not absent
public RTC memory-PD enum calls. CPU ON/OFF reference balancing vetoes/allows retention PD
without clearing other owners; permission is not observed PD proof. Internal
pull/wake changes are diagnostic-only, not a production-ready fix.

ACK/status/results identify matrix/version and always include sleep timeline
uncertainty. Plans carry knobs/deep_sleep/poll_ms plus role/ref_group; records
carry verified-knob/codec metadata and entry boot identity. Match every deep
record's entry/resume chain, timer wake, deep-sleep reset and RTC duration:
only a valid matching pending journal may resume. Require retrieval and live
boot to equal the last timer-resume boot (or original armed boot with no
resumes); an ordinary intervening reset is recovery evidence, not quantitative
continuity. Virtual timestamps retain the run epoch and add ceil(1% RTC elapsed)
+50 ms per resume: a conservative unvalidated slow-RC timing assumption. Keep
this uncertainty inside guards; do not infer deep sleep from light-sleep counters.
Do not relabel rail power as proven camera operation.

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
