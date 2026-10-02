# Espressif ESP32-S3-BOX-3

This profile runs Muse's avatar, touch settings, push-to-talk, spoken replies,
images, BLE setup, Wi-Fi, and home-network tunnel on the **BOX-3**. It is not
a profile for the original ESP32-S3-BOX or BOX-Lite.

Hardware initialization uses the pinned `espressif/esp-box-3` 3.2.0 BSP. Its
[source](https://github.com/espressif/esp-bsp/tree/master/bsp/esp-box-3) supplies
the pin map and detects the ST7789/TT21100 and ILI9341/GT911 display revisions.
The main unit has 16 MB flash, 16 MB octal PSRAM, a 320×240 touch LCD, ES8311
speaker codec and ES7210 microphone ADC.

## Build on Windows

Use an **ESP-IDF v6.0.1** PowerShell environment. The repository requires this
version; a 5.5 environment is not supported. From the repository's `esp32`
directory, define the profile arguments once:

```powershell
$box3Args = @(
    '-B', 'build-muse-espressif-box-3',
    '-DIDF_TARGET=esp32s3',
    '-DSDKCONFIG=build-muse-espressif-box-3/sdkconfig',
    '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-espressif-box-3'
)
idf.py @box3Args menuconfig
```

Under **ESP32 Device SDK**, enter your SDK token from
**gadgets.muse.ai → Account → SDK tokens** (`CONFIG_GADGET_SDK_TOKEN`). Save
and exit, then build:

```powershell
idf.py @box3Args build
```

The token stays in the ignored `build-muse-espressif-box-3/sdkconfig` and the
compiled firmware. Do not put it in the board overlay or commit it. A build
without the token is useful for compilation checks, but is not a completed
pairing setup.

The firmware is `build-muse-espressif-box-3/muse-gadget.bin`. Flashing also
needs the bootloader and partition table from that directory; use `idf.py`
with the same arguments instead of writing just the app at an assumed offset.

On Linux/macOS, after activating ESP-IDF v6.0.1:

```sh
tools/muse/board.sh build box3
```

The helper uses the same build directory. Builds for different boards share
`managed_components/` and `dependencies.lock`; do not run them concurrently.

## Flash and pair

Connect a data cable to the main unit's USB-C connector. List ports without
resetting the device:

```powershell
python -m serial.tools.list_ports -v
```

The native ESP32-S3 USB Serial/JTAG device normally has VID:PID `303A:1001`.
Other Espressif boards share that ID; identify the BOX-3's port before flashing.
Replace `COM7` below with that port. To retain the factory firmware, make a
full flash backup before the first Muse flash:

```powershell
python -m esptool --chip esp32s3 -p COM7 read-flash 0 0x1000000 box3-factory.bin
idf.py @box3Args -p COM7 flash monitor
```

Store the backup outside version control. Flashing replaces the factory
firmware and its partition layout. If the port is absent or flashing cannot
connect, hold BOOT/CONFIG, tap RESET, release BOOT/CONFIG, and check ports again.
Exit the monitor with **Ctrl+]**.

Pair `MuseGadget-XXXXXX` in the Muse app, provide Wi-Fi credentials, and press
BOOT/CONFIG to confirm when prompted. Once connected, hold BOOT/CONFIG to
record a voice message and release it to send. Settings use the touchscreen.

## Controls and limits

- BOOT/CONFIG (GPIO0): pairing confirmation, push-to-talk and wake from sleep.
- The hardware microphone mute button cuts ADC power and its data output.
  The port reads its active-low status on GPIO1 and reinitializes the ES7210
  after unmuting; it also boots while muted. A short settling interval after
  unmute is discarded. The button is not reassigned as a menu button.
- The capacitive home button is not integrated; use touch settings.
- Power off puts the ESP32 into deep sleep and disables the backlight and
  speaker amplifier. It cannot disconnect USB power; dock peripherals may
  stay powered. BOOT/CONFIG or RESET wakes it.
- Dock sensors, SD card, IR, battery telemetry, and wake-word detection are
  not integrated. Voice uses the SDK's existing push-to-talk processing.

## Hardware verification

Validation for this port used ESP-IDF v6.0.1: BOX-3, AIPI, and the default C5
firmware builds passed. The BOX-3 signed image is `0x211000` bytes with 48%
of its 4 MB app slot free, and all overlay settings reached the generated
configuration. The host suite ran 144 tests successfully, with three optional
crypto tests skipped for unavailable dependencies. The production avatar and
pairing UI were rendered in the SDL simulator with a temporary 320×240 BOX-3
board profile; the simulator does not exercise the real settings UI or drivers.

An initial hardware boot check on 2026-10-02 passed after backing up the
original flash and programming the token-configured image. The ESP32-S3 rev
0.2 reported 16 MB PSRAM, initialized the ILI9341 display and GT911 touch
controller, opened both audio codecs at 16 kHz stereo, and advertised over
BLE. Microphone self-test captured both channels. A 30-second startup capture
showed no panic or reboot loop. The owner subsequently confirmed pairing and
audible recording chirps, but reported silent voice notes. Both ADC channels
returned zero until reboot. The BOX-3 schematic (ADC and Mute sheets) shows
that hardware mute removes ES7210 power; the initial BSP-only integration did
not restore its lost configuration. The microphone adapter now rebuilds that
configuration after unmute without restarting the speaker or shared I2S bus.
Host coverage exercises muted boot, mute/unmute, a mute cycle during playback,
gain restoration, sleep/wake, and initialization retries. The updated image
booted without a panic and its acoustic loopback test measured both channels
responding to speaker tones at all tested gain settings. Physical mute/unmute
and intelligible voice-note validation still need owner confirmation. Complete these
checks before treating the port as fully hardware-tested:

1. Boot reports the BOX-3 board name and PSRAM, without panics or reset loops.
2. The display has correct colors/orientation and touch aligns at all corners.
3. BLE pairing, button confirmation and Wi-Fi reconnect work.
4. Push-to-talk records intelligible audio, spoken replies play, volume and
   microphone gain work, and the hardware mute button silences capture.
   Unmuting must restore speech without RESET; also test booting while muted.
5. Images and settings work while the network session remains connected.
6. Screen timeout, touch/button wake, power off and BOOT/CONFIG wake work.
