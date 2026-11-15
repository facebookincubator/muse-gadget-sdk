# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

"""The Linux runner is the ESP32 runner: same sources, same model, same pins."""

import re
from pathlib import Path

LINUX = Path(__file__).resolve().parents[1]
REPO = LINUX.parent
ESP_WAKEWORD = REPO / "esp32" / "components" / "muse_wakeword"
BUILD = (LINUX / "wakeword" / "build.sh").read_text()


def test_linux_builds_the_esp32_runner_sources_and_model():
    for name in ("ww_runner_espnn.cpp", "ww_frontend.c", "ww_detector.c", "hey_muse_pte.S.in"):
        assert (ESP_WAKEWORD / name).is_file()
        assert f'$WW/{name}' in BUILD or name in BUILD
    assert '"$WW/models/hey_muse.pte"' in BUILD
    assert '"$WW/host/et_pal.cpp"' in BUILD
    # No copies or second model under linux/: its only C is the checker
    # and the tap's resampler, neither part of the runner.
    runner_names = {p.name for p in ESP_WAKEWORD.rglob("*") if p.suffix in (".c", ".cpp", ".h", ".pte")}
    for path in (LINUX / "wakeword").rglob("*"):
        assert path.name not in runner_names, path
        if path.suffix in (".c", ".cpp", ".h", ".pte"):
            assert path.relative_to(LINUX / "wakeword").as_posix() in ("ww_resample.c", "tools/ww_run.c"), path
    assert not list((LINUX / "src").rglob("*.pte"))


def test_registry_pins_match_the_firmware():
    manifest = (ESP_WAKEWORD / "idf_component.yml").read_text()
    pins = dict(re.findall(r'^  ([a-z0-9-]+/[a-z0-9_-]+):\n    version: "==([^"]+)"', manifest, re.M))
    archives = dict(re.findall(r'"([a-z0-9-]+/[a-z0-9_-]+)\|([^|]+)\|', BUILD))
    assert pins == {name: archives[name] for name in pins}
    assert archives["espressif/esp-nn"] == "1.5.0"   # what the firmware's lock resolves today
    for name, version, file, sha in re.findall(r'"([a-z0-9-]+/[a-z0-9_-]+)\|([^|]+)\|([^|]+)\|([0-9a-f]{64})"', BUILD):
        assert file.endswith(f"-v{version}.zip") and name.split("/")[1] in file


def test_shipped_metadata_has_no_local_paths():
    for path in (ESP_WAKEWORD / "models").iterdir():
        if path.suffix in (".json", ".md"):
            text = path.read_text()
            assert "/Users/" not in text and "/home/" not in text and "/data/users" not in text, path
