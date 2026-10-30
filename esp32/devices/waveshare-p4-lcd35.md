# Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5

The P4 runs Muse on the 320×480 ST7796 SPI LCD with FT6336 touch, one ES8311 for the speaker and the mic,
and an AXP2101 for battery status. The onboard ESP32-C6 provides Wi-Fi and BLE over ESP-Hosted SDIO. The
console is UART0 on the CH343 USB bridge (`1a86:55d3`).

The side buttons are BOOT, RESET and PWR. BOOT (GPIO35) is push-to-talk. PWR is read through the AXP2101:
a press turns the screen off or on, two quick presses toggle phone setup, a 1.5 s hold powers the board
off, and a press turns it back on. RESET restarts the P4.

Images from Muse (`display.draw_url`) are off: the P4's ROM has no JPEG decoder.

## Build

Activate ESP-IDF **v6.0.1** with the `esp32p4` tools installed:

```sh
tools/muse/board.sh build p4lcd35
```

Or directly:

```sh
idf.py -B build-muse-waveshare-p4-lcd35 -DIDF_TARGET=esp32p4 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-p4-lcd35" build
```

The overlay targets P4 silicon v3.x, which the boot log and `esptool chip-id` report. Waveshare also
ships the board with v1.x silicon, which needs a build with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` and
`CONFIG_ESP32P4_REV_MIN_100`. Set `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360` too: ESP-IDF does not guarantee
the overlay's 400 MHz before silicon v3. That build is untested.

## C6 coprocessor

The host side is ESP-Hosted 2.12, so the C6 must run a 2.12 ESP-Hosted slave with Wi-Fi and BLE over
SDIO. The board we tested shipped with an older slave: it reported version 0.0.0 and BLE pairing failed.
Update it once from the P4 with ESP-Hosted's `host_performs_slave_ota` example and a 2.12 slave image
for the C6, such as Espressif's
[esp32c6-v2.12.13.bin](https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.12.13.bin).
We tested 2.12.13 on both the P4 and the C6.
Flashing Muse does not touch the C6. If its firmware can't take an update over SDIO, the C6's own UART
and IO9 are on header J6: pin 25 RXD, 27 TXD, 29 IO9, 31 GND.
