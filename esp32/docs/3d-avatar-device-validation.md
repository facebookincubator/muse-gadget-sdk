# 3D-style Muse: Waveshare 1.75C validation

Validated on a physical Waveshare ESP32-S3-Touch-AMOLED-1.75C over USB on
2026-11-22. The avatar branch is based on main at `ded4222`; the flashed
renderer revision is `ca5f3c5`. Unrelated motion and reply edits were excluded.

## Build and flash

- ESP-IDF v6.0.3, ESP32-S3, Muse + Waveshare 1.75C + bench overlays.
- Signed application: 2,560,000 bytes in a 4,194,304-byte OTA slot; 39% free.
- Flash write verified by esptool. A local backup covers bootloader,
  partition table, settings, both OTA slots and product data. It contains
  private device data and is not committed.
- Pairing, Wi-Fi and Link connectivity survived flashing and a subsequent
  reset. Boot reached the UI and audio initialization without a captured
  panic, assertion failure or reboot loop.

## Physical-device checks

- Captured 466x466 screen images for idle, boot, listening, thinking,
  speaking, error, off and happy through a single persistent USB session.
  Visually verified the six distinct non-idle state images in a contact sheet.
  These are framebuffer screenshots from the device, not photographs.
- Microphone self-test reported left/right capture at approximately
  -58.0/-58.5 dBFS and capture/playback rates of 16,123/16,161 Hz against a
  nominal 16,000 Hz. This establishes codec/capture activity, not acoustic
  quality or speech-recognition accuracy.
- Requested built-in MP3 playback. Decoder produced 58,752 samples
  (3.67 seconds) in 161 ms; firmware returned to its normal loop with no
  captured audio error. Audible quality was not assessed remotely.
- Serial screen sleep and wake both logged successfully; a subsequent
  screenshot showed the idle UI and Link remained online.
- Heartbeat after playback: approximately 87 KB internal RAM free,
  62 KB largest internal block, 79 KB DMA-capable RAM free, and 2,992 KB
  PSRAM free. The recorded minimum internal heap was approximately 15 KB.

## Limits

- Bench mode enables framebuffer screenshots and temporarily allocates a
  full-screen image. Screenshot transfers interrupt ordinary animation
  timing and are unsuitable for measuring frame rate.
- No measured frame-rate, battery-runtime or power-consumption claim.
  Physical touch interaction and end-to-end spoken conversations were not
  exercised in this session.
- Artwork remains seven still images with procedural movement and fades;
  facial animation and independent limb motion are not implemented.
- The board was left awake in idle, paired and online, running the bench
  firmware. Its build configuration and SDK token remain local.
