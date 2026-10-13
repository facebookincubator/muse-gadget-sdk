# ALIENTEK ATK-DNESP32S3-BOX

This profile runs Muse's avatar, push-to-talk, a two-key menu, images, BLE
setup, Wi-Fi and the home-network tunnel on the **ATK-DNESP32S3-BOX V1.1**
(the silkscreen on the back of the board says V1.1). It is not a profile for
ALIENTEK's BOX0, BOX2 or BOX3, which use other pins.

The box has an ATK-MWS3S N16R8 module (16 MB flash, 8 MB octal PSRAM), a
320×240 ST7789 LCD on an 8-bit i80 bus, one ES8311 codec for the speaker and
its single mic, and an XL9555 I/O expander for the backlight, the speaker
amplifier and two of the keys. ALIENTEK publishes no BSP for it; the pins
follow xiaozhi-esp32's
[atk-dnesp32s3-box board](https://github.com/78/xiaozhi-esp32/tree/main/main/boards/alientek/atk-dnesp32s3-box),
and K1 and K2 were found on a unit.

## Build and flash

After activating ESP-IDF v6.0.1, set your SDK token in the build directory's
`sdkconfig` (see [`../AGENTS.md`](../AGENTS.md)), then:

```sh
tools/muse/board.sh build atk-box
tools/muse/board.sh flash atk-box
```

Use the USB-C port marked USB-SLAVE: it is the chip's own USB Serial/JTAG
(`303a:1001`). The port marked UART is a CH343 bridge to the console UART.
To keep the factory firmware, back up the whole flash first and store it
outside version control:

```sh
python -m esptool --chip esp32s3 -p PORT read-flash 0 0x1000000 atk-box-factory.bin
```

## Controls and limits

The keys run along the top edge: K2, K1, K0 from the left.

- K0 (BOOT, GPIO0): push-to-talk, pairing confirmation and wake from power
  off. Held at reset it enters the ROM bootloader, as on any S3.
- K1 (XL9555 P0.4): opens the menu and steps down it; K0 selects.
- K2 (XL9555 P0.3): unused.
- The backlight is a switch on the expander (P0.7), not PWM: brightness is on
  or off.
- Power off is deep sleep with the backlight and amplifier off; K0 or RESET
  wakes it. USB power can't be cut.
- Captions show Chinese and Japanese (`CONFIG_MUSE_CJK_FONT`, on in this
  profile).
- Not integrated: the buzzer, the TF card slot and the USB-A host port. There
  is no battery reading.

## Hardware verification

Checked on a V1.1 unit with this profile:

1. Boot reports the board name and 8 MB PSRAM, with no panic or reset loop.
2. The display has correct colours and orientation; the key hints sit by the
   keys, on the face and on the menu's bottom bar.
3. BLE pairing with the Muse app, K0 confirmation, Wi-Fi and the Muse session.
4. Push-to-talk: voice notes are transcribed and replies show as captions.
   The mic is on the left slot. The speaker was checked with the
   `>test.loopback` console command: its tones reach the mic.
5. The menu: volume changes and is kept across a restart.
6. Power off, and K0 waking it; auto-sleep and waking on a key.

Not yet checked: images from Muse and the mic-gain setting.
