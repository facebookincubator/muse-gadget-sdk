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

## Cached poses on the 1.75C

The final bench run used the same physical board, two LVGL draw workers and
40 ms UI tick as the baseline. Timing and framebuffer snapshots were taken
in separate passes. An LVGL refresh measures the UI work for the entire
screen, including the meter, ring, captions and avatar.

| Physical device scenario | Previous 3D build, average refresh | Cached build, average refresh |
|---|---:|---:|
| Recording a real voice note, steady | 102–107 ms | 4.6–5.5 ms |
| Thinking after sending the note, steady | 82–84 ms | 5.7–6.4 ms |
| Speaking pose, bench command with no playback | 65 ms | 1.1–1.2 ms |
| Remote reply audio playing, steady | No equivalent baseline capture | 2.0–2.6 ms |

The new note was 13.18 seconds long. Each steady recording window completed
about 25 refreshes per second and had **zero avatar invalidations**. The ring
and meter still transferred roughly 264,000–296,000 panel pixels per two-second
window. Steady thinking also sustained about 25 refreshes per second. For
speaking without playback, seven mouth updates in 2.24 seconds each requested
27 × 19 pixels. Panel alignment brought the average transfer to 560 pixels per
update. The full base contains 102,400 pixels.

The service returned one reply. The firmware logged the first reply audio
after 13.31 seconds and completed 13.81 seconds of audio. This interval
includes the remote turn. The UI timings in the table measure local draws.
The local MP3 self-test separately decoded 3.67 seconds of audio in 166 ms.

### Entering a state

In the first implementation, thinking and error entry took about 410 ms.
The tuned implementation uses native-resolution run/literal packets and places
the cached pose directly in its reply layout. Preparing a different base uses
bounded slices. The last complete base stays available until the replacement
is ready. Changing an expression selects an immutable patch over that base.
LVGL drains both draw workers before returning to the UI timers, so the same
base and patch remain readable for every strip of one refresh. The clock
waits for the final submitted band to complete before arming its next hold.

Across 20 controlled state changes, the interval from the UI observing the
new state to the UI observing the completed panel transfer was 97–157 ms.
Median was 139 ms and nearest-rank p95 was 156 ms. The UI polls completion,
so this includes up to a scheduling interval after the driver's notification.
On the actual note, listening, thinking and speaking entry measured 152,
140 and 161 ms respectively. The 150 ms entry target was exceeded on some
transitions. The recording and thinking refresh targets were met. No optical
latency, battery savings or paired 2D baseline were measured.

### Memory and build sizes

The two base caches occupy 409,600 bytes of PSRAM. Two prepared patch sets
use another 38,480 bytes and the shared preparation scratch uses 10,240 bytes.
Total allocated renderer PSRAM is **458,320 bytes**. The asset blob in flash
is 606,508 bytes. This trades some OTA headroom for faster preparation of
native-resolution images. The bench and standard signed images each used
2,822,144 bytes in a 4,194,304-byte slot, leaving 1,372,160 bytes.
The partition table and device settings are independent of the renderer.

During the physical note, heartbeat samples reported about 87 KB free internal
RAM, 79–80 KB DMA-capable RAM and a minimum internal heap of 15 KB. Sampled
free PSRAM was about 1,849–1,881 KB during the turn. The build also passed
the on-device sleep/wake and local MP3 checks. The pose controller, clipped
pixel copies, exact reference images, allocation failures, corrupted
preparation and cancelled pose preparation have host regression coverage.

### Live call

The user placed a call on a temporary call-enabled build with the same cached
renderer. The service returned HTTP 200 after 876 ms and the call became live
after 1.56 s. It played 1,172 audio frames. The firmware counted zero dropped
frames and zero bad audio frames. Working appeared during an actual call, with
zero autonomous avatar invalidations in steady windows. The ring continued
at about 25 refreshes per second. The first end-call request timed out and
the subsequent connection confirmed the call had ended.

The user observed delayed reply audio and Working remaining visible after
Muse had sent a response. The board logged a completed response, presented
Working 1.42 s later, then presented Speaking another **12.65 s** later.
Preparing and presenting that Speaking update took 165 ms from the UI
observing the state change. During the intervening Working windows, average
LVGL refreshes took about 7–8 ms. The hands-free call code chooses Working
when the session reports outstanding work and its local speaker gate is
open for microphone input. The capture lacks per-response audio arrival,
queue depth and first-speaker-write timestamps. It cannot distinguish
remote audio generation, transport delay, buffered playback or a stale
work indication. Live-call A/V synchronization remains unresolved.

The original voice-note bench image was restored after the call. Esptool
verified its hash. The board booted to its 466 × 466 UI, completed the
audio self-test and retained pairing, speaker volume and microphone gain.
