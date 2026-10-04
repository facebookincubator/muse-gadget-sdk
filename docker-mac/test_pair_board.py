# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""Mac relay and actual SDK endpoint tests without a radio, board or network."""
import argparse
import importlib.util
import json
import os
import shlex
import stat
import subprocess
import sys
from pathlib import Path

import pytest

from musegadget import config

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pair_board", Path(__file__).with_name("pair-board.py"))
pair_board = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pair_board)
TOKEN = "mgst_" + "A" * 43
PEER = ROOT / "linux/tests/relay_peer.py"


def options(**changes):
    values = dict(target="root@192.0.2.2", remote_python="/opt/musegadget/venv/bin/python",
                  state_dir="/var/lib/musegadget", timeout=600, port=22, sudo=False, force=False)
    values.update(changes)
    return argparse.Namespace(**values)


def launch(args, directory, sdk_token_env=None):
    env = dict(os.environ, PYTHONPATH=str(ROOT / "linux/src") + os.pathsep + str(ROOT / "linux/tests"),
               MUSEGADGET_STATE_DIR=str(directory), PYTHONDONTWRITEBYTECODE="1")
    env.pop(config.SDK_TOKEN_ENV, None)
    if sdk_token_env is not None:
        env[config.SDK_TOKEN_ENV] = sdk_token_env
    return subprocess.Popen([sys.executable, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)


def test_ssh_requires_verified_host_and_quotes_remote_arguments():
    args = options(state_dir="/tmp/a path; touch /tmp/injected", sudo=True, force=True)
    command = pair_board.ssh_command(args)
    assert "BatchMode=yes" in command and "StrictHostKeyChecking=yes" in command
    assert "-T" in command
    remote = shlex.split(command[-1])
    assert remote[:3] == ["sudo", "-n", "env"]
    assert "MUSEGADGET_STATE_DIR=" + args.state_dir in remote
    assert remote[-1] == "--force"
    assert not any(TOKEN in arg for arg in command)


@pytest.mark.parametrize("target", ["-oProxyCommand=bad", "root@host;whoami", "", "host name"])
def test_ssh_rejects_option_and_shell_injection_targets(target):
    with pytest.raises(ValueError):
        pair_board.ssh_command(options(target=target))


@pytest.mark.parametrize("existing_env", [None, "mgst_" + "C" * 42 + "A"])
def test_end_to_end_board_enrollment_keeps_identity_and_credentials_on_target(tmp_path, capsys, existing_env):
    board_state = tmp_path / "board"
    config.save_json(config.IDENTITY_FILE, {"mac": "02:00:00:00:00:01"}, board_state)
    before = (board_state / config.IDENTITY_FILE).read_bytes()
    remote = launch([str(PEER), "board", "--timeout", "5"], board_state, existing_env)
    peers = []

    def helper(name):
        assert name == "MuseGadget000001"
        process = launch([str(PEER), "phone"], tmp_path / "mac")
        peers.append(process)
        return process

    assert pair_board.relay(remote, timeout=5, sdk_token=TOKEN, launch_helper=helper) == 0
    assert remote.returncode == 0 and peers[0].returncode == 0
    assert (board_state / config.IDENTITY_FILE).read_bytes() == before
    assert (board_state / config.SDK_TOKEN_FILE).read_text().strip() == TOKEN
    record = config.load_json(config.PAIRING_FILE, board_state)
    assert record["access_token"] == "synthetic-board-access"
    assert record["refresh_token"] == "synthetic-board-refresh"
    assert stat.S_IMODE((board_state / config.PAIRING_FILE).stat().st_mode) == 0o600
    assert stat.S_IMODE(board_state.stat().st_mode) == 0o700
    assert not (tmp_path / "mac").exists()
    visible = capsys.readouterr()
    logs = remote.stderr.read() + peers[0].stderr.read() + visible.out + visible.err
    assert "Paired." in logs
    for secret in (TOKEN, "synthetic-board-access", "synthetic-board-refresh"):
        assert secret not in logs
    remote.stderr.close()
    peers[0].stderr.close()


def test_interrupted_hidden_token_prompt_does_not_launch_ssh(tmp_path, monkeypatch, capsys):
    helper = tmp_path / "helper"
    helper.touch()
    monkeypatch.setattr(pair_board.mac_ble, "HELPER", helper)
    def missing_input(prompt):
        raise EOFError
    monkeypatch.setattr(pair_board.getpass, "getpass", missing_input)
    monkeypatch.setattr(pair_board.subprocess, "Popen", lambda *a, **kw: pytest.fail("SSH launched"))
    assert pair_board.main(["root@192.0.2.2", "--sdk-token-prompt"]) == 1
    assert "Enrollment relay stopped" in capsys.readouterr().err


def test_remote_endpoint_rejects_existing_pairing_before_touching_credentials(tmp_path):
    config.save_json(config.PAIRING_FILE, {"access_token": "existing"}, tmp_path)
    before = (tmp_path / config.PAIRING_FILE).read_bytes()
    process = launch(["-m", "musegadget.pair_stdio"], tmp_path)
    out, err = process.communicate(timeout=5)
    assert process.returncode == 1 and out == ""
    assert "already paired" in err
    assert (tmp_path / config.PAIRING_FILE).read_bytes() == before


@pytest.mark.parametrize("start_message", [
    {"op": "start", "version": 999, "sdk_token": TOKEN},
    {"op": "start", "version": 1, "sdk_token": "invalid-secret-value"},
    {"op": "start", "version": 1},
])
def test_remote_endpoint_rejects_invalid_start_without_creating_identity(tmp_path, start_message):
    state = tmp_path / "state"
    process = launch(["-m", "musegadget.pair_stdio"], state)
    out, err = process.communicate(json.dumps(start_message) + "\n", timeout=5)
    assert process.returncode == 1 and out == ""
    assert "relay failed" in err
    assert "invalid-secret-value" not in err and TOKEN not in err
    assert not state.exists()


def test_closing_mac_window_cancels_board_without_saving_pairing(tmp_path):
    remote = launch([str(PEER), "board", "--timeout", "5"], tmp_path / "board")
    def helper(name):
        return launch(["-c", "pass"], tmp_path / "mac")
    with pytest.raises(RuntimeError, match="failed or was cancelled"):
        pair_board.relay(remote, timeout=5, sdk_token=TOKEN, launch_helper=helper)
    assert remote.poll() is not None
    assert not (tmp_path / "board" / config.PAIRING_FILE).exists()
    remote.stderr.close()
