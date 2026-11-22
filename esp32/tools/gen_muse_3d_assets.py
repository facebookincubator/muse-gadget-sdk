#!/usr/bin/env python3
"""Convert the checked-in 3D Muse concept art to 160x160 little-endian RGB565.

Requires ImageMagick's ``magick`` command. The firmware embeds the generated
files directly; this tool is only needed when the source art changes.
"""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "avatar" / "3d"
STATES = ("idle", "boot", "listening", "thinking", "speaking", "error", "off")
SIZE = 160


def convert(name: str) -> None:
    source = ART / f"{name}.png"
    rgb = subprocess.check_output(
        ["magick", str(source), "-resize", f"{SIZE}x{SIZE}!", "-depth", "8", "rgb:-"]
    )
    if len(rgb) != SIZE * SIZE * 3:
        raise ValueError(f"{source}: expected {SIZE * SIZE * 3} RGB bytes, got {len(rgb)}")
    result = bytearray(SIZE * SIZE * 2)
    for i in range(SIZE * SIZE):
        r, g, b = rgb[3 * i : 3 * i + 3]
        value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        result[2 * i] = value & 0xFF
        result[2 * i + 1] = value >> 8
    (ART / f"muse3d_{name}.rgb565").write_bytes(result)


if __name__ == "__main__":
    for state in STATES:
        convert(state)
