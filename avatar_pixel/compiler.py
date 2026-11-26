# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Bounded static image processing and the frozen MSPIX01 binary contract."""

from collections import Counter
from dataclasses import dataclass, field
import hashlib
import io
import re
import struct
import warnings
import zlib

from PIL import Image, ImageOps, UnidentifiedImageError

MAX_IMAGE_BYTES = 2 * 1024 * 1024
MAX_EDGE = 2048
MAX_SOURCE_PIXELS = 4_194_304
MAX_PACK_BYTES = 65_536
MAX_PREVIEW_BYTES = 512 * 1024
PREVIEW_SCALE = 4
PROFILES = (64, 96)
MAGIC = b"MSPIX01\0"
CAPABILITIES = 15
HEADER = struct.Struct("<8sIHHHHI32sII")
assert HEADER.size == 64


class AvatarError(ValueError):
    """A public, fixed error code; never includes input data or decoder details."""

    def __init__(self, code: str):
        self.code = code
        super().__init__(code)


@dataclass(frozen=True)
class Compilation:
    # Avoid accidentally leaking private bytes through dataclass repr/logging.
    pack: bytes = field(repr=False)
    preview_png: bytes = field(repr=False)
    manifest: dict = field(repr=False)


def validate_options(profile=64, colors=24, dither=False, source_key=None):
    if type(profile) is not int or profile not in PROFILES:
        raise AvatarError("invalid_profile")
    if type(colors) is not int or not 2 <= colors <= 64:
        raise AvatarError("invalid_colors")
    if type(dither) is not bool:
        raise AvatarError("invalid_dither")
    if source_key is not None and (
        type(source_key) is not str
        or re.fullmatch(r"[0-9a-fA-F]{64}", source_key) is None
    ):
        raise AvatarError("invalid_source_key")


def _check_source(image):
    width, height = image.size
    if (
        width < 1
        or height < 1
        or width > MAX_EDGE
        or height > MAX_EDGE
        or width * height > MAX_SOURCE_PIXELS
    ):
        raise AvatarError("image_dimensions_exceeded")
    if image.format not in ("PNG", "JPEG", "WEBP"):
        raise AvatarError("unsupported_image")
    if getattr(image, "n_frames", 1) != 1 or getattr(image, "is_animated", False):
        raise AvatarError("animated_image")


def _decode(encoded):
    if type(encoded) is not bytes or not encoded:
        raise AvatarError("invalid_image")
    if len(encoded) > MAX_IMAGE_BYTES:
        raise AvatarError("image_bytes_exceeded")
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(
                io.BytesIO(encoded), formats=["PNG", "JPEG", "WEBP"]
            ) as image:
                _check_source(image)
                image.verify()
            with Image.open(
                io.BytesIO(encoded), formats=["PNG", "JPEG", "WEBP"]
            ) as image:
                _check_source(image)
                image.load()
                oriented = ImageOps.exif_transpose(image)
                try:
                    return oriented.convert("RGBA")
                finally:
                    if oriented is not image:
                        oriented.close()
    except AvatarError:
        raise
    except (Image.DecompressionBombError, Image.DecompressionBombWarning):
        raise AvatarError("image_dimensions_exceeded") from None
    except (
        UnidentifiedImageError,
        OSError,
        ValueError,
        SyntaxError,
        EOFError,
        OverflowError,
    ):
        raise AvatarError("invalid_image") from None


def _rgb565(rgb):
    r, g, b = rgb[:3]
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def _rgb888(color):
    r, g, b = (color >> 11) & 31, (color >> 5) & 63, color & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _distance(a, b):
    return sum((a[i] - b[i]) ** 2 for i in range(3))


