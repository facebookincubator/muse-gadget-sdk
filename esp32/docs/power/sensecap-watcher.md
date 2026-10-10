# SenseCAP Watcher: USB-free PPK2 power profiling

For other boards and devices, start with the [porting recipe](README.md);
this page is its worked example.

## Measurement status

The USB-free diagnostic sweep completed: **23 measured states, four explicit
skips, no firmware or collector errors**. See the [measured profile and limitations](sensecap-watcher-findings.md)
and [scrubbed CSV](sensecap-watcher-profile.csv). A second sleep-matrix sweep
covers light-sleep knobs and timer deep sleep: see the
[sleep deep dive](sensecap-watcher-sleep-findings.md) and
[its CSV](sensecap-watcher-sleep-profile.csv). Figures are conditional
receive-window estimates under an assumed 250 ppm drift budget and 1-second
guards; neither sample-exact timing nor a production power fix is claimed.
The fixture replaces the internal battery with PPK2 VOUT on VBAT and common
GND. It has no external protection; USB/source overlap suitability is an
operator assertion, not an independently characterized charger/power-path
result. DUT terminal voltage was not independently measured.

USB overlap is only used to preserve operation before and after the sweep.
The measurement target is settled, USB-free operating states—not transition
energy or a sample-perfect characterization of the power handoff.
The default diagnostic image is not the normal Muse
application: it measures reversible board/peripheral loads without background
voice, provisioning, and reconnection tasks changing the state under test.
Production application profiles must be recorded separately.

## Hardware safety and measurement boundary

The target is the Seeed SenseCAP Watcher: ESP32-S3, 32 MB flash, 8 MB octal
PSRAM, SPD2010 LCD/touch, ES8311 output codec, ES7243/ES7243E microphone ADC,
speaker PA, Himax camera/vision coprocessor, PCA9535 power expander, and RTC.

The [vendor schematic](https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher/blob/main/Hardware/SenseCAP_Watcher_v1.0_SCH.pdf),
revision A1, sheet 4, identifies J9 pin 1 as GND, pin 2 as VBAT, and pin 3 as
Temp (the battery thermistor). **Verify connector orientation and polarity on
the physical board; do not infer it from wire color or the drawing position.**
The charger U3 BAT/FB is directly connected to VBAT. USB VBUS feeds its input,
and the BSP exposes charge-status inputs, not a software charger enable.
Disconnecting the battery alone does **not** prevent USB charging into an
external source attached to VBAT.

Before enabling source mode:

1. Disconnect/isolate the internal Li-ion cell. Never put a PPK2 source in
   parallel with it.
2. Connect PPK2 VOUT to the board's verified battery input with a common GND.
   Verify that the fixture permits brief USB/source overlap, accounting for the
   charger, battery-detection/thermistor state, and any onboard blocking. Use
   suitable reverse-current protection or charger isolation where the actual
   power path requires it. Record the fixture and the basis of that assessment;
   do not modify/solder a live battery circuit.
3. Verify USB overlap cannot source current back into PPK2 VOUT. Both Watcher
   USB-C sockets supply VBUS: neither may remain powered during capture.
4. Set an explicitly authorized source voltage, for example **3872 mV**.
   This is a battery-input example, **not USB VBUS or a direct 3.3 V rail**.
   Measure voltage at the DUT under load: a blocking diode and wires can drop
   voltage. PPK setpoint-based energy is not necessarily DUT energy.
5. Supply the PPK2 according to Nordic's instructions, with adequate peak
   capacity. Do not treat its roughly 1 A ceiling as an overload test target.
   Stop and investigate brownouts/overrange rather than averaging them away.
6. Close conflicting PPK tools with the owner's consent. Do not kill another
   acquisition or keeper. The runner claims the whole PPK identity and opens
   the chosen CDC interface exclusively.

Do not short the Temp pin to GND to bypass charging protection. Do not enable
charging into the PPK. If the wiring is unknown, stop before output ON.

