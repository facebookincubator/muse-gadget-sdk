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

## Driver

The board file, `components/muse/boards/board_viewe_smartring_plus.c`,
drives the hardware directly. It follows VIEWE's BSP,
`viewesmart/smartring_plus` 1.1.2, without depending on it: pins from its
`board_config.h`, the B-panel init table from `lcd_init_seq.h`
(`viewe_smartring_display_init.h`), and the battery handling from
`bsp_battery.c` and `bsp_axp2101.c` (`viewe_smartring_power.h`). The ST77916
panel, CST816S touch and LVGL adapter components are pulled in only when
`MUSE_BOARD_ID` is `viewe_smartring_plus`. The alias in `tools/muse/board.sh`
is `smartring-plus`.

## Display, audio and power

LVGL's task runs on the Muse UI core. Touch is the CST816S driver, which
pulses the controller's reset (GPIO40) at start. The panel draws through two
10-line buffers in internal RAM, as the EchoEar-2ST does with the same panel,
instead of the BSP's 40-line PSRAM buffers: those need a fresh 28.8 KB
internal DMA buffer on every flush, which may not be available once Wi-Fi,
BLE and the Muse session are up.

The board file doesn't use the BSP's `bsp_audio_init()`, which opens the
codec itself and leaves the speaker muted; Muse never unmutes a handle it did
not open. It builds a 16 kHz duplex I2S (`MUSE_AUDIO_RATE`) and returns
separate speaker and mic handles. `mic_slot` is 0.

Battery: on V2 boards the AXP2101 turns on the ALDO3 peripheral rail and
charges the 600 mAh cell at 300 mA to 4.2 V, as the BSP sets it, and reports
its gauge percent, voltage, charging and USB. On V1 boards GPIO1 reads the
VCC_OUT rail; percent comes from the cell's voltage curve, and charging (and
USB) from the rail rising above 4.25 V.

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

Activate **ESP-IDF v6.0.3**, as this repository requires. See Hardware
verification below for what was checked on the board.

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

These checks were made with the earlier BSP-based port, built locally with
ESP-IDF v6.0.3 and no warnings: the screen powers on after Wi-Fi comes up,
touch orientation is correct, and slot 0 carries the live microphone. The
direct driver keeps the BSP's pins, panel table, touch driver and battery
settings but hasn't been rechecked on the board. Muse app pairing and app
connection were not tested (region/app availability).
