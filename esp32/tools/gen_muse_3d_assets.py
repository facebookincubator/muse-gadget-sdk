#!/usr/bin/env python3
"""Compile registered 320 px Muse poses and expression patches. Requires ImageMagick."""
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

ART = Path(__file__).resolve().parents[1] / "avatar" / "3d"
POSES = ("boot", "idle", "listening", "thinking", "speaking", "error", "off", "working")
SIZE = 320
ROWS = 16


def load(name):
    path = ART / (name + ".png")
    if subprocess.check_output(["magick", "identify", "-format", "%wx%h", path]) != b"320x320":
        raise ValueError(f"Wrong dimensions: {path}")
    rgb = subprocess.check_output(["magick", path, "-colorspace", "sRGB", "-depth", "8", "rgb:-"])
    assert len(rgb) == SIZE * SIZE * 3
    return rgb


def paste_feature(target, center, donor, source, half, scale_y=1.0, scale_x=1.0):
    """Feather a registered crop; squeeze only the inner feature, fixing its outer border."""
    cx, cy = center
    sx, sy = source
    w, h = half
    for dy in range(-h, h + 1):
        for dx in range(-w, w + 1):
            alpha = min(1, (w - abs(dx)) / 3, (h - abs(dy)) / 3)
            if alpha == 0:
                continue
            # Stretch the surrounding skin into the space vacated by the feature.
            fy = min(h, abs(dy) / scale_y)
            x = sx + dx / scale_x
            y = sy + math.copysign(fy, dy)
            ix, iy = math.floor(x), math.floor(y)
            pos = ((cy + dy) * SIZE + cx + dx) * 3
            for c in range(3):
                def channel(xx, yy):
                    return donor[(yy * SIZE + xx) * 3 + c]
                a = channel(ix, iy) * (ix + 1 - x) + channel(ix + 1, iy) * (x - ix)
                b = channel(ix, iy + 1) * (ix + 1 - x) + channel(ix + 1, iy + 1) * (x - ix)
                v = a * (iy + 1 - y) + b * (y - iy)
                target[pos + c] = round(v * alpha + target[pos + c] * (1 - alpha))


def rgb565(rgb):
    return b"".join(struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
                    for r, g, b in zip(rgb[::3], rgb[1::3], rgb[2::3]))


def patch(base, changed):
    # Compare final quantized pixels; all crops share the base's exact sampling grid.
    changed = rgb565(changed)
    points = [i for i in range(SIZE * SIZE) if changed[2*i:2*i+2] != base[2*i:2*i+2]]
    if not points:
        raise ValueError("Empty expression")
    x, r = min(i % SIZE for i in points), max(i % SIZE for i in points) + 1
    y, b = points[0] // SIZE, points[-1] // SIZE + 1
    return (x, y, r-x, b-y), b"".join(changed[2*(j*SIZE+x):2*(j*SIZE+r)] for j in range(y,b))


def expressions(name, art, manifest):
    spec = manifest["poses"][name]
    idle = manifest["poses"]["idle"]
    closed = manifest["closed_eye_source"]
    base = bytearray(art[name])

    def eyes(img, shut=False, height=1):
        source = closed if shut else idle
        for center, src in zip(spec["eyes"], source["eyes"]):
            paste_feature(img, center, art[source.get("file", "idle")], src, (14, 12), height)

    def smile(img, wide=1):
        paste_feature(img, spec["mouth"], art["idle"], idle["mouth"], (14, 10), scale_x=wide)

    if name == "boot":
        eyes(base, shut=True)
    if name == "speaking":
        smile(base)
    variants = {}
    if name in ("boot", "idle"):
        heights = (0.25, 0.50, 0.75, 1.0) if name == "boot" else (0.50, 0)
        for stage, height in enumerate(heights, 1):
            frame = base.copy()
            eyes(frame, shut=height == 0, height=height or 1)
            variants[stage] = frame
    if name == "speaking":
        frame = base.copy()
        paste_feature(frame, spec["mouth"], art[name], spec["mouth"], (14, 10), 0.5)
        variants.update({1: frame, 2: art[name]})
    if name == "thinking":
        frame = base.copy()
        for stage, (cx, cy, radius) in enumerate(spec["dots"], 1):
            for y in range(cy-radius-3, cy+radius+4):
                for x in range(cx-radius-3, cx+radius+4):
                    d = math.hypot(x-cx, y-cy)
                    alpha = min(1, max(0, radius+0.5-d)) + 0.22 * max(0, 1-abs(d-radius)/3)
                    alpha = min(1, alpha)
                    pos = (y*SIZE+x)*3
                    for c, v in enumerate((236, 183, 255)):
                        frame[pos+c] = round(frame[pos+c]*(1-alpha) + v*alpha)
            variants[stage] = frame.copy()
    if "eyes" in spec:
        frame = base.copy()
        eyes(frame, shut=True)
        smile(frame, 1.35)
        variants[5] = frame
    return base, variants


