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

# Muse boards on the USB 5 V input: measured power-test sweeps

## Scope and fixture

The initial measurements used the power-test image (`CONFIG_MUSE_POWER_TEST`)
on five boards, swept with
`"usb_powered":1` and measured with a POWER-Z KM003C on each board's 5 V USB
input. These are **USB-input currents, not battery currents**: read
[km003c.md](km003c.md) for what USB adds and how steps compare with
battery-side numbers. All rows are in [usb5v-profile.csv](usb5v-profile.csv).

- The four S3 boards, 2026-10-10 and 2026-10-11. Two runs per board back to back, cables untouched
  between them. Five seconds settling, 20 seconds capture, 1 s guard at both
  ends, meter read 20 times per second, about 18 s kept per window.
- Firmware: images built from this change (`muse-gadget.bin` SHA-256):
  - Waveshare 1.75C: `6b804ff749e2e89bea115e5a96b9a4ffeb9acade625a89360ea7619c35c65755`
  - Waveshare 2.16: `6034a8116d3a070f7f0028406a60a2c82a8e034d1a65e2214093c13719e42164`
  - AIPI Lite: `8b4cd5c53454ad86b59bfd0b0a0038172e37030ebf536999eae3946dcc9f7c0b`
  - SenseCAP Watcher: `bbe8bc62fc8cc78b76abe87e076e48e1d6377102b1ca0082989536025d44bc06`
- Waveshare 1.75C and 2.16: battery in, `"charging_off":1` (AXP2101 charging
  off for the run, restored after).
- AIPI Lite: battery removed. It has no charge control; with a part-charged
  battery fitted, every row read 12 to 29 mA higher.
- SenseCAP Watcher: battery fitted (4.17 V in its ADC row), no charge control.
  Earlier runs on the same power-up had opened and closed the codecs:
  `codecs_closed_post` is 0.5 mA above `codecs_closed_pre`, so the state in
  [sensecap-watcher-findings.md](sensecap-watcher-findings.md) where earlier
  firmware left the codecs running does not appear.
- A computer stays on USB. The 1.75C, 2.16 and AIPI use the native USB
  Serial/JTAG console, which keeps them out of light sleep while a host is
  attached: their DFS baselines are USB-awake idle and reported skipped. The
  Watcher's UART console lets it sleep.

## Measurements

Run 1 / run 2, mA at the 5 V input.

| State | 1.75C | 2.16 | AIPI Lite | Watcher |
|---|---|---|---|---|
| `baseline_pre` (DFS) | 17.5 / 20.4 | 18.0 / 18.2 | 21.8 / 21.9 | 8.2 / 8.1 |
| `cpu_fixed_40_idle` | 15.1 / 17.9 | 15.4 / 15.6 | 19.4 / 19.4 | 19.8 / 19.8 |
| `cpu_fixed_240_idle` | 29.8 / 32.6 | 30.7 / 30.9 | 34.2 / 34.3 | 34.6 / 34.6 |
| `lcd_black_bl_100` | 26.4 / 29.1 | 27.3 / 27.4 | 50.6 / 50.6 | 64.7 / 64.6 |
| `lcd_white_bl_100` | 98.7 / 101.0 | 91.9 / 92.2 | 50.6 / 50.6 | not in matrix |
| `lcd_anim_bl_100` | 94.4 / 96.9 | 100.4 / 100.7 | 63.0 / 63.3 | not in matrix |
| `codecs_open`, amplifier on (Watcher: `codecs_open_amp_on`) | 63.9 / 64.0 | 61.3 / 61.4 | 52.4 / 52.5 | 66.7 / 66.7 |
| `codecs_closed_post` | 25.6 / 23.7 | 23.4 / 23.5 | 27.0 / 27.1 | 28.5 / 28.5 |
| `wifi_scanning` | 61.1 / 59.8 | 59.7 / 60.1 | 62.5 / 62.6 | 65.5 / 64.9 |
| `baseline_post` (DFS) | 20.9 / 18.8 | 18.2 / 18.2 | 22.3 / 22.4 | 8.0 / 8.1 |

