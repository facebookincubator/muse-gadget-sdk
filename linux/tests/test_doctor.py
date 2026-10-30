# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import annotations

import errno
import json
import subprocess
from pathlib import Path
from typing import Literal
from unittest.mock import Mock

import pytest

from musegadget import cli, config, doctor

TOKEN: str = "mgst_" + "A" * 42 + "w"


@pytest.mark.parametrize("saved", ["{", "[]", "{}", '"secret"',
                                   '{"access_token": "secret", "refresh_token": " "}'])
def test_invalid_pairing_is_reported_without_echoing_contents(
    tmp_path: Path, saved: str,
) -> None:
    (tmp_path / config.PAIRING_FILE).write_text(saved, encoding="utf-8")
    result: doctor.CheckResult = doctor.check_pairing(tmp_path)
    assert result["status"] == "FAIL"
    assert "pair" in result["hint"]
    assert "secret" not in doctor.format_check(result)


def test_pairing_accepts_saved_tokens_without_creating_identity(tmp_path: Path) -> None:
    (tmp_path / config.PAIRING_FILE).write_text(
        json.dumps({"access_token": "secret-access", "refresh_token": "secret-refresh"}),
        encoding="utf-8",
    )
    before: bytes = (tmp_path / config.PAIRING_FILE).read_bytes()
    result: doctor.CheckResult = doctor.check_pairing(tmp_path)
    assert result["status"] == "OK"
    assert "secret" not in doctor.format_check(result)
    assert (tmp_path / config.PAIRING_FILE).read_bytes() == before
    assert not (tmp_path / config.IDENTITY_FILE).exists()


def test_missing_pairing_leaves_directory_absent(tmp_path: Path) -> None:
    directory: Path = tmp_path / "not-created"
    assert doctor.check_pairing(directory)["status"] == "FAIL"
    assert not directory.exists()


def test_non_utf8_pairing_is_reported(tmp_path: Path) -> None:
    (tmp_path / config.PAIRING_FILE).write_bytes(b"\xff")
    assert doctor.check_pairing(tmp_path)["status"] == "FAIL"


@pytest.mark.parametrize("error", [PermissionError(errno.EACCES, "secret"),
                                  OSError(errno.EIO, "secret")])
def test_unreadable_pairing_is_reported_without_exception_contents(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, error: OSError,
) -> None:
    monkeypatch.setattr(Path, "read_text", Mock(side_effect=error))
    result: doctor.CheckResult = doctor.check_pairing(tmp_path)
    assert result["status"] == "FAIL"
    assert "read" in result["detail"]
    assert "secret" not in doctor.format_check(result)


@pytest.mark.parametrize("token,status", [(TOKEN, "OK"), ("", "FAIL"), ("secret", "FAIL")])
def test_sdk_token_checks_format_without_echoing_it(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, token: str, status: str,
) -> None:
    monkeypatch.delenv(config.SDK_TOKEN_ENV, raising=False)
    (tmp_path / config.SDK_TOKEN_FILE).write_text(token, encoding="utf-8")
    result: doctor.CheckResult = doctor.check_sdk_token(tmp_path)
    assert result["status"] == status
    assert TOKEN not in doctor.format_check(result)
    assert "secret" not in doctor.format_check(result)


