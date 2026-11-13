#!/usr/bin/env python3
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

"""Headless smoke and deterministic framebuffer tests for the UI simulator."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

WIDTH = HEIGHT = 412
HERE = Path(__file__).resolve().parent
SCENARIOS = tuple(sorted((HERE / "scenarios").glob("*.txt")))


def read_ppm(path: Path) -> bytes:
    raw = path.read_bytes()
    header = f"P6\n{WIDTH} {HEIGHT}\n255\n".encode()
    assert raw.startswith(header), f"{path}: wrong PPM header"
    pixels = raw[len(header) :]
    assert len(pixels) == WIDTH * HEIGHT * 3, f"{path}: truncated framebuffer"
    assert len(set(pixels)) > 8, f"{path}: framebuffer has too few colours"
    return pixels


def render(binary: Path, scenario: Path, output: Path) -> tuple[str, subprocess.CompletedProcess[str]]:
    env = {**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}
    proc = subprocess.run(
        [
            str(binary),
            "--headless",
            "--scenario",
            str(scenario),
            "--run-ms",
            "200",
            "--screenshot",
            str(output),
        ],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    assert proc.returncode == 0, f"{scenario.name}:\n{proc.stdout}\n{proc.stderr}"
    pixels = read_ppm(output)
    return hashlib.sha256(pixels).hexdigest(), proc


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    assert binary.is_file(), binary
    assert SCENARIOS, "no simulator scenarios found"

    with tempfile.TemporaryDirectory(prefix="muse-simulator-test-") as tmp:
        tmp_path = Path(tmp)
        hashes: dict[str, str] = {}
        for scenario in SCENARIOS:
            first, _ = render(binary, scenario, tmp_path / f"{scenario.stem}-1.ppm")
            second, _ = render(binary, scenario, tmp_path / f"{scenario.stem}-2.ppm")
            assert first == second, f"{scenario.name}: framebuffer is not deterministic"
            hashes[scenario.stem] = first

        assert len(set(hashes.values())) == len(hashes), f"scenarios rendered identically: {hashes}"

        # Showing shutdown must not lock subsequent preview state selections.
        after_off = tmp_path / "after-off.txt"
        after_off.write_text("face=off\n" + (HERE / "scenarios/listening.txt").read_text())
        recovered, _ = render(binary, after_off, tmp_path / "after-off.ppm")
        assert recovered == hashes["listening"], "Off prevented the next preview state"

        # Drawing a second image over an undismissed image after idle sleep must
        # wake the display and replace the canvas instead of staying behind the
        # black sleep cover.
        awake_img = tmp_path / "awake-img.txt"
        awake_img.write_text("face=idle\nimage=1080e0\n")
        expected_img, _ = render(binary, awake_img, tmp_path / "awake-img.ppm")
        assert expected_img != hashes["idle"], "image draw did not cover the face"

        asleep_img = tmp_path / "asleep-img.txt"
        asleep_img.write_text("face=idle\nimage=e02010\nadvance=50\nasleep=true\n")
        raw_asleep = tmp_path / "asleep-img.ppm"
        env = {**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}
        proc = subprocess.run(
            [str(binary), "--headless", "--scenario", str(asleep_img),
             "--run-ms", "50", "--screenshot", str(raw_asleep)],
            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
        )
        assert proc.returncode == 0, f"asleep-img:\n{proc.stdout}\n{proc.stderr}"
        header = f"P6\n{WIDTH} {HEIGHT}\n255\n".encode()
        assert set(raw_asleep.read_bytes()[len(header):]) == {0}, "sleep cover was not black"

        rewake_img = tmp_path / "rewake-img.txt"
        rewake_img.write_text(
            "face=idle\nimage=e02010\nadvance=50\nasleep=true\nadvance=50\nimage=1080e0\n"
        )
        woken_img, _ = render(binary, rewake_img, tmp_path / "rewake-img.ppm")
        assert woken_img == expected_img, "repeat image draw did not wake the sleeping display"

        invalid = tmp_path / "invalid.txt"
        for setting in ("face=definitely-not-a-mode", "level=nan"):
            invalid.write_text(f"{setting}\n")
            proc = subprocess.run(
                [str(binary), "--headless", "--scenario", str(invalid)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=10,
            )
            assert proc.returncode == 2
            assert "unsupported or invalid setting" in proc.stderr

    for name, digest in sorted(hashes.items()):
        print(f"{name}: {digest}")


if __name__ == "__main__":
    main()
