# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

import hashlib
import json
import sys
from pathlib import Path
from types import SimpleNamespace

from musegadget.wakeword import _WheelModel

SIGNAL = Path(__file__).resolve().parents[1] / "wakeword"
REPO_ROOT = Path(__file__).resolve().parents[2]
ESP_WAKEWORD = REPO_ROOT / "esp32" / "components" / "muse_wakeword"


def test_signal_sources_are_the_esp32_sources():
    expected = [
        "ww_frontend.c",
        "ww_frontend.h",
        "ww_frontend_config.h",
        "ww_detector.c",
        "ww_detector.h",
    ]
    for filename in expected:
        assert (ESP_WAKEWORD / filename).is_file()
    assert (ESP_WAKEWORD / "include" / "ww_runner.h").is_file()
    for filename in [*expected, "ww_runner.h"]:
        assert not (SIGNAL / filename).exists()
    sidecar = json.loads(
        (
            REPO_ROOT
            / "linux"
            / "src"
            / "musegadget"
            / "data"
            / "wakeword"
            / "hey_muse_xnnpack.json"
        ).read_text()
    )
    header_sha = hashlib.sha256(
        (ESP_WAKEWORD / "ww_frontend_config.h").read_bytes()
    ).hexdigest()
    assert header_sha == sidecar["frontend_header_sha256"]


def test_shipped_wakeword_metadata_has_no_local_paths():
    # Provenance sidecars ship in the public package: no machine paths.
    data = REPO_ROOT / "linux" / "src" / "musegadget" / "data" / "wakeword"

    def strings(value):
        if isinstance(value, str):
            yield value
        elif isinstance(value, dict):
            for key, item in value.items():
                yield key
                yield from strings(item)
        elif isinstance(value, list):
            for item in value:
                yield from strings(item)

    for name in ("hey_muse_xnnpack.json", "validation.json"):
        for text in strings(json.loads((data / name).read_text())):
            assert not text.startswith(("/", "~")), (name, text)
            assert "/Users/" not in text and "/home/" not in text, (name, text)

def test_wheel_loader_keeps_model_file_ownership_in_runtime(tmp_path, monkeypatch):
    # ESP embeds aligned flash bytes; Linux must continue to use the file API.
    path = tmp_path / "model with spaces.pte"
    path.write_bytes(b"synthetic model for public-loader contract tests")
    loaded = []
    methods = []

    def tensor_info(shape):
        return SimpleNamespace(sizes=lambda: shape, dtype=lambda: 6)

    meta = SimpleNamespace(
        num_inputs=lambda: 1,
        num_outputs=lambda: 1,
        input_tensor_meta=lambda index: tensor_info((1, 40, 1, 1)),
        output_tensor_meta=lambda index: tensor_info((1, 3)),
    )

    def load_program(argument):
        assert isinstance(argument, Path) and argument == path
        loaded.append(argument)
        method = SimpleNamespace(metadata=meta)
        methods.append(method)
        return SimpleNamespace(
            method_names={"forward"}, load_method=lambda name: method
        )

    runtime = SimpleNamespace(
        backend_registry=SimpleNamespace(
            is_available=lambda name: name == "XnnpackBackend"
        ),
        load_program=load_program,
    )
    monkeypatch.setitem(sys.modules, "torch", SimpleNamespace())
    monkeypatch.setitem(
        sys.modules,
        "executorch.runtime",
        SimpleNamespace(Runtime=SimpleNamespace(get=lambda: runtime)),
    )
    model = _WheelModel(path)
    try:
        assert model._method is methods[0]
        model.reset()
        assert loaded == [path, path]
        assert model._method is methods[1] and methods[0] is not methods[1]
    finally:
        model.close()
    assert model._method is None and model._program is None
