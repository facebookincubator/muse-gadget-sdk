# SenseCAP Watcher: light-sleep and deep-sleep deep dive

## Scope

One sleep-matrix (v2) sweep on 2026-10-09: **26 states, all OK**, including
two timer-only deep-sleep resets with a validated resume chain. Figures are
**conditional receive-window estimates under an assumed 250 ppm clock-drift
budget and 1-second edge guards**, plus each deep-sleep record's RTC timeline
uncertainty (0.31 s and 0.61 s). Use the [scrubbed CSV](sensecap-watcher-sleep-profile.csv)
for numbers. Fixture, source setpoint (3.872 V) and caveats are as in the
[peripheral profile](sensecap-watcher-findings.md) and the
[setup guide](sensecap-watcher.md).

- Codecs genuinely cold: the board was fully power-cycled (USB unplugged, PPK
  output off) after flashing, and only a status query ran before arming.
- Every light-sleep state uses DFS 40–240 MHz with automatic light sleep and
  logged about 19.97 s asleep of 20 s, 20 entries (4 at 5 s polling).
- Rails resting: LCD/touch, ADC divider, PA, Himax and Wi-Fi off.
- Measured image SHA-256:
  `6c8e372c9c9a1b69ffad7625c389eb7d101fcb0e64a1ceec6ae3ec78f15309cc`.

### Transport

The PPK2 collector stalled for about 10 s and overflowed its raw-queue entry
cap **about 100 s after the last state ended**. The analyzer's opt-in
`--pre-fault-prefix` mode accepts only windows ending before
`fault − max consumer delay − guard`, with byte conservation checked: 26/26
rows end at least 89 s before that cutoff. All selected windows are contiguous,
with coverage 99.81–100.70%, |sample/host drift| ≤ 0.09 s, label consumer delay
≤ 0.12 s and zero decoded loss. The capture as a whole stays marked faulted.
The receiver's entry cap now scales with its byte budget so a 16 MiB buffer is
fully usable.

## Results

| State | Knobs | Mean mA | Min–max mA | Source mW | Included s | Coverage % |
|---|---|---:|---:|---:|---:|---:|
| `cold_ref` | codec_cold | 3.681 | 0.848–67.3 | 14.25 | 17.9 | 99.997 |
| `cold_i2s_low` | i2s_low | 3.601 | 0.827–70.3 | 13.94 | 17.9 | 99.997 |
| `cold_i2s_hiz` | i2s_hiz | 3.649 | 0.827–62.9 | 14.13 | 17.9 | 99.997 |
| `cold_i2s_low_b` | i2s_low | 3.602 | 0.810–65.0 | 13.95 | 17.9 | 99.997 |
| `codec_initialized` | codec_initialized | 15.603 | 11.851–90.9 | 60.41 | 17.9 | 99.997 |
| `codec_suspended` | codec_suspended | 2.970 | 0.677–68.8 | 11.50 | 17.9 | 99.997 |
| `warm_ref` | — | 2.972 | 0.609–68.0 | 11.51 | 17.9 | 99.997 |
| `uart_hiz` | uart_hiz | 2.969 | 0.652–68.0 | 11.50 | 17.9 | 99.983 |
| `warm_ref_2` | — | 2.968 | 0.639–71.8 | 11.49 | 17.9 | 99.997 |
| `rgb_low` | rgb_low | 2.966 | 0.656–65.0 | 11.48 | 17.9 | 100.700 |
| `warm_ref_3` | — | 2.963 | 0.643–76.4 | 11.47 | 17.9 | 99.983 |
| `pulls_off` | pulls_off | 2.961 | 0.656–74.1 | 11.46 | 17.9 | 99.997 |
| `warm_ref_4` | — | 2.963 | 0.613–75.6 | 11.47 | 17.9 | 99.997 |
| `unused_hiz` | unused_hiz | 2.962 | 0.626–72.6 | 11.47 | 17.9 | 100.012 |
| `warm_ref_5` | — | 2.961 | 0.669–69.6 | 11.47 | 17.9 | 100.012 |
| `gpio_isolate` | gpio_isolate | 3.062 | 0.652–62.7 | 11.86 | 18.0 | 100.011 |
| `warm_ref_6` | — | 2.957 | 0.630–71.1 | 11.45 | 17.9 | 99.811 |
| `cpu_pd` | cpu_pd | 2.519 | 0.363–71.1 | 9.75 | 17.9 | 99.997 |
| `warm_ref_7` | — | 2.956 | 0.588–71.8 | 11.45 | 17.9 | 99.997 |
| `best_combined` | all seven¹ | 2.458 | 0.367–69.6 | 9.52 | 17.9 | 99.997 |
| `warm_ref_8` | — | 2.954 | 0.643–67.3 | 11.44 | 18.0 | 99.996 |
| `best_combined_b` | all seven¹ | 2.454 | 0.584–72.6 | 9.50 | 17.9 | 99.997 |
| `best_poll_5s` | all seven¹, 5 s poll | 2.422 | 0.711–60.9 | 9.38 | 17.9 | 99.997 |
| `warm_ref_9` | — | 2.952 | 0.669–74.1 | 11.43 | 17.9 | 100.041 |
| `deep_sleep_timer` | default deep-sleep pad isolation | 1.590 | 0.626–3.4 | 6.16 | 18.4 | 99.993 |
| `deep_sleep_timer_held` | I2S + RGB held low | 1.124 | 0.074–12.4 | 4.35 | 17.9 | 99.997 |

