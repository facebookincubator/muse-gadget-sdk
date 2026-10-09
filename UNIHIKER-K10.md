# UNIHIKER K10 as a Muse gadget

This is a community port of the Muse Gadgets ESP32 firmware to the
[DFRobot UNIHIKER K10](https://www.dfrobot.com/product-2904.html). It is not
made or endorsed by Meta. The code is under the Apache 2.0
[LICENSE](LICENSE) in this repository.

Muse runs in the cloud. The K10 is the screen, the buttons, and the status
lights. Each person needs their own SDK token. Do not put a token in a file
you commit, and do not flash someone else's token.

## What works

- ESP32-S3, 16 MB flash, 8 MB octal PSRAM
- 2.8" 240×320 ILI9341 screen and the Muse avatar
- BLE advertising as `MuseGadget-XXXXXX`, then Wi-Fi after pairing
- Button A: push-to-talk and pairing confirm
- Button B: menu
- Three dim WS2812 LEDs that follow the link state (orange while advertising,
  blue while connecting or waiting for the confirm press, green when connected)

## What does not work yet

- Speaker and microphones. The ES7243E ADC at I2C address `0x11` does not
  answer, so voice is unavailable.
- Camera capture, microSD, the AHT20, LTR303ALS, and SC7A20H sensors, and
  battery voltage. Those parts are on the board and are not wired in this
  port.

## What you need

- A UNIHIKER K10 and a USB-C cable that carries data
- [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/get-started/windows-setup.html)
  with the ESP32-S3 toolchain
- Your own token from [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens)
  (Account > SDK tokens). Read the
  [Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms) first.
- The Muse phone app to pair. As of October 2026 that app is only on the
  US and Canadian App Store and Google Play.

## Back up the stock firmware

From `esp32/`, after you have activated ESP-IDF (`export.ps1` on Windows,
`export.sh` on macOS or Linux). Replace `COM5` with the port that shows up
as USB Serial/JTAG (`303a:1001`).

```powershell
python -m esptool --chip esp32s3 -p COM5 read-flash 0 0x1000000 unihiker-k10-stock.bin
```

Keep that file. It is your board's factory image. To put it back:

```powershell
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0 unihiker-k10-stock.bin
```

## Build and flash

On Windows, open the ESP-IDF 6.0.1 PowerShell and run
`. $env:USERPROFILE\esp\esp-idf-v6.0.1\export.ps1` if `idf.py` is not already
on the path. The commands below are from the `esp32` directory. The same
`-B`, target, and defaults flags are required on every command.

```powershell
idf.py -B build-muse-unihiker-k10 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-unihiker-k10/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-unihiker-k10" set-target esp32s3

idf.py -B build-muse-unihiker-k10 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-unihiker-k10/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-unihiker-k10" menuconfig
```

In menuconfig set **ESP32 Device SDK > Muse Gadgets SDK token**, save, then:

```powershell
idf.py -B build-muse-unihiker-k10 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-unihiker-k10/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-unihiker-k10" build

idf.py -B build-muse-unihiker-k10 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-unihiker-k10/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-unihiker-k10" -p COM5 flash monitor
```

If flash cannot connect, hold **BOOT**, tap **RST**, release **BOOT**, and
run flash again.

On macOS or Linux the helper is `tools/muse/board.sh build k10` and
`tools/muse/board.sh flash k10` from the `esp32` directory. Set the token
with `idf.py menuconfig` the same way. The build directory is
`build-muse-unihiker-k10`.

If the ROM log stays silent after flash, change
`CONFIG_ESPTOOLPY_FLASHMODE_DIO` to QIO in
`esp32/devices/sdkconfig.muse-unihiker-k10`, delete
`build-muse-unihiker-k10`, and build again. DFRobot's PlatformIO file for
this board says QIO. This port uses DIO, which booted on the board it was
written against.

## Pair

The serial log should include `muse: board: UNIHIKER K10` and a BLE name
`MuseGadget-XXXXXX`. In the Muse app, turn on **Settings > Devices >
Developer mode**, then **Add Device**. When the screen asks you to confirm,
press **A**. Connected is green on the avatar and on the three LEDs.

Button B steps the on-screen menu. Menu power-off deep-sleeps the chip.
Wake it with **BOOT**, not A or B.
