# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

import hashlib
import json
import struct
import sys
from contextlib import nullcontext
from types import SimpleNamespace

import pytest

from musegadget import wakeword_check as checker


def fixtures(tmp_path):
    frames = struct.pack("<" + "f" * (40 * 40), *([0.0] * (40 * 40)))
    expected = struct.pack("<" + "f" * (40 * 3), *([0.0, 1.0, 0.0] * 40))
    (tmp_path / "validation_frames.f32").write_bytes(frames)
    (tmp_path / "validation_expected.f32").write_bytes(expected)
    manifest = {
        "schema_version": 1,
        "clips": 1,
        "frames_per_clip": 40,
        "input_shape": [1, 40, 1, 1],
        "output_shape": [1, 3],
        "frames_sha256": hashlib.sha256(frames).hexdigest(),
        "expected_sha256": hashlib.sha256(expected).hexdigest(),
    }
    (tmp_path / "validation.json").write_text(json.dumps(manifest))
    return manifest


def test_fixture_dimensions_hashes_and_nonfinite_values_are_rejected(tmp_path):
    manifest = fixtures(tmp_path)
    loaded, frames, expected = checker._fixtures(tmp_path)
    assert loaded == manifest and len(frames) == len(expected) == 40
    for key, bad in (
        ("schema_version", 99),
        ("clips", True),
        ("clips", 17),
        ("frames_per_clip", 29),
        ("output_shape", [3]),
        ("frames_sha256", "0" * 64),
    ):
        (tmp_path / "validation.json").write_text(json.dumps({**manifest, key: bad}))
        with pytest.raises(checker.CompatibilityError):
            checker._fixtures(tmp_path)
    fixtures(tmp_path)
    (tmp_path / "validation_expected.f32").write_bytes(b"truncated")
    with pytest.raises(checker.CompatibilityError, match="Truncated"):
        checker._fixtures(tmp_path)
    manifest = fixtures(tmp_path)
    bad = (
        struct.pack("<f", float("nan"))
        + (tmp_path / "validation_frames.f32").read_bytes()[4:]
    )
    (tmp_path / "validation_frames.f32").write_bytes(bad)
    manifest["frames_sha256"] = hashlib.sha256(bad).hexdigest()
    (tmp_path / "validation.json").write_text(json.dumps(manifest))
    with pytest.raises(checker.CompatibilityError, match="nonfinite"):
        checker._fixtures(tmp_path)


def test_sequential_parity_requires_reset_and_independent_programs(monkeypatch):
    class Method:
        counter = 0

    class Program:
        def __init__(self):
            self.method = Method()

        def load_method(self, name):
            assert name == "forward"
            return self.method

    class Runtime:
        def load_program(self, path):
            return Program()

    def execute(torch, method, frame):
        method.counter += 1
        score = method.counter / 1000
        return score, 1 - score, 0.0

    monkeypatch.setattr(checker, "_execute", execute)
    torch = SimpleNamespace(no_grad=nullcontext)
    manifest = {"clips": 2, "frames_per_clip": 40}
    frames = [(0.0,) * 40] * 80
    expected = [(n / 1000, 1 - n / 1000, 0.0) for n in range(1, 41)] * 2
    report = checker._parity(
        Runtime(), torch, "synthetic.pte", manifest, frames, expected
    )
    assert report["max_abs_error"] == 0 and report["reset_replay"] == "bit-identical"
    cached = Program()
    stale = SimpleNamespace(load_program=lambda path: cached)
    with pytest.raises(checker.CompatibilityError, match="Reset/independent"):
        checker._parity(stale, torch, "synthetic.pte", manifest, frames, expected)
    wrong = [(1.0, 0.0, 0.0)] * 80
    with pytest.raises(checker.CompatibilityError, match="Sequential parity"):
        checker._parity(Runtime(), torch, "synthetic.pte", manifest, frames, wrong)


def test_forward_contract_checks_dtype_shape_and_finite_values():
    class Tensor:
        def __init__(self, values, shape=(1, 3), dtype="float32"):
            self.values, self.shape, self.dtype = values, shape, dtype
            self.device = SimpleNamespace(type="cpu")

        def reshape(self, *sizes):
            return self

        def __iter__(self):
            return iter(self.values)

    torch = SimpleNamespace(
        Tensor=Tensor,
        float32="float32",
        tensor=lambda data, dtype: Tensor(data, dtype=dtype),
    )
    valid = SimpleNamespace(execute=lambda inputs: [Tensor([0.2, 0.3, 0.5])])
    assert checker._execute(torch, valid, [0.0] * 40) == (0.2, 0.3, 0.5)
    for bad in (
        [],
        [Tensor([0.0] * 3, shape=(3,))],
        [Tensor([0.0] * 3, dtype="float64")],
        [Tensor([float("nan"), 0, 1])],
    ):
        method = SimpleNamespace(execute=lambda inputs: bad)
        with pytest.raises(checker.CompatibilityError):
            checker._execute(torch, method, [0.0] * 40)


def test_missing_wheel_is_an_actionable_compatibility_failure(monkeypatch):
    monkeypatch.setitem(sys.modules, "torch", None)
    with pytest.raises(
        checker.CompatibilityError, match="Cannot import the supplied wheel"
    ):
        checker.check()


def test_audio_dependencies_checked_without_importing_or_opening_audio(monkeypatch):
    monkeypatch.setitem(sys.modules, "sounddevice", None)
    monkeypatch.setattr(checker.metadata, "version", lambda name: "synthetic")
    monkeypatch.setattr(
        checker.ctypes.util, "find_library", lambda name: "synthetic-portaudio"
    )
    monkeypatch.setattr(
        checker.ctypes, "CDLL", lambda path: SimpleNamespace(Pa_GetVersion=object())
    )
    assert checker.audio_prerequisites()["portaudio_available"] is True

    def missing(path):
        raise OSError("synthetic missing library")

    monkeypatch.setattr(checker.ctypes, "CDLL", missing)
    with pytest.raises(checker.CompatibilityError, match="PortAudio is unavailable"):
        checker.audio_prerequisites()

    def no_package(name):
        raise checker.metadata.PackageNotFoundError(name)

    monkeypatch.setattr(checker.metadata, "version", no_package)
    with pytest.raises(checker.CompatibilityError, match="musegadget\\[voice\\]"):
        checker.audio_prerequisites()


def test_checker_command_emits_structured_success_and_failure(monkeypatch, capsys):
    checked = []

    def check(**options):
        checked.append(options["check_audio"])
        return {"ok": True, "microphone_opened": False}

    monkeypatch.setattr(checker, "check", check)
    assert checker.main(["--check-audio"]) == 0 and checked == [True]
    assert json.loads(capsys.readouterr().out)["microphone_opened"] is False

    def fail(**unused):
        raise checker.CompatibilityError("Synthetic reset mismatch")

    monkeypatch.setattr(checker, "check", fail)
    assert checker.main([]) == 1
    assert json.loads(capsys.readouterr().out) == {
        "ok": False,
        "error": "Synthetic reset mismatch",
    }