On the three generic boards the codec driver switches the speaker amplifier
on with the codecs. The Watcher with its amplifier off reads 62.4 / 62.3 mA.

Steps, from run 1 at the 5 V input:

- Codecs open with the amplifier on, over closed (`codecs_open` minus
  `codecs_closed_post`): 1.75C 38.3 mA, 2.16 38.0 mA, AIPI Lite 25.4 mA,
  Watcher 38.2 mA.
- CPU fixed 240 over 40 MHz: 14.7, 15.4, 14.9 and 14.8 mA.
- Display: on the AMOLED boards the black rows stay within 0.1 mA at every
  brightness; white 0 to 100 % adds 72.3 mA (1.75C) and 64.5 mA (2.16). On
  the AIPI Lite's LCD, backlight 0 to 100 % adds 23.1 mA, and black and white
  frames differ by 0.4 mA at 25 %, 1.0 mA at 50 % and under 0.05 mA at 0 and
  100 %.

## Run-to-run differences

- 2.16: the two runs agree within 0.36 mA, Wi-Fi scanning within 0.41 mA.
- AIPI Lite: within 0.32 mA.
- Watcher: within 0.10 mA except Wi-Fi scanning (0.63 mA).
- 1.75C: run 2 is 2.3 to 2.9 mA higher than run 1 in every row before the
  first codec open. The open-codec rows agree within 0.06 mA. After the
  close, run 1 is the higher one, by 1.9 mA (`codecs_closed_post`) and
  2.1 mA (`baseline_post`). The ES8311 and ES7210 snapshots read the same
  values in both runs in every closed and open row. The cause is not
  identified.
- An earlier build of this change on the 2.16 (`muse-gadget.bin` SHA-256
  `9470f930f5896511ed244f46d9c70250f6a4b094551d32d8e64d35a34195161a`), two
  runs: A right after flashing, B after an AXP2101 power-off and on (PWR held
  over 10 s). Before the first codec open, B read the registers' power-on
  values (ES8311 0x0E 0x6A, ES7210 0x01 0x20) and A the values seen after a
  codec close (0x7F and 0x7F). B was 0.50 to 0.67 mA higher than A in every
  row before the close (animation 1.30 mA), including the open-codec rows,
  where both read the same register values; after the close they agreed
  within 0.13 mA, except Wi-Fi scanning (0.87 mA).

## ESP-Mosaico, 2026-11-08

Two runs through the existing USB-C ESP-Iris connection, with the POWER-Z
KM003C inline and the camera module disconnected. The battery was fitted:
100% and 0 mA before and after both runs, 4183 / 4171 mV before and
4174 / 4169 mV afterward. Charge control is not supported. Cables stayed
untouched between the accepted runs.

Five seconds settling and 20 Hz meter polling. Run 1 used 30 s capture and
5 s guards at both ends; run 2 used 40 s capture and 10 s guards. Both kept
about 20 s per window. Maximum clock-anchor uncertainty was 3.882 / 2.308 s,
below the respective guards; minimum sample coverage was 99.74 / 99.66%.
Mean VBUS across the windows ranged from 5.231 to 5.254 V.
Two earlier attempts at run 2 were rejected
by the 5 s timing guard; another failed before arm on device selection.
A later attempt exceeded the 10 s guard during high host load. None is
included here. Run 2 used a persistent `iris run` client to keep the shared
Gateway open between polls.

Measured firmware and host collector: local revision
`0062a77e4a4436978d7345528d53fbd9e6165388`, including the retained board-1
partition table. `muse-gadget.bin` SHA-256:
`d238e003aa2b6c054c01d0f3fd6dac9d95a35af5e3a3f9f9ece770e396423572`.
The firmware and collector source files match this change; only host tests
and documentation changed afterward. The local table is excluded from this
change. Both accepted runs used the same boot.

