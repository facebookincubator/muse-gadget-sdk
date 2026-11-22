# 3D-style Muse on the Waveshare 1.75C

The seven PNGs are the idle, boot, listening, thinking, speaking, error, and
power-off concept images approved for this board. Each is converted to a
160×160 little-endian RGB565 file of exactly 51,200 bytes. The firmware embeds
those files in flash and scales them to the 320×320 face area with bilinear
sampling. It adds a small breathing motion, voice-level motion while speaking,
and short crossfades between states. The remaining boards still use the
procedural pixel avatar.

The PNGs are source art. To regenerate the RGB565 files after changing them,
install ImageMagick and run from `esp32/`:

```sh
python3 tools/gen_muse_3d_assets.py
```

The images depict the Jollybot character. As with `avatar/jollybot.gif`, the
repository's Apache license does not grant rights to the character. See the
root README's license note.

These are pre-rendered 3D-style images, not a 3D model or on-device 3D engine.
The 1.75C's actual frame rate and power draw need physical-device validation.
