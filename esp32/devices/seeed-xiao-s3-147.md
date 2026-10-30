# Seeed XIAO ESP32-S3 1.47 Touch Display

The [Seeed Studio XIAO 1.47" IPS Touch Display (ESP32-S3)](https://www.seeedstudio.com/1-47-Inch-Touch-Display-Powered-by-XIAO-ESP32-S3-Plus-p-6996.html)
combines a XIAO ESP32-S3 Plus with a small display board: a 172×320 IPS
screen, capacitive touch, a PDM microphone and two user buttons.

<img src="https://files.seeedstudio.com/wiki/Display_Gadgets/imgs/147_ESP32S3Plus_display_hardware_heroNEW.jpg" width="480" alt="Seeed Studio XIAO 1.47-inch IPS touch display powered by XIAO ESP32-S3 Plus">

| Hardware | Specification |
|---|---|
| Controller | XIAO ESP32-S3 Plus, dual-core ESP32-S3 up to 240 MHz |
| Memory | 16 MB flash, 8 MB octal PSRAM |
| Display | 1.47" 172×320 IPS LCD, JD9853A, SPI |
| Input | Capacitive touch (AXS5106L), PDM microphone, USR1 and USR2 |
| Wireless | 2.4 GHz Wi-Fi and Bluetooth LE |
| USB | Native USB Serial/JTAG (`303a:1001`) |

## What it does with Muse

The Muse avatar, touch settings, BLE pairing, Wi-Fi, USR1 push-to-talk,
Chinese, Japanese and Korean captions, and USR2 screen sleep and wake.
Replies from Muse are text on the screen.

The board has a microphone and no speaker, so it runs as a microphone-only
gadget: Settings > Sound shows the microphone gain and level, and replies are
always read on screen. The board's LSM6DS3 IMU, MicroSD slot and battery input
aren't used yet, and OTA is off for this profile: update it over USB.

Checked on one unit: boot, BLE pairing with USR1, Wi-Fi, push-to-talk with
replies on screen, touch settings, USR2 sleep and wake, and power off from
Settings with USR1 to wake. Recognition is
best within about 20 cm in a quiet room.

## Pins

Pins come from the [Seeed board guide](https://wiki.seeedstudio.com/getting_started_1.47_inch_touch_display_esp32s3/),
the [XIAO ESP32-S3 Plus pin map](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
and [Seeed_GFX2](https://github.com/Seeed-Studio/Seeed_GFX2) (the JD9853A
register sequence, the AXS5106L reports and the `0519_DashBoard_147_ESP32`
PDM example). The adapted panel sequence keeps Seeed's
[MIT notice](licenses/Seeed_GFX2.txt).

| Function | XIAO pin | GPIO |
|---|---|---|
| PDM clock / data | D0 / D1 | 1 / 2 |
| LCD CS / DC | D2 / D3 | 3 / 4 |
| LCD SCK / MOSI | D8 / D10 | 7 / 9 |
| LCD and touch reset / backlight | D17 / D18 | 13 / 12 |
| Touch SDA / SCL / INT | D4 / D5 / D7 | 5 / 6 / 44 |
| USR1 talk / USR2 screen | D19 / D15 | 11 / 42 |
| SD chip select, held high | D6 | 43 |

The JD9853A runs on the ST7789-compatible driver with the vendor's register
sequence: MADCTL `0x48` (MX+BGR), X offset 34, Y offset 0, inversion off.
Touch X is mirrored to match. The LCD and touch share one reset line, pulsed
once at boot before either starts.

The PDM microphone is on I2S0, which is the S3's only PDM receiver, at 16 kHz.
The board removes the PDM filter's DC offset; Muse applies the gain set in
Settings > Sound (30 dB by default).
The console is the chip's own USB, since UART0's GPIO43/44 carry SD CS and the
touch interrupt.

## Build and flash

Use ESP-IDF v6.0.1. From `esp32/`:

```sh
tools/muse/board.sh build xiao147
idf.py -B build-muse-seeed-xiao-s3-147 menuconfig   # ESP32 Device SDK > Muse Gadgets SDK token
tools/muse/board.sh build xiao147
tools/muse/board.sh flash xiao147
```

With several native-USB boards attached, pass the board's USB serial number
(its MAC) or its port to `flash`.

## Pair and use

1. In the Muse app, turn on **Settings > Devices > Developer mode**, tap
   **Add Device** and pick the `MuseGadget-XXXXXX` the screen shows.
2. Press **USR1** to confirm pairing, then choose a 2.4 GHz Wi-Fi network.
3. Hold **USR1** while you speak and release it to send. The reply appears on
   the screen.
4. Press **USR2** to turn the screen off, and again to wake it.
5. Swipe left on the face for Settings; swipe up and down to scroll.

On the 172 px wide screen, settings rows show each name and its value on
separate lines.