Run 1 / run 2, mA at the USB input. The CSV contains all 23 measured rows
per run, adding `baseline_cpu_post` and black/white LCD rows at 0%, 25%
and 50% brightness to the table below.

| State | Run 1 | Run 2 |
|---|---:|---:|
| `baseline_pre` | 44.60 | 47.10 |
| `cpu_fixed_40_idle` | 42.87 | 45.61 |
| `cpu_fixed_80_idle` | 49.54 | 53.01 |
| `cpu_fixed_160_idle` | 52.97 | 56.46 |
| `cpu_fixed_240_idle` | 56.80 | 59.10 |
| `cpu_fixed_320_idle` | 63.40 | 66.30 |
| `lcd_black_bl_100` | 56.34 | 58.66 |
| `lcd_white_bl_100` | 124.51 | 126.87 |
| `lcd_anim_bl_100` | 109.54 | 112.43 |
| `codecs_closed_pre` | 51.08 | 53.41 |
| `codecs_open` | 80.85 | 81.37 |
| `mic_capture_16k` | 80.97 | 81.49 |
| `sine_1k_minus18dbfs` | 81.23 | 81.73 |
| `codecs_closed_post` | 52.68 | 53.41 |
| `wifi_scanning` | 91.35 | 93.11 |
| `baseline_post` | 47.11 | 47.63 |

Both runs completed with 24 records: 20 `ok` rows and four `skipped` rows.
Iris holds a no-light-sleep lock, so the three DFS baseline rows are measured
USB-awake idle and skipped as sleep tests. The separate amplifier-control
row is unsupported and has no measurement window. All five fixed clocks
read back at their requested frequencies.

Before the first codec open, run 2 minus run 1 ranged from 1.61 to
3.49 mA. Run 1 read ES8311 `0x0e=0x6a`, `0x14=0x10`; run 2 read
`0x0e=0x7f`, `0x14=0x00`. After closing, both read the latter values.
The closed-codec rows after audio differed by 0.73 mA; the final baselines
differed by 0.52 mA. The cause of the earlier difference is not established.
Run 2 began with the suspended-codec readback already present.

Across all 10 shared rows in the 1.75C table above, Mosaico's USB input
was 15.5 to 33.3 mA higher, comparing run 2 on both boards. The load
steps were similar: CPU 40 to 240 MHz added 13.5 / 14.7 mA, and white
minus black at 100% brightness added 68.2 / 71.9 mA (Mosaico / 1.75C).
The cause of the higher input current is not established.

In a separate connection check on the same power-test image, idle with the
battery charging, the POWER-Z input moved from the Mac with ESP-Iris
enumerated to a data-less charger and back. It read 5.23 V, 107.6 mA
(563 mW), then 5.11 V, 105.1 mA (537 mW), then 5.23 V, 107.4 mA.
These are 20 s means after 5 s settling at each connection. The Mac/Iris
connection condition read about 2.5 mA (26 mW) higher than the charger in
this check; the input voltage also changed.

## Limits

- USB input: the board, its input path and anything powered only on USB.
  Absolute values are not battery use, and a step's size depends on where
  its load sits in the power tree (see [km003c.md](km003c.md)).
- The tool takes single meter readings at 20 Hz, about 360 per S3 window
  and 400 per Mosaico window:
  enough for window means, not for peaks shorter than the gap between
  readings.
- Windows are cut from each record's own start and end times, mapped to
  host time through the board's console replies, with 1 s guards on S3
  and 5 / 10 s guards on Mosaico run 1 / run 2.
- Two runs per board give no confidence interval.

## reTerminal Sticky (2026-11-12)

