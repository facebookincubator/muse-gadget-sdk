# VIEWE SmartRing-Plus

This profile builds Muse's avatar, touch settings, on-screen push-to-talk,
images, BLE setup, Wi-Fi, and the home-network tunnel for the VIEWESMART
SmartRing-Plus (ESP32-S3-N16R8). Pairing with the Muse app and everything that
needs it are untested on this board; see Hardware verification below.

Buy it from [VIEWE](https://viewedisplay.com/product/smart-ring-plus-esp32-ai-chatbot-1-8-inch-smart-touch-display/).

The panel is a 360×360 round ST77916 on QSPI with a CST816 touch controller.
Audio is an ES8311, with the speaker amplifier enable on GPIO45 and one analog
microphone. Battery telemetry is a GPIO1 ADC on V1 boards and an AXP2101 on
V2 boards. BOOT is GPIO0 and is inside the case. The console is the chip's
own USB Serial/JTAG port (303a:1001). Flash is 16 MB and PSRAM is 8 MB octal,
so the build uses `partitions_muse.csv`.

## BSP

`viewesmart/smartring_plus` 1.1.2 is vendored at
`components_vendor/viewesmart__smartring_plus`. It sits outside `components/`
so ESP-IDF doesn't pick it up for every build: other targets would compile its
S3 pin map and pull in its LVGL and panel dependencies. The version in that
directory's `idf_component.yml` is `"1.1.2"`, which satisfies `^1.1.2`. A
prerelease tag such as `1.1.2-muse` does not.
`components/muse/idf_component.yml` sets

```yaml
viewesmart/smartring_plus:
  version: "^1.1.2"
  override_path: "../../components_vendor/viewesmart__smartring_plus"
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

The board file leaves `power_off` out, so Settings has no Power off. The BSP
has a GPIO47 soft power-off (`bsp_power_shutdown()`), but it hasn't been
checked with this port. Turn the board off with its switch.

The board leaves `display_pause` out. Pausing LVGL on battery would also stop
touch, and with BOOT inside the case touch is the only way to wake the screen.
LVGL keeps running while the screen is dark, so a tap wakes it, at the cost of
the light sleep the paused boards get.

## Talk, pairing and reset

The mic icon at the bottom centre, under the caption and above the page dots,
is the talk button (`touch_talk` on `muse_board_t`):

- Hold it to talk and let go to send.
- While Muse is asking you to confirm a pairing, a tap on the screen
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
