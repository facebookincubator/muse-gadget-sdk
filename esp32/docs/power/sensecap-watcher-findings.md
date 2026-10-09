# SenseCAP Watcher: measured USB-free state profile

## Scope and fixture

One opt-in diagnostic sweep completed on 2026-10-09: **27 journal records,
23 measured states, four explicit skips, no firmware errors**. These are
**conditional receive-window estimates under an assumed 250 ppm clock-drift
budget and 1-second edge guards**, not sample-exact measurements or production
application/battery-life figures. Follow the [setup and lifecycle guide](sensecap-watcher.md)
and use the [scrubbed CSV](sensecap-watcher-profile.csv) for numeric comparisons.

- Internal battery removed; PPK2 VOUT on VBAT at the battery connector, common
  GND. Source setpoint **3.872 V**, not an independently measured DUT voltage.
- Watcher USB present for flashing/arming, absent throughout every reported
  capture, restored and verified before source OFF. The same source owner
  remained ON throughout; USB overlap/transition energy is excluded.
- No external protection fitted. Overlap suitability was an operator assertion,
  not an independent charger/power-path characterization.
- ESP-IDF v6.0.1, Watcher board overlay plus
  `devices/sdkconfig.muse-watcher-power-test`; diagnostic firmware in this change.
  Normal UI, account, voice and network startup are bypassed. The capture used
  base `96004055a58d0baf9850376eb3950c1e2869efde` plus these diagnostic additions,
  before integration onto newer main. The Watcher BSP and diagnostic C source
  are unchanged by that rebase; broader production application changes are
  outside the measured scope.
  Measured-image SHA-256:
  `8ca8857ad354cf1de4a20b533bf836f2ea8dc40ae1432b803739bd5c21aca327`.
- Five seconds settling, 20 seconds capture per supported state, one repeat.
  Ambient temperature, component rail voltages and external DUT terminal voltage
  were not independently recorded. SD/Grove workloads were not exercised.

## Measurements

All values are **total board-input** current. Power is mean current multiplied
by the **source setpoint**. Min/max are observed sampled extrema, not guaranteed
circuit peaks. Retained digits describe numerical estimates, not independently
validated calibration accuracy or repeatability; one repeat supplies no
between-run confidence interval. Display, audio, ADC and Wi-Fi captures use fixed 80 MHz awake
idle; `cpu_fixed_*` uses the named fixed clock. The three `baseline_*` rows use
DFS 40–240 MHz plus automatic light sleep. See the history caveat below before
subtracting any baseline.

