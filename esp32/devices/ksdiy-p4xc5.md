# KSDIY P4XC5 (4.3 inch)

The P4 runs Muse, the ST7102 480×800 MIPI display, ST7123 touch and ES8311/ES7210 audio. The onboard ESP32-C5 provides both Wi-Fi and BLE over ESP-Hosted SDIO. K1 (GPIO35) is push-to-talk; K2/BOOT (GPIO0) opens the menu. AXP2101 supplies power and battery readings.

## Build

Activate ESP-IDF **v6.0.1** with the `esp32p4` tools installed:

```sh
tools/muse/board.sh build p4xc5
```

Or directly (also works in an activated Windows IDF terminal):

```sh
idf.py -B build-muse-ksdiy-p4xc5 -DIDF_TARGET=esp32p4 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-ksdiy-p4xc5" build
```

Configure your SDK token locally with the same build directory and `menuconfig`; it is not stored in the overlay. Flash the **P4** native USB port, not the C5 port. The helper matches native USB Serial/JTAG (303a:1001); with several devices attached specify its serial number or use `idf.py -p COMx flash` with the same profile arguments.

## Hardware revisions

The main overlay targets P4 silicon v3.x, matching the existing vendor firmware. For v1.x use the additional revision overlay last; do not flash a v3 binary to v1 silicon:

```sh
idf.py -B build-muse-ksdiy-p4xc5-v1 -DIDF_TARGET=esp32p4 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-ksdiy-p4xc5;devices/sdkconfig.muse-ksdiy-p4xc5-v1" build
```

## C5 coprocessor

The host component is pinned to ESP-Hosted **2.12.x** (`~2.12.6` in `main/idf_component.yml`). The onboard C5 must run a **matching 2.12.x** ESP-Hosted slave build with **both Wi-Fi and Bluetooth/NimBLE HCI enabled**, SDIO transport, and the board's C5 wiring. A 3.x-only C5 image will show RPC timeouts on scan and connect. Wi-Fi-only C5 firmware cannot advertise Muse's BLE pairing service. A P4 application flash does not update C5 firmware. C5 pins must come from the board's slave project/schematic, not the P4 pin numbers below.

Vendor reference for Wi-Fi scan timing and Hosted Kconfig: `ESP32P4_KSDIY/P4_C5_4.3_Firmware/04.advanced.p4c5_board_test` (`main/wifi_scanner.c`, `sdkconfig.defaults`). That project expects C5 slave merge-bin **`ESP32C5_V2.12.13_0X0.bin`** (see `c5_fw/README.txt` there). Flash or update the C5 image with the vendor `ksdiy_c5_sdio_flasher` flow if scan/connect RPC times out while SDIO transport looks healthy.

P4 host SDIO: CLK18, CMD19, D0=14, D1=15, D2=16, D3=17; active-high C5 reset GPIO54. Shared I2C0: SDA7/SCL8. Backlight GPIO6. Sources: local vendor `ESP32P4_KSDIY/P4_C5_4.3_Firmware/50.application.music_performer` and pinned KSDIY BSP/audio components.

Wi-Fi regulatory domain on this board defaults to **CN, channels 1–13** (set on the C5 after Hosted RPC is up). Do not rely on `esp_wifi_get_country` over RPC when the coprocessor is busy; garbled country logs usually mean the C5 slave firmware does not match Hosted 2.12.x.

## Audio and verification

Muse's API remains stereo s16 at 16kHz. P4 playback expands each channel to high-aligned s32, matching TX 2×32bit and RX 4×16bit on the shared clock. RX selects MIC1 (slot0) and speaker reference (slot2); only MIC1 is used for voice. Original Muse boards keep their existing audio path.

After flashing, check PSRAM detection, no panic/reboot loop, touch coordinates, K1 recording, K2 menu, battery/USB status, normal-speed audio and MuseGadget BLE advertising. Pairing and network/audio behavior require a board with the matching C5 firmware and a locally configured SDK token.

`display.draw_url` is disabled on this target: its current JPEG decoder depends on a ROM decoder that P4 does not provide. The avatar, settings screen and touch UI remain available.