Two accepted sweeps on one unit with the
[Sticky diagnostic image](km003c.md#reterminal-sticky-diagnostic).
All 32 measured rows are in [usb5v-profile.csv](usb5v-profile.csv).

- POWER-Z KM003C on the USB input, computer attached, battery fitted.
  Cables stayed unchanged. These are USB-input currents, including the bridge.
- The battery met the USB-plus-voltage plateau criterion before each run:
  three fresh polls at least one minute apart, charging=false, USB input at
  least 50% below the original 543.133 mA reference and stable within
  max(2 mA, 5%), battery voltage at least 4100 mV with at most 10 mV spread.
  Run 1 qualified at 26.43 to 26.85 mA and 4187 to 4188 mV;
  run 2 at 26.57 to 26.65 mA and 4186 mV.
  SOC was logged, not used as the gate.
- Charging stayed enabled but off in every window. All 48 application/start/end
  gauge receipts per run, plus the restoration receipt, reported zero signed
  current. Any positive receipt rejected a run regardless of its final flag.
- Five seconds settling, 20 s capture, 1 s guards, 20 Hz meter reads.
  Windows retained 358 to 380 samples over 18.0 to 19.0 s.
  Qualification and both captures used one open CH343 UART connection.
  Each capture kept one boot ID across all windows. Both passed on the first attempt.
- Firmware SHA-256: `9611e21036731ef2f349d739fe08f9c775b8dd3d60b98d09241094c96a9b9dca`,
  verified by independent flash readback.
- FULL and FAST alternated black and white frames at 80 MHz, with a 500 ms
  rest after each completed refresh. Each started from a completed FULL white
  seed before settling. Three panel-resting references used the same 80 MHz
  CPU setting and 20 ms polling interval. Touch and SD rails stayed at init levels.

For e-paper rows, `actual.io_frames` counts BUSY-confirmed refreshes completed
in the untrimmed 20 s capture: FULL 9 / 9, FAST 17 / 17
in runs 1 / 2. CSV `samples` counts POWER-Z readings, not panel refreshes.
Window means include the 500 ms rests; they are not energy per refresh.

Mean mA / mW at the USB input:

| State | Run 1 | Run 2 |
|---|---:|---:|
| `baseline_pre` | 13.12 / 68.0 | 12.97 / 67.3 |
| `cpu_fixed_40_idle` | 23.78 / 123.3 | 23.62 / 122.5 |
| `cpu_fixed_80_idle` | 30.97 / 160.5 | 30.99 / 160.6 |
| `cpu_fixed_160_idle` | 34.78 / 180.3 | 34.72 / 179.9 |
| `cpu_fixed_240_idle` | 39.26 / 202.8 | 38.74 / 200.0 |
| `baseline_cpu_post` | 13.19 / 68.5 | 13.15 / 68.2 |
| `epaper_off_pre` | 31.25 / 162.0 | 30.69 / 159.1 |
| `epaper_full` | 34.13 / 176.8 | 34.39 / 178.1 |
| `epaper_off_between` | 31.11 / 161.2 | 30.91 / 160.3 |
| `epaper_fast` | 34.69 / 179.5 | 34.84 / 180.4 |
| `epaper_off_post` | 31.00 / 160.6 | 30.90 / 160.1 |
| `mic_off_pre` | 30.89 / 160.2 | 30.79 / 159.6 |
| `mic_capture_16k` | 30.82 / 159.8 | 30.88 / 160.1 |
| `mic_off_post` | 30.79 / 159.5 | 30.74 / 159.4 |
| `wifi_scanning` | 69.21 / 355.9 | 69.01 / 355.2 |
| `baseline_post` | 13.17 / 68.3 | 12.97 / 67.3 |

The two runs differ by at most 0.56 mA per row. Endpoint baselines
differ by 0.058 and 0.001 mA,
within max(2 mA, 5% of their mean).

### Console-open observation

Opening the CH343 port rebooted this board, followed by a brief charge
top-up. The accepted captures kept the console open from battery
qualification through both sweeps.

### Sticky limits

- One unit, two accepted runs; no confidence interval or battery runtime.
- USB input includes the bridge and powered board rails. The 20 Hz samples
  do not resolve peaks shorter than the gap between reads.
- Gauge receipts are boundary observations, not continuous charge monitoring.
- The panel deep-sleep request had no controller readback. LCD brightness,
  external codecs and speaker output are unsupported.