| State | Mean mA | Min–max mA | Source mW | Included s | Samples | Coverage % |
|---|---:|---:|---:|---:|---:|---:|
| `baseline_pre` | 36.151 | 32.326–106.138 | 139.977 | 18.0 | 1,800,192 | 100.0107 |
| `cpu_fixed_40_idle` | 50.643 | 41.430–70.313 | 196.089 | 17.9 | 1,789,697 | 99.9831 |
| `cpu_fixed_80_idle` | 57.655 | 44.614–84.013 | 223.238 | 17.9 | 1,789,952 | 99.9973 |
| `cpu_fixed_160_idle` | 64.422 | 51.325–86.299 | 249.441 | 17.9 | 1,789,954 | 99.9974 |
| `cpu_fixed_240_idle` | 69.267 | 55.877–100.028 | 268.204 | 17.9 | 1,789,952 | 99.9973 |
| `baseline_cpu_post` | 36.105 | 32.225–103.846 | 139.800 | 17.9 | 1,789,952 | 99.9973 |
| `lcd_rail_off_pre` | 57.500 | 44.509–82.490 | 222.642 | 17.9 | 1,789,952 | 99.9973 |
| `lcd_black_bl_0` | 67.468 | 47.671–90.110 | 261.235 | 17.9 | 1,789,952 | 99.9973 |
| `lcd_black_bl_25` | 76.179 | 51.325–117.607 | 294.964 | 17.9 | 1,789,952 | 99.9973 |
| `lcd_black_bl_50` | 85.156 | 52.842–124.496 | 329.726 | 17.9 | 1,790,721 | 100.0403 |
| `lcd_black_bl_100` | 103.472 | 87.823–127.560 | 400.644 | 17.9 | 1,790,209 | 100.0117 |
| `lcd_rail_off_post` | 61.366 | 45.823–84.013 | 237.608 | 17.9 | 1,790,207 | 100.0116 |
| `codecs_closed_pre` | 61.126 | 45.875–84.775 | 236.680 | 17.9 | 1,789,952 | 99.9973 |
| `codecs_open_amp_off` | 62.832 | 46.983–82.490 | 243.287 | 17.9 | 1,790,207 | 100.0116 |
| `codecs_open_amp_on` | 66.457 | 52.842–84.013 | 257.322 | 17.9 | 1,789,952 | 99.9973 |
| `mic_capture_16k` | 62.879 | 46.825–89.348 | 243.469 | 17.9 | 1,789,954 | 99.9974 |
| `sine_1k_minus18dbfs` | 66.658 | 52.842–84.013 | 258.098 | 17.9 | 1,790,207 | 100.0116 |
| `codecs_closed_post` | 26.068 | 19.526–39.050 | 100.936 | 17.9 | 1,789,952 | 99.9973 |
| `adc_divider_rail_on` | 26.197 | 19.622–39.205 | 101.436 | 17.9 | 1,789,954 | 99.9974 |
| `adc_sample_1hz` | 27.540 | 20.876–44.929 | 106.637 | 18.0 | 1,799,680 | 99.9822 |
| `adc_divider_off_post` | 27.441 | 20.635–40.238 | 106.251 | 17.9 | 1,789,954 | 99.9974 |
| `wifi_scanning` | 74.127 | 24.379–391.621 | 287.018 | 18.0 | 1,799,937 | 99.9965 |
| `wifi_associated_idle` | skipped | — | — | — | — | — |
| `ble_advertising` | skipped | — | — | — | — | — |
| `camera_capture` | skipped | — | — | — | — | — |
| `deep_sleep_timer` | skipped | — | — | — | — | — |
| `baseline_post` | 3.077 | 0.660–72.594 | 11.912 | 17.9 | 1,789,952 | 99.9973 |

The CSV also preserves sampled charge and source-setpoint energy for the
**included samples only**; neither is a full-state or full-sweep integral.
Blank skipped-state cells mean not measured, never zero current.

### Applied-state evidence

- Backlight readback: 255/511/1023 of 1023 for requested 25/50/100%, static
  black display. The display/touch rail is shared; no touch workload was tested.
- Audio: both codecs and duplex 16 kHz I2S open; PA separately off/on. Microphone
  frames are drained/discarded, not recorded. Both active stream states report
  320,160 frames over their nominal 20-second captures. The sine is 1 kHz,
  -18 dBFS peak, configured speaker volume 25/100, not a maximum-output test.
- ADC: the requested 1 Hz service reports 104 conversions, in eight-sample
  batches. Do not assume an exact achieved 1 Hz cadence from the state name.
  The last calibrated onboard batch estimates VBAT at 3731 mV; this is not an
  external voltage validation or a capture-average voltage.
- Wi-Fi: unassociated active scanning, ten completed scans reported. No AP
  credentials, association, RSSI or DTIM profile was used or published.
- Light sleep: positive measured sleep evidence in each DFS baseline. The final
  baseline reports 40 sleep entries and 19.971 seconds in the firmware's sleep
  accounting during its nominal 20-second capture. This is not an independently
  sampled low-current duty-cycle measurement.

## Interpretation and power work

**The off-state is history-dependent.** The initial codecs have never been
initialized; the later closed state follows an open/close cycle. Although the
power-expander output/direction readbacks agree, fixed-80-MHz
`codecs_closed_pre` is 61.126 mA versus 26.068 mA afterward. The DFS baseline
similarly changes from 36.151 to 3.077 mA. The sequence demonstrates a large
initialization/cleanup-history effect, **not an isolated 35 mA codec saving**.
Do not pool these baselines or subtract the final warm baseline from earlier
cold measurements to assign peripheral costs. Cold boot versus explicit codec
initialization/power-down is a priority for a focused controlled follow-up;
other initialization/cleanup effects must also be isolated.