def test_sdk_token_uses_existing_environment_precedence(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv(config.SDK_TOKEN_ENV, TOKEN)
    monkeypatch.setattr(Path, "read_bytes", Mock(side_effect=PermissionError))
    assert doctor.check_sdk_token(tmp_path)["status"] == "OK"


def test_missing_sdk_token_is_reported(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv(config.SDK_TOKEN_ENV, raising=False)
    result: doctor.CheckResult = doctor.check_sdk_token(tmp_path)
    assert result["status"] == "FAIL"
    assert "No SDK token" in result["detail"]


@pytest.mark.parametrize("error", [PermissionError(errno.EACCES, "secret"),
                                  OSError(errno.EIO, "secret")])
def test_sdk_token_read_errors_are_not_treated_as_absent(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, error: OSError,
) -> None:
    monkeypatch.delenv(config.SDK_TOKEN_ENV, raising=False)
    monkeypatch.setattr(Path, "read_bytes", Mock(side_effect=error))
    result: doctor.CheckResult = doctor.check_sdk_token(tmp_path)
    assert result["status"] == "FAIL"
    assert "Cannot read" in result["detail"]
    assert "secret" not in doctor.format_check(result)


@pytest.mark.parametrize("code,output,status", [(0, "active\n", "OK"),
                                              (3, "inactive\n", "FAIL"),
                                              (4, "unknown\n", "FAIL"),
                                              (1, "active\n", "FAIL")])
def test_service_checks_both_state_and_exit_code(
    monkeypatch: pytest.MonkeyPatch, code: int, output: str, status: str,
) -> None:
    monkeypatch.setattr(doctor.shutil, "which", Mock(return_value="/usr/bin/systemctl"))
    run: Mock = Mock(return_value=subprocess.CompletedProcess([], code, output, "secret"))
    monkeypatch.setattr(doctor.subprocess, "run", run)
    result: doctor.CheckResult = doctor.check_service()
    assert result["status"] == status
    assert "secret" not in doctor.format_check(result)
    run.assert_called_once_with(
        ["/usr/bin/systemctl", "--no-pager", "is-active", "musegadget.service"],
        capture_output=True, text=True, timeout=5, check=False,
    )


def test_missing_systemctl_has_actionable_failure(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(doctor.shutil, "which", Mock(return_value=None))
    result: doctor.CheckResult = doctor.check_service()
    assert result["status"] == "FAIL"
    assert "systemd" in result["hint"]


@pytest.mark.parametrize("error", [subprocess.TimeoutExpired("secret", 5),
                                  OSError(errno.EACCES, "secret")])
def test_service_execution_errors_are_reported(
    monkeypatch: pytest.MonkeyPatch, error: Exception,
) -> None:
    monkeypatch.setattr(doctor.shutil, "which", Mock(return_value="/usr/bin/systemctl"))
    monkeypatch.setattr(doctor.subprocess, "run", Mock(side_effect=error))
    result: doctor.CheckResult = doctor.check_service()
    assert result["status"] == "FAIL"
    assert "secret" not in doctor.format_check(result)


@pytest.mark.parametrize("code,output,status", [(0, "Controller ab\n\tPowered: yes\n", "OK"),
                                              (0, "\tPowered: no\n", "WARN"),
                                              (0, "No default controller available\n", "WARN"),
                                              (1, "\tPowered: yes\n", "WARN")])
def test_bluetooth_checks_default_controller_power(
    monkeypatch: pytest.MonkeyPatch, code: int, output: str, status: str,
) -> None:
    monkeypatch.setattr(doctor.shutil, "which", Mock(return_value="/usr/bin/bluetoothctl"))
    run: Mock = Mock(return_value=subprocess.CompletedProcess([], code, output, "secret"))
    monkeypatch.setattr(doctor.subprocess, "run", run)
    result: doctor.CheckResult = doctor.check_bluetooth()
    assert result["status"] == status
    assert "secret" not in doctor.format_check(result)
    run.assert_called_once_with(
        ["/usr/bin/bluetoothctl", "show"], capture_output=True, text=True,
        timeout=5, check=False,
    )


def test_missing_bluetooth_tool_is_only_a_warning(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(doctor.shutil, "which", Mock(return_value=None))
    assert doctor.check_bluetooth()["status"] == "WARN"


@pytest.mark.parametrize("error", [subprocess.TimeoutExpired("secret", 5),
                                  OSError(errno.EACCES, "secret")])
def test_bluetooth_execution_errors_are_reported(
    monkeypatch: pytest.MonkeyPatch, error: Exception,
) -> None:
    monkeypatch.setattr(doctor.shutil, "which", Mock(return_value="/usr/bin/bluetoothctl"))
    monkeypatch.setattr(doctor.subprocess, "run", Mock(side_effect=error))
    result: doctor.CheckResult = doctor.check_bluetooth()
    assert result["status"] == "WARN"
    assert "secret" not in doctor.format_check(result)


@pytest.mark.parametrize("online,status", [(True, "OK"), (False, "FAIL")])
def test_network_reuses_setup_probe_with_timeout(
    monkeypatch: pytest.MonkeyPatch, online: bool, status: str,
) -> None:
    probe: Mock = Mock(return_value=online)
    monkeypatch.setattr(doctor.network, "is_online", probe)
    assert doctor.check_network()["status"] == status
    probe.assert_called_once_with(timeout=5.0)


@pytest.mark.parametrize("status,exit_code", [("OK", 0), ("FAIL", 1), ("WARN", 0)])
def test_cli_renders_checks_and_sets_exit_status(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str],
    status: Literal["OK", "FAIL", "WARN"], exit_code: int,
) -> None:
    result: doctor.CheckResult = {"name": "Test", "status": status, "detail": "Checked.",
                                  "hint": "Next step."}
    monkeypatch.setattr(doctor, "run_checks", Mock(return_value=(result,)))
    assert cli.main(["doctor"]) == exit_code
    output: str = capsys.readouterr().out
    assert f"[{status}] Test: Checked." in output
    assert "Next step." in output


def test_unsupported_platform_does_not_probe_or_create_state(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(doctor.platform, "system", Mock(return_value="Windows"))
    probe: Mock = Mock(side_effect=AssertionError("must not probe"))
    monkeypatch.setattr(doctor.network, "is_online", probe)
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path / "absent"))
    assert cli.main(["doctor"]) == 1
    assert not (tmp_path / "absent").exists()
    probe.assert_not_called()


def test_doctor_continues_after_failures_without_changing_saved_files(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(doctor.platform, "system", Mock(return_value="Linux"))
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path))
    monkeypatch.delenv(config.SDK_TOKEN_ENV, raising=False)
    (tmp_path / config.PAIRING_FILE).write_bytes(b"broken-secret")
    (tmp_path / config.SDK_TOKEN_FILE).write_text(TOKEN, encoding="utf-8")
    before: dict[str, bytes] = {path.name: path.read_bytes() for path in tmp_path.iterdir()}
    monkeypatch.setattr(doctor.shutil, "which", Mock(return_value=None))
    monkeypatch.setattr(doctor.network, "is_online", Mock(return_value=True))
    results: tuple[doctor.CheckResult, ...] = doctor.run_checks()
    assert [result["status"] for result in results] == ["FAIL", "OK", "FAIL", "WARN", "OK"]
    assert {path.name: path.read_bytes() for path in tmp_path.iterdir()} == before
