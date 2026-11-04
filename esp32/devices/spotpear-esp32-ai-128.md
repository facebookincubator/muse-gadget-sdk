# SpotPear ESP32-AI-1.28 v1.0 (experimental)

This profile targets the old, non-legged ESP32-S3 board with a MAX98357A
speaker amplifier and NP0557 microphone. It does not target the ES8311
`sp-esp32-s3-1.28-box` revision.

The pin map and panel setup come from the inspected
[reference firmware](https://github.com/tomorrow56/xiaozhi-esp32/tree/fed7f14669fddecc7a658c806ab072a972a2af97/main/boards/spotpear/spotpear-esp32-ai-1.28).
Muse validation is tracked separately below.

| Peripheral | Configuration |
| --- | --- |
| Memory | 16 MB DIO flash, 8 MB octal PSRAM at 80 MHz |
| USB console | Native USB Serial/JTAG (303a:1001); identify the actual unit before flashing |
| GC9A01 | 240×240, SPI3, mode 0, 40 MHz, BGR, inversion on, mirror X only |
| LCD pins | DC10 / CS13 / CLK14 / MOSI17 / RESET18 |
| Backlight | GPIO3, active high, PWM |
| BOOT | GPIO0, active low; pairing confirmation and hold-to-record voice notes |
| Speaker | I2S0 TX, BCLK15 / WS16 / DATA7, 24 kHz, 32-bit left slot |
| Microphone | I2S1 RX, BCLK5 / WS4 / DATA6, 16 kHz, 32-bit left slot; L/R grounded |

No touch, MCLK, audio-control I2C, battery gauge, or software power latch is
configured. There is no auxiliary menu button. Use BLE setup and the existing
serial settings interface. This port uses Muse's avatar, not the Xiaozhi
StackChan rendering code.

Muse's audio API uses 16 kHz stereo PCM. The board adapter reads the left
microphone slot and supplies a silent right slot, and converts speaker PCM
to 24 kHz with streaming linear interpolation before writing 32-bit samples.
Volume uses esp_codec_dev software volume; microphone gain is applied after
DC removal with saturation. Interpolation preserves timing but is not a
high-quality reconstruction filter; listening tests remain necessary.
Audio close/open stops and restarts the two I2S controllers independently.

## Build

Use ESP-IDF **v6.0.1**, as required by this repository, separately from the
reference Xiaozhi project's v6.1 installation.

```sh
tools/muse/board.sh build spotpear
```

Store the SDK token in
`build-muse-spotpear-esp32-ai-128/sdkconfig`, never in the board overlay.
Firmware binaries containing the personal token are private artifacts.

## Hardware validation

As of 2026-10-30, boot, PSRAM, UI/audio initialization, iOS pairing and
Wi-Fi setup, microphone voice-note capture, audible local speaker loopback,
and readable Japanese replies have been verified on one unit. The upstream-based submission is built separately; its software validation is
reported in the pull request. It has not been reflashed for this submission.
Video, image rendering, OTA, repeated sleep/wake cycles and long-running
use remain unverified.

Switching from Xiaozhi changes the partition layout: do not flash the Muse
application at Xiaozhi's application offset. Preserve the private full-flash
backup, then use the Muse build's bootloader, partition table and flash_args
when switching firmware. Do not publish full-flash dumps, NVS or SDK tokens.

## Voice-note controls

Hold BOOT while speaking, then release it to send a voice note. Recording
also stops at the normal 15-second limit. Replies appear as Japanese captions;
no TTS is included. Live calls remain disabled in this profile.

Local speaker loopback was audible on 2026-10-30 at volume 70. Standard
voice-note replies intentionally use silent PCM for caption timing, so the
SPEAKING label alone does not indicate generated speech.

## Japanese captions

The profile enables CONFIG_MUSE_CJK_FONT with the bundled 16 px GNU Unifont
fallback. It includes Japanese punctuation, hiragana, katakana and BMP CJK
ideographs. The optional Noto Sans SC subset is left off to avoid its reduced
kanji coverage. On this 240 x 240 screen the SDK switches Japanese captions
to its larger CJK caption layout. Readable Japanese
voice-note replies were verified on the physical screen. Following the Waveshare LCD128
layout, captions sit near the bottom, 34 px above the screen edge, in a
160 px wide band. The avatar is reduced to 128 px and moved up by 25 px,
leaving a 5 px gap above the two-line Japanese band. Paging uses the narrower
band width so text clears the curved edges. Standard replies remain text.
