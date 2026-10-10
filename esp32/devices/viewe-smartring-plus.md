# VIEWE SmartRing-Plus

This profile runs Muse's avatar, touch settings, on-screen push-to-talk,
images, BLE setup, Wi-Fi, and the home-network tunnel on the VIEWESMART
SmartRing-Plus (ESP32-S3-N16R8).

Buy it from [VIEWE](https://viewedisplay.com/product/smart-ring-plus-esp32-ai-chatbot-1-8-inch-smart-touch-display/).

The panel is a 360×360 round ST77916 on QSPI with a CST816 touch controller.
Audio is an ES8311, with the speaker amplifier enable on GPIO45 and one analog
microphone. Battery telemetry is a GPIO1 ADC on V1 boards and an AXP2101 on
V2 boards. BOOT is GPIO0 and is inside the case. The console is the chip's
own USB Serial/JTAG port (303a:1001). Flash is 16 MB and PSRAM is 8 MB octal,
so the build uses `partitions_muse.csv`.

## BSP

`viewesmart/smartring_plus` 1.1.2 is vendored at
`components/viewesmart__smartring_plus`. The directory name has two
underscores because `override_path` has to match the managed component name.
The version in that directory's `idf_component.yml` is `"1.1.2"`, which
satisfies `^1.1.2`. A prerelease tag such as `1.1.2-muse` does not.
`components/muse/idf_component.yml` sets

```yaml
viewesmart/smartring_plus:
  version: "^1.1.2"
  override_path: "../viewesmart__smartring_plus"
```

and pulls it in only when `MUSE_BOARD_ID` is `viewe_smartring_plus`. The
alias in `tools/muse/board.sh` is `smartring-plus`.

The vendored constraints are `lvgl >=9.4.0,<10.0.0` and
`esp_codec_dev >=1.3.4,<2.0.0`, so the component solves against Muse's
LVGL 9.5 and esp_codec_dev ~1.5.

## Display, audio and power

`bsp_display_init_with_task()` starts the panel and pins LVGL's task to the
Muse UI core. Touch is the CST816 input device the BSP registers.

Do not call `bsp_audio_init()` from the board file. It opens the codec itself
and leaves the speaker muted, and Muse never unmutes a handle it did not
open. `board_viewe_smartring_plus.c` builds a 16 kHz duplex I2S
(`MUSE_AUDIO_RATE`) and returns separate speaker and mic handles. `mic_slot`
is 0.

`power_off()` logs and returns `ESP_ERR_NOT_SUPPORTED`. Choosing Power off
in Settings shows "COULDN'T POWER OFF". Turn the board off with the hardware
switch.

## Talk, pairing and reset

The mic icon at the bottom centre, under the caption and above the page dots,
is the talk button (`touch_talk` on `muse_board_t`):

- Let go within 0.4 s and the note keeps recording until the next tap.
- Hold longer than 0.4 s and let go to send, the same as push-to-talk.
- While Muse is asking you to confirm a pairing, a tap on the pairing card
  confirms it.
- BOOT still produces the same talk events if you can reach it.

To forget Wi-Fi and the app pairing, open Settings, then MUSE, and tap
Reset pairing again within 5 seconds. The board restarts.

## Build

This repository documents ESP-IDF v6.0.1. The SmartRing-Plus port was
compiled locally with ESP-IDF v6.0.3. Hardware has been verified on the board,
but Muse app pairing and app connection were not tested (region/app availability).

On Linux or macOS, from `esp32`:

```sh
tools/muse/board.sh build smartring-plus
```

On Windows, from `esp32` in an ESP-IDF PowerShell environment:

```powershell
$ringArgs = @(
    '-B', 'build-muse-viewe-smartring-plus',
    '-DIDF_TARGET=esp32s3',
    '-DSDKCONFIG=build-muse-viewe-smartring-plus/sdkconfig',
    '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-viewe-smartring-plus'
)
idf.py @ringArgs build
```

## SDK token

A firmware that pairs needs an SDK token. It is not in git. Before flashing,
run `idf.py @ringArgs menuconfig` (or the `tools/muse/board.sh` build, then
menuconfig against `build-muse-viewe-smartring-plus/sdkconfig`) and set
**ESP32 Device SDK > Muse Gadgets SDK token** (`CONFIG_GADGET_SDK_TOKEN`).
The token stays in that sdkconfig and in the compiled binary. Do not copy it
into `devices/sdkconfig.muse-viewe-smartring-plus`, and do not commit the
build directory. A build without the token only checks that the port
compiles.

## Flash

Back up the flash before the first Muse flash. On Windows, with the same
`$ringArgs`:

```powershell
idf.py @ringArgs -p COMx flash monitor
```

On Linux or macOS, `tools/muse/board.sh flash smartring-plus` finds the
USB Serial/JTAG port.

## Hardware verification

The local ESP-IDF v6.0.3 build finished with no warnings. Hardware has been
verified: the screen powers on after Wi-Fi comes up, touch orientation is
correct, and slot 0 carries the live microphone. Muse app pairing and app
connection were not tested (region/app availability).
