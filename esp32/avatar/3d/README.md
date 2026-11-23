# Muse poses for the Waveshare 1.75C

The bundled renderer displays eight 320 × 320 RGB565 poses. Listening, working,
error and goodbye hold still. Boot opens its eyes, idle blinks, thinking reveals
thought dots, and speaking cycles its mouth on a fixed timer. A pet reaction
temporarily replaces the face on eligible poses. The UI draws the activity ring
on its own schedule.

The source PNGs depict Jollybot. The repository's Apache license does not
grant rights to the character. See the root README's license note. The
listening, working and goodbye images were generated with imagegen using the
approved Muse portraits as references. The original goodbye art supplies
the closed eyes. Mouth and eye variants use registered crops from the same
character. All feature coordinates in `manifest.json` use the source PNG's
top-left origin.

To regenerate the embedded artwork, run the following from `esp32/` with
ImageMagick on the path.

```sh
python3 tools/gen_muse_3d_assets.py
```

The generator produces `muse3d.bin` and its C index. Each 16-row base block
contains literal or repeated little-endian RGB565 pixels. Expression patches
store the smallest rectangle surrounding the changed RGB565 pixels, including
the original texture between features. The base and its patches share one
sampling grid when prepared for a smaller layout. The normal 320-pixel layout
copies the native artwork directly.

The contact sheet shows boot, idle, listening, thinking, speaking, error,
goodbye and working from top to bottom. The first column is the base, the
next four columns are available expression stages, and the last is the pet
reaction. Teal outlines mark the bounds of changed pixels and appear only
in the contact sheet. [Expression contact sheet](expressions.png)
