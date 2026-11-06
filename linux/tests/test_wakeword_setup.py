# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

import hashlib
import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SETUP = ROOT / "wakeword" / "setup.sh"


def run(*args, env=None):
    return subprocess.run(
        ["bash", str(SETUP), *map(str, args)],
        env=env,
        capture_output=True,
        text=True,
        timeout=10,
    )


def test_help_and_required_explicit_wheel(tmp_path):
    assert run("--help").returncode == 0
    missing = run("--venv", tmp_path / "unused")
    assert (
        missing.returncode != 0 and "actual trusted ExecuTorch wheel" in missing.stderr
    )
    assert not (tmp_path / "unused").exists()
    assert run("--wheel").returncode != 0


def test_dry_run_creates_nothing_and_never_opens_audio_or_services(tmp_path):
    wheel = tmp_path / "trusted-executorch.whl"
    wheel.write_bytes(b"synthetic wheel for command-preview tests")
    venv, build = tmp_path / "new environment", tmp_path / "build"
    result = run("--wheel", wheel, "--venv", venv, "--build-dir", build, "--dry-run")
    assert result.returncode == 0, result.stderr
    assert (
        "pip install" in result.stdout and "musegadget.wakeword_check" in result.stdout
    )
    assert (
        "build_signal.sh" in result.stdout
        and "libmuse_wakeword_signal.so" in result.stdout
    )
    assert "voice-call" not in result.stdout and "systemctl" not in result.stdout
    assert "sudo" not in result.stdout
    assert not venv.exists() and not build.exists()


def test_wheel_checksum_and_existing_environment_are_checked_first(tmp_path):
    wheel = tmp_path / "executorch.whl"
    wheel.write_bytes(b"synthetic")
    venv = tmp_path / "env"
    sha = hashlib.sha256(wheel.read_bytes()).hexdigest()
    assert (
        run(
            "--wheel", wheel, "--wheel-sha256", sha, "--venv", venv, "--dry-run"
        ).returncode
        == 0
    )
    wrong = run(
        "--wheel", wheel, "--wheel-sha256", "0" * 64, "--venv", venv, "--dry-run"
    )
    assert wrong.returncode != 0 and "checksum does not match" in wrong.stderr
    assert run("--wheel", wheel, "--wheel-sha256", "bad", "--dry-run").returncode != 0
    venv.mkdir()
    refused = run("--wheel", wheel, "--venv", venv, "--dry-run")
    assert refused.returncode != 0 and "--reuse-venv" in refused.stderr
    accepted = run("--wheel", wheel, "--venv", venv, "--reuse-venv", "--dry-run")
    assert accepted.returncode == 0
    assert "--no-deps --force-reinstall" in accepted.stdout
    assert "env -u PYTHONPATH" in accepted.stdout


def test_install_commands_use_supplied_wheel_and_isolated_environment(tmp_path):
    # Execute the real orchestration against stub commands, never pip/system apt.
    checkout = tmp_path / "linux"
    (checkout / "wakeword").mkdir(parents=True)
    script = checkout / "wakeword" / "setup.sh"
    script.write_bytes(SETUP.read_bytes())
    log = tmp_path / "commands.log"
    build_script = checkout / "wakeword" / "build_signal.sh"
    build_script.write_text(
        '#!/usr/bin/env bash\nprintf "signal-build:%s\\n" "$1" >> "$TEST_LOG"\n'
    )
    venv = tmp_path / "env"
    commands = tmp_path / "commands"
    commands.mkdir()
    python = commands / "stub-python"
    python.write_text(
        "#!/usr/bin/env bash\n"
        'printf "venv-create:%s\\n" "$*" >> "$TEST_LOG"\n'
        'mkdir -p "$3/bin"\n'
        'cp "$STUB_VENV_PYTHON" "$3/bin/python"\n'
        'chmod +x "$3/bin/python"\n'
    )
    python.chmod(0o755)
    interpreter = tmp_path / "venv-python"
    interpreter.write_text(
        '#!/usr/bin/env bash\nprintf "venv-python:%s\\n" "$*" >> "$TEST_LOG"\n'
    )
    compiler = commands / "cc"
    compiler.write_text("#!/usr/bin/env bash\nexit 0\n")
    compiler.chmod(0o755)
    wheel = tmp_path / "approved-executorch.whl"
    wheel.write_bytes(b"synthetic wheel")
    torch_wheel = tmp_path / "approved-torch.whl"
    torch_wheel.write_bytes(b"synthetic compatible torch wheel")
    env = {
        **os.environ,
        "PATH": str(commands) + os.pathsep + os.environ["PATH"],
        "TEST_LOG": str(log),
        "STUB_VENV_PYTHON": str(interpreter),
    }
    result = subprocess.run(
        [
            "bash",
            str(script),
            "--wheel",
            str(wheel),
            "--torch-wheel",
            str(torch_wheel),
            "--python",
            str(python),
            "--venv",
            str(venv),
        ],
        env=env,
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 0, result.stderr
    text = log.read_text()
    assert str(wheel) in text and str(torch_wheel) in text and "[voice]" in text
    installs = [line for line in text.splitlines() if "-m pip install" in line]
    assert (
        len(installs) == 1
        and str(wheel) in installs[0]
        and str(torch_wheel) in installs[0]
    )
    assert "musegadget.wakeword_check" in text and "signal-build:" in text
    assert "systemctl" not in text and "voice-call" not in text
    assert "listening remains OFF" in result.stdout


def test_explicit_torch_and_prebuilt_signal_options_do_not_guess_dependencies(tmp_path):
    wheel = tmp_path / "executorch.whl"
    wheel.write_bytes(b"synthetic wheel")
    torch_wheel = tmp_path / "torch.whl"
    torch_wheel.write_bytes(b"synthetic torch")
    library = tmp_path / "libsignal.so"
    library.write_bytes(b"synthetic library")
    result = run(
        "--wheel",
        wheel,
        "--torch-wheel",
        torch_wheel,
        "--signal-library",
        library,
        "--venv",
        tmp_path / "env",
        "--dry-run",
    )
    assert result.returncode == 0, result.stderr
    assert "build_signal.sh" not in result.stdout
    assert str(torch_wheel) in result.stdout and str(library) in result.stdout
    assert "--check-audio" in result.stdout
    assert (
        run("--wheel", wheel, "--torch-sha256", "0" * 64, "--dry-run").returncode != 0
    )
    assert (
        run(
            "--wheel", wheel, "--torch-wheel", tmp_path / "missing.whl", "--dry-run"
        ).returncode
        != 0
    )
    assert (
        run(
            "--wheel", wheel, "--signal-library", tmp_path / "missing.so", "--dry-run"
        ).returncode
        != 0
    )
    assert not (tmp_path / "env").exists()