The measurement is total current entering the selected board input. It
includes conversion losses and other loads on that input. One-feature-at-a-time
(OAT) deltas are *marginal board-level costs*, not isolated IC rail readings.
The schematic's annotated currents are design figures, not our measurements.
This minimum sweep does not separately exercise PSRAM traffic, SD/Grove loads,
standalone touch polling, or RTC features. These remain uncharacterized rather
than being inferred from an idle baseline; the explicit radio/camera/deep-sleep
skips are listed below.

## Software setup

Use ESP-IDF **v6.0.1** and the existing Watcher board overlay. Host acquisition:

```sh
cd esp32
python3 -m venv tools/power/.venv
tools/power/.venv/bin/python -m pip install -r tools/power/requirements.txt
```

Discovery reads descriptors only; it does not power or probe every interface:

```sh
tools/power/.venv/bin/python tools/power/ppk2_profile.py discover
python3 tools/muse/ports.py --list
```

The Watcher's CH342 USB bridge has two serial interfaces. The ESP32 console is
the second; the other is the Himax coprocessor. Identify it by USB descriptors
and the running firmware's board status, not a copied port name. Never flash
the Himax interface with an ESP32 image.

### Build and flash the diagnostic image

Activate ESP-IDF, then build in a separate directory:

```sh
. "$HOME/.espressif/esp-idf-v6.0.1/export.sh"
idf.py -B build-watcher-power-test -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-watcher-power-test/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-sensecap-watcher;devices/sdkconfig.muse-watcher-power-test" build
```

The diagnostic image has no need for account credentials, pairing, or a SDK
token. It bypasses the normal application. Keep Secure Boot, flash encryption,
and pairing eFuse writes disabled. Preserve existing NVS and factory backups.
For a first-ever Muse flash, follow `devices/README.md` to back up the factory
partition first. A backup of data already overwritten cannot recreate factory
data.

**Do not use plain `idf.py flash` on the Watcher.** Its CH342 drops unpaced
packets. Flash only the ESP32 console with the maintained paced tool:

```sh
cd build-watcher-power-test
python ../tools/muse/paced_esptool.py --chip esp32s3 -p "$DUT_PORT" -b 115200 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```

Do not interrupt the several-minute flash. Verify the boot identifies the
power-test image and reaches its ready console without panic/reboot loops.

## Continuous-power lifecycle

One process owns PPK2 from **before USB removal** through **confirmed USB
reconnection**. Starting a power keeper, killing it, and reopening the PPK to
measure is not an acceptable handoff. Neither state boundaries nor duration
expiry switches power off.

