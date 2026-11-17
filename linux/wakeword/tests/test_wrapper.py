# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The native Hey Muse library through musegadget.wakeword.

Contract tests run anywhere. The rest need the library built by
linux/wakeword/build.sh, found through MUSE_WAKEWORD_LIBRARY or the default
build directory (CI builds it first), and skip without it. All audio here is
self-made (tones and silence); MUSE_WAKEWORD_CLIPS can point at a directory
of 16 kHz WAV files with known wake words for a local end-to-end run.
"""

from __future__ import annotations

import ast
import math
import os
import struct
import sys
import unittest
import warnings
import wave
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src"))
from musegadget.wakeword import (  # noqa: E402
    ResearchWakeWordWarning,
    WakeWordDetector,
    WakeWordUnavailableError,
    _candidates,
)

MODEL = ROOT.parent / "esp32" / "components" / "muse_wakeword" / "models" / "hey_muse.pte"


def library_available() -> bool:
    return any(path.is_file() for path in _candidates())


def tones(seconds=6.0, rate=16000) -> bytes:
    samples = []
    for i in range(int(seconds * rate)):
        t = i / rate
        samples.append(int(7000 * math.sin(2 * math.pi * (250 + 150 * t) * t) * (int(t * 3) % 2)))
    return struct.pack("<%dh" % len(samples), *samples)


def detector(**options) -> WakeWordDetector:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", ResearchWakeWordWarning)
        return WakeWordDetector(**options)


class ContractTests(unittest.TestCase):
    def test_bad_sensitivity(self):
        with self.assertRaises(ValueError):
            WakeWordDetector(sensitivity="maximum")

    def test_missing_custom_model(self):
        with self.assertRaises(WakeWordUnavailableError):
            WakeWordDetector(model_path="/nonexistent/hey_muse.pte")

    def test_an_empty_model_file_is_refused_not_replaced(self):
        import tempfile

        with tempfile.NamedTemporaryFile(suffix=".pte") as f:
            with self.assertRaises(WakeWordUnavailableError):
                WakeWordDetector(model_path=f.name)

    def test_missing_library_is_actionable(self):
        with patch.dict(os.environ, {"MUSE_WAKEWORD_LIBRARY": "/nonexistent/library.so"}):
            with self.assertRaisesRegex(WakeWordUnavailableError, "setup.sh"):
                WakeWordDetector()

    def test_library_lookup_order(self):
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop("MUSE_WAKEWORD_LIBRARY", None)
            with patch.object(sys, "prefix", "/opt/env"):
                found = _candidates()
        self.assertEqual(found[0], Path("/opt/env/lib/musegadget/libmuse_wakeword.so"))
        self.assertEqual(found[1].parts[-3:], ("wakeword", "build", "libmuse_wakeword.so"))
        with patch.dict(os.environ, {"MUSE_WAKEWORD_LIBRARY": "/x/lib.so"}):
            self.assertEqual(_candidates(), [Path("/x/lib.so")])
        # A file in the current directory stays that file, not a bare name
        # ctypes would look up on the loader's search path.
        with patch.dict(os.environ, {"MUSE_WAKEWORD_LIBRARY": "./lib.so"}):
            self.assertEqual(_candidates(), [Path.cwd() / "lib.so"])

    def test_model_files_are_read_once_and_bounded(self):
        import tempfile
        from musegadget.wakeword import _MAX_MODEL_BYTES, _read_model

        with tempfile.TemporaryDirectory() as tmp:
            big = Path(tmp) / "big.pte"
            with open(big, "wb") as f:
                f.truncate(_MAX_MODEL_BYTES + 1)
            with self.assertRaises(OSError):
                _read_model(big)
            # A FIFO with no writer: opening it must neither block nor read.
            fifo = Path(tmp) / "fifo.pte"
            os.mkfifo(fifo)
            with self.assertRaisesRegex(OSError, "not a regular file"):
                _read_model(fifo)
            small = Path(tmp) / "small.pte"
            small.write_bytes(b"x" * 10)
            self.assertEqual(_read_model(small), b"x" * 10)

    def test_import_loads_nothing(self):
        tree = ast.parse((ROOT / "src/musegadget/wakeword.py").read_text())
        modules = [
            node.module if isinstance(node, ast.ImportFrom) else node.names[0].name
            for node in tree.body
            if isinstance(node, (ast.Import, ast.ImportFrom))
        ]
        for heavy in ("torch", "executorch", "executorch.runtime", "numpy"):
            self.assertNotIn(heavy, modules)
        top_level_calls = [
            node for node in tree.body
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
        ]
        self.assertEqual(top_level_calls, [])


@unittest.skipUnless(library_available(), "build the library with linux/wakeword/build.sh")
class NativeLibraryTests(unittest.TestCase):
    def test_tones_and_silence_never_wake(self):
        with detector() as d:
            self.assertFalse(d.feed(b"\0" * 64000))
            self.assertFalse(d.feed(tones()))

    def test_split_samples_and_chunking_are_consistent(self):
        pcm = tones()
        with detector() as d:
            whole = d.feed(pcm)
            for size in (1, 3, 641, 32001):
                d.reset()
                fired = False
                for start in range(0, len(pcm), size):
                    fired |= d.feed(pcm[start:start + size])
                self.assertEqual(fired, whole, size)

    def test_construction_warns_about_synthetic_training(self):
        with self.assertWarns(ResearchWakeWordWarning):
            WakeWordDetector().close()

    def test_close_is_idempotent_and_final(self):
        d = detector()
        d.close()
        d.close()
        with self.assertRaises(RuntimeError):
            d.feed(b"\0\0")
        with self.assertRaises(RuntimeError):
            d.reset()

    def test_input_must_be_bytes(self):
        with detector() as d, self.assertRaises(TypeError):
            d.feed(bytearray(4))

    def test_the_shipped_model_loads_from_a_file_too(self):
        with detector(model_path=MODEL) as d:
            self.assertFalse(d.feed(tones()))

    def test_a_file_that_is_not_the_model_is_refused(self):
        import tempfile

        with tempfile.NamedTemporaryFile(suffix=".pte") as f:
            f.write(MODEL.read_bytes()[:4096])
            f.flush()
            with self.assertRaises(WakeWordUnavailableError):
                detector(model_path=f.name)

    def test_the_environment_copy_is_found_without_an_override(self):
        import shutil
        import tempfile

        built = next(path for path in _candidates() if path.is_file())
        with tempfile.TemporaryDirectory() as prefix:
            copy = Path(prefix) / "lib" / "musegadget" / "libmuse_wakeword.so"
            copy.parent.mkdir(parents=True)
            shutil.copyfile(built, copy)
            environment = {k: v for k, v in os.environ.items() if k != "MUSE_WAKEWORD_LIBRARY"}
            with patch.dict(os.environ, environment, clear=True), patch.object(sys, "prefix", prefix):
                self.assertEqual(_candidates()[0], copy)
                with detector() as d:
                    self.assertFalse(d.feed(tones(1.0)))

    def test_a_model_planning_huge_buffers_is_refused(self):
        # forward's two planned buffers declared at 2 GiB and 2 GiB + 32,
        # whose rounded sum wraps a 32-bit size_t.
        import tempfile

        data = bytearray(MODEL.read_bytes())
        for offset, fmt, value in ((0x10, "<Q", 0x9D8), (0xC0, "<I", 0x8FC), (0x9BC, "<I", 3),
                                   (0x9C0, "<q", 0), (0x9C8, "<q", 0x80000000), (0x9D0, "<q", 0x80000020)):
            struct.pack_into(fmt, data, offset, value)
        with tempfile.NamedTemporaryFile(suffix=".pte") as f:
            f.write(bytes(data))
            f.flush()
            with self.assertRaises(WakeWordUnavailableError):
                detector(model_path=f.name)

    def test_the_native_resampler_matches_the_python_filter(self):
        from musegadget.wakeword_input import Pcm24To16

        samples = []
        for i in range(24000):
            t = i / 24000
            samples.append(int(12000 * math.sin(2 * math.pi * (300 + 3000 * t) * t)))
        pcm = struct.pack("<%dh" % len(samples), *samples)
        reference = struct.unpack("<%dh" % (len(pcm) // 3), Pcm24To16().feed(pcm)[: len(pcm) // 3 * 2])
        with detector() as d:
            for size in (2, 6, 642, 1920, len(pcm)):
                resampler = d.resampler()
                out = b"".join(resampler.feed(pcm[i:i + size]) for i in range(0, len(pcm), size))
                got = struct.unpack("<%dh" % (len(out) // 2), out)
                self.assertEqual(len(got), len(reference))
                self.assertLessEqual(max(abs(a - b) for a, b in zip(got, reference)), 1, size)
                resampler.reset()
                self.assertEqual(resampler.feed(pcm[:1920]), out[:1280])
                resampler.close()

    def test_independent_instances_have_independent_state(self):
        pcm = tones()
        with detector() as a, detector() as b:
            a.feed(pcm[: len(pcm) // 2])
            self.assertEqual(b.feed(pcm), detector().feed(pcm))


@unittest.skipUnless(
    library_available() and os.environ.get("MUSE_WAKEWORD_CLIPS"),
    "set MUSE_WAKEWORD_CLIPS to a directory of 16 kHz WAV clips with a wake word each",
)
class ClipTests(unittest.TestCase):
    def test_the_clips_wake_it(self):
        # Detection isn't perfect: report how many clips woke it, and
        # require most of them to.
        clips = sorted(Path(os.environ["MUSE_WAKEWORD_CLIPS"]).glob("*.wav"))
        self.assertTrue(clips)
        caught = 0
        with detector() as d:
            for clip in clips:
                with wave.open(str(clip)) as w:
                    self.assertEqual((w.getframerate(), w.getnchannels(), w.getsampwidth()), (16000, 1, 2))
                    pcm = w.readframes(w.getnframes())
                d.reset()
                caught += d.feed(pcm)
        print(f"{caught} of {len(clips)} clips woke the detector", file=sys.stderr)
        self.assertGreater(caught, len(clips) // 2)


if __name__ == "__main__":
    unittest.main()
