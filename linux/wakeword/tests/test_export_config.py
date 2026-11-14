# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0.

"""Tests for exporter config validation; no export/build performed here."""

from __future__ import annotations

import copy
import importlib.util
import os
import unittest
from pathlib import Path

# The single exporter lives with the ESP wake-word component. The env var, if
# set, points at a checkout root (the test only ever ran from this checkout).
ROOT = Path(os.environ.get("MUSE_WAKEWORD_SOURCE", Path(__file__).resolve().parents[3]))
# The frontend header lives in the ESP32 tree; there is no copy here.
HEADER = (
    Path(__file__).resolve().parents[3]
    / "esp32"
    / "components"
    / "muse_wakeword"
    / "ww_frontend_config.h"
)
spec = importlib.util.spec_from_file_location(
    "hey_muse_export",
    ROOT / "esp32/components/muse_wakeword/tools/export_hey_muse.py",
)
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)


class ExportConfigTests(unittest.TestCase):
    def setUp(self):
        import re

        header = HEADER
        self.header = header
        self.config = dict(exporter.EXPECTED_FRONTEND)
        text = header.read_text()
        for key, symbol in (
            ("norm_mean", "kWwFrontendNormMean"),
            ("norm_std", "kWwFrontendNormStd"),
        ):
            match = re.search(rf"{symbol}\[40\]\s*=\s*\{{([^}}]+)\}}", text)
            self.config[key] = [
                float(value.strip().rstrip("f"))
                for value in match.group(1).split(",")
                if value.strip()
            ]

    def test_frozen_frontend_matches(self):
        exporter.validate_frontend(self.config, self.header)

    def test_reject_wrong_sample_rate(self):
        self.config["sample_rate"] = 24000
        with self.assertRaisesRegex(ValueError, "sample_rate"):
            exporter.validate_frontend(self.config, self.header)

    def test_reject_changed_normalization(self):
        changed = copy.deepcopy(self.config)
        changed["norm_mean"][0] += 0.1
        with self.assertRaisesRegex(ValueError, "norm_mean"):
            exporter.validate_frontend(changed, self.header)

    def test_reject_nonfinite_normalization(self):
        self.config["norm_std"][0] = float("nan")
        with self.assertRaises(ValueError):
            exporter.validate_frontend(self.config, self.header)

    def test_reject_truncated_normalization(self):
        self.config["norm_std"].pop()
        with self.assertRaises(ValueError):
            exporter.validate_frontend(self.config, self.header)

    def test_generated_header_matches_committed_header(self):
        # The committed header is the exporter's output, byte for byte.
        import json
        import tempfile

        sidecar = json.loads(
            (
                ROOT
                / "linux/src/musegadget/data/wakeword/hey_muse_xnnpack.json"
            ).read_text()
        )
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "ww_frontend_config.h"
            exporter.write_frontend_header(
                out, sidecar["frontend"], sidecar["checkpoint_sha256"]
            )
            self.assertEqual(out.read_bytes(), self.header.read_bytes())


if __name__ == "__main__":
    unittest.main()