def _palette(pixels, colors):
    # Transparent RGB does not consume the budget or dilute visible distinctions.
    histogram = Counter(_rgb565(pixel) for pixel in pixels if pixel[3])
    if not histogram:
        raise AvatarError("empty_alpha")
    candidates = sorted(histogram)
    if len(candidates) <= colors:
        return candidates
    # Canonical ordering removes dependence on pixel traversal/tie ordering.
    sample = Image.new("RGB", (sum(histogram.values()), 1))
    quantized = None
    try:
        sample.putdata(
            [_rgb888(color) for color in candidates for _ in range(histogram[color])]
        )
        quantized = sample.quantize(colors=colors, method=Image.Quantize.MEDIANCUT)
        raw_palette = quantized.getpalette()
        selected = []
        candidate_rgb = {color: _rgb888(color) for color in candidates}
        for index in sorted(set(quantized.tobytes())):
            target = raw_palette[index * 3 : index * 3 + 3]
            # Representatives are DISTINCT source RGB565 values, not means that
            # might collapse to one RGB565 value and lose the visible palette.
            representative = min(
                (color for color in candidates if color not in selected),
                key=lambda color: (_distance(candidate_rgb[color], target), color),
            )
            selected.append(representative)
        return sorted(selected)
    finally:
        sample.close()
        if quantized is not None:
            quantized.close()


