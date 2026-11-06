# Speaking gestures

The bundled avatar uses a local, procedural controller in
`components/muse/muse_speaking.h`. It adds no network requests, speech buffering,
or dependency on response wording. The character renders at the board's existing
frame period. Custom avatar renderers remain responsible for their own movement.

The vocabulary is deliberately general:

- **Right-hand raise:** lift the right paw vertically beside the face and lower
  it, taking about 1.15 seconds when uninterrupted.
- **Step-by-step:** bring a paw inward in front of the torso and make three
  compact beats at increasing heights. The support paw stays low. This is a
  different path and silhouette from raising a hand; it takes about 1.90 seconds.
- **Open presentation:** lift and spread both paws outward together, then relax
  them. This balanced, broad pose takes about 1.65 seconds.
- **Left-hand raise:** a separate 1.15-second action using the left paw.
- **Rest:** lower the paws and keep subtle breathing/blinking. Rest durations
  vary between movement families, and leading hands alternate across gestures/turns.

The active sequence cycles right raise → steps → left raise → open presentation,
with a rest between each action. The two raises are independent actions, with
another movement between them. Opening actions rotate across replies too, so short
responses do not always show the same first action. The next family is selected
when an action starts: a mid-action speech pause settles the hands and resumes
with the next family instead of repeatedly restarting the same gesture.

These are continuous poses rather than fixed clips. On leaving speaking, the
renderer blends the current hand positions into the next state's pose over
0.28 seconds. Mouth animation stops with speaking; the hands may finish relaxing
afterward. A long response produces a sequence of individual gestures and rests,
not one animation stretched to the response length. Large frame-time gaps are
bounded so they cannot advance through a queue of overdue gestures.

Speech energy scales articulation and gently changes hand amplitude. Once the
controller has heard voice energy, a quiet period longer than 0.28 seconds lets
the hands settle; movement resumes after a short rest. Entirely silent caption
playback gets both hand gestures and a visual speaking rhythm when replies are
paced with silent PCM. The mouth varies between closed, rounded and open shapes,
with short phrase breaths. This adds no audio. Once actual voice energy is
detected, audio drives the mouth for the
rest of that reply, including closing during real pauses. Visible mouth shapes
approximate articulation; they are not phoneme lip sync.

Brows, gaze and small face nods follow the active gesture. Raises briefly lift
the brows and glance toward the raised hand; step beats add small nods; the open
presentation lifts the brows and widens the eyes. Normal blinking continues,
and the face relaxes between gestures.

Feet stay planted, body lean follows the active hand, and paws remain inside
the 64-pixel drawing area. The shading bounds include extended hands, so larger
gestures do not disappear at the old body rectangle's edges.

## Verification

`test_speaking_animation.py` exercises cutoffs at 179 different points, long
responses, real pauses, resumed speech, silent captions and preview warm-up.
It also runs the production renderer through repeated state changes under
AddressSanitizer and UndefinedBehaviorSanitizer.

The existing GIF tool renders silent caption playback, voiced speaking, long
speaking, short replies, an audio pause and an interruption into listening:

```sh
python3 -m unittest discover -s esp32/tests -p test_speaking_animation.py
python3 esp32/tools/muse/make_gifs.py --default /tmp/muse-speaking-previews
```

GIF generation needs Pillow. It uses the actual renderer, without audio output.
