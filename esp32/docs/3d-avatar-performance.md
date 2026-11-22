# 1.75C avatar performance investigation

Measured on the physical Waveshare 1.75C on 2026-11-22, using the
main-based 3D-style renderer and the bench overlay. Screenshot transfers
were not active during these measurements. Timing callbacks surround
LVGL's render event; these measure UI render completion, not optical
panel presentation or end-to-end voice latency.

## Initial measurements

| Scenario | Completed UI renders / second | Render wall time |
|---|---:|---:|
| Idle, steady state | about 14.9 | about 65 ms average |
| Thinking, animated progress ring | about 12.0–12.3 | about 79–81 ms average |
| Speaking pose, without live speech | about 14.8 | about 65 ms average |
| State transition windows | about 12–14 | individual renders up to 132–135 ms |
| Local MP3 decode/playback startup | about 13.7 in the affected window | one render reached 224 ms |

The requested avatar tick interval is 40 ms (25 Hz). An ordinary 65 ms
render already exceeds that budget. The built-in MP3 sample decoded in
165 ms; after startup the steady speaking pose returned to about 65 ms
per render. This local sample is not a network voice round trip.

## Repeated physical voice note

The user repeated a voice note during the timing capture. The device recorded
14.24 seconds and reported RMS -38.4 dBFS, one clipped sample and a 607,790-byte
request. No screenshot transfers ran during the note.

| Phase | Completed UI renders / second | Render wall time |
|---|---:|---:|
| Recording, steady windows | about 9.1–9.5 | about 102–107 ms average |
| Waiting/thinking | about 11.6–11.9 | about 82–84 ms average |
| Spoken local waiting notice | about 11.2–13.9 | about 70–87 ms average |
| Idle afterward | about 14.7–14.9 | about 65 ms average |

The stream acknowledged the request 1,041 ms after release. No reply arrived
within 20 seconds, so the firmware generated its local waiting notice. The
logged first audio at about 19.98 seconds was that notice, not an actual
remote answer. The logs do not isolate the reason for the remote reply wait;
they do not establish that the avatar caused it. The timing capture ended
without a subsequent remote answer in its recorded log.

The heartbeat remained online. After the note, free internal RAM was about
86 KB (largest block 62 KB), free PSRAM about 2,992 KB, and the recorded
minimum internal heap remained about 15 KB. These observations support a
rendering/scheduling bottleneck rather than a demonstrated memory exhaustion.

## Causes supported by the implementation

- Every tick invalidates the entire 320x320 avatar, including ticks when
  its integer position has not changed. At roughly 15 Hz that is about
  1.5 million avatar output pixels processed per second.
- Every output pixel is bilinearly sampled from 160x160 RGB565 artwork.
  During the 200 ms crossfade it samples both the old and new image and
  blends them, increasing the work during exactly the moment a state
  change needs to feel responsive.
- The scaling counters report roughly 2.75–2.83 million microseconds of
  summed decoder work per two-second steady-state window. Strip decoders
  can run concurrently, so this is summed work across workers, not elapsed
  time or a single-core CPU percentage. It identifies resampling as a
  substantial cost; it does not isolate display transfer or scheduler time.
- Movement is rounded to integer source pixels with `lroundf`. Each source
  pixel becomes about two screen pixels. This creates visible jumps even
  if the UI could sustain its 25 Hz target.
- Seven still images supply the character states. Crossfading between them
  cannot reproduce the independent eyes, mouth and limb motion of the
  procedural pixel renderer. At 15 Hz a 200 ms dissolve has only about
  three frame intervals, so it can look both slow and chunky.

## Recommended optimization order

1. Precompute or cache scaled artwork so strip decoding can mostly copy
   pixels, spending PSRAM or flash to reduce repeated CPU work. Measure
   the memory tradeoff alongside networking and audio.
2. Keep motion coordinates fractional through sampling and redraw only
   when visible content changes. Preserve correct invalidation for smooth
   artwork rather than reusing the pixel renderer's coarse cell sampling.
3. Shorten or remove dissolves for immediate states such as listening and
   speaking; use pose animations for expressive transitions later.
4. Add actual blink, mouth and gesture frames or separately animated
   features. This addresses expressive motion, which faster rendering
   alone cannot supply.

These are baseline findings, not a claim that optimization has already
been implemented. A paired 2D timing run has not been performed, so no
measured speedup ratio versus the 2D renderer is claimed.
