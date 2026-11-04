# ALIENTEK ATK-DNESP32S3-BOX2

This profile runs Muse's avatar, push-to-talk, live voice calls, a two-key
menu, BLE setup, Wi-Fi and the home-network tunnel on the
**ATK-DNESP32S3-BOX2** (Wi-Fi version). It is not the profile for the V1.1
box ([setup](atk-dnesp32s3-box.md)), the BOX0 or the BOX3, which use other
pins.

The port is experimental: it runs on one unit, and the checklist at the end
says what has and hasn't been checked there.

The box has an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM, a 240×320
ST7789 LCD on an 8-bit i80 bus with a PWM backlight (GPIO21), an ES8389 codec
for the speaker and mic, a battery read through a divider on GPIO1, an SC7A20
accelerometer, and a 16-bit XL9555 I/O expander (0x20) that holds the power
latch, the speaker amplifier, the analog and 4G rails, the charger lines and
three of the keys. ALIENTEK publishes no BSP for it; the pins, the panel's
tuning table, the expander's lines and the battery table follow xiaozhi-esp32's
[atk-dnesp32s3-box2-wifi board](https://github.com/78/xiaozhi-esp32/tree/main/main/boards/alientek/atk-dnesp32s3-box2-wifi).
The accelerometer (I2C 0x19, WHO_AM_I 0x11) and key Q's role were found on a
unit; xiaozhi doesn't use either.

## Build and flash

After activating ESP-IDF v6.0.3, set your SDK token in the build directory's
`sdkconfig` (see [`../AGENTS.md`](../AGENTS.md)), then:

```sh
tools/muse/board.sh build atk-box2
tools/muse/board.sh flash atk-box2
```

The tools expect the chip's own USB Serial/JTAG (`303a:1001`) on the USB-C
port. To keep the factory firmware, back up the whole flash first and store it
outside version control:

```sh
python -m esptool --chip esp32s3 -p PORT read-flash 0 0x1000000 atk-box2-factory.bin
```

### Options

The overlay keeps the avatar's and audio's static buffers (about 18 KB) in
PSRAM (`CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM`), for the internal RAM Link and
Muse's sessions need. Live voice calls (`CONFIG_MUSE_REALTIME_VOICE=y`, see
"Live voice calls" in [`../AGENTS.md`](../AGENTS.md)) are off unless the
build's `sdkconfig` (or `idf.py -B build-muse-atk-dnesp32s3-box2 menuconfig`)
turns them on.

## Controls

- M (XL9555 P0.7, active high): talk. A tap (under 300 ms) starts or ends a
  live call, or only wakes the screen; held longer it records a voice note
  from the start of the press. Also pairing confirmation, and
  select in the menu.
- R (BOOT, GPIO0): opens the menu and steps down it. Held at reset it enters
  the ROM bootloader, as on any S3.
- L (XL9555 P0.5): turns the speaker on and off.
- Q (XL9555 P0.6, listed by xiaozhi but unused there): power. A tap turns
  the screen off or on, a 2 s hold powers off. Its resting level is read at
  boot, so either polarity works.

## Screen

- Landscape, 320×240, the way the box sits on a desk. Turned over, the
  picture turns with it within a second: the accelerometer is read twice a
  second, and a turn counts after two readings. Lying flat or on a side keeps
  it as it is.
- The key icons sit along the top edge, M in the middle and R on the right;
  they don't move when the picture turns over.
- Captions show Chinese, Japanese and Korean (`CONFIG_MUSE_CJK_FONT`).
- LVGL draws through two 8-line strips in internal RAM (`DRAW_BUF_LINES`,
  10 KB). Larger strips leave Link and Muse's sessions too little internal RAM:
  with 32 lines Muse couldn't connect (`int=12K/4K` in the heartbeat).

## Power and limits

- Power off, on battery, drives CHG_CTRL low and opens the power latch
  (XL9555 P1.2), as xiaozhi does. On USB power the chip keeps running, so it
  falls back to deep sleep with the backlight and amplifier off; R, or M or Q
  through the expander's interrupt on GPIO2, wakes it.
- The battery percentage comes from xiaozhi's table of raw ADC counts; no
  voltage is reported. Charging is the charger's CHRG line (P1.7). While
  charging, the reading is raised by the charge current.
- Up to eight Wi-Fi networks are kept; the box joins whichever saved one is in
  range. Add one in the Muse app, or with the `wifi.ssid`, `wifi.pass` and
  `wifi.connect` console commands.
- Not integrated: the TF card slot, the USB-A host port (VBUS_EN stays off)
  and the BOX2-4G's modem.

## Hardware verification

Checked on one unit, powered over USB:

- Boot reports the board name and 8 MB PSRAM, with no panic or reset loop.
- BLE pairing with the Muse app (with an SDK token in the build), Wi-Fi and
  the Muse session. With 16-line draw strips the heartbeat read
  `int=34K/23K min=19K` once Muse connected.
- The speaker plays the built-in MP3 (console key `m`) and the
  `>test.loopback` tones; the mic records voice notes that reach Muse.
- Keys M, R, L and Q as above.
- The picture turns over with the box.

Still to check:

1. Where the key icons should sit.
2. Internal RAM with the 8-line strips, through a live call and a voice note
   (`min=` in the heartbeat should stay above about 8K).
3. A boot on battery: the board stays on after the power key is released.
4. Playback after the audio has rested on battery (asleep, unplugged) and
   reopened: xiaozhi keeps the ES8389's output open for fear that reopening
   the duplex bus leaves it silent on IDF 6. On USB the audio never rests.
5. The menu: volume changes and is kept across a restart.
6. Battery: the percentage and the charging state.
7. Power off on battery, and which key turns it back on; on USB, deep sleep
   and waking on R, M and Q.
