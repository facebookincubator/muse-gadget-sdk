# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""Credential handling and SDK delegation, using synthetic tokens only."""
import importlib.util
import io
import os
import stat
import sys
from pathlib import Path

import pytest

from musegadget import cli, config

spec = importlib.util.spec_from_file_location("pair_mac", Path(__file__).with_name("pair-mac.py"))
pair_mac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pair_mac)
TOKEN = "mgst_" + "A" * 43


@pytest.fixture
def pairing_environment(monkeypatch, tmp_path):
    monkeypatch.delenv(config.SDK_TOKEN_ENV, raising=False)
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path / "state"))
    helper = tmp_path / "helper"
    helper.touch()
    monkeypatch.setattr(pair_mac.mac_ble, "HELPER", helper)
    # main installs the transport in sys.modules; undo that after each test.
    monkeypatch.setitem(sys.modules, "musegadget.ble_server", pair_mac.mac_ble)
    return tmp_path / "state"


def test_save_token_is_private_and_preserves_environment(monkeypatch, tmp_path):
    monkeypatch.setenv(config.SDK_TOKEN_ENV, "previous-value")
    directory = tmp_path / "state"
    pair_mac.save_token(" " + TOKEN + "\n", directory)
    assert (directory / config.SDK_TOKEN_FILE).read_text() == TOKEN + "\n"
    assert stat.S_IMODE(directory.stat().st_mode) == 0o700
    assert stat.S_IMODE((directory / config.SDK_TOKEN_FILE).stat().st_mode) == 0o600
    assert os.environ[config.SDK_TOKEN_ENV] == "previous-value"
    assert list(directory.iterdir()) == [directory / config.SDK_TOKEN_FILE]


@pytest.mark.parametrize("invalid", ["", "not-a-token", "mgst_" + "B" * 43])
def test_invalid_token_does_not_create_state_or_modify_environment(monkeypatch, tmp_path, invalid):
    monkeypatch.delenv(config.SDK_TOKEN_ENV, raising=False)
    directory = tmp_path / "state"
    with pytest.raises(ValueError):
        pair_mac.save_token(invalid, directory)
    assert not directory.exists()
    assert config.SDK_TOKEN_ENV not in os.environ


def test_token_replacement_is_atomic_and_keeps_identity(monkeypatch, tmp_path):
    monkeypatch.delenv(config.SDK_TOKEN_ENV, raising=False)
    directory = tmp_path / "state"
    directory.mkdir()
    identity = directory / "identity.json"
    identity.write_text("identity-placeholder")
    pair_mac.save_token(TOKEN, directory)
    replacement = "mgst_" + "C" * 42 + "A"
    pair_mac.save_token(replacement, directory)
    assert config.sdk_token(directory) == replacement
    assert identity.read_text() == "identity-placeholder"
    assert sorted(p.name for p in directory.iterdir()) == ["identity.json", "sdk_token"]


def test_noninteractive_missing_token_fails_without_launch(pairing_environment, monkeypatch, capsys):
    monkeypatch.setattr(pair_mac.sys, "stdin", io.StringIO())
    monkeypatch.setattr(cli, "main", lambda argv: pytest.fail("SDK should not launch"))
    assert pair_mac.main([]) == 1
    assert "Run pairing in a terminal" in capsys.readouterr().err
    assert not pairing_environment.exists()


def test_environment_token_is_saved_for_docker_and_sdk_args_preserved(pairing_environment, monkeypatch):
    monkeypatch.setenv(config.SDK_TOKEN_ENV, TOKEN)
    calls = []
    monkeypatch.setattr(cli, "main", lambda argv: calls.append(argv) or 0)
    assert pair_mac.main(["--force", "--timeout", "120"]) == 0
    assert calls == [["pair", "--force", "--timeout", "120"]]
    assert (pairing_environment / config.SDK_TOKEN_FILE).read_text() == TOKEN + "\n"


def test_help_does_not_prompt_or_create_credentials(pairing_environment, monkeypatch):
    monkeypatch.setattr(pair_mac.getpass, "getpass", lambda *a: pytest.fail("token prompt"))
    monkeypatch.setattr(cli, "main", lambda argv: 0 if argv == ["pair", "--help"] else 1)
    assert pair_mac.main(["--help"]) == 0
    assert not pairing_environment.exists()


def test_missing_helper_gives_setup_instruction(pairing_environment, monkeypatch, capsys):
    monkeypatch.setattr(pair_mac.mac_ble, "HELPER", pairing_environment / "missing-helper")
    assert pair_mac.main([]) == 1
    assert "setup-mac.sh" in capsys.readouterr().err
