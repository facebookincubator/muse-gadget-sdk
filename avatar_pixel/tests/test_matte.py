# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

import hashlib
import unittest

from PIL import Image, ImageDraw

from avatar_pixel import AvatarError, compile_image, validate_pack
from avatar_pixel.matte import (
    MATTE_EDGE,
    _normalize_rgb565,
    _round_ratio,
    matte_uniform_white,
)
from avatar_pixel.tests.fixtures import avatar, encode
from avatar_pixel.tests.test_compiler import parts


def outlined_white(size=64):
    image = Image.new("RGBA", (size, size), (255, 255, 255, 255))
    draw = ImageDraw.Draw(image)
    draw.rectangle(
        (size // 4, size // 4, 3 * size // 4 - 1, 3 * size // 4 - 1),
        fill="white",
        outline="black",
        width=max(1, size // 32),
    )
    return image


def soft_vector(strong=(200, 200, 200), current=(232, 232, 232)):
    image = Image.new("RGBA", (9, 9), (255, 255, 255, 255))
    image.putpixel((4, 4), (*strong, 255))
    image.putpixel((3, 4), (*current, 255))
    return image


class MatteTests(unittest.TestCase):
    def test_outlined_white_foreground_and_enclosed_highlight_are_preserved(self):
        with outlined_white() as image:
            original = image.tobytes()
            matted, stats = matte_uniform_white(image)
            with matted:
                self.assertEqual(image.tobytes(), original)
                self.assertEqual(
                    stats,
                    {
                        "applied": True,
                        "removed": 3072,
                        "softened": 0,
                        "width": 64,
                        "height": 64,
                    },
                )
                self.assertEqual(matted.getpixel((0, 0)), (0, 0, 0, 0))
                self.assertEqual(matted.getpixel((32, 32)), (255, 255, 255, 255))
                self.assertEqual(matted.getpixel((16, 16)), (0, 0, 0, 255))
        with outlined_white() as image:
            draw = ImageDraw.Draw(image)
            draw.rectangle((22, 22, 41, 41), fill=(30, 150, 210, 255))
            image.putpixel((32, 32), (255, 255, 255, 255))
            matted, stats = matte_uniform_white(image)
            with matted:
                self.assertTrue(stats["applied"])
                self.assertEqual(matted.getpixel((32, 32)), (255, 255, 255, 255))

    def test_four_connected_not_diagonal_color_key(self):
        with Image.new("RGBA", (9, 9), "white") as image:
            ImageDraw.Draw(image).rectangle((2, 2, 6, 6), fill="black")
            image.putpixel((2, 2), (255, 255, 255, 255))
            image.putpixel((3, 3), (255, 255, 255, 255))
            matted, _stats = matte_uniform_white(image)
            with matted:
                self.assertEqual(matted.getpixel((2, 2)), (0, 0, 0, 0))
                self.assertEqual(matted.getpixel((3, 3)), (255, 255, 255, 255))

    def test_colored_nonuniform_or_bad_corner_border_skips_unchanged(self):
        with outlined_white() as image:
            for color in (
                (220, 220, 220, 255),
                (255, 128, 64, 255),
                (10, 240, 250, 255),
            ):
                ImageDraw.Draw(image).rectangle((0, 0, 63, 63), outline=color)
                matted, stats = matte_uniform_white(image)
                with matted:
                    self.assertFalse(stats["applied"])
                    self.assertEqual(matted.tobytes(), image.tobytes())
        with outlined_white() as image:
            # White corner requirement is stronger than the >=98% border gate.
            image.putpixel((0, 0), (230, 230, 230, 255))
            matted, stats = matte_uniform_white(image)
            with matted:
                self.assertFalse(stats["applied"])
                self.assertEqual(matted.tobytes(), image.tobytes())
        with outlined_white() as image:
            # 6 of 252 unique border pixels is below 98%, despite white corners.
            for x in range(10, 16):
                image.putpixel((x, 0), (0, 0, 0, 255))
            matted, stats = matte_uniform_white(image)
            with matted:
                self.assertFalse(stats["applied"])
                self.assertEqual(matted.tobytes(), image.tobytes())

    def test_unique_border_exact_98_percent_gate(self):
        with outlined_white() as image:
            # 247/252 is >=98%, so 5 nonwhite border cells may remain foreground.
            for x in range(10, 15):
                image.putpixel((x, 0), (0, 0, 0, 255))
            matted, stats = matte_uniform_white(image)
            with matted:
                self.assertTrue(stats["applied"])
                self.assertEqual(matted.getpixel((10, 0)), (0, 0, 0, 255))
        for size in ((1, 64), (64, 1)):
            with Image.new("RGBA", size, "white") as image:
                point = (0, 32) if size[0] == 1 else (32, 0)
                image.putpixel(point, (0, 0, 0, 255))
                matted, stats = matte_uniform_white(image)
                with matted:
                    self.assertTrue(stats["applied"])
                    self.assertEqual(stats["removed"], 63)
                    self.assertEqual(matted.getpixel(point), (0, 0, 0, 255))

    def test_existing_alpha_and_hidden_rgb_are_unchanged_even_large(self):
        for size in ((64, 64), (2048, 1024)):
            with Image.new("RGBA", size, (250, 248, 249, 255)) as image:
                image.putpixel((0, 0), (12, 130, 240, 0))
                image.putpixel((1, 1), (200, 30, 100, 128))
                matted, stats = matte_uniform_white(image)
                with matted:
                    self.assertEqual(matted.size, size)
                    self.assertEqual(matted.tobytes(), image.tobytes())
                    self.assertEqual(
                        stats,
                        {
                            "applied": False,
                            "removed": 0,
                            "softened": 0,
                            "width": 0,
                            "height": 0,
                        },
                    )
                data = encode(image)
            normal = compile_image(data)
            opted = compile_image(data, remove_white_background=True)
            self.assertEqual(normal.pack, opted.pack)
            self.assertEqual(normal.preview_png, opted.preview_png)

    def test_blank_white_skips_instead_of_empty_alpha(self):
        for size in ((1, 1), (64, 64), (2048, 1024)):
            with Image.new("RGBA", size, (250, 246, 252, 255)) as image:
                matted, stats = matte_uniform_white(image)
                with matted:
                    self.assertFalse(stats["applied"])
                    self.assertEqual(stats["removed"], 0)
                    self.assertEqual(matted.tobytes(), image.tobytes())
                result = compile_image(encode(image), remove_white_background=True)
                self.assertFalse(result.manifest["white_background"]["applied"])
                self.assertEqual(
                    set(parts(result.pack)[2]),
                    {0, 255} if size[0] != size[1] else {255},
                )
                validate_pack(result.pack)

    def test_rgb565_normalization_and_integer_rounding_vectors(self):
        for raw, normalized in (
            ((0, 0, 0), (0, 0, 0)),
            ((255, 255, 255), (255, 255, 255)),
            ((232, 232, 232), (239, 235, 239)),
            ((200, 200, 200), (206, 203, 206)),
            ((231, 231, 231), (231, 231, 231)),
        ):
            self.assertEqual(_normalize_rgb565(raw), normalized)
        self.assertEqual(_round_ratio(1, 2), 1)
        self.assertEqual(_round_ratio(3, 2), 2)
        self.assertEqual(_round_ratio(5, 2), 3)
        self.assertEqual(_round_ratio(5, 3), 2)

    def test_deterministic_soft_mask_and_decontamination_vectors(self):
        for strong, current, expected in (
            ((0, 0, 0), (231, 231, 231), (0, 0, 0, 24)),
            ((0, 0, 0), (232, 232, 232), (51, 0, 51, 20)),
            ((200, 200, 200), (232, 232, 232), (213, 203, 213, 98)),
        ):
            with self.subTest(strong=strong, current=current):
                with soft_vector(strong, current) as image:
                    original = image.tobytes()
                    matted, stats = matte_uniform_white(image)
                    again, again_stats = matte_uniform_white(image)
                    with matted, again:
                        self.assertEqual(matted.getpixel((3, 4)), expected)
                        self.assertEqual(
                            matted.getpixel((4, 4)), (*_normalize_rgb565(strong), 255)
                        )
                        self.assertEqual(stats["removed"], 79)
                        self.assertEqual(stats["softened"], 1)
                        self.assertEqual(matted.tobytes(), again.tobytes())
                        self.assertEqual(stats, again_stats)
                        self.assertEqual(image.tobytes(), original)

    def test_largest_strong_neighbor_and_immutable_pass_order(self):
        with soft_vector((200, 200, 200)) as image:
            image.putpixel((3, 5), (0, 0, 0, 255))
            matted, stats = matte_uniform_white(image)
            mirrored = image.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
            reflected, reflected_stats = matte_uniform_white(mirrored)
            with matted, mirrored, reflected:
                self.assertEqual(matted.getpixel((3, 4)), (51, 0, 51, 20))
                with matted.transpose(Image.Transpose.FLIP_LEFT_RIGHT) as expected:
                    self.assertEqual(expected.tobytes(), reflected.tobytes())
                self.assertEqual(stats, reflected_stats)

    def test_soft_edge_requires_removed_neighbor_and_strong_foreground(self):
        with outlined_white() as image:
            image.putpixel((32, 32), (232, 232, 232, 255))
            matted, stats = matte_uniform_white(image)
            with matted:
                self.assertEqual(matted.getpixel((32, 32)), (239, 235, 239, 255))
                self.assertEqual(stats["softened"], 0)
        with Image.new("RGBA", (9, 9), "white") as image:
            image.putpixel((4, 4), (232, 232, 232, 255))
            matted, stats = matte_uniform_white(image)
            with matted:
                self.assertEqual(matted.getpixel((4, 4)), (239, 235, 239, 255))
                self.assertEqual(stats["softened"], 0)

    def test_large_opaque_analysis_is_box_bounded_before_mask_without_crop(self):
        with Image.new("RGBA", (2048, 1024), "white") as image:
            ImageDraw.Draw(image).rectangle(
                (512, 256, 1535, 767), fill="white", outline="black", width=32
            )
            matted, stats = matte_uniform_white(image)
            with matted:
                self.assertEqual(matted.size, (MATTE_EDGE, MATTE_EDGE // 2))
                self.assertTrue(stats["applied"])
                self.assertEqual((stats["width"], stats["height"]), matted.size)
                self.assertEqual(matted.getpixel((233, 116)), (255, 255, 255, 255))
            result = compile_image(
                encode(image), profile=96, remove_white_background=True
            )
            self.assertEqual(
                (result.manifest["source_width"], result.manifest["source_height"]),
                (2048, 1024),
            )
            self.assertEqual(result.manifest["content_box"], [0, 24, 96, 72])
            self.assertLessEqual(
                max(
                    result.manifest["white_background"]["width"],
                    result.manifest["white_background"]["height"],
                ),
                466,
            )

    def test_compiler_optional_pack_has_alpha_and_remains_nonempty(self):
        with outlined_white() as image:
            data = encode(image)
        for profile in (64, 96):
            for dither in (False, True):
                result = compile_image(
                    data, profile=profile, dither=dither, remove_white_background=True
                )
                palette, indices, alpha = parts(result.pack)
                self.assertIn(0, alpha)
                self.assertTrue(any(alpha))
                self.assertLess(max(indices), len(palette))
                self.assertTrue(result.manifest["white_background"]["applied"])
                self.assertEqual(result.manifest["flags"], 0)
                self.assertEqual(result.manifest["capabilities"], 15)
                self.assertEqual(
                    result.manifest["source_key"], hashlib.sha256(data).hexdigest()
                )
                validate_pack(result.pack)

    def test_false_default_is_byte_and_manifest_identical(self):
        for kind in ("robot", "animal", "plant", "logo"):
            with avatar(kind) as image:
                data = encode(image)
            normal = compile_image(data)
            explicit = compile_image(data, remove_white_background=False)
            self.assertEqual(normal, explicit)
            self.assertNotIn("white_background", normal.manifest)
            if kind == "robot":
                # PNG encoder bytes vary across Pillow/zlib builds. Fix the
                # opaque generation key so this prior-version golden vector
                # tests the package/pixel output, not that encoding detail.
                fixed = compile_image(data, source_key="42" * 32)
                self.assertEqual(
                    hashlib.sha256(fixed.pack).hexdigest(),
                    "ef24c14a4da734a69fa7a5e65547a3aedff93d6ca4bc85a6f427f616b27c25c4",
                )

    def test_library_option_strict_boolean_before_decode(self):
        for value in (None, 0, 1, "true", [], {}, 1.0, float("nan")):
            with self.assertRaisesRegex(AvatarError, "invalid_remove_white_background"):
                compile_image(b"not decoded", remove_white_background=value)

    def test_helper_input_bounds_and_mode(self):
        for size, mode in (((2049, 1), "RGBA"), ((1, 1), "RGB")):
            with Image.new(mode, size) as image:
                with self.assertRaisesRegex(ValueError, "invalid_matte_image"):
                    matte_uniform_white(image)


if __name__ == "__main__":
    unittest.main()
