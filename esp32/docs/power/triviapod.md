# TriviaPOD power-test image

`CONFIG_MUSE_POWER_TEST` builds for the M5Stack TriviaPOD on the shared
backend, `boards/board_power_test_generic.c`, with the matrix in
[km003c.md](km003c.md) plus five rows for the vibration motor. Like the other
power-test images it never starts the application: no pairing, no SDK token,
no stored Wi-Fi. Measured sweeps are in
[usb5v-findings.md](usb5v-findings.md#triviapod-2026-11-10) and
[usb5v-profile.csv](usb5v-profile.csv).

## Build

With ESP-IDF v6.0.3 active, from `esp32/`:

```sh
idf.py -B build-muse-m5stack-triviapod-power-test -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-m5stack-triviapod-power-test/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-triviapod;devices/sdkconfig.muse-m5stack-triviapod-power-test" build
```

## Charging off

`"charging_off":1` clears bit 0 (`CHG_EN`) of M5PM1 register `0x06`
(`PWR_CFG`), as named in M5Stack's
[M5PM1 driver](https://github.com/m5stack/M5PM1/blob/8f1f1a60b3040088cd0ca2b9eb9d024ccc4d907f/src/M5PM1.h).
The write changes only that bit, shares a mutex with the USB LED's update of
the same register and checks the whole register on readback.

Charging is switched back on, with readback, on finish, abort and error, by
the guard task once the run's deadline has passed, and at the next boot of
this image. The normal TriviaPOD firmware also sets the bit at the start of
board init, so flashing it back repairs a run that was cut. Nothing restores
the bit while the ESP32 is not running.

## Matrix

The shared rows: fixed 40, 80, 160 and 240 MHz idle, black and white frames
at 0, 25, 50 and 100 %, the colour animation, codecs closed and open, mic
capture, a quiet sine and Wi-Fi scanning. The TriviaPOD has one ES8311 and no
separate ADC or amplifier row. On lit static frames LVGL is paused with the
panel left on; dark rows use the board's normal display pause, which cuts the
panel rail.

The motor rows drive the M5IOE1's PWM1 (IO9) at 2000 Hz and 60 % duty
(`0x8999` in the duty register), all at a fixed 80 MHz with the display dark:

| State | Load | Capture |
|---|---|---|
| `motor_idle_pre` | motor off | as requested |
| `motor_on_60pct` | motor on continuously | as requested, at most 25 s with settling |
| `motor_idle_between` | the pulse schedule with the motor off | 302 s |
| `motor_pulses_30_70ms` | 30 ms pulse, 70 ms pulse 1 s later, every 5 s | 302 s |
| `motor_idle_post` | the pulse schedule with the motor off | 302 s |

- The two idle rows around the pulses run the same schedule and the same I2C
  writes with zero duty, so the pulse row differs from them by the motor only.
- A pulse is held for its width after the enabling write returns.
- The three long rows always capture 302 s. The plan in the arm reply carries
  each row's `capture_ms`, and the one-hour bound and the charging guard count
  it.
- A worker separate from the sweep checks the motor every 10 ms. It ends the
  run as `error` if a write or readback fails, if a pulse edge is more than
  20 ms late, if a pulse stays on over 120 ms, or if the continuous row
  passes 30 s. A failed stop is retried every 10 ms.
- After each motor row the image prints one `motor_pulse` line per pulse, with
  the times of the enabling write and of the verified stop. The host tool
  saves them in the capture's header. They are software times and include the
  I2C work: they are not the width of the current pulse.

## Run

```sh
python3 tools/power/km003c_ptest.py --console PORT --output triviapod-usb5v.jsonl \
  --charging-off --jitter-ms 10
```

A sweep takes about 26 minutes. `--jitter-ms 10` moves each meter read by up
to 10 ms either way, so the 20 Hz polling does not stay in step with pulses
that start on a fixed schedule. The meter still cannot resolve one 30 ms
pulse: the pulse rows give a mean over the window.

The tool now keeps the sign of each current sample in the saved samples
(summaries use magnitudes) and saves the samples of successful runs too.
