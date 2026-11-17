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

import json
import sys
from types import SimpleNamespace

import pytest

from musegadget import wakeword_check as checker


def test_a_library_that_cannot_run_is_an_actionable_failure(monkeypatch):
    def broken():
        raise OSError("synthetic missing symbol")

    monkeypatch.setattr(checker, "_detector_check", broken)
    with pytest.raises(checker.CompatibilityError, match="could not run"):
        checker.check()


def test_inconsistent_answers_fail_the_check(monkeypatch):
    def inconsistent():
        raise checker.CompatibilityError("The runner answered differently for the same audio")

    monkeypatch.setattr(checker, "_detector_check", inconsistent)
    with pytest.raises(checker.CompatibilityError, match="differently"):
        checker.check()


def test_success_report_never_opens_the_microphone(monkeypatch):
    monkeypatch.setattr(checker, "_detector_check", lambda: {"chunk_and_reset_check": "consistent"})
    report = checker.check()
    assert report["ok"] is True and report["microphone_opened"] is False
    assert report["acoustic_accuracy_verified"] is False


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
