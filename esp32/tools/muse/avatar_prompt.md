You are writing muse_pixel.c for me: the pixel-art avatar on the board's screen, the
small voice assistants that talk to you. Right now they show the default
avatar. Replace it with yourself: the avatar you have in Muse.
The current muse_pixel.c follows this prompt. Start from it and keep all of
its code that isn't about how the default avatar looks.

YOUR AVATAR
- Work from your own avatar as set in Muse: its picture or description, in
  your profile, your persona or your files. Match its silhouette and colours
  first, details second.
- If your avatar has its own animation states (idle, listening, thinking,
  talking, and so on), carry their poses and motion over to the matching
  modes below.
- Open the file with a comment block, below the copyright line, that
  describes the avatar you drew: its name, shape, colours and personality.
  I use it to check that you drew the right character.
- If you can't find your avatar, reply with one line,
  "NO AVATAR: <what you looked for>", and no code.

OUTPUT
One complete C file in a single ```c fenced block, and nothing after it.
Keep the "// Copyright (c) Meta Platforms, Inc. and affiliates." line at
the top. The file must compile without warnings with `cc -O2 -Wall` on a
desktop and with ESP-IDF's GCC. Include only <math.h>, <stdbool.h>,
<stdint.h>, <stdlib.h>, <string.h> and "muse_pixel.h".

HARDWARE
ESP32-S3 at 240 MHz renders at 25 fps (40 ms per frame). ESP32-C6 at 160 MHz
has no FPU and renders at 20 fps (50 ms per frame).

API (muse_pixel.h, unchanged; implement exactly these)
  #define MUSE_PX_W 64
  #define MUSE_PX_H 64
  typedef enum { MUSE_MODE_BOOT, MUSE_MODE_IDLE, MUSE_MODE_LISTENING,
                 MUSE_MODE_THINKING, MUSE_MODE_SPEAKING, MUSE_MODE_ERROR,
                 MUSE_MODE_OFF, MUSE_MODE_COUNT } muse_mode_t;  (muse_state.h)
  typedef struct {
      muse_mode_t mode;
      float t;        // seconds since boot
      float mode_t;   // seconds in the current mode
      float level;    // 0..1 live audio level (mic when listening, voice when speaking)
      float happy;    // 0..1 pet reaction; rises to 1, eases out over ~1.6 s
      float working;  // 0 or 1, only in THINKING: Muse is busy on a call's work
  } muse_pose_t;
  uint32_t muse_pixel_accent(muse_mode_t mode);    // 0xRRGGBB accent for the UI around the avatar
  uint32_t muse_pixel_working_accent(void);       // optional: accent while pose->working; 0 keeps THINKING's
  void muse_pixel_render(const muse_pose_t *pose); // draw one frame into the 64x64 grid
  void muse_pixel_set_size(int px);                // screen size the grid is scaled to, capped at 512
  void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
      // write screen pixels [x0..x1] x [y0..y1] of the scaled frame as RGB565,
      // stride_px apart; called per display strip, so never build the full image

KEEP FROM THE ORIGINAL (reuse its code verbatim where it fits)
- Framebuffer: uint8_t palette indices, 64x64, black background (index 0 is
  0x000000; the round screen's bezel is black).
- Palette: an enum of colour roles, at most 32 entries (the default has 29, the
  background included): outline, dark/mid/light/highlight body tones, face
  tones, eye, shine, blush, mouth, tongue, a 4-step per-mode glow ramp,
  aura x2, sparkle, accent, shadow, heart, white. Fixed avatar colours go in
  one table. A per-mode scheme table (glow ramp and accent) blends toward
  the current mode with 1 - expf(-dt * 7). Precompute RGB565 and a 0.72x
  "dim" copy of every entry once per frame.
- Time: state that lasts across frames (palette blend, blink and gaze
  timers, sparkle phases) lives in statics. Each frame's dt is the change
  in pose->t, clamped to 0..0.2 s, with 0.04 s on the first frame.
- muse_pixel_set_size / muse_pixel_scale exactly as in the original: a
  screen-to-cell map (at most 512 entries) with the high bit marking a
  cell's last pixel, the dim palette on those last pixels when cells are 3+
  pixels (the faint pixel grid), and memcpy of repeated rows.
- Look: 4x4 Bayer ordered dithering for shading and soft edges; a hard 1 px
  outline around the silhouette (4-neighbour test on a part mask) plus
  seams where limbs overlap the body; a top-left light; a dithered floor
  shadow; a state-tinted rim light on the lit edge; tiny '.#o' stamped
  bitmaps for eyes, mouths, hearts and the alert mark.
- Size and place: the character fills about 32-36 px wide by 46-48 px tall,
  centred at x = 32, feet near y = 56.5, leaving room around it for the
  aura, rings and sparkles.

PERFORMANCE (hard limits)
- Per-pixel work in integer fixed point (Q12, ONE = 1 << 12). No float, pow,
  sqrt, sin or cos inside per-pixel loops; use lookup tables built once
  (the original has |u|^2.7, |u|^3.6 and sqrt tables). Floats are fine per
  frame and per part.
- Bound the shading loops to the character's bounding box.
- Target: under 10 ms per frame on the ESP32-S3 and under 40 ms on the C6.
  Roughly: at most a few dozen integer ops per covered pixel.
- Static memory only: the framebuffer, a same-size part mask, tables. No
  malloc.

ANIMATION BEATS (every one of these, adapted to your body)
- All modes: gentle breathing (body width and height +-3%); random blinks
  every 2.2-5.2 s with occasional double blinks (~0.16 s each); gaze that
  wanders to random targets every 1.2-3.6 s and eases there
  (1 - expf(-dt * 14)); sparkles orbiting behind and in front of the body;
  a soft dithered aura in the mode's colours.
- BOOT: pops up from a squash (0.6 s), then opens its eyes at ~0.9 s;
  sparkles appear one by one.
- IDLE: slow bob; arms, wings or paws sway.
- LISTENING: wide eyes, small "o" mouth, raised brows, hands up beside the
  face like cupping an ear, expanding dotted rings and sound waves that
  grow with `level`, gaze fixed forward.
- THINKING: eyes glance up and side to side, "hmm" mouth, one paw to the
  chin, gentle lean, three thought dots stepping up beside the head, faster
  sparkles.
- THINKING with working = 1 (a live call where Muse is off doing what it was
  asked, with nothing to say yet): busy rather than puzzled, e.g. both paws
  in front tapping on a little keyboard, eyes down on the work, a small
  spinner beside the head, a warmer scheme. With working = 0 it's plain
  THINKING.
- SPEAKING: mouth opens with `level` (plus a tiny flutter so it never
  freezes), body bobs with the voice, arms gesture, feet shuffle, rings and
  waves, extra blush.
- ERROR: X eyes, flat mouth, quick side-to-side shake for the first 0.6 s,
  "!" beside the head, red scheme. `happy` is ignored in ERROR.
- OFF (powering down, ~1.3 s): waves goodbye, eyes close, glow fades out.
- happy > 0 (petted, in any mode but ERROR): hops, arms up and wiggling,
  happy ^^ eyes, big grin, two hearts floating up. It must read as joy at
  64 px.
- Keep it readable at 64x64: expressions come from 2-5 px shapes, so
  exaggerate. The face needs strong contrast against the body.

PER-MODE SCHEMES (keep these unless your colours clash with them)
  BOOT      ffffff cfe0ff 8fa8ff 5a5fe0  accent a9c0ff
  IDLE      f4e8ff c7a4ff 9a6bff 5b3fd9  accent a77dff
  LISTENING e8faff 8fdcff 3fa2ff 2a5bd7  accent 5cb8ff
  THINKING  ffe6ff ff9cf0 d35bff 7a2bd9  accent e07bff
  SPEAKING  eafff4 9ff5cf 3fd9a0 1f9a7a  accent 6ff0bf
  ERROR     ffd6d6 ff6b6b c7304a 6b1a3a  accent ff5c5c
  OFF       d8d4ff 8f86d9 5a4fb0 2e2870  accent 7c72d0

BUILDING THE BODY
Model yourself from a few analytic parts in the original's style, not a
hand-drawn bitmap: a superellipse (or a few) for the body and head, an
inner superellipse for the face panel, rotated ellipses for limbs, stamped
bitmaps for eyes and mouth. Parts then move, squash and follow the pose for
free. Shade body tones from a fake surface normal (dot with the light),
plus Bayer dither, plus a stable per-position hash for texture (fur,
feathers, scales: whatever suits you), so the texture doesn't shimmer as
it moves.