¹ i2s_low, uart_hiz, rgb_low, pulls_off, unused_hiz, gpio_isolate, cpu_pd.

### A/B/A comparisons

Each variant is compared against the count-weighted mean of the nearest
reference before and after it (`watcher_power.py compare`). "Drift" is
|A_after − A_before|; a delta within the drift is unresolved. Above-drift is a
diagnostic, not a significance test, and there is one repeat.

| Variant | A mA | B mA | B − A µA | Drift µA | Resolved |
|---|---:|---:|---:|---:|---|
| `cold_i2s_hiz` (vs I2S low) | 3.601 | 3.649 | +47 | 0.8 | yes |
| `uart_hiz` | 2.970 | 2.969 | −1 | 3.7 | no |
| `rgb_low` | 2.965 | 2.966 | +0 | 4.9 | no |
| `pulls_off` | 2.963 | 2.961 | −2 | 0.1 | yes (negligible) |
| `unused_hiz` | 2.962 | 2.962 | +0 | 1.7 | no |
| `gpio_isolate` | 2.959 | 3.062 | **+103** | 4.2 | yes (worse) |
| `cpu_pd` | 2.957 | 2.519 | **−438** | 0.6 | yes |
| `best_combined` | 2.955 | 2.458 | −497 | 2.3 | yes |
| `best_combined_b` | 2.953 | 2.454 | −499 | 2.3 | yes |
| `best_poll_5s` | 2.953 | 2.422 | −530 | 2.3 | yes |

## Findings

1. **The 36 mA "cold baseline" in the peripheral run was not cold.** That run
   followed production firmware, which leaves the codecs open while on USB.
   The codec rail is always powered from USB/VBAT, so reflashing and ESP-only
   resets keep the codecs running. After a real power cycle, power-on-default
   codecs rest at **3.68 mA**.
2. **Suspending the codecs saves 0.71 mA** versus their power-on default
   (2.97 vs 3.68 mA). Constructor-only initialization without open/close costs
   **+11.9 mA** (15.6 mA): the driver's setup enables the ES7243E ADC. Firmware
   should leave the codecs either streaming or explicitly suspended, never
   just constructed or left open while idle.
3. **CPU power-down in light sleep is the only large software lever:
   −0.44 mA (−15%)**, repeatable within 1 µA of reference drift. Production
   firmware disables it on the Watcher because retention takes about 8.5 KB of
   internal RAM that Wi-Fi/BLE need. Enabling it needs a runtime RAM and
   stability soak on production firmware first; that follow-up is outside this
   change.
4. **Sleep GPIO isolation is counterproductive here: +0.10 mA.** Floating
   pads that the board pulls or drives cost more than holding them. Keep
   `CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND`/`PM_SLP_DISABLE_GPIO` off.
5. **Pin hygiene is negligible in light sleep:** UART high-Z, RGB DIN low,
   internal pull-ups off and unused-interface high-Z each move ≤ 2 µA. Driving
   I2S low instead of floating saves 47 µA.
6. **Polling is cheap.** 5 s instead of 1 s VBUS polling saves about 32 µA
   (combined variants, 2.454 → 2.422 mA). The remaining mean-vs-median gap is
   not from these wakes.
7. **Deep sleep (timer): 1.59 mA default, 1.12 mA with I2S and RGB pads held
   low (−0.47 mA).** The ESP32-S3 itself needs microamps in deep sleep, so about
   1.1 mA is the board's always-on floor (regulators, codecs on their own LDO,
   USB-bridge I/O supply, RGB LED, expander and pull-ups). The light-sleep floor
   is about 1.3 mA higher, mostly the ESP32-S3 with PSRAM/flash retained.
8. **Unexplained post-run state:** after the second deep-sleep resume, the
   restored resting state drew about 4.0 mA (not a measured state; observed
   in the post-sweep tail). This should be investigated before any production
   deep-sleep use.

**No production defaults change in this PR.** CPU power-down is the clear next
candidate (findings 3), pending the RAM soak. The codec rail itself can't be
switched on a stock board: the optional `EXP_AUDIO_EN` link from expander P15
to the codec LDO enable appears unfitted (Seeed's BSP uses P15 as an input).

## Limitations

- One repeat; no between-run confidence interval. Temperature and DUT
  terminal voltage were not recorded. Total board-input current at the source
  setpoint, not IC-level rails.
- `cpu_pd` reports permission granted, not observed CPU power-down; the
  compile-time retention support is present in every state of this image, so
  reference values may differ slightly from the peripheral image.
- Deep-sleep windows depend on the assumed RTC slow-clock accuracy (1% of
  elapsed plus 50 ms per resume), which is not validated.
- Diagnostic image only: no LVGL, touch, Wi-Fi association, BLE or production
  tasks. Production resting power must be measured on production firmware.
