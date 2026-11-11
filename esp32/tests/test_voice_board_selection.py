# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0

"""Execute voice-board selection and board-helper wiring without ESP-IDF."""

import itertools
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SELECTOR = ROOT / "main" / "voice_board_select.cmake"
PRODUCT_SOURCES = {
    "VOICE_PE": "voice_board.c",
    "RESPEAKER_LITE": "voice_board_respeaker_lite.c",
    "RESPEAKER_XVF3800": "voice_board_xvf3800.c",
    "WAVESHARE_154G": "voice_board_waveshare_154g.c",
}
PRODUCT_STATUS = {
    "VOICE_PE": "VOICE_RING",
    "RESPEAKER_LITE": "RESPEAKER_LITE",
    "RESPEAKER_XVF3800": "NONE",
    "WAVESHARE_154G": "WAVESHARE_EPD154G",
}


class VoiceBoardSelectionTests(unittest.TestCase):
    def run_selector(self, products=(), statuses=(), source=None, error=None):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "select.cmake"
            lines = ["cmake_minimum_required(VERSION 3.16)", "set(GADGET_SRCS)"]
            lines += [f"set(CONFIG_HOMEHUB_VOICE_BOARD_{board} TRUE)" for board in products]
            lines += [f"set(CONFIG_HOMEHUB_LED_BACKEND_{status} TRUE)" for status in statuses]
            lines.append(f'include("{SELECTOR.as_posix()}")')
            if source is not None:
                lines.append(
                    f'if(NOT "${{GADGET_SRCS}}" STREQUAL "{source}")\n'
                    '  message(FATAL_ERROR "wrong source: ${GADGET_SRCS}")\n'
                    "endif()"
                )
            script.write_text("\n".join(lines) + "\n")
            result = subprocess.run(
                ["cmake", "-P", str(script)], capture_output=True, text=True, timeout=10
            )
        output = result.stdout + result.stderr
        normalized_output = " ".join(output.split())
        if error is None:
            self.assertEqual(result.returncode, 0, output)
        else:
            self.assertNotEqual(result.returncode, 0, output)
            self.assertRegex(normalized_output, error)

    def test_every_valid_board_maps_to_one_source(self):
        for product, source in PRODUCT_SOURCES.items():
            with self.subTest(product=product):
                self.run_selector([product], [PRODUCT_STATUS[product]], source=source)

    def test_unspecified_and_zero_selection_have_no_fallback(self):
        for products in ((), ("UNSPECIFIED",)):
            with self.subTest(products=products):
                self.run_selector(products, ["NONE"], error="exactly one.*UNSPECIFIED has no fallback")

    def test_every_conflicting_pair_is_rejected(self):
        for product in PRODUCT_SOURCES:
            with self.subTest(product=product, unspecified=True):
                self.run_selector(
                    [product, "UNSPECIFIED"],
                    [PRODUCT_STATUS[product]],
                    error="UNSPECIFIED conflicts with",
                )
        for products in itertools.combinations(PRODUCT_SOURCES, 2):
            statuses = sorted({PRODUCT_STATUS[product] for product in products})
            with self.subTest(products=products):
                self.run_selector(products, statuses, error="selects multiple drivers")
        self.run_selector(PRODUCT_SOURCES, PRODUCT_STATUS.values(), error="selects multiple drivers")

    def test_invalid_product_status_pairs_are_rejected(self):
        cases = [
            ("VOICE_PE", "NONE"),
            ("VOICE_PE", "RESPEAKER_LITE"),
            ("RESPEAKER_LITE", "VOICE_RING"),
            ("RESPEAKER_LITE", "NONE"),
        ]
        cases += [
            ("RESPEAKER_XVF3800", status)
            for status in (None, "VOICE_RING", "RESPEAKER_LITE")
        ]
        cases += [
            ("WAVESHARE_154G", status)
            for status in (None, "NONE", "VOICE_RING", "RESPEAKER_LITE")
        ]
        for product, status in cases:
            with self.subTest(product=product, status=status):
                self.run_selector(
                    [product], [] if status is None else [status], error="requires HOMEHUB_LED_BACKEND"
                )

    def test_listed_product_without_a_source_branch_is_rejected(self):
        selector = SELECTOR.read_text().replace(
            "    RESPEAKER_XVF3800\n", "    RESPEAKER_XVF3800\n    NEW_BOARD\n", 1
        )
        self.assertIn("NEW_BOARD", selector)
        with tempfile.TemporaryDirectory() as directory:
            copy = Path(directory) / "voice_board_select.cmake"
            copy.write_text(selector)
            script = Path(directory) / "select.cmake"
            script.write_text(
                "cmake_minimum_required(VERSION 3.16)\nset(GADGET_SRCS)\n"
                "set(CONFIG_HOMEHUB_VOICE_BOARD_NEW_BOARD TRUE)\n"
                "set(CONFIG_HOMEHUB_LED_BACKEND_NONE TRUE)\n"
                f'include("{copy.as_posix()}")\n'
            )
            result = subprocess.run(
                ["cmake", "-P", str(script)], capture_output=True, text=True, timeout=10
            )
        output = " ".join((result.stdout + result.stderr).split())
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("HOMEHUB_VOICE_BOARD_NEW_BOARD has no driver source", output)

    def test_kconfig_covers_selector_and_preserves_legacy_defaults(self):
        kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text()
        selector = SELECTOR.read_text()
        config_boards = set(re.findall(r"config HOMEHUB_VOICE_BOARD_([A-Z0-9_]+)", kconfig))
        products_match = re.search(
            r"set\(HOMEHUB_VOICE_BOARD_PRODUCTS\s+([^)]*)\)", selector, re.DOTALL
        )
        self.assertIsNotNone(products_match)
        selector_boards = set(products_match.group(1).split())
        self.assertEqual(config_boards, set(PRODUCT_SOURCES) | {"UNSPECIFIED"})
        self.assertEqual(selector_boards, set(PRODUCT_SOURCES))

        voice = re.search(
            r"^    config HOMEHUB_VOICE\n(.*?)^    choice HOMEHUB_VOICE_BOARD",
            kconfig,
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(voice)
        self.assertIn("depends on SPIRAM\n", voice.group(1))
        self.assertNotIn("HOMEHUB_LED_BACKEND", voice.group(1).split("depends on SPIRAM", 1)[0])
        defaults = re.findall(r"^        default y.*$", voice.group(1), re.MULTILINE)
        self.assertEqual(
            [line.strip() for line in defaults],
            [
                "default y if HOMEHUB_LED_BACKEND_VOICE_RING",
                "default y if HOMEHUB_LED_BACKEND_RESPEAKER_LITE",
            ],
        )
        self.assertIn(
            "default HOMEHUB_VOICE_BOARD_VOICE_PE if HOMEHUB_LED_BACKEND_VOICE_RING", kconfig
        )
        self.assertIn(
            "default HOMEHUB_VOICE_BOARD_RESPEAKER_LITE if HOMEHUB_LED_BACKEND_RESPEAKER_LITE",
            kconfig,
        )
        self.assertIn("default HOMEHUB_VOICE_BOARD_UNSPECIFIED", kconfig)

    def test_main_includes_selector_only_for_voice_and_legacy_overlays_are_explicit(self):
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text()
        voice = re.search(
            r'^if\(CONFIG_HOMEHUB_VOICE\)\n(.*?)^endif\(\)', cmake, re.MULTILINE | re.DOTALL
        )
        self.assertIsNotNone(voice)
        self.assertIn('include("voice_board_select.cmake")', voice.group(1))
        self.assertEqual(cmake.count('include("voice_board_select.cmake")'), 1)
        for overlay, product in (
            ("home-assistant-voice", "VOICE_PE"),
            ("seeed-respeaker-lite", "RESPEAKER_LITE"),
            ("waveshare-s3-epaper-154g", "WAVESHARE_154G"),
        ):
            text = (ROOT / "devices" / f"sdkconfig.{overlay}").read_text()
            self.assertIn(f"CONFIG_HOMEHUB_VOICE_BOARD_{product}=y\n", text)

    def test_board_helper_syntax_usage_and_real_command_wiring(self):
        helper = ROOT / "tools" / "board.sh"
        subprocess.run(["bash", "-n", str(helper)], check=True, timeout=10)
        usage = subprocess.run(
            ["bash", str(helper), "--help"], capture_output=True, text=True, timeout=10
        )
        self.assertEqual(usage.returncode, 0, usage.stdout + usage.stderr)
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            fake_idf = tmp / "idf.py"
            fake_idf.write_text('#!/bin/sh\nprintf \'%s\\n\' "$@" > "$IDF_ARGS"\n')
            fake_idf.chmod(0o755)
            env = os.environ.copy()
            env["PATH"] = f"{tmp}{os.pathsep}{env['PATH']}"
            for board in (
                "home-assistant-voice",
                "seeed-respeaker-lite",
                "seeed-respeaker-xvf3800",
            ):
                with self.subTest(board=board):
                    self.assertIn(board, usage.stdout)
                    args_file = tmp / "args"
                    env["IDF_ARGS"] = str(args_file)
                    result = subprocess.run(
                        ["bash", str(helper), board, "build"],
                        env=env,
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    args = args_file.read_text().splitlines()
                    build_dir = ROOT / f"build-{board}"
                    self.assertEqual(
                        args,
                        [
                            "-B",
                            str(build_dir),
                            "-DIDF_TARGET=esp32s3",
                            f"-DSDKCONFIG={build_dir / 'sdkconfig'}",
                            "-DSDKCONFIG_DEFAULTS="
                            f"sdkconfig.defaults;devices/sdkconfig.{board}",
                            "build",
                        ],
                    )


if __name__ == "__main__":
    unittest.main()
