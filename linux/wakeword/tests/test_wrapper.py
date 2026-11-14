# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

"""Signal/wrapper tests without torch; opt in separately to real wheel tests."""

from __future__ import annotations

import ctypes
import gc
import math
import os
import struct
import sys
import unittest
import warnings
from pathlib import Path
from unittest.mock import patch

ROOT = Path(os.environ.get("MUSE_SDK_LINUX", Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(ROOT / "src"))
from musegadget.wakeword import (
    ResearchWakeWordWarning,
    WakeWordDetector,
    WakeWordUnavailableError,
    _WheelModel,
)


class ScriptedModel:
    def __init__(self, path):
        self.frames = 0
        self.fail_at = None
        self.error = RuntimeError("scripted wheel inference error")
        self.closed = False

    def posteriors(self, features):
        self.frames += 1
        if self.frames == self.fail_at:
            raise self.error
        return (1.0, 0.0, 0.0)

    def reset(self):
        self.frames = 0

    def close(self):
        self.closed = True


class ContractTests(unittest.TestCase):
    def test_bad_sensitivity(self):
        with self.assertRaises(ValueError):
            WakeWordDetector(sensitivity="maximum")

    def test_missing_custom_model(self):
        with self.assertRaises(WakeWordUnavailableError):
            WakeWordDetector(model_path="/nonexistent/hey_muse.pte")

    def test_missing_library_actionable(self):
        with patch.dict(
            os.environ, {"MUSE_WAKEWORD_LIBRARY": "/nonexistent/library.so"}
        ):
            with self.assertRaisesRegex(WakeWordUnavailableError, "build_signal.sh"):
                WakeWordDetector()

    def test_import_is_lazy(self):
        # No construction/import side effects: check the module in a fresh namespace.
        import ast

        tree = ast.parse((ROOT / "src/musegadget/wakeword.py").read_text())
        top_level_imports = [
            node for node in tree.body if isinstance(node, (ast.Import, ast.ImportFrom))
        ]
        modules = [
            node.module if isinstance(node, ast.ImportFrom) else node.names[0].name
            for node in top_level_imports
        ]
        self.assertNotIn("torch", modules)
        self.assertNotIn("executorch.runtime", modules)


class SignalWrapperTests(unittest.TestCase):
    def setUp(self):
        self.model_patch = patch("musegadget.wakeword._WheelModel", ScriptedModel)
        self.model_patch.start()
        self.addCleanup(self.model_patch.stop)
        warning_context = warnings.catch_warnings()
        warning_context.__enter__()
        warnings.simplefilter("ignore", ResearchWakeWordWarning)
        self.addCleanup(warning_context.__exit__, None, None, None)
        self.detector = WakeWordDetector()
        self.addCleanup(self.detector.close)
        self.library = self.detector._library
        self.library.ww_signal_frames_seen.argtypes = [ctypes.c_void_p]
        self.library.ww_signal_frames_seen.restype = ctypes.c_uint64

    def frames_seen(self):
        return self.library.ww_signal_frames_seen(self.detector._handle)

    def test_byte_chunking_reset_and_full_consumption_after_fire(self):
        samples = [
            int(5000 * math.sin(i * 2 * math.pi * 440 / 16000)) for i in range(16000)
        ]
        pcm = struct.pack(f"<{len(samples)}h", *samples)
        self.assertTrue(self.detector.feed(pcm))
        self.assertEqual(self.frames_seen(), 49)
        self.assertEqual(self.detector._model.frames, 49)
        self.detector.reset()
        chunk_fire = False
        for start in range(0, len(pcm), 137):
            chunk_fire |= self.detector.feed(pcm[start : start + 137])
        self.assertTrue(chunk_fire)
        self.assertEqual(self.frames_seen(), 49)
        self.assertEqual(self.detector._model.frames, 49)
        self.detector.feed(b"\x7f")
        self.detector.reset()
        self.assertFalse(self.detector.feed(b"\0" * 1023))
        self.assertEqual(self.frames_seen(), 0)
        self.assertFalse(self.detector.feed(b"\0"))
        self.assertEqual(self.frames_seen(), 1)

    def test_empty_and_closed(self):
        self.assertFalse(self.detector.feed(b""))
        with self.assertRaises(TypeError):
            self.detector.feed(bytearray(640))
        model = self.detector._model
        self.detector.close()
        self.detector.close()
        self.assertTrue(model.closed)
        self.assertIsNone(self.detector._callback)
        for operation in (lambda: self.detector.feed(b""), self.detector.reset):
            with self.assertRaisesRegex(RuntimeError, "closed"):
                operation()

    def test_repeated_reset(self):
        for _ in range(10):
            self.detector.reset()
            self.assertFalse(self.detector.feed(bytes(640)))
        self.assertEqual(self.frames_seen(), 0)

    def test_context_warning_and_cleanup(self):
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always", ResearchWakeWordWarning)
            with WakeWordDetector(sensitivity="low") as detector:
                self.assertFalse(detector.feed(bytes(640)))
            self.assertEqual(len(caught), 1)
            self.assertIn(
                "not validated on real microphone recordings",
                str(caught[0].message),
            )
        with self.assertRaises(RuntimeError):
            detector.reset()

    def test_warning_as_error(self):
        with warnings.catch_warnings():
            warnings.simplefilter("error", ResearchWakeWordWarning)
            with self.assertRaises(ResearchWakeWordWarning):
                WakeWordDetector()
        gc.collect()

    def test_callback_error_propagates_and_poison_requires_reset(self):
        model = self.detector._model
        model.fail_at = 3
        with self.assertRaisesRegex(RuntimeError, "scripted wheel") as caught:
            self.detector.feed(bytes(5000))
        self.assertIs(caught.exception, model.error)
        self.assertEqual(model.frames, 3)
        with self.assertRaisesRegex(RuntimeError, "reset required"):
            self.detector.feed(b"")
        model.fail_at = None
        self.detector.reset()
        self.assertFalse(self.detector.feed(bytes(1024)))
        self.assertEqual(model.frames, 1)

    def test_callback_base_exception_not_swallowed(self):
        self.detector._model.fail_at = 1
        self.detector._model.error = KeyboardInterrupt("scripted cancellation")
        with self.assertRaises(KeyboardInterrupt):
            self.detector.feed(bytes(1024))
        self.assertTrue(self.detector._poisoned)

    def test_failed_reset_keeps_stream_unavailable(self):
        with patch.object(
            self.detector._model, "reset", side_effect=OSError("model unreadable")
        ):
            with self.assertRaises(OSError):
                self.detector.reset()
        with self.assertRaisesRegex(RuntimeError, "reset required"):
            self.detector.feed(b"")
        self.detector.reset()
        self.assertFalse(self.detector.feed(bytes(1024)))


@unittest.skipUnless(
    os.environ.get("MUSE_WAKEWORD_RUN_WHEEL_TESTS") == "1",
    "requires explicit real-wheel test environment",
)
class WheelRuntimeTests(unittest.TestCase):
    def setUp(self):
        warning_context = warnings.catch_warnings()
        warning_context.__enter__()
        warnings.simplefilter("ignore", ResearchWakeWordWarning)
        self.addCleanup(warning_context.__exit__, None, None, None)

    def test_synthetic_public_runtime_checker(self):
        from musegadget.wakeword_check import check

        report = check()
        self.assertTrue(report["ok"])
        self.assertEqual(report["synthetic_frames"], 300)
        self.assertEqual(report["reset_replay"], "bit-identical")
        self.assertEqual(report["independent_instances"], "bit-identical")

    def test_actual_pcm_chunking(self):
        with WakeWordDetector() as detector:
            first = detector.feed(bytes(32000))
            detector.reset()
            second = False
            for start in range(0, 32000, 137):
                second |= detector.feed(bytes(min(137, 32000 - start)))
            self.assertEqual(first, second)

    def test_corrupt_model_fails_closed(self):
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "broken.pte"
            path.write_bytes(b"not an ExecuTorch program")
            with self.assertRaises(WakeWordUnavailableError):
                WakeWordDetector(model_path=path)

    def test_wheel_instances_do_not_share_history(self):
        path = ROOT / "src/musegadget/data/wakeword/hey_muse_xnnpack.pte"
        model = _WheelModel(path)
        other = _WheelModel(path)
        try:
            features = [math.sin(i * 0.3) for i in range(40)]
            reference = [model.posteriors(features) for _ in range(80)]
            for index in range(80):
                model.posteriors([0.0] * 40)
                self.assertEqual(other.posteriors(features), reference[index])
            model.reset()
            self.assertEqual([model.posteriors(features) for _ in range(80)], reference)
        finally:
            model.close()
            other.close()


if __name__ == "__main__":
    unittest.main()
