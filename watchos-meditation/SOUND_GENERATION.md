# How the background sound is generated

The background is fresh procedural audio. Muse chooses a recipe, then Python
and NumPy synthesize samples on Vercel's CPU. **There is no learned background
audio model in this version.** Kokoro is a separate neural model for speech.

## 1. Muse directs the texture

The watch asks Muse for 16 short spoken phrases and six sound recipes. Each
recipe selects one of `brush`, `fabric`, `rain`, `leaves`, `wood` and `water`,
with four numeric controls between 0 and 1:

| Control | Effect in the synthesizer |
|---|---|
| `brightness` | Changes noise filter cutoffs; lower values reduce high frequencies. |
| `activity` | Scales event levels, and changes the spacing of rain events. |
| `pace` | Changes event spacing for wood, water, leaves, fabric and brush. |
| `space` | Scales stereo placement and movement of individual events. |

The prompt asks for gentle values: brightness/activity/pace 0.2–0.5 and space
0.4–0.8. Validation accepts the bounded 0–1 range. Only allowlisted scenes and
finite numbers reach synthesis; generated code, audio URLs and executables are
not accepted. Sound starts with a default recipe if Muse has not replied yet.
Sounds-only sessions also use defaults because they do not request a guide.

Example audio request, with the bearer key supplied separately in its header:

```json
{
  "request_id": "09b6c617-6029-45a0-a611-c514fdb7ad20",
  "kind": "sound",
  "scene": "brush",
  "recipe": {"brightness": 0.3, "activity": 0.3, "pace": 0.4, "space": 0.6}
}
```

Use a new UUID for every real generation request.

## 2. NumPy creates a new stereo recording

[`Cloud/calm_synthesis.py`](Cloud/calm_synthesis.py) chooses a fresh random seed
and allocates 64 seconds of stereo samples at 44,100 Hz. Each layer starts from
new random samples; no source audio file is loaded or repeated.

First it makes a quiet bed of filtered noise independently for each channel.
FFT-domain weighting shapes the spectrum, reduces very low frequencies and
rolls off the high end. Slowly changing random amplitude anchors prevent a
perfectly constant hiss. Rain and water get a stronger bed; wood gets very little.

Next it places events with randomized timing, duration, pressure, level and
stereo position. A sine-squared envelope makes their beginnings and endings
gentle. Equal-power panning and small lateral movement position each event.

| Scene | Main event construction |
|---|---|
| Brush | Filtered noise strokes with irregular pressure, roughly 1.2–3.5 seconds each. |
| Fabric | Short, darker noise rustles, roughly 0.7–2.3 seconds. |
| Leaves | Longer irregular noise rustles, roughly 1.3–4 seconds. |
| Water | Broad noise washes, roughly 3–7 seconds, over a diffuse bed. |
| Rain | Dense short noise drops with a small downward-sweeping tonal component. |
| Wood | Three exponentially damped sine resonances with a rounded attack. |

These techniques suggest textures; they do not physically simulate real water,
leaves or fabric. The same scene can recur, but its random waveform changes.
The stereo bed remains independently generated even when event `space` is zero.

## 3. Postprocessing prepares the download

[`Cloud/calm_audio.py`](Cloud/calm_audio.py) trims two seconds from each end,
leaving **60 seconds**. It applies a 129-tap low-pass filter with a 6.5 kHz cutoff,
softly compresses transients with `tanh`, and scales toward a −24 dBFS RMS level
subject to a −5 dBFS peak bound. These are digital sample levels, not a guarantee
of acoustic loudness at a listener's ears. It adds short opening/closing fades
and encodes stereo **PCM16 WAV at 44.1 kHz**, about **10.6 MB per segment**.

The authenticated endpoint returns WAV bytes with SHA-256, duration, seed,
generator and recipe-source headers. The watch checks the download before
accepting it. WAV bytes are streamed over HTTP after rendering completes;
this is not incremental real-time sample synthesis or token-by-token audio.

## 4. The watch makes consecutive segments flow together

The watch buffers upcoming segments and crossfades different clips over four
seconds. It reduces background gain while Kokoro phrases play. ASMR mix changes
scenes; choosing a single soundscape keeps its theme but still makes new samples.

Saved clips let returning users begin sooner while Vercel generates fresh
material. Up to three saved sounds can open a session; fresh arrivals replace
queued saved clips. The library is bounded to 1 GiB between sessions, so clips
can be reused across sessions. They are not looped to fill gaps within a session.
If requests are too slow, the queue can run out and there will be a quiet pause.

## What Vercel and Redis do

Vercel runs NumPy synthesis, Kokoro speech inference and the Muse SDK relay.
Redis stores Muse enrollment/rotating credentials, request IDs and separate
sound/voice leases. **Redis does not store the generated audio or phrase text.**
Audio is stored on the watch; Kokoro weights are cached in Vercel's temporary
filesystem. Cold starts may need to download and load the model again.

There is no external TTS API fee in this implementation, but Vercel compute,
network transfer and Redis usage still count toward their service limits.
Uncompressed stereo downloads are substantial; this example does not claim
unlimited free hosting or uninterrupted generation.
