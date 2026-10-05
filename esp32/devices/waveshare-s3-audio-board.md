# Waveshare ESP32-S3-AUDIO-Board

This profile runs without an external display. The seven-LED ring shows
pairing, connection and recording status. Hold BOOT, speak, then release;
Muse replies as text in the phone app. Spoken replies require a separate
TTS integration. There is no wake-word detector in this profile.

## Hardware

ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, ES8311 playback codec,
ES7210 microphone ADC and TCA9555 IO expander. Use the board's USB-C data
port (ESP32 native USB Serial/JTAG, `303a:1001`) and 2.4 GHz Wi-Fi.
No display, camera, SD card or battery is required.

| Signal | GPIO / expander pin |
|---|---|
| I2C SCL / SDA | 10 / 11 |
| I2S MCLK / BCLK / WS | 12 / 13 / 14 |
| I2S microphone DIN / speaker DOUT | 15 / 16 |
| Seven WS2812 LEDs | 38 |
| BOOT, active low | 0 |
| Amplifier enable, active high | TCA9555 EXIO8 |
| K1 / K2 / K3, active low | TCA9555 EXIO9 / EXIO10 / EXIO11 |

The ES7210 exposes four PCM16 channels packed into two 32-bit I2S slots
at 16 kHz. The vendor calls this RMNM: amplifier reference, first mic,
unused, second mic. This profile selects the first microphone for Muse;
it does not implement echo cancellation or combine the two microphones.
Both codecs share the ESP32's clocks. Playback converts the shared player's
48 kHz stereo PCM32 to the board's 16 kHz bus; the clock rate stays fixed.
The amplifier stays off until playback, and microphone capture starts only
on a talk press.

References: [Waveshare product docs](https://docs.waveshare.com/ESP32-S3-AUDIO-Board),
[official demo and schematic](https://docs.waveshare.com/ESP32-S3-AUDIO-Board/Resources-And-Documents).
Pin assignments and PCM layout follow schematic v1.1 and the official demo's
`ESP-IDF/factory_01/main/hardeware_driver/bsp_board.[ch]`.
Codec register programming uses Espressif's `esp_codec_dev` component.
The vendor also lists an `-EN` SKU; that variant has not been separately verified.

## Build and pair

Use ESP-IDF v6.0.1. From `esp32/`:

```sh
tools/board.sh waveshare-s3-audio-board build
idf.py -B build-waveshare-s3-audio-board menuconfig
```

Set your [SDK token](https://gadgets.muse.ai/settings/sdk-tokens) under
ESP32 Device SDK, then rebuild and flash:

```sh
tools/board.sh waveshare-s3-audio-board build
tools/board.sh waveshare-s3-audio-board flash /dev/cu.usbmodemXXXX
```

Use the actual serial port (`/dev/ttyACM0` is typical on Linux). Identify the
board and back up its existing firmware before the first flash; see
[flashing instructions](../AGENTS.md#flash). Flashing replaces the vendor
firmware. Keep backups and credentials outside version control.

Enable Developer mode under Settings > Devices in the Muse app, add
`MuseGadget-audio-XXXXXX`, press BOOT to confirm pairing and finish Wi-Fi
setup. Hold BOOT to record, then release to send. Check the corresponding
question and reply in the phone app; a green status light alone does not
prove a voice turn succeeded.

## Controls and limitations

- BOOT: pairing confirmation during setup; push-to-talk once Muse is ready.
- Hold K1: momentary software microphone mute. While K1 is held, BOOT keeps
  its setup role; holding BOOT for five seconds forgets pairing and Wi-Fi.
  K1 is not a hardware privacy switch.
- K2 / K3: decrease / increase speaker volume in five-percent steps; the ring
  shows volume briefly and the setting persists across restarts.
- RESET: restart. Holding BOOT while resetting enters the ROM bootloader.
- No on-device text display, TTS provider, wake word, AEC, battery reporting,
  SD card, camera or external-display support. OTA is off by default.

## Verification

Built with ESP-IDF v6.0.1, with 28% free in the 2 MB app partition.
The complete host suite passes (187 tests). Regression builds pass for the
C5 DevKitC-1, ideaspark, SenseCAP Indicator, Voice PE and reSpeaker Lite.
Host tests exercise microphone vs. reference-channel selection, full-scale
PCM, stereo isolation, overflow in playback conversion, capture start/stop,
fail-closed mute, amplifier control, volume limits, button debounce,
short/failed I2S writes and initialization failure cleanup.

Verified on hardware: ESP32-S3 with 16 MB flash and 8 MB PSRAM, boot,
seven-LED ring initialization, ES8311/ES7210 initialization, voice task start
and BLE advertising as `MuseGadget-audio-XXXXXX`. A local bench using this
same driver captured 80,000 PCM16 samples in five seconds and completed
microphone replay plus three one-second speaker test transfers without I2S
errors.
On 2026-10-05 the operator confirmed phone pairing and completed voice
conversations. A Muse app screenshot shows two transcribed voice notes
and their text replies; a live serial capture confirms Wi-Fi, the control
session and tunnel are up. This verifies the BOOT push-to-talk flow and usable
microphone input. Perceived recording loudness has not been measured.
A software restart retained pairing and restored Wi-Fi, the control session
and tunnel by the nine-second heartbeat, with no panic or error during the
40-second capture. Audible board playback, K1/K2/K3 controls and clearing
setup are not yet verified.
