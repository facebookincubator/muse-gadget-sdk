# Power profiling other boards and devices

The SenseCAP Watcher work in this directory is one instance of a reusable
method: power a board from a Nordic PPK2 through its battery input, remove
USB, measure settled states, and analyze them with explicit quality checks.
This recipe says what carries over to other hardware and what you have to
write. The Watcher [setup guide](sensecap-watcher.md) is the worked example;
read it alongside this page.

Pick the lowest level that answers your question:

| Level | You get | Device changes | Reuses |
|---|---|---|---|
| 1. Labelled source capture | Continuous source power and per-label mean/min/max/percentiles/charge | None | `tools/power/` (PPK2 owner) |
| 2. Autonomous sweep on another ESP32 board here | Device-timestamped USB-free states, firmware evidence, guarded analysis, A/B/A comparisons, timer deep sleep | Board hooks and a state matrix for the diagnostic image | Level 1, `tools/power/power_sweep.py`, the C state machine |
| 3. Autonomous sweep on another platform (Zephyr, other MCUs) | As level 2 | Port the state machine's platform boundary, or speak its line protocol natively | Level 1, `tools/power/power_sweep.py` |

None of this authorizes hardware access. Flashing, opening serial ports and
enabling PPK2 output each need the board owner's explicit go-ahead.

## 0. Fixture (every level)

Work through this with the board's schematic before any output is enabled,
and record the answers next to your results.

1. **Battery input.** Find the battery connector or pads and their pinout.
   Remove the cell; never put PPK2 in parallel with a battery. Connect PPK2
   VOUT to the battery positive and a common ground.
2. **Battery detection and thermistor.** Many chargers and PMUs (e.g. AXP2101)
   refuse to run, or behave differently, without a battery-present or NTC
   signal. Note what the board expects and how the fixture satisfies it. Do
   not short a thermistor pin to defeat charge protection.
3. **Charger path and USB overlap.** The usual handoff keeps USB connected
   until the PPK2 source is on, then removes USB. If the charger connects VBUS
   to VBAT, USB present means charging into the PPK2. Decide, from the actual
   power path, whether brief overlap is acceptable or needs a blocking diode,
   ideal diode or charger disable. Record the basis (measured, datasheet, or
   operator assertion); don't claim protection that isn't fitted.
4. **Source voltage.** Choose an explicit setpoint in millivolts inside the
   battery's and PMU's range (3872 mV was a Li-ion example). A series diode
   lowers the voltage at the board; measure it under load if you want DUT
   energy rather than source-setpoint energy.
5. **Peak current.** The PPK2 source is good to roughly 1 A. Radio TX bursts
   reached ~0.4 A on the Watcher; check your board's worst case.