def _indices(pixels, palette, width, dither):
    rgbs = [_rgb888(color) for color in palette]
    exact = {color: i for i, color in enumerate(palette)}
    cache = {}
    indices = bytearray(len(pixels))
    if not dither:
        for offset, pixel in enumerate(pixels):
            if not pixel[3]:
                continue
            color = _rgb565(pixel)
            if color not in cache:
                cache[color] = exact.get(color)
                if cache[color] is None:
                    rgb = _rgb888(color)
                    cache[color] = min(
                        range(len(palette)), key=lambda i: (_distance(rgb, rgbs[i]), i)
                    )
            indices[offset] = cache[color]
        return bytes(indices)

    # Fixed-point Floyd-Steinberg, no floating point or hidden color diffusion.
    # Error is discarded at fully transparent cells. Alpha remains independent.
    row = [[0, 0, 0] for _ in range(width + 2)]
    for y in range(len(pixels) // width):
        next_row = [[0, 0, 0] for _ in range(width + 2)]
        for x in range(width):
            offset = y * width + x
            pixel = pixels[offset]
            if not pixel[3]:
                continue
            rgb = tuple(
                max(0, min(255, pixel[c] + row[x + 1][c] // 16)) for c in range(3)
            )
            index = min(range(len(palette)), key=lambda i: (_distance(rgb, rgbs[i]), i))
            indices[offset] = index
            for c in range(3):
                error = rgb[c] - rgbs[index][c]
                row[x + 2][c] += error * 7
                next_row[x][c] += error * 3
                next_row[x + 1][c] += error * 5
                next_row[x + 2][c] += error
        row = next_row
    return bytes(indices)


def validate_pack(pack: bytes) -> dict:
    """Validate an immutable pack before handing it to a generic installer.

    No ownership/trust assertion is made: source_key is opaque, caller-supplied
    generation data. Device binding is outside this converter's responsibility.
    """
    if type(pack) is not bytes or not 64 <= len(pack) <= MAX_PACK_BYTES:
        raise AvatarError("invalid_pack_size")
    magic, total, width, height, count, flags, caps, key, payload_crc, header_crc = (
        HEADER.unpack_from(pack)
    )
    if magic != MAGIC or total != len(pack):
        raise AvatarError("invalid_pack_header")
    if not 16 <= width <= 128 or not 16 <= height <= 128 or not 1 <= count <= 64:
        raise AvatarError("invalid_pack_dimensions")
    if flags != 0 or caps != CAPABILITIES:
        raise AvatarError("invalid_pack_capabilities")
    expected = 64 + 2 * count + 2 * width * height
    if len(pack) != expected:
        raise AvatarError("invalid_pack_size")
    if zlib.crc32(pack[:60]) != header_crc or zlib.crc32(pack[64:]) != payload_crc:
        raise AvatarError("invalid_pack_crc")
    cells = width * height
    indices = pack[64 + 2 * count : 64 + 2 * count + cells]
    alpha = pack[64 + 2 * count + cells :]
    if max(indices) >= count:
        raise AvatarError("invalid_pack_index")
    if not any(alpha):
        raise AvatarError("empty_alpha")
    return {
        "format": "MSPIX01",
        "width": width,
        "height": height,
        "palette_count": count,
        "flags": flags,
        "capabilities": caps,
        "source_key": key.hex(),
        "total_size": total,
        "payload_crc32": payload_crc,
        "header_crc32": header_crc,
    }


def compile_image(
    encoded: bytes, *, profile=64, colors=24, dither=False, source_key=None
) -> Compilation:
    """Compile bytes only. Contain-fit/no crop; source background and A8 kept.

    Callers can supply a 64-character opaque digest, never a credential or path.
    Same bytes/options + supported Pillow version yield the same pack/preview.
    No input/output is stored, logged, fetched, or used to infer anatomy.
    """
    validate_options(profile, colors, dither, source_key)
    image = _decode(encoded)
    fitted = None
    canvas = None
    preview = None
    try:
        source_width, source_height = image.size
        longest = max(image.size)
        fit_width = max(1, (source_width * profile + longest // 2) // longest)
        fit_height = max(1, (source_height * profile + longest // 2) // longest)
        resample = (
            Image.Resampling.NEAREST if longest <= profile else Image.Resampling.BOX
        )
        fitted = image.resize((fit_width, fit_height), resample=resample)
        canvas = Image.new("RGBA", (profile, profile), (0, 0, 0, 0))
        left, top = (profile - fit_width) // 2, (profile - fit_height) // 2
        canvas.paste(fitted, (left, top))  # no alpha mask: do not square A8
        if hasattr(canvas, "get_flattened_data"):
            pixels = list(canvas.get_flattened_data())
        else:
            pixels = list(canvas.getdata())  # Pillow 10/11 compatibility
        palette = _palette(pixels, colors)
        indices = _indices(pixels, palette, profile, dither)
        alpha = bytes(pixel[3] for pixel in pixels)
        payload = struct.pack(f"<{len(palette)}H", *palette) + indices + alpha
        key = (
            bytes.fromhex(source_key)
            if source_key is not None
            else hashlib.sha256(encoded).digest()
        )
        total = 64 + len(payload)
        if total > MAX_PACK_BYTES:
            raise AvatarError("pack_bytes_exceeded")
        header = HEADER.pack(
            MAGIC,
            total,
            profile,
            profile,
            len(palette),
            0,
            CAPABILITIES,
            key,
            zlib.crc32(payload),
            0,
        )
        header = header[:60] + struct.pack("<I", zlib.crc32(header[:60]))
        pack = header + payload
        manifest = validate_pack(pack)

        # Preview is the actual RGB565/A8 sprite, not an unquantized approximation.
        canvas.putdata(
            [(*_rgb888(palette[index]), a) for index, a in zip(indices, alpha)]
        )
        preview = canvas.resize(
            (profile * PREVIEW_SCALE, profile * PREVIEW_SCALE), Image.Resampling.NEAREST
        )
        # New image has no EXIF, ICC, text chunks, filenames or timestamps.
        output = io.BytesIO()
        preview.save(output, format="PNG", compress_level=9, optimize=False)
        preview_png = output.getvalue()
        if len(preview_png) > MAX_PREVIEW_BYTES:
            raise AvatarError("preview_bytes_exceeded")
        manifest.update(
            {
                "source_width": source_width,
                "source_height": source_height,
                "content_box": [left, top, left + fit_width, top + fit_height],
                "requested_colors": colors,
                "dither": dither,
                "preview_width": preview.width,
                "preview_height": preview.height,
                "preview_size": len(preview_png),
            }
        )
        return Compilation(pack, preview_png, manifest)
    finally:
        image.close()
        for temporary in (fitted, canvas, preview):
            if temporary is not None:
                temporary.close()
