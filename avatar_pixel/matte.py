# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Conservative, opt-in uniform-white matte; no anatomy or general segmentation."""

from collections import deque

from PIL import Image

MATTE_EDGE = 466
MAX_SOURCE_EDGE = 2048


def _normalize_rgb565(pixel):
    r, g, b = pixel[:3]
    r, g, b = r >> 3, g >> 2, b >> 3
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _round_ratio(numerator, denominator):
    # Nonnegative nearest integer, ties UP, matching integer C/round semantics.
    # No Python bankers rounding or floating-point/platform dependence.
    return (numerator + denominator // 2) // denominator


def _neighbors(index, width, height):
    x, y = index % width, index // width
    if x:
        yield index - 1
    if x + 1 < width:
        yield index + 1
    if y:
        yield index - width
    if y + 1 < height:
        yield index + width


def matte_uniform_white(image):
    """Return a NEW RGBA image and bounded applied/removed/softened statistics.

    Input is never mutated. Nonopaque input is returned unchanged, including
    hidden RGB and every alpha value, without shrinking. Opaque mask work is
    at most 466 per edge (BOX shrink before mask); a skipped matte returns an
    unchanged original copy, not the analysis downsample. Counts/width/height
    describe the analysis grid, not final compiler-profile cells. Alpha input
    skips analysis entirely and reports a zero-sized analysis grid.
    """
    if (
        not isinstance(image, Image.Image)
        or image.mode != "RGBA"
        or min(image.size) < 1
        or max(image.size) > MAX_SOURCE_EDGE
    ):
        raise ValueError("invalid_matte_image")
    stats = {"applied": False, "removed": 0, "softened": 0, "width": 0, "height": 0}
    with image.getchannel("A") as channel:
        if channel.getextrema() != (255, 255):
            return image.copy(), stats

    longest = max(image.size)
    work = image
    if longest > MATTE_EDGE:
        size = tuple(
            max(1, (edge * MATTE_EDGE + longest // 2) // longest) for edge in image.size
        )
        work = image.resize(size, Image.Resampling.BOX)
    try:
        width, height = work.size
        stats.update(width=width, height=height)
        if hasattr(work, "get_flattened_data"):
            source = work.get_flattened_data()
        else:
            source = work.getdata()  # Pillow 10/11 compatibility
        rgb = [_normalize_rgb565(pixel) for pixel in source]
        minima = bytes(min(pixel) for pixel in rgb)
        core = bytes(
            minimum >= 240 and max(pixel) - minimum <= 16
            for pixel, minimum in zip(rgb, minima)
        )
        corners = (0, width - 1, (height - 1) * width, width * height - 1)
        # Unique border cells, including degenerate 1-pixel dimensions.
        border = set(range(width)) | set(range((height - 1) * width, height * width))
        border.update(y * width for y in range(height))
        border.update(y * width + width - 1 for y in range(height))
        if not all(core[index] for index in corners) or (
            sum(core[index] for index in border) * 100 < len(border) * 98
        ):
            return image.copy(), stats

        retained = bytearray(b"\xff" * (width * height))
        queue = deque()
        for index in sorted(border):
            if core[index]:
                retained[index] = 0
                queue.append(index)
        while queue:
            for index in _neighbors(queue.popleft(), width, height):
                if retained[index] and core[index]:
                    retained[index] = 0
                    queue.append(index)
        removed = len(retained) - retained.count(255)
        if removed == len(retained):
            return image.copy(), stats  # Never manufacture an empty-alpha pack.

        # Source RGB, minima and core retention are immutable during this pass.
        # Strong foreground (<220) can never be a soft candidate (>=224).
        alpha = retained.copy()
        softened = 0
        for index, pixel in enumerate(rgb):
            minimum = minima[index]
            if not retained[index] or minimum < 224 or max(pixel) - minimum > 32:
                continue
            neighbors = tuple(_neighbors(index, width, height))
            if not any(retained[neighbor] == 0 for neighbor in neighbors):
                continue
            strength = max(
                (
                    255 - minima[neighbor]
                    for neighbor in neighbors
                    if retained[neighbor] and minima[neighbor] < 220
                ),
                default=0,
            )
            if not strength:
                continue
            opacity = max(1, min(255, _round_ratio((255 - minimum) * 255, strength)))
            alpha[index] = opacity
            softened += opacity < 255

        # Compute ALL alpha first, then decontaminate against a white backdrop.
        output = []
        for pixel, opacity in zip(rgb, alpha):
            if opacity == 0:
                output.append((0, 0, 0, 0))
            elif opacity == 255:
                output.append((*pixel, 255))
            else:
                output.append(
                    (
                        *tuple(
                            max(
                                0,
                                min(
                                    255,
                                    _round_ratio(
                                        max(0, channel - (255 - opacity)) * 255, opacity
                                    ),
                                ),
                            )
                            for channel in pixel
                        ),
                        opacity,
                    )
                )
        result = Image.new("RGBA", work.size)
        result.putdata(output)
        stats.update(applied=True, removed=removed, softened=softened)
        return result, stats
    finally:
        if work is not image:
            work.close()