6. **Staying on without USB.** Find any power latch the firmware must hold
   (the Watcher's expander `EXP_PWR_SYSTEM` bit, a PMU power-key hold).
7. **Rails that survive an MCU reset.** Codecs, sensors, displays or radios on
   always-on rails keep whatever state earlier firmware left them in, through
   reflashing and ESP-only resets. A "cold" measurement needs a full power
   cycle (USB out, PPK2 output off). On the Watcher this was a 33 mA effect.
8. **USB detection.** Level 2/3 firmware must see USB disappear: a VBUS GPIO,
   expander input or PMU register. Note it and its polarity.
9. **Console.** A UART bridge (CH34x, CP210x) or native USB-Serial-JTAG. Either
   works: the console disappears while USB is out and results are read after
   reconnecting. A connected USB-Serial-JTAG vetoes automatic light sleep, so
   only USB-free windows are meaningful.

## 1. Any device: labelled source capture

No firmware changes. One long-lived PPK2 owner keeps source power continuous
while you label windows from the host. Follow the PPK2 skill
(`../../../.claude/skills/ppk2-power-profile/SKILL.md`) for install, safety
acknowledgments and recovery. In outline:

```sh
cd esp32
tools/power/.venv/bin/python tools/power/ppk2_profile.py discover
# DUT USB still connected; fixture verified:
tools/power/.venv/bin/python tools/power/ppk2_profile.py hold \
  --port "$PPK_PORT" --voltage-mv 3872 --receiver-max-bytes 16777216 \
  --ack-source-wiring --ack-battery-isolated --ack-no-charging-backfeed \
  --ack-usb-connected-before-start \
  --output "$RUN/ppk.jsonl" --control-dir "$RUN/control"
```

After READY, remove DUT USB. Put the device in each state by something that
doesn't need USB (BLE/NUS, a button, a timed script already on the device),
wait for it to settle, then label a window:

```sh
C="tools/power/.venv/bin/python tools/power/ppk2_profile.py control --control-dir $RUN/control"
$C begin --label idle --duration-s 20     # or begin … then end
$C status                                  # check faults/coverage between states
# reconnect DUT USB first, then:
$C finish --usb-reconnected
```

Each label ends with an `end` record carrying `mean_uA`, `min_uA`, `max_uA`,
`p50_uA`…`p99_uA`, `sample_count`, `coverage_ratio_estimate`, sampled charge
and source-setpoint energy, plus loss counters:

```sh
jq -c 'select(.type=="end") | {label, mean_uA, max_uA, sample_count, coverage_ratio_estimate, decoded_missing_frame_count}' "$RUN/ppk.jsonl"
```

Limits: labels are host receive times, not device events. Change the device
state, wait well past its settling time, then `begin`; `end` before changing
it again. Reject a label with coverage outside 99–101% or any nonzero loss
counter. Bracket every variant between two reference labels (A/B/A) and
report the drift between them.

## 2. Another ESP32 board in this repo

The diagnostic image (`CONFIG_MUSE_POWER_TEST`) splits into a
board-neutral core and board hooks:

- `components/muse/muse_power_test.{c,h}`: line protocol, arm/run state
  machine, RTC journal, light-sleep accounting, deep-sleep resume chain, dry
  run. It compiles on the host against fakes (`MUSE_PTEST_HOST`), which is how
  `tests/test_power_test.py` tests it.
- `components/muse/boards/muse_power_test_board.h`: the board-neutral porting
  surface, `muse_ptest_board_*`. The Watcher implementation and its unchanged
  peripheral/sleep descriptor tables stay in `board_sensecap_watcher.c` under
  `CONFIG_MUSE_POWER_TEST`; the core contains no Watcher board include or tables.
- `tools/power/power_sweep.py`: arm/collect/analyze/compare/dryrun. It lives with
  the PPK owner/receiver because it coordinates their captures; the existing
  `tools/muse/chat.py` USB transport (and sibling `ports.py`) is reused via an
  explicit import path, not moved or duplicated. The plan and `board` name
  come from firmware. `analyze --device "<board name>"` overrides the name;
  Watcher is the default only for legacy bundles without board identity.
  New captures use `power-sweep`; legacy `watcher-sweep` captures remain
  accepted. Legacy acquisition/dryrun type tags remain unchanged for bundle
  compatibility. Acquisition/collect provenance, wire schema 1, matrix versions,
  journal layout/budget, prefix analysis and comparison semantics are unchanged.

### Steps

1. **Gather the power facts** (section 0) plus, per peripheral, which rail,
   expander bit or GPIO powers it and which pins it drives. Cite the
   schematic and vendor BSP as `devices/AGENTS.md` asks for any board work.
2. **Board-neutral core: done.** `CONFIG_MUSE_POWER_TEST`,
   `muse_power_test.{c,h}` / `muse_power_test_run()` and
   `boards/muse_power_test_board.h` are shared already. Do not rename or fork
   the core for another board. Extend the supported-board `depends on` list
   in `components/muse/Kconfig` after implementing that board's hooks/matrices;
   CMake includes the common core independently of the selected BSP. Keep the
   Watcher conformance tests green.
3. **Implement the board hooks** for your board:

   | Hook | Contract |
   |---|---|
   | `name()`, `matrix(name,count)` | Stable board name for status/ACK/results; immutable board-owned descriptor table for `peripheral` or `sleep`. Unknown/unsupported matrix returns NULL and count zero; rejected strictly. No allocation. |
   | `init` | Bring up only what the diagnostic needs (I2C, expander/PMU, power latch held). No production app, NVS or radios. |
   | `power(out)` | Read USB presence (and charging if known) without the ADC. Called often; must be cheap and must not change state. |
   | `apply(state)` | Put every owned peripheral into the state's configuration and revert anything the previous state changed (knobs, rails, codecs). Return `ESP_ERR_NOT_SUPPORTED` for an unsupported state, other errors for real failures. |
   | `readback(out)` | Report what is actually configured: owned outputs, PWM duty, codec/driver state, knobs applied. Check things you drive; report, don't assert, chip registers you can't verify. |
   | `service(load)` | Per-tick work for active loads (stream audio, sample an ADC). Bounded time. |
   | `uart_restore` | Reconnect console pins if a state parked them; called as soon as USB reappears. |
   | `prepare_deep(held)`, `release_deep_holds`, `deep_holds_owned` | Pad holds for deep-sleep states, and their recovery after any reset. Only needed for deep-sleep states. |
   | `codec_regs`, `codec_history`, `restore_codec_history`, `warm_seen` | Retained evidence for reset-surviving peripheral history. Boards without codecs return zeroed/unknown snapshots and cold history; do not remove the shared hooks or claim unknown registers are verified. |
   | `usb_dryrun(on)` | Allow diagnostic USB rehearsal where board-safe, without changing PM or quantitative run state; clear on exit. |

4. **Define the board-owned matrix** in your BSP, returned by
   `muse_ptest_board_matrix`; never insert board tables in the core. Each entry
   is a `muse_ptest_state_t`:
   load, CPU policy (0 = DFS + automatic light sleep), knobs, poll interval,
   `role` (`ref`/`variant`/`other`) and `ref_group`. Rules:
   - Put every variant between two references of the same group, so `compare`
     can measure it A/B/A.
   - Cold states first, once per power cycle (sleep matrices reject repeats).
   - Deep-sleep states last.
   - Keep unsupported features as explicit `MUSE_PTEST_UNSUPPORTED` skips,
     never silent omissions.
   - Mind the RTC journal budget (static assert in the core).
5. **Add an overlay**, `devices/sdkconfig.muse-<board>-power-test`, loaded after
   the board's own overlays. Copy `sdkconfig.muse-watcher-power-test`:
   `CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE`,
   `CONFIG_PM_LIGHT_SLEEP_CALLBACKS`, `CONFIG_PM_PROFILING`, OTA and bug reports
   off, console settings for your bridge. Leave
   `CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND` off unless you measure it: GPIO sleep
   isolation cost +0.10 mA on the Watcher.
6. **Extend the host tests.** Add your board's fakes beside
   `tests/power_test_harness.c` and `tests/power_test_bsp_harness.c`,
   cover apply/revert, readback and USB-return paths, then run the full suite
   (`python3 -m unittest discover -s tests -p 'test_*.py'`).
7. **Build and flash** in its own build directory, the way `../../AGENTS.md`
   describes for your board.
8. **Rehearse on USB.** `power_sweep.py dryrun --port … --matrix sleep` applies
   and reads back every non-deep state with USB connected and reports
   per-state errors. Fix everything it reports before using the PPK2. It
   consumes cold state, so power-cycle afterwards.
9. **Capture** as in the Watcher guide: start the PPK2 owner with USB connected,
   pass the coverage preflights, `arm`, remove USB, wait the reported bound,
   reconnect, `collect`, finish the owner. Then
   `analyze --guard-s 1.0 --clock-drift-ppm 250 --device "<board>"` and
   `compare`.
10. **Publish** a scrubbed CSV and findings like
    `sensecap-watcher-sleep-findings.md`: no serial numbers, boot/run IDs,
    host paths or network names.

## 3. Other platforms (Zephyr, other MCUs)

Level 1 works today for anything the PPK2 can power. For an autonomous sweep
there are two routes; neither has been done yet for a non-ESP32 device.

- **Port the core.** The state machine depends on `esp_err_t` codes and a small
  platform boundary of static `fw_*` functions at the top of
  `muse_power_test.c`: clocks (`fw_timer_now`, `fw_rtc_now`), USB and
  state hooks (`fw_power`, `fw_apply`, `fw_readback`, `fw_service`), console
  output (`fw_write`, `fw_flush`) and deep sleep (`fw_timer_reset`,
  `fw_prepare_deep`, `fw_enter_deep`). The host build supplies these with fakes
  and an `esp_err.h` shim (`tests/power_test_fakes/`); a Zephyr port
  would supply them from Zephyr APIs and keep the journal in retained RAM
  (`__noinit` / a retained-memory region).
- **Speak the protocol natively.** `power_sweep.py` only needs the line
  protocol documented in `muse_power_test.h` and the guide's "State
  matrix and evidence" section: `>ptest.status=<nonce>` echoing the nonce with
  `boot_id`, `now_us` and `usb`; `>ptest.arm=…` acknowledged with the plan; a
  sweep that starts on USB absence; `>ptest.results` with per-record
  `measure_start_us`/`end_us`/`status`/`apply_err`. Use the host-compiled C
  core as the conformance reference: the host tests feed its real output
  through `arm`, `collect` and `analyze`.

## 4. Lessons that apply everywhere

- **Host load.** The PPK2 streams 400 kB/s; a busy host (parallel compilers,
  endpoint agents) can stall the consumer. Use `--receiver-max-bytes 16777216`,
  pause other builds, and require the idle and statistics preflights to pass
  (99–101% coverage, zero loss, backlog plus consumer delay under 0.5 s)
  before removing USB. Check health from the FIFO while USB is out.
- **Cold means power-cycled.** See section 0, item 7.
- **Keep the protocol UART clean.** Route logs away from it, mute ROM printf
  sinks, and drain TX before parking UART pins. The Watcher dry run caught
  merged lines from exactly these.
- **Prove light sleep.** Report measured sleep time and entries per window;
  enabled PM is configuration, not evidence. Any held PM lock (a USB console
  lock, a driver) silently prevents sleep.
- **Treat numbers as conditional.** Host receive windows with guard bands and
  an assumed clock-drift budget, not hardware timestamps; one repeat gives no
  confidence interval; source-setpoint energy is not DUT energy.
- **Change production code only on A/B/A evidence.** A difference smaller than
  the drift between its references is unresolved.
