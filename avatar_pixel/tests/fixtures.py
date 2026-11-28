# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Synthetic, generic fixtures only; no bundled character or personal asset."""

import io

from PIL import Image, ImageDraw


def encode(image, format="PNG", **options):
    stream = io.BytesIO()
    image.save(stream, format=format, **options)
    return stream.getvalue()


def avatar(kind):
    if kind == "robot":
        image = Image.new("RGBA", (80, 80), (224, 210, 170, 255))
        draw = ImageDraw.Draw(image)
        draw.rectangle((20, 15, 60, 45), fill=(40, 110, 200, 255))
        draw.rectangle((24, 49, 56, 68), fill=(100, 160, 220, 255))
        draw.rectangle((27, 25, 33, 31), fill=(0, 0, 0, 255))
        draw.rectangle((46, 25, 52, 31), fill=(255, 255, 255, 255))
        draw.line((40, 15, 40, 5), fill=(220, 60, 40, 255), width=3)
    elif kind == "animal":
        image = Image.new("RGBA", (48, 64), (170, 10, 200, 0))
        draw = ImageDraw.Draw(image)
        draw.ellipse((6, 16, 42, 55), fill=(230, 150, 60, 255))
        draw.polygon(((9, 25), (5, 3), (23, 18)), fill=(180, 75, 35, 255))
        draw.polygon(((24, 18), (43, 3), (39, 26)), fill=(180, 75, 35, 255))
        draw.ellipse((14, 30, 18, 34), fill=(0, 0, 0, 255))
        draw.ellipse((30, 30, 34, 34), fill=(0, 0, 0, 255))
        draw.rectangle((16, 55, 34, 61), fill=(80, 180, 220, 128))
    elif kind == "plant":
        image = Image.new("RGBA", (32, 96), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        draw.line((16, 10, 16, 76), fill=(20, 120, 40, 255), width=3)
        draw.ellipse((1, 20, 15, 40), fill=(45, 170, 70, 255))
        draw.ellipse((17, 38, 30, 59), fill=(90, 210, 100, 255))
        draw.polygon(((4, 76), (28, 76), (23, 92), (9, 92)), fill=(190, 75, 50, 255))
    elif kind == "logo":
        image = Image.new("RGBA", (120, 40), (255, 255, 255, 255))
        draw = ImageDraw.Draw(image)
        draw.polygon(((5, 35), (20, 5), (35, 35)), fill=(220, 40, 50, 255))
        draw.rectangle((45, 5, 75, 35), fill=(30, 150, 200, 255))
        draw.ellipse((85, 5, 115, 35), fill=(40, 160, 60, 255))
    else:
        raise ValueError("unknown synthetic kind")
    return image


def gradient(size=96):
    image = Image.new("RGBA", (size, size))
    image.putdata(
        [
            ((x * 17 + y * 3) % 256, (x * 5 + y * 11) % 256, (x * 9 + y * 7) % 256, 255)
            for y in range(size)
            for x in range(size)
        ]
    )
    return image
