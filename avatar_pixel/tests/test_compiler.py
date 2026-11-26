# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

import hashlib
import io
import struct
import unittest
import zlib

from PIL import Image

from avatar_pixel import AvatarError, compile_image, validate_pack
from avatar_pixel.compiler import HEADER, MAX_IMAGE_BYTES
from avatar_pixel.tests.fixtures import avatar, encode, gradient


def parts(pack):
    _, _, w, h, count, *_ = HEADER.unpack_from(pack)
    palette = struct.unpack_from(f"<{count}H", pack, 64)
    start = 64 + count * 2
    return palette, pack[start : start + w * h], pack[start + w * h :]


def fix_crcs(pack):
    data = bytearray(pack)
    struct.pack_into("<I", data, 56, zlib.crc32(data[64:]))
    struct.pack_into("<I", data, 60, zlib.crc32(data[:60]))
    return bytes(data)


class CompilerTests(unittest.TestCase):
    def assert_error(self, code, function, *args, **kwargs):
        with self.assertRaises(AvatarError) as caught:
            function(*args, **kwargs)
        self.assertEqual(code, caught.exception.code)

    def test_diverse_shapes_and_profiles(self):
        for kind in ("robot", "animal", "plant", "logo"):
            with avatar(kind) as image:
                data = encode(image)
            for profile in (64, 96):
                for dither in (False, True):
                    with self.subTest(kind=kind, profile=profile, dither=dither):
                        result = compile_image(
                            data, profile=profile, colors=24, dither=dither
                        )
                        metadata = validate_pack(result.pack)
                        self.assertEqual(metadata["width"], profile)
                        self.assertEqual(metadata["height"], profile)
                        self.assertEqual(metadata["flags"], 0)
                        self.assertEqual(metadata["capabilities"], 15)
                        self.assertLessEqual(metadata["palette_count"], 24)
                        self.assertEqual(result.pack[:8], b"MSPIX01\0")
                        self.assertEqual(
                            metadata["source_key"], hashlib.sha256(data).hexdigest()
                        )
                        self.assertEqual(
                            len(result.pack),
                            64 + 2 * metadata["palette_count"] + 2 * profile * profile,
                        )
                        palette, indices, alpha = parts(result.pack)
                        self.assertLess(max(indices), len(palette))
                        self.assertTrue(any(alpha))
                        self.assertLessEqual(len(result.pack), 65536)
                        with Image.open(io.BytesIO(result.preview_png)) as preview:
                            self.assertEqual(preview.size, (profile * 4, profile * 4))
                            self.assertEqual(preview.mode, "RGBA")
                            self.assertEqual(preview.info, {})
                            pixel = preview.getpixel((profile * 2, profile * 2))
                            self.assertEqual(
                                pixel,
                                preview.getpixel((profile * 2 + 3, profile * 2 + 3)),
                            )
                        again = compile_image(
                            data, profile=profile, colors=24, dither=dither
                        )
                        self.assertEqual(result, again)

    def test_rgb565_little_endian_and_black_not_transparency(self):
        with Image.new("RGBA", (64, 64), (0, 0, 0, 255)) as image:
            for y in range(64):
                for x in range(32, 64):
                    image.putpixel((x, y), (255, 0, 0, 255))
            result = compile_image(encode(image), colors=2)
        palette, indices, alpha = parts(result.pack)
        self.assertEqual(palette, (0, 0xF800))
        self.assertEqual(result.pack[64:68], b"\x00\x00\x00\xf8")
        self.assertEqual(indices[0], 0)
        self.assertEqual(indices[63], 1)
        self.assertEqual(set(alpha), {255})

    def test_transparent_hidden_rgb_cannot_collapse_visible_palette(self):
        with Image.new("RGBA", (64, 64), (255, 0, 255, 0)) as image:
            image.putpixel((4, 4), (0, 0, 0, 255))
            image.putpixel((5, 4), (8, 0, 0, 128))
            image.putpixel((6, 4), (0, 4, 0, 1))
            result = compile_image(encode(image), colors=3)
        palette, indices, alpha = parts(result.pack)
        self.assertEqual(palette, (0, 32, 2048))
        self.assertEqual(len({indices[260], indices[261], indices[262]}), 3)
        self.assertEqual(alpha[260:263], b"\xff\x80\x01")
        self.assertEqual(indices[0], 0)
        self.assertEqual(alpha[0], 0)

    def test_nontransparent_quantization_distinctions_and_limits(self):
        with gradient() as image:
            data = encode(image)
        for colors in (2, 24, 64):
            result = compile_image(data, profile=96, colors=colors)
            palette, indices, _ = parts(result.pack)
            self.assertEqual(len(palette), colors)
            self.assertEqual(len(set(palette)), colors)
            self.assertGreater(len(set(indices)), 1)
        self.assertNotEqual(
            compile_image(data, colors=2).pack,
            compile_image(data, colors=2, dither=True).pack,
        )

    def test_contain_fit_background_alpha_no_crop(self):
        with Image.new("RGBA", (32, 16), (248, 128, 16, 128)) as image:
            result = compile_image(encode(image), profile=64)
        self.assertEqual(result.manifest["content_box"], [0, 16, 64, 48])
        palette, _, alpha = parts(result.pack)
        self.assertEqual(len(palette), 1)
        self.assertEqual(alpha[: 16 * 64], bytes(16 * 64))
        self.assertEqual(alpha[16 * 64 : 48 * 64], b"\x80" * (32 * 64))
        with Image.open(io.BytesIO(result.preview_png)) as preview:
            self.assertEqual(preview.getpixel((0, 64))[3], 128)
            self.assertEqual(preview.getpixel((0, 0))[3], 0)

    def test_enlargement_is_nearest_and_never_smooths(self):
        with Image.new("RGBA", (2, 2), (255, 0, 0, 255)) as image:
            image.putpixel((1, 0), (0, 0, 255, 255))
            result = compile_image(encode(image))
        palette, indices, alpha = parts(result.pack)
        self.assertEqual(len(palette), 2)
        self.assertEqual(len(set(indices)), 2)
        self.assertEqual(set(alpha), {255})
        self.assertEqual(indices[:32], indices[:1] * 32)
        self.assertEqual(indices[32:64], indices[32:33] * 32)

    def test_caller_digest_opaque_not_a_trusted_override(self):
        with avatar("plant") as image:
            data = encode(image)
        key = "ab" * 32
        default = compile_image(data)
        supplied = compile_image(data, source_key=key.upper())
        self.assertEqual(supplied.manifest["source_key"], key)
        self.assertEqual(default.pack[64:], supplied.pack[64:])
        self.assertEqual(default.preview_png, supplied.preview_png)

    def test_static_formats_and_orientation(self):
        with avatar("robot") as image:
            for format in ("PNG", "JPEG", "WEBP"):
                with self.subTest(format=format):
                    rgb = image.convert("RGB") if format == "JPEG" else image
                    try:
                        result = compile_image(encode(rgb, format))
                    finally:
                        if rgb is not image:
                            rgb.close()
                    self.assertEqual(result.manifest["width"], 64)
        with Image.new("RGB", (40, 20), "red") as image:
            exif = Image.Exif()
            exif[274] = 6
            result = compile_image(encode(image, "JPEG", exif=exif))
        self.assertEqual(result.manifest["source_width"], 20)
        self.assertEqual(result.manifest["source_height"], 40)
        self.assertEqual(result.manifest["content_box"], [16, 0, 48, 64])

    def test_dimension_bytes_and_decode_bounds(self):
        self.assert_error(
            "image_bytes_exceeded", compile_image, b"x" * (MAX_IMAGE_BYTES + 1)
        )
        for value in (
            b"",
            b"not an image",
            bytearray(b"abc"),
            "https://example.invalid/avatar.png",
        ):
            self.assert_error("invalid_image", compile_image, value)
        with Image.new("RGB", (2049, 1), "red") as image:
            self.assert_error("image_dimensions_exceeded", compile_image, encode(image))
        with Image.new("RGB", (2048, 2048), "blue") as image:
            self.assertEqual(
                compile_image(encode(image)).manifest["source_width"], 2048
            )
        with avatar("robot") as image:
            data = encode(image)
        malicious = bytearray(data)
        struct.pack_into(">II", malicious, 16, 50000, 50000)
        struct.pack_into(">I", malicious, 29, zlib.crc32(malicious[12:29]))
        self.assert_error("image_dimensions_exceeded", compile_image, bytes(malicious))
        self.assert_error("invalid_image", compile_image, data[: len(data) // 2])
        self.assert_error("invalid_image", compile_image, data[:40])

    def test_no_animation_or_other_decoders(self):
        with (
            Image.new("RGBA", (16, 16), "red") as first,
            Image.new("RGBA", (16, 16), "blue") as second,
        ):
            for format in ("PNG", "WEBP"):
                with self.subTest(format=format):
                    data = encode(
                        first,
                        format,
                        save_all=True,
                        append_images=[second],
                        duration=100,
                        loop=0,
                    )
                    self.assert_error("animated_image", compile_image, data)
            for format in ("GIF", "TIFF", "BMP"):
                self.assert_error("invalid_image", compile_image, encode(first, format))

    def test_options_are_strict_and_finite(self):
        with avatar("logo") as image:
            data = encode(image)
        cases = {
            "profile": (
                True,
                False,
                "64",
                64.0,
                16,
                128,
                None,
                float("nan"),
                float("inf"),
            ),
            "colors": (True, "24", 2.0, 1, 65, None, float("nan")),
            "dither": (0, 1, "false", None, [], {}),
            "source_key": (b"a" * 32, "x" * 64, "ab" * 31, "ab" * 32 + "\n", 0),
        }
        for option, values in cases.items():
            for value in values:
                with self.subTest(option=option, value=value):
                    self.assert_error(
                        "invalid_" + option, compile_image, data, **{option: value}
                    )

    def test_fully_transparent_rejected_even_with_hidden_art(self):
        with Image.new("RGBA", (64, 64), (255, 128, 64, 0)) as image:
            self.assert_error("empty_alpha", compile_image, encode(image))

    def test_pack_validation_exact_crcs_and_every_index(self):
        with avatar("robot") as image:
            pack = compile_image(encode(image), colors=2).pack
        self.assertEqual(HEADER.size, 64)
        self.assert_error(
            "invalid_pack_crc", validate_pack, pack[:56] + bytes(8) + pack[64:]
        )
        self.assert_error("invalid_pack_header", validate_pack, pack + b"x")
        for offset, value, code in (
            (0, 0, "invalid_pack_header"),
            (12, 0, "invalid_pack_dimensions"),
            (18, 1, "invalid_pack_capabilities"),
            (20, 14, "invalid_pack_capabilities"),
        ):
            invalid = bytearray(pack)
            invalid[offset] = value
            self.assert_error(code, validate_pack, fix_crcs(invalid))
        invalid = bytearray(pack)
        count = HEADER.unpack_from(pack)[4]
        start = 64 + 2 * count
        invalid[start] = count
        invalid[start + 64 * 64] = 0  # transparent index must still be valid
        self.assert_error("invalid_pack_index", validate_pack, fix_crcs(invalid))
        invalid = bytearray(pack)
        invalid[-64 * 64 :] = bytes(64 * 64)
        self.assert_error("empty_alpha", validate_pack, fix_crcs(invalid))
        self.assert_error("invalid_pack_size", validate_pack, b"x" * 65537)

    def test_results_do_not_repr_private_content(self):
        with avatar("logo") as image:
            result = compile_image(encode(image))
        self.assertEqual(repr(result), "Compilation()")
        self.assertNotIn(result.manifest["source_key"], repr(result))


if __name__ == "__main__":
    unittest.main()
