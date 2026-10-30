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

# Waveshare ESP32-S3-Touch-AMOLED-2.06

This profile runs Muse's avatar, touch settings, push-to-talk, images, BLE
setup, Wi-Fi and home-network tunnel on the **Waveshare
ESP32-S3-Touch-AMOLED-2.06**. It is not the profile for the ESP32-C6 version
of the same watch (`c6-206`), which has no PSRAM and different pins.

Sources: [vendor repo](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06)
(clone it, don't read it on the web), its
`Schematic/ESP32-S3-Touch-AMOLED-2.06-Schematic-V1.0.pdf`, and Waveshare's
BSP `waveshare/esp32_s3_touch_amoled_2_06` 2.0.0 from the ESP Component
Registry, which pulls in `waveshare/esp_lcd_sh8601` and
`espressif/esp_lcd_touch_ft5x06`.

## Hardware

| | |
|---|---|
| Chip | ESP32-S3R8, 240 MHz, native USB Serial/JTAG |
| Memory | 32 MB GD25Q256 flash, 8 MB octal PSRAM (in the chip package) |
| Display | 2.06" 410×502 CO5300 AMOLED over QSPI, driven as an SH8601 with a 22-column gap: CS 12, PCLK 11, D0–D3 on 4–7, RST 8 |
| Touch | FT3168 at I2C 0x38 (ft5x06 driver): INT 38, RST 9 |
| I2C bus (shared) | SCL 14, SDA 15: touch, codecs, AXP2101, PCF85063, QMI8658 |
| Audio | ES8311 speaker codec and ES7210 ADC with two microphones. I2S MCLK 16, BCLK 41, LRCK 45, speaker data 40, mic data 42, amp enable 46 |
| Power | AXP2101. Its IRQ line is not wired to the ESP32 |
| Buttons | BOOT (Key1, GPIO0) and PWR (Key3, AXP2101 PWRON only), on the right edge, BOOT above PWR |
| Other, not used by Muse | Vibration motor (GPIO18), PCF85063 RTC, QMI8658 IMU, TF slot (CLK 2, CMD 1, D0 3) |

## Build and flash

```sh
tools/muse/board.sh build s3-206        # -> build-muse-waveshare-s3-206/muse-gadget.bin
tools/muse/board.sh flash s3-206        # finds the port by its USB serial number (the chip's MAC)
```

Set `CONFIG_GADGET_SDK_TOKEN` in `build-muse-waveshare-s3-206/sdkconfig`
before building (see `AGENTS.md`). The build uses `devices/sdkconfig.muse` and
`devices/sdkconfig.muse-waveshare-s3-206` with `partitions_muse.csv`. The
console and flashing both go through the chip's own USB port.

The board ships with Waveshare's factory firmware. Back up the flash before
the first Muse flash:

```sh
python -m esptool --chip esp32s3 -p PORT -b 921600 read-flash 0 0x2000000 waveshare-s3-2.06-factory.bin
```

To go back, `python -m esptool --chip esp32s3 -p PORT write-flash 0 waveshare-s3-2.06-factory.bin`.
Restore the whole image, not just the app: the factory partition table is
different.

## Controls and limits

- **BOOT (GPIO0):** push-to-talk, pairing confirmation and wake.
- **PWR:** wired only to the AXP2101. The port latches its key edges over I2C
  as the aux button. Because the PMU's IRQ isn't wired to the ESP32, PWR can't
  wake the chip from light sleep, so use BOOT to wake it. The PMU powers the
  board on and off itself.
- Replies from Muse are text and show as captions, as on the other boards.
- The motor, IMU, RTC and TF card are not integrated.

## Notes for maintainers

- The vendor's `bsp/display.h` uses `esp_err_t` without including `esp_err.h`,
  as the 2.16's does; `board_waveshare_s3_206.c` includes it first.
- The board file follows `board_waveshare_s3_216.c`: only the panel comes from
  the BSP, and LVGL draws bands in PSRAM through `muse_lcd_bands.c`. The BSP's
  own `bsp_display_start()` uses `esp_lvgl_port` with internal draw buffers.
  It is still linked, because the BSP depends on it, but it is never started.
- `bsp_display_new()` runs Waveshare's init table, sets the 22-column gap and
  turns the panel on. The CO5300 needs even-aligned windows. Brightness is the
  panel command `0x51`, and sleep is plain `SLPIN`/`SLPOUT`.
- Touch keeps the BSP's mapping, with no swap or mirror.
- On screen-off, only LVGL is paused. With nothing polling it, the FT3168
  drops to its own monitor mode. Its hibernate (`0xA5` = 3) can only be left
  through the reset line, and that reset loses the thresholds the ft5x06
  driver writes at init.
- The BSP opens I2S as mono at 22.05 kHz unless `bsp_audio_init()` gets a
  config first. `audio_init` passes Muse's stereo config, as the C6 2.06 does.
- PMU rails are left as the AXP2101 boots them. Waveshare's AXP2101 example
  switches none, and the 2.16's `muse_pmu_keep_rails()` mask hasn't been
  checked against this schematic.
- Flash is set to 32 MB from the schematic's GD25Q256. Waveshare's examples
  configure 16 MB; `esptool flash-id` on a real board reports 32 MB
  (GigaDevice `c8 4019`).
- Free internal memory dips to about 7 KB during boot and the first Wi-Fi
  scan, close to the 2.16's 6 KB. Measure before adding anything that
  allocates internally.
- `gpio_install_isr_service ... already installed` at boot is harmless: the
  touch registration tries to install the GPIO ISR service, which is already
  running.

## Hardware verification

Checked on a real board (ESP32-S3 rev v0.2, 32 MB flash, 8 MB octal PSRAM),
built with ESP-IDF v6.1:

- `esptool flash-id` reports 32 MB. The board boots as
  `Waveshare ESP32-S3-Touch-AMOLED-2.06` with 8 MB PSRAM at 80 MHz, without
  panics or reset loops.
- The 410×502 UI fills the panel with no offset or wrapped columns, and
  touches land where tapped, corners included.
- ES8311 and ES7210 open, and both microphones pass the audio self-test (equal
  levels, correlation 0.97, capture and playback at about 16 kHz).
- BLE pairing (confirmed with BOOT) and Wi-Fi work.
- BOOT push-to-talk sends a voice note, and PWR registers as aux.
- After a screen-off on battery, BOOT wakes the board and touch still responds.

Not yet checked: the home-network tunnel, images from Muse, and long runs.
