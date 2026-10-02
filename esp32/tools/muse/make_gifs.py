#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Render every Muse animation to an animated GIF using the firmware's renderer.

    python3 tools/muse/make_gifs.py [--default | --src FILE] [out_dir]     (default: ./gifs)

Draws your own avatar (components/muse/avatar/muse_pixel.c, see AVATAR_RECIPE.md)
when there is one, else the default avatar. Needs a C compiler and Pillow.
"""
import argparse
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_SRC = os.path.join(ROOT, "avatar", "muse_pixel.c")
CUSTOM_SRC = os.path.join(ROOT, "components", "muse", "avatar", "muse_pixel.c")
FRAME_MS = 40


def renderer_source(default=False):
    """The muse_pixel.c the firmware build picks: yours if it exists."""
    return DEFAULT_SRC if default or not os.path.exists(CUSTOM_SRC) else CUSTOM_SRC


def iter_render_frames(src, scale=5):
    """Compile the real renderer and return RGB frames grouped by animation.

    scale=1 exports native 64x64 pixels; scale=5 preserves the GIF preview size.
    CC may name a compiler (or a compiler plus flags) for host builds.
    """
    from PIL import Image

    if type(scale) is not int or not 1 <= scale <= 5:
        raise ValueError("scale must be an integer from 1 to 5")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "muse_anim")
        subprocess.run(
            shlex.split(os.environ.get("CC", "cc")) +
            ["-O2", "-Wall", "-Werror", f"-DMUSE_ANIM_SCALE={scale}",
             "-I", "components/muse", "tools/muse/anim.c", os.path.abspath(src), "-lm", "-o", exe],
            cwd=ROOT, check=True, capture_output=True, text=True, timeout=60,
        )
        frames_dir = os.path.join(tmp, "frames")
        os.makedirs(frames_dir)
        subprocess.run([exe, frames_dir], check=True, capture_output=True, text=True, timeout=60)
        for name in sorted(os.listdir(frames_dir)):
            d = os.path.join(frames_dir, name)
            frames = []
            for f in sorted(os.listdir(d)):
                with Image.open(os.path.join(d, f)) as im:
                    frames.append(im.convert("RGB"))
            yield name, frames


def render_frames(src, scale=5):
    """Collect native preview frames; GIF export can iterate one state at a time."""
    return dict(iter_render_frames(src, scale))


def save_gif(frames, path):
    """Use one shared palette to prevent colours shimmering between frames."""
    from PIL import Image

    strip = Image.new("RGB", (frames[0].width, frames[0].height * len(frames)))
    for i, frame in enumerate(frames):
        strip.paste(frame, (0, i * frame.height))
    palette = strip.quantize(colors=256, method=Image.Quantize.MEDIANCUT)
    indexed = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]
    indexed[0].save(path, format="GIF", save_all=True, append_images=indexed[1:],
                    duration=FRAME_MS, loop=0, optimize=False)


def render(src, out):
    """Write one 320px GIF per animation; return (path, frame count) pairs."""
    os.makedirs(out, exist_ok=True)
    paths = []
    for name, frames in iter_render_frames(src):
        path = os.path.join(out, name + ".gif")
        save_gif(frames, path)
        paths.append((path, len(frames)))
    return paths


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out_dir", nargs="?", default=os.path.join(ROOT, "gifs"))
    which = ap.add_mutually_exclusive_group()
    which.add_argument("--default", action="store_true", help="draw the default avatar even if you have your own")
    which.add_argument("--src", help="draw this muse_pixel.c")
    args = ap.parse_args()
    src = os.path.abspath(args.src) if args.src else renderer_source(args.default)
    print(f"renderer: {os.path.relpath(src, ROOT)}")
    try:
        paths = render(src, os.path.abspath(args.out_dir))
    except subprocess.CalledProcessError as e:
        sys.exit((e.stdout or "") + (e.stderr or "") + f"{os.path.relpath(src, ROOT)} doesn't build")
    for path, n in paths:
        print(f"{path}  ({n} frames)")


if __name__ == "__main__":
    main()