Keep the Watcher USB plugged in. Once the electrical prerequisites have
actually been verified, start the owner in a persistent terminal (or an
agent's managed background task). Use fresh output/control paths:

```sh
tools/power/.venv/bin/python tools/power/ppk2_profile.py hold \
  --port "$PPK_PORT" --voltage-mv 3872 \
  --ack-source-wiring --ack-battery-isolated --ack-no-charging-backfeed \
  --ack-usb-connected-before-start \
  --output "$RUN/ppk.jsonl" --control-dir "$RUN/control"
```

The acknowledgment flags record a verified setup; they do not make unknown
wiring safe. Wait for the owner's READY message and fault-free sample drain.
If serial permissions are denied, use a user-authorized hardware-access
session; this skill does not grant permission or disable a sandbox.

Arm the actual USB-gated firmware sequence without reopening PPK:

```sh
tools/power/.venv/bin/python tools/muse/watcher_power.py arm \
  --port "$DUT_PORT" --ppk-control-dir "$RUN/control" \
  --settle-ms 5000 --capture-ms 20000 --repeats 1 \
  --output "$RUN/acquisition.json"
```

Only after both the PPK owner and firmware acknowledge readiness:

1. Prompt the operator to remove **all Watcher USB power**. Leave PPK USB and
   the verified fixture connected. The firmware starts on actual VBUS absence,
   not on the host's guess or the time the question was displayed.
2. Do not use USB logs during measurement. The independent sweep applies each
   state, waits for settling, and captures fixed intervals. Current acquisition
   continues in the same PPK process throughout.
3. Wait the conservative bound reported by `arm`, measured from unplugging.
   Then prompt the operator to reconnect Watcher USB. An elapsed bound is not
   proof of successful completion; retrieve the device's result journal.
4. Retrieve results while the PPK owner remains ON. The RTC journal must match
   the armed run/boot, and live firmware must confirm USB has returned:

```sh
tools/power/.venv/bin/python tools/muse/watcher_power.py collect \
  --port "$DUT_PORT" --ppk-control-dir "$RUN/control" \
  --acquisition "$RUN/acquisition.json" --output "$RUN/results.json" \
  --usb-reconnected
```

Only that final explicit confirmation releases PPK output. On any error,
PPK stays owned/powered; reconnect the Watcher safely, query status, and use
the owner control client to finish explicitly. Do not unplug PPK or kill it
while it is the DUT's only source. Process/host crash and physical USB loss
cannot be made fail-safe in software; use an appropriate hardware fixture
where uninterrupted power is a safety requirement.

## State matrix and evidence

The firmware arm acknowledgment lists the exact plan for that build. Each
result contains the state name, repeat, virtual apply/capture/end timestamps,
error/status, and hardware configuration/readback. Planned
but unsupported states are explicitly skipped, never successful zero-current
results. The baseline is repeated before/after groups to identify failure to
restore a peripheral or hysteresis.

### Matrix selection and protocol

`watcher_power.py arm --matrix peripheral` (or omission) selects the unchanged
27-state peripheral matrix, version 1, with one or two repeats. The strict
firmware arm JSON accepts only the four required keys `run_id`, `settle_ms`,
`capture_ms`, `repeats`, plus optional `matrix:"peripheral"|"sleep"`.
`--matrix sleep --repeats 1` selects sleep matrix version 2. More than one sleep
repeat is rejected as `sleep_requires_one_repeat_cold_not_reversible`: codec
constructors change external registers, and an MCU-only reset cannot recreate
an untouched cold codec. Start this characterization with genuinely untouched
codec hardware; there is no automatic power-cycle or cold-state reset.

Sleep v2 runs: `cold_ref`, `cold_i2s_low`, `cold_i2s_hiz`,
`cold_i2s_low_b`, `codec_initialized`, `codec_suspended`, then warm references
bracketing UART high-Z, RGB DIN low, internal-pulls-off, unused-interface
high-Z, sleep GPIO isolation, CPU power-down permission, and combined knobs.
`best_combined_b` and `best_poll_5s` are followed by `warm_ref_9`, then
`deep_sleep_timer` and `deep_sleep_timer_held`. This final reference makes the
matrix **26 states**, explicitly added to bracket both late variants for A/B/A.
The ACK is authoritative for the complete order and conservative runtime;
5000 ms settling + 20000 ms capture gives a **940000 ms** maximum duration.

ACK/status/results carry `matrix`, `matrix_version`, `resume_count`,
`timeline_offset_us`, and `timeline_uncertainty_us` (including zero). Plan
entries add `knobs`, `deep_sleep`, `poll_ms`, `role` (`ref`, `variant`, `other`)
and `ref_group` (`cold`, `warm`, or empty). Records preserve index/id/name and
add entry `boot_id`, uncertainty, polling and reference metadata; `actual`
adds numeric `knobs_applied` and `codec_state`. Ordinary VBUS polling is
1000 ms; the combined polling variant uses 5000 ms and retains boundary
wakeups. UART0 pins are re-routed as soon as the next power sample detects USB
or uncertain power, and on state reversion/finish/abort. It is not an
asynchronous VBUS interrupt; deep sleep cannot poll VBUS.

`codec_initialized` runs audio/I2S initialization and codec constructors,
but never opens wrapper streams; both I2S directions are then stopped.
ES8311 construction writes reset/clock/system setup (including 0D=FA), and
ES7243/ES7243E construction **enables the ADC**. Wrapper close on a never-opened
stream does not undo those writes. `codec_suspended` actually opens then closes
both codecs. The diagnostic checks each owned low-level disable callback;
register bytes are **evidence, never value assertions**. Failed
initialization/open/disable attempts get the vendor's ordered shutdown sequence
after DMA stops, followed by transport-checked snapshots; a poisoned driver is
not reused that boot. Warm bootstrap is checked even without recreated wrappers,
and finish/abort restoration requires readback before it can report success.
A fenced, independent
sticky warm-attempt marker is stored **before constructors** and survives retained
resets and held-pad release; lost/torn run journals cannot manufacture a cold
label. A genuine rail power cycle, not an MCU reset, is needed to start cold again.
Neither state switches off the always-powered codec rail.

The parking pins are narrow, schematic-backed connected nets: I2S
10/11/12/16 outputs and DIN15; UART0 43/44; RGB DIN40; I2C0 47/48 and EXP_INT2
(external R30/R31 2.2 kΩ and R131 10 kΩ pull-ups); knob41/42 (external
R122/R123 100 kΩ); and unowned shared Himax/SD interfaces 4/5/6/21/46/17/18
only with SD/AI rails off. Disabling knob inputs sacrifices production wake
behavior. RGB DIN low is not LED rail-off. Diagnostic isolation uses IDF's
sleep configuration/switch, then exempts LCD/touch rail-off low pads and any
selected I2S/RGB low outputs. Ordinary reversion disables the switch and
restores saved pad configuration; held deep outputs are configured low before
hold release, then safely floated during fresh-boot restoration.

Only the diagnostic overlay enables CPU retention/power-down support. Every
state without `cpu_pd` holds one tracked CPU-domain ON reference; `cpu_pd`
releases that reference with OFF, never AUTO (AUTO clears other owners).
IDF v6.0.1 uses the remaining ON references to veto CPU power-down; eligibility
and other vetoes can still prevent it. The knob reports permission, not proof
that CPU power-down occurred. Compile-time retention support itself may add
overhead even while vetoed; comparability to the earlier binary needs measurement.

### Codec bytes and error-site evidence

Records, STATUS and RESULTS add `codec_regs`, `codec_regs_available`,
`codec_regs_expected` and `last_error_log`. `codec_regs.dac` has ES8311 keys
`00,01,02,0d,0e,12,14`; `codec_regs.adc` has ES7243 keys `00,05,06` or ES7243E
keys `00,01,04,f9`, with `adc_variant:"es7243"|"es7243e"|"unknown"`.
Values are integer bytes or null for failed/unavailable reads; unknown ADC
has an empty map. Only I2C transport errors fail snapshots. Cold and open
states have no fixed expected-byte assertion. Other `codec_regs_expected`
values compare source-requested final writes, **not guaranteed silicon
readback or proven power state**; false never rejects a measurement.

Source expectations (hex): ES8311 initialized `0d=fa`; suspended
`01=00,02=00,0d=fc,0e=ff,12=02,14=00`. ES7243 initialized
`00=01,05=13,06=00`; suspended `06=5c`. ES7243E initialized
`00=80,01=3a,04=01,f9=00`; suspended `00=1e,01=00,04=01,f9=01`.
GPIO/pin configuration and owned expander outputs remain checked; external
input values and codec/ADC registers are reported, not asserted.

A 26-slot deduplicating RTC pool preserves each sleep record's last snapshot
without overwriting another record. Peripheral repeats can exceed 26 distinct
snapshots; excess snapshots are explicitly unavailable, never a run failure.
This changes the private retained-journal version to 3, not wire schema 1 or
matrix versions. ERROR logs are captured without forwarding to UART, reset
per state, truncated to 96 printable ASCII characters, stripped of ANSI/CR/LF,
and quote/backslash/nonprintable characters sanitized. Wildcard logging is
NONE; only `board` and `watcher_ptest` ERROR tags use the capture-only hook.
Both ROM printf sinks are disconnected after init so EARLY/DRAM output cannot
interleave with UART protocol bytes. Successful rows/readbacks/restoration
clear ignored component errors; log fields describe failures only. The first fatal
record/log is checkpointed before cleanup, then restoration results are saved.
Logs are evidence, not a substitute for error codes.

### USB-only transition dry run

With operator-authorized console access and USB present, before any PPK run:

```sh
python3 tools/muse/watcher_power.py dryrun --port "$DUT_PORT" \
  --matrix sleep --output "$RUN/dryrun.json" --timeout-s 60
```

This sends strict `>ptest.dryrun={"matrix":"sleep"}`. Extra/duplicate keys,
other matrices, active ARMED/RUNNING runs, and absent/unknown USB are rejected.
It applies and reads back sleep indices 0–23, skips both deep states, and
restores resting configuration. Each attempted state emits `dryrun_state`
with identity, error/log, codec bytes/state and `knobs_applied`; exactly one
`dryrun_done` reports `states,errors,restore_error,last_error_log` after accepted
execution. `errors` counts failed state rows plus a failed restoration.
Apply errors still get readback; verified USB loss/unknown stops further
states but does not skip restoration or the final frame.

The UART driver drains TX with a bounded `uart_wait_tx_done` before either
UART pad is parked, and the dry run drains every state line and its final
line; FIFO completion, not a fixed pacing sleep, prevents truncating a prior
warm-reference frame. Drain failure leaves pads connected, stops further
states, restores resting configuration, and fails final restoration/transport
verification. A final-DONE-only drain failure emits an explicit `type:error`
`dryrun_done_tx_failed` after the single DONE; the host must reject it, not
call the run successful. No later state is silently treated as applied after
transport failure.

`pm_exercised:false`: no capture/settle dwell, PM/CPU reference changes,
light-sleep measurement or deep entry. The USB no-light-sleep lock remains
held even on lost/unknown VBUS; UART is restored before state output, including
loss after brief parking. The RTC run journal is not changed. Hardware codec
warm-history markers necessarily change: **dry run consumes cold state;
fully power-cycle the codec rail before a cold quantitative sweep.**
The host uses fresh nonced STATUS before/after, requires the same boot and
live USB, banks raw/error frames, and exits nonzero for errors/protocol loss.
The timeout is configurable 1–300 seconds; this is diagnostic execution,
not power data. Never open/change a PPK source for this command.

### Timer-deep-sleep journal and timing

Deep states disable all wake sources before enabling timer-only wake for
`settle_ms + capture_ms`. RTC slow and fast memories are retained: ESP-IDF
6.0.1's S3 `rtc_sleep_init()` forces both powered; its public RTC memory-PD
domains are absent on S3, so optional retain-ON calls are capability-guarded.
An independent fenced pad-ownership marker is committed before the first hold
and cleared only after all releases; it is consulted even if the large run
journal is invalid, before UART allocation. A separate sticky warm marker is
never cleared by pad release. Combined recovery banks plus run journal consume
7,680 bytes on both C host and S3 target ABIs (7,680-byte static ceiling),
leaving 512 bytes for SDK RTC metadata; the current IDF image uses 7,716 total
RTC slow bytes including its 36 bytes of SDK data. A
checksum/version/bounds-validated journal first marks `deep_sleep_pending`;
entry virtual and RTC timestamps are banked before sleeping. Configuration
failure never enters sleep. Only a matching pending record, deep-sleep reset
and timer wake can resume, after checking VBUS is still absent; all other
interrupted resets remain `incomplete_reboot`. No UART-ready reply is emitted
while the resumed sweep is running.

The record's `deep_sleep` object carries `entry_boot_id`, `resume_boot_id`,
`programmed_us`, `rtc_slept_us`, `wake_cause:"timer"`, and
`resume_reset_reason:"deepsleep"`. Its `boot_id` is the entry boot;
`run_boot_id` stays the armed boot and `retrieval_boot_id` is the current boot.
`fw_now = esp_timer_get_time() + timeline_offset_us` continues the run epoch,
with offset `entry_virtual + RTC_elapsed - new_esp_timer`. Deep capture begins
at entry + settling and ends at the RTC-derived wake/early-boot time, before
normal resumed initialization. Do not treat LS counters as deep-sleep proof.

Each RTC resume adds **ceil(1% of RTC elapsed) + 50 ms** uncertainty. IDF's
internal RC slow clock is calibrated against XTAL, with retained/calibrated
RTC accumulation (`esp_clk.c`, `sleep_modes.c`); temperature-dependent drift
is not specified here. This is a conservative **engineering assumption**, not
a validated oscillator bound or sample-exact synchronization. Include it in
guards; short windows can become unusable. Fresh ARM preserves the current
virtual offset/uncertainty so the preceding STATUS clock sync stays meaningful.
A retained completed journal may reconstruct timing after another reset for
private recovery evidence, but quantitative collection still rejects an
unexpected retrieval/live boot: it must match the final timer-resume boot
(or the armed boot when there were no resumes).

Initial peripheral characterization targets:

- Radios-off idle with DFS and automatic light sleep, versus fixed
  40/80/160/240 MHz awake idle.
- LCD rail off versus static black display, backlight 0/25/50/100%. The rail
  includes touch; powered static display is not equivalent to production LVGL
  animation. Record display/touch task state and CPU policy.
- Closed audio codecs versus open 16 kHz duplex I2S/codecs, PA off/on;
  microphone capture drained locally, then bounded -18 dBFS sine playback.
  Shared-bus states must not be labelled as independent microphone-only costs.
- Battery divider off/on, separately from ADC conversion work.
- Wi-Fi off versus bounded scanning. Associated modem-sleep/no-PS/traffic
  states require a separately configured AP and signal/DTIM evidence.
- Explicitly skipped features stay visible: camera functional capture,
  connected BLE/NUS, deep sleep, SD I/O and attached Grove loads require
  safe, supported controls and verified hardware before being claimed.

For production firmware, separately record unpaired advertising, paired
connected idle, screen animation, screen-off first two minutes, Wi-Fi-napped
rest, wake/reconnect, offline voice recording, and reply playback. Use the
actual production telemetry (`tools/muse/power.py`) and controlled stimuli.
Do not transfer diagnostic figures into production battery-life estimates.

## Reporting and acceptance criteria

```sh
tools/power/.venv/bin/python tools/muse/watcher_power.py analyze \
  --results "$RUN/results.json" --samples "$RUN/ppk.jsonl" \
  --output "$RUN/profile.json" --guard-s 1.0 --clock-drift-ppm 250
```

Those arguments are the model assumptions used for the committed example,
not universal validated clock bounds. Choose and report a justified timing
model for each run. Pre/post UART checks bound average drift, not interior
wander; absent hardware timing, estimates remain conditional on that model.

The report uses actual firmware intervals, a bounded pre-unplug UART clock
sync using per-request echoed nonces (late replies are discarded), guard bands,
and only complete, contiguous aggregate windows. The sample file's session
header must match the armed owner, host-clock epoch, voltage, and sample rate.
Exactly one `watcher-sweep` begin record supplies the recording's immutable
host/sample anchor. Drift is measured from that boundary, including the
producer's queued-prefix accounting where available, rather than from the
owner's startup. Its decode-loss baseline is also snapshotted at that boundary,
not taken from a later control reply. Earlier failed preflight remains in the capture and report;
it is not counted as test data, and does not excuse new loss during recording.
It rejects failed
states, missing/overfull sample coverage, collector faults, and sample-clock
or receive-backlog timing exceeding the guard. Accepted rows are explicitly
**receive-window estimates**, not exact sample-synchronized rail measurements.
PPK2's Python stream has no exact lost-frame locations. Percentiles from
per-window reservoirs cannot be recombined; use retained raw samples or a
properly timed histogram to compute state quantiles. Never interpolate missing
current or turn empty/rejected captures into zero-current results.

### Optional pre-fault prefix (offline only)

Without an explicit flag, a final collector fault still rejects every row. If
an otherwise clean sweep is followed by a timestamped raw receiver
`queue_overflow` or `read_error`, analyze a **new** prefix report:

```sh
tools/power/.venv/bin/python tools/muse/watcher_power.py analyze \
  --results "$RUN/results.json" --samples "$RUN/ppk.jsonl" \
  --output "$RUN/profile-prefix.json" --guard-s 1.0 --clock-drift-ppm 250 \
  --pre-fault-prefix
tools/power/.venv/bin/python tools/muse/watcher_power.py compare \
  --profile "$RUN/profile-prefix.json" --output "$RUN/compare-prefix.json"
```

The original receiver fault must match the final collector fault. New decoded
missing, invalid, and late deltas must remain zero. Consumed bytes must account
for all four-byte decoded frames (including known pre-BEGIN invalid/missing
frames); a bounded surplus is accepted only with receiver byte conservation,
unchanged retained/invalidated queue bytes, and evidence of at most one
already-dequeued 64 KiB batch plus three framing bytes. Missing or inconsistent
proof fails closed; no post-fault dequeue is allowed by the receiver contract.

`transport_fault_cutoff` reports the fault host offset, the larger final
session/receiver `max_consumer_delay_s`, cutoff, accepted whole-row count, and
byte-accounting evidence. The cutoff is **fault arrival host offset − final
observed consumer delay − requested guard**. Any guarded state-window end beyond
it rejects the entire row—never shorten a state into an apparently valid result.
Existing per-window coverage, gaps, loss, clock drift and timing gates still
apply. Only the selected windows' captured `max_observed_backlog_bytes` peaks
are used; a later global backlog spike does not rewrite their earlier evidence.
The capture/receiver remains **INVALID** after the fault; this does not validate
later continuation, prove a complete capture, change the conditional timing
model, or authorize source restart/shutdown.

The raw receiver's separate entry cap now scales with its byte capacity:
`max(4096, ceil(capacity_bytes / 1024))`, bounded at 16384 entries. A 16 MiB
budget therefore permits roughly 1 KiB chunks to approach its byte cap rather
than overflowing at 4096 entries near 4 MiB. Tiny chunks can still exhaust the
bounded metadata cap. Overflow remains latched, queued continuation invalid,
and raw drain/continuous source ownership unchanged.

Publish current mean/min/max, sample count/coverage, included duration,
settling/guard, sampled charge, source-voltage-basis energy, and measurement
limitations. Record DUT voltage at load, fixture, firmware commit/config,
ambient temperature, radios/AP/connection settings, audio level, fitted SD and
Grove loads, and repeats. Scrub local USB identifiers, host paths, credentials,
and network names from committed findings. Keep raw captures local unless
explicitly appropriate to publish them.

For a suspected fix, compare A/B/A at the same voltage/config/stimulus, check
function/wake recovery, and report repeat variability. Attribute a power fix
only after hardware evidence shows improvement; static code inspection alone
is not a measured finding.

## References

- Seeed [SenseCAP-Watcher-Firmware](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware),
  inspected revision `8e37f7ccd3d836bb99f483e0f6e74e8252406644`:
  `components/sensecap-watcher/include/sensecap-watcher.h` maps expander rails
  and active-low VBUS; `sensecap-watcher.c` supplies vendor power sequencing.
- [Seeed open hardware](https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher),
  `Hardware/SenseCAP_Watcher_v1.0_SCH.pdf`, sheets 3–4 for topology and charger.
- [Nordic PPK2](https://www.nordicsemi.com/Products/Development-hardware/Power-Profiler-Kit-2)
  and [Python API](https://github.com/IRNAS/ppk2-api-python), pinned version
  0.9.2; acquisition units are µA at nominal 100 ksample/s.
