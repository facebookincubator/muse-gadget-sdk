# Timer device captures

Framebuffer captures from a physical Waveshare ESP32-S3-Touch-AMOLED-1.75C,
using `tools/muse/snap.py`. The display is 466 × 466 pixels; the capture tool
scales each image 3×. These are device screenshots, not device photos or
simulator renders.

- `picker.png`: scrolling duration wheels and Start.
- `countdown.png`: fixed-width digit cells and the breathing yellow ring.
- `finished.png`: centered alert and red ring; tapping restores the duration.

The countdown and finished captures used a temporary bench build that seeds
a duration at startup; the UI and input handlers are the Timer example's.
The final installed app starts on the picker. No bench hooks are included in
the example.