def generate():
    manifest = json.loads((ART / "manifest.json").read_text())
    assert set(manifest["poses"]) == set(POSES) and (manifest["size"], manifest["block_rows"]) == (SIZE, ROWS)
    art = {name: load(name) for name in (*POSES, manifest["closed_eye_source"]["file"])}
    blob = bytearray()
    tables = []
    max_patch_bytes = 0
    with tempfile.TemporaryDirectory() as tmp:
        thumbs = []
        for name in POSES:
            base, variants = expressions(name, art, manifest)
            packed = rgb565(base)
            blocks = []
            for i in range(0, len(packed), SIZE*ROWS*2):
                data = zlib.compress(packed[i:i+SIZE*ROWS*2], 9)
                blocks.append(len(blob))
                blob.extend(data)
            blocks.append(len(blob))
            entries = []
            used = 0
            assert set(variants) == {"boot":{1,2,3,4,5},"idle":{1,2,5},"thinking":{1,2,3,5},
                                     "speaking":{1,2,5},"working":{5},"listening":{5}}.get(name,set())
            for stage, frame in variants.items():
                rect, data = patch(packed, frame)
                if len(blob) % 2:
                    blob.append(0)
                entries.append(f"[{stage}] = {{ {{{', '.join(map(str, rect))}}}, {len(blob)} }}")
                blob.extend(data)
                used += len(data)
            max_patch_bytes = max(max_patch_bytes, used)
            tables.append(f"[POSE_{name.upper()}] = {{\n    {{{', '.join(map(str, blocks))}}},\n    " +
                          ("{" + ",\n     ".join(entries) + "}" if entries else "{{{0}, 0}}") + "\n},")
            for stage in range(6):
                thumb = Path(tmp) / f"{name}-{stage}.png"
                args = ["magick", "-size", "320x320", "-depth", "8", "rgb:-", "-resize", "160x160"]
                if stage in variants:
                    (x,y,w,h), _ = patch(packed, variants[stage])
                    args += ["-stroke","#50c8c0","-fill","none","-draw",
                             f"rectangle {x//2},{y//2} {(x+w-1)//2},{(y+h-1)//2}"]
                rgb = base if stage == 0 else variants.get(stage, bytes(len(base)))
                subprocess.run(args+[thumb], input=rgb, check=True)
                thumbs.append(thumb)
        subprocess.run(["magick", "montage", *thumbs, "-tile","6x8","-geometry","+0+0","-strip",
                        "-define","png:exclude-chunk=date,time", ART/"expressions.png"], check=True)
    (ART/"muse3d.bin").write_bytes(blob)
    (ART/"muse3d_assets.inc").write_text(
        "/* Generated by tools/gen_muse_3d_assets.py. */\n"
        f"#define PATCH_BYTES {max_patch_bytes}\nstatic const pose_art_t assets[POSE_COUNT] = {{\n" +
        "\n".join(tables) + "\n};\n")


if __name__ == "__main__":
    generate()