Within this one sequence:

- Fixed 240 versus 40 MHz awake idle differs by **18.625 mA**. DFS controls
  around that group agree within 0.046 mA. This supports investigating lower
  idle clocks/automatic light sleep, not choosing a universal performance policy.
- Backlight 100 versus 25% differs by **27.293 mA** at the same display/CPU
  request. LCD rail-off controls shift by 3.865 mA, so do not present a precise
  history-independent LCD rail cost from their pooled baseline.
- Open-codec PA idle ON versus OFF differs by **3.625 mA**. Microphone draining
  with PA off is close to the open/PA-off value, but that is a different workload;
  it does not prove microphone capture is free or provide an identical A/B/A.
- Scanning averages **74.127 mA**, with a sampled maximum **391.621 mA**.
  Schedule scans sparingly, but do not infer associated-idle/modem-sleep costs.
- ADC controls move across the sequence. These readings do not establish a
  stable isolated divider/conversion cost.

**No production power fix is claimed or enabled by this PR.** The opt-in
harness provides the controls to verify candidates. A production change must
pass a same-voltage/config/stimulus A/B/A comparison, repeated variability and
wake/function checks. Changing defaults from this single, history-dependent
BSP sweep would overstate the evidence.

## Timing and transport limits

Analysis used `--guard-s 1.0 --clock-drift-ppm 250`. Each quantitative row retains
only complete contiguous 100 ms receive windows inside the guarded device
capture, with 99–101% nominal 100,000 samples/s coverage. Actual row coverage is
99.9822–100.0403%. The immutable BEGIN host/sample boundary excludes preflight
without erasing its evidence; new recording loss still rejects a row.

Pre/post UART observations give whole-run average device-versus-host drift
about -29.76 ppm, with an RTT-bounded interval **[-237.71, +178.27] ppm**.
The offset change is +0.04051 seconds with combined half-RTT uncertainty
±0.28311 seconds. This does **not** validate a 100 ppm allowance over that
interval. The assumed 250 ppm budget covers the observed endpoint envelope,
but endpoint checks **cannot bound interior clock wander**. Estimates remain
conditional on that model and the receive-time proxy; no hardware timestamps
or exact transition alignment are claimed.

The accepted owner received 578,820,096 bytes and decoded 144,705,024 valid
samples across its entire lifetime, including USB overlap/preflight. All
reported receiver/read/overflow/discard/decoder-invalid/missing/late and cleanup
counters were zero. These counters are not proof that the instrument/OS never
lost an unobservable sample. Maximum observed backlog was 0.06630 seconds and
consumer delay 0.13123 seconds; the largest selected sample/host drift was
0.15618 seconds. No transport/coverage tolerance was relaxed to accept new loss.
Earlier failed preflights are excluded; synthetic conversion speed is not
acquisition-completeness evidence. No raw per-sample file or recombined
reservoir quantiles are published.

## Validation and remaining scope

- Diagnostic ESP-IDF build/link and paced ESP32 flash passed; app size
  0xe1000 bytes in a 4 MiB slot. Boot reached the diagnostic console without
  initialization error or reboot loop.
- Final rebased full host suite: 368 tests, two skips, including the 25 C-backed
  diagnostic tests. All 81 PPK and 40 Watcher host tests also passed after the
  startup-only fixes. The rebased diagnostic image rebuilt at 0xe1000 bytes;
  its test/Watcher/tickless/PM configuration and compiled harness were checked.
  `git diff --check` passed.
- Firmware completed the matching journal with no apply, cleanup, restore or
  reboot error. Source shutdown occurred only after live USB restoration and
  durable result retrieval; source owner and host awake guard both ended cleanly.
- Associated Wi-Fi, BLE advertising, camera capture and deep sleep were explicit
  skips. PSRAM traffic, SD/Grove, standalone touch and RTC workloads remain
  uncharacterized. Normal Muse application power and battery life remain untested.
- Committed content excludes raw logs, host paths, USB identities, run/boot
  identities, credentials, network names and account data.
