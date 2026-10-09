# SenseCAP Watcher: USB-free PPK2 power profiling

## Measurement status

The USB-free diagnostic sweep completed: **23 measured states, four explicit
skips, no firmware or collector errors**. See the [measured profile and limitations](sensecap-watcher-findings.md)
and [scrubbed CSV](sensecap-watcher-profile.csv). Figures are conditional
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
result contains the state name, repeat, apply/capture/end timestamps in ESP
boot microseconds, error/status, and hardware configuration/readback. Planned
but unsupported states are explicitly skipped, never successful zero-current
results. The baseline is repeated before/after groups to identify failure to
restore a peripheral or hysteresis.

Initial characterization targets:

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
