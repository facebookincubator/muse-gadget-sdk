# Waveshare ESP32-S3-Touch-AMOLED-2.16

This profile runs Muse's avatar, touch settings, push-to-talk, images, BLE
setup, Wi-Fi and home-network tunnel on the **Waveshare ESP32-S3-Touch-AMOLED-2.16**.
It is not a profile for the 1.75 or 1.75C (`s3n`, `s3`), which are round and
use different pins.

Sources: [Waveshare wiki](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16),
[vendor repo](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16)
(clone it, don't read it on the web) and
[schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.16/ESP32-S3-Touch-AMOLED-2.16-Schematic.pdf).
Display, touch and codec come from Waveshare's BSP,
`waveshare/esp32_s3_touch_amoled_2_16` 2.0.1 from the ESP Component Registry,
which pulls in `waveshare/esp_lcd_touch_cst9217`.

## Hardware

| | |
|---|---|
| Chip | ESP32-S3 (QFN56, rev v0.2), 240 MHz, native USB Serial/JTAG |
| Memory | 16 MB quad flash, 8 MB octal PSRAM |
| Display | 2.16" 480×480 CO5300 AMOLED over QSPI: CS 12, SCLK 38, D0–D3 on 4–7, RST 39 |
| Touch | CST9220 (it answers the CST9217 driver and reports chip type 0x9220): INT 11, RST 40 |
| I2C bus (shared) | SCL 14, SDA 15: touch, codecs, AXP2101, PCF85063, QMI8658 |
| Audio | ES8311 speaker codec and ES7210 dual mic ADC. I2S MCLK 42, BCLK 9, LRCK 45, speaker data 8, mic data 10, amp enable 46 |
| Power | AXP2101. DCDC1 is VCC3V3 and ALDO1 is the codecs' A3V3; the schematic leaves the other rails unconnected. Its IRQ line is not wired to the ESP32 |
| Other, not used by Muse | PCF85063 RTC (INT 13), QMI8658 IMU (INT1 17, INT2 21), TF slot on SPI (CMD 1, CLK 2, D0 3, CS 41) |

## Build and flash

```sh
tools/muse/board.sh build s3-216        # -> build-muse-waveshare-s3-216/muse-gadget.bin
tools/muse/board.sh flash s3-216        # finds the port by its USB serial number (the chip's MAC)
```

Set `CONFIG_GADGET_SDK_TOKEN` in `build-muse-waveshare-s3-216/sdkconfig`
before building (see `AGENTS.md`). The build uses `devices/sdkconfig.muse` and
`devices/sdkconfig.muse-waveshare-s3-216` with the 16 MB `partitions_muse.csv`;
the app takes about 2.1 MB of its 4 MB slot. The console and flashing both go
through the chip's own USB port (`/dev/cu.usbmodem*`). Reflashing keeps
pairing and Wi-Fi settings.

The board ships with Waveshare's factory demo. Back up the flash before the
first Muse flash; the whole 16 MB takes about two minutes:

```sh
python -m esptool --chip esp32s3 -p PORT -b 921600 read-flash 0 0x1000000 waveshare-2.16-factory.bin
```

To go back, `python -m esptool --chip esp32s3 -p PORT write-flash 0 waveshare-2.16-factory.bin`.
The factory partition table is a different layout (factory app at `0x20000`,
`ota_0` at `0x620000`, two 3 MB data partitions after it), so restore the
whole image, not just the app.

## Controls and limits

The three buttons are on the top edge. Seen from the front they are BOOT, PWR
and KEY3 from left to right; the label on the back lists them the other way
round (`+/KEY`, `PWR`, `BOOT/-`). The mic icon sits under KEY3 and the power
icon under BOOT.

- **KEY3 (GPIO18, active low):** push-to-talk, pairing confirmation and wake.
- **PWR:** wired only to the AXP2101. The port latches its key edges over I2C
  and treats them as talk too. The PMU powers the board off after a 10 s hold.
  Because its IRQ isn't wired to the ESP32, PWR can't wake the chip from light
  sleep.
- **BOOT (GPIO0):** the aux button.
- Replies from Muse are text and show as captions; there is no spoken output
  yet (see `README.md`, "Replies from Muse are text").
- The IMU, RTC and TF card are not integrated.
- Touch is single-point only (LVGL gesture recognition is off).

## Notes for maintainers

- The vendor's `bsp/display.h` uses `esp_err_t` without including `esp_err.h`;
  `board_waveshare_s3_216.c` includes it first.
- The BSP's own `bsp_display_start()` draws from PSRAM, which needs a fresh
  46 KB internal DMA buffer per flush and fails once Wi-Fi and BLE are up. The
  board file starts the display itself and draws in bands through two fixed
  internal buffers (`muse_lcd_bands.c`), as the 1.75C does.
- Touch is mapped as the BSP's own `bsp_display_start()` maps it at rotation 0
  (`swap_xy` and `mirror_y`).
- The CO5300 needs even-aligned update windows. Brightness is the panel's
  command `0x51`; sleep is plain `SLPIN`/`SLPOUT` (`0x10`/`0x11`), because the
  driver's own sleep enters deep standby and its wake pulses the reset line,
  and panel (GPIO39) and touch (GPIO40) resets are separate lines here.
- Internal RAM is the tightest resource: free internal memory dipped to about
  6 KB while Wi-Fi joined after boot and settles at 14–35 KB with the tunnel
  up. Measure before adding anything that allocates internally (a second TLS
  session, big buffers).

## Hardware verification

Checked on a real board, USB powered, with no battery attached:

- Boots as `Waveshare ESP32-S3-Touch-AMOLED-2.16` with 8 MB PSRAM, no panics or
  reset loops over 10-minute runs.
- The panel draws the 480×480 UI and avatar; the touch controller is detected.
- ES8311 and ES7210 come up and pass the audio self-test; the speaker plays the
  built-in test sound.
- BLE pairing, Wi-Fi, the control session and the home-network tunnel work.
- KEY3 push-to-talk sends a voice note and Muse's text reply shows as captions.
- Muse sends a picture to the screen.
