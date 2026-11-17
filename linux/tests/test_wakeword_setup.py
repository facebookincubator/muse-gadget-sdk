# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

import os
import platform
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SETUP = ROOT / "wakeword" / "setup.sh"

# The helper refuses to run anywhere but the Linux gadget host, before it
# parses anything else; these tests exercise what it does there.
pytestmark = pytest.mark.skipif(platform.system() != "Linux", reason="setup.sh runs only on Linux")


def run(*args, env=None, script=SETUP):
    return subprocess.run(["bash", str(script), *map(str, args)], env=env,
                          capture_output=True, text=True, timeout=10)


def test_help_and_unknown_options():
    assert run("--help").returncode == 0
    assert run("--venv").returncode != 0
    assert run("--wheel", "x.whl").returncode != 0   # no wheel anymore


def test_dry_run_creates_nothing_and_never_opens_audio_or_services(tmp_path):
    venv, build = tmp_path / "new environment", tmp_path / "build"
    result = run("--venv", venv, "--build-dir", build, "--dry-run")
    assert result.returncode == 0, result.stderr
    assert "pip install" in result.stdout and "musegadget.wakeword_check" in result.stdout
    assert "build.sh" in result.stdout and "libmuse_wakeword.so" in result.stdout
    for absent in ("voice-call", "systemctl", "sudo", "torch", "executorch"):
        assert absent not in result.stdout
    assert not venv.exists() and not build.exists()


def test_existing_environment_and_missing_inputs_are_refused_first(tmp_path):
    venv = tmp_path / "env"
    venv.mkdir()
    refused = run("--venv", venv, "--dry-run")
    assert refused.returncode != 0 and "--reuse-venv" in refused.stderr
    assert run("--venv", venv, "--reuse-venv", "--dry-run").returncode == 0
    assert run("--library", tmp_path / "missing.so", "--dry-run").returncode != 0
    assert run("--deps", tmp_path / "missing", "--dry-run").returncode != 0


def test_install_builds_then_checks_with_that_library(tmp_path):
    # Execute the real orchestration against stub commands, never pip or apt.
    checkout = tmp_path / "linux"
    (checkout / "wakeword").mkdir(parents=True)
    script = checkout / "wakeword" / "setup.sh"
    script.write_bytes(SETUP.read_bytes())
    log = tmp_path / "commands.log"
    (checkout / "wakeword" / "build.sh").write_text(
        '#!/usr/bin/env bash\nprintf "build:%s\\n" "$*" >> "$TEST_LOG"\n'
        'mkdir -p "$2" && printf synthetic > "$2/libmuse_wakeword.so"\n')
    commands = tmp_path / "commands"
    commands.mkdir()
    python = commands / "stub-python"
    python.write_text(
        "#!/usr/bin/env bash\n"
        'printf "venv-create:%s\\n" "$*" >> "$TEST_LOG"\n'
        'mkdir -p "$3/bin"\n'
        'cp "$STUB_VENV_PYTHON" "$3/bin/python"\n'
        'chmod +x "$3/bin/python"\n')
    python.chmod(0o755)
    interpreter = tmp_path / "venv-python"
    interpreter.write_text(
        '#!/usr/bin/env bash\nprintf "venv-python:%s lib=%s prefix=%s\\n" "$*" "${MUSE_WAKEWORD_LIBRARY:-}" '
        '"$(dirname "$(dirname "$0")")" >> "$TEST_LOG"\n')
    for tool in ("cc", "c++", "unzip"):
        (commands / tool).write_text("#!/usr/bin/env bash\nexit 0\n")
        (commands / tool).chmod(0o755)
    env = {**os.environ, "PATH": str(commands) + os.pathsep + os.environ["PATH"],
           "TEST_LOG": str(log), "STUB_VENV_PYTHON": str(interpreter)}
    deps = tmp_path / "archives"
    deps.mkdir()
    venv, build = tmp_path / "env", tmp_path / "out"
    result = run("--python", python, "--venv", venv, "--build-dir", build, "--deps", deps,
                 env=env, script=script)
    assert result.returncode == 0, result.stderr
    lines = log.read_text().splitlines()
    assert lines[0].startswith("venv-create:-m venv ")
    assert "-m pip install" in lines[1] and "[voice]" in lines[1]
    assert lines[2] == f"build:--out {build} --deps {deps}"
    # The environment gets its own copy, and the check finds it with no
    # override, the way every later command will.
    assert (venv / "lib" / "musegadget" / "libmuse_wakeword.so").read_bytes() == b"synthetic"
    assert lines[3].startswith("venv-python:-m musegadget.wakeword_check --check-audio lib= ")
    assert lines[3].endswith(f"prefix={venv}")
    assert "listening remains OFF" in result.stdout


def test_a_supplied_library_skips_the_build(tmp_path):
    library = tmp_path / "libmuse_wakeword.so"
    library.write_bytes(b"synthetic")
    result = run("--library", library, "--venv", tmp_path / "env", "--dry-run")
    assert result.returncode == 0, result.stderr
    assert "build.sh" not in result.stdout
    assert str(library) in result.stdout and "lib/musegadget/libmuse_wakeword.so" in result.stdout


def test_build_never_deletes_what_is_already_in_its_output(tmp_path):
    # With --out . from linux/, "src" was the package: the build works in
    # scratch space of its own and leaves everything else alone.
    out = tmp_path / "out"
    for name in ("src", "obj"):
        (out / name).mkdir(parents=True)
        (out / name / "keep").write_text("mine")
    result = subprocess.run(["bash", str(ROOT / "wakeword" / "build.sh"), "--out", str(out), "--offline",
                             "--deps", str(tmp_path)], capture_output=True, text=True, timeout=30)
    assert result.returncode != 0 and "--offline" in result.stderr   # no archives here
    assert (out / "src" / "keep").read_text() == "mine"
    assert (out / "obj" / "keep").read_text() == "mine"
    assert not [p for p in out.iterdir() if p.name.startswith(".ww-build")]
