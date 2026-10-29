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

"""Read-only local diagnostics; never pair, rotate tokens, or start services."""

from __future__ import annotations

import json
import os
import platform
import shutil
import subprocess
from pathlib import Path
from typing import Literal, TypedDict

from musegadget import config, network
from musegadget.muse_api import API_BASE


class CheckResult(TypedDict):
    name: str
    status: Literal["OK", "FAIL", "WARN"]
    detail: str
    hint: str


def check_pairing(directory: Path) -> CheckResult:
    """Check saved credential structure without displaying or validating secrets online."""
    path: Path = directory / config.PAIRING_FILE
    try:
        saved: object = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {"name": "Pairing", "status": "FAIL", "detail": "No saved pairing.",
                "hint": "Run sudo musegadget pair, then add the device in the Muse app."}
    except PermissionError:
        return {"name": "Pairing", "status": "FAIL", "detail": "Cannot read saved pairing.",
                "hint": "Run sudo musegadget doctor to read the root-owned state directory."}
    except ValueError:
        return {"name": "Pairing", "status": "FAIL", "detail": "Saved pairing is not valid JSON.",
                "hint": "Run sudo musegadget pair --force to pair again."}
    except OSError as exc:
        return {"name": "Pairing", "status": "FAIL",
                "detail": f"Cannot read saved pairing (OS error {exc.errno}).",
                "hint": f"Check the state directory: {directory}."}
    if not isinstance(saved, dict) or not all(
        isinstance(saved.get(key), str) and saved[key].strip()
        for key in ("access_token", "refresh_token")
    ):
        return {"name": "Pairing", "status": "FAIL", "detail": "Saved pairing lacks device tokens.",
                "hint": "Run sudo musegadget pair --force to pair again."}
    return {"name": "Pairing", "status": "OK", "detail": "Device tokens are saved.",
            "hint": "This checks local structure, not whether Muse accepts the tokens."}


def check_sdk_token(directory: Path) -> CheckResult:
    """Reuse SDK token validation, checking file access before its optional-file reader."""
    if not os.environ.get(config.SDK_TOKEN_ENV):
        try:
            (directory / config.SDK_TOKEN_FILE).read_bytes()
        except FileNotFoundError:
            return {"name": "SDK token", "status": "FAIL", "detail": "No SDK token file.",
                    "hint": "Run bash install.sh --sdk-token TOKEN with a token from gadgets.muse.ai."}
        except PermissionError:
            return {"name": "SDK token", "status": "FAIL", "detail": "Cannot read SDK token file.",
                    "hint": "Run sudo musegadget doctor to read the root-owned state directory."}
        except OSError as exc:
            return {"name": "SDK token", "status": "FAIL",
                    "detail": f"Cannot read SDK token file (OS error {exc.errno}).",
                    "hint": f"Check the state directory: {directory}."}
    try:
        token: str | None = config.sdk_token(directory)
    except ValueError:
        return {"name": "SDK token", "status": "FAIL", "detail": "SDK token format is invalid.",
                "hint": "Copy a fresh token from gadgets.muse.ai; check MUSEGADGET_SDK_TOKEN too."}
    if token is None:
        return {"name": "SDK token", "status": "FAIL", "detail": "SDK token is empty.",
                "hint": "Run bash install.sh --sdk-token TOKEN with a token from gadgets.muse.ai."}
    return {"name": "SDK token", "status": "OK", "detail": "SDK token format is valid.",
            "hint": "This does not check expiry or revocation online."}


def check_service() -> CheckResult:
    """Inspect the installed systemd unit without starting or restarting it."""
    executable: str | None = shutil.which("systemctl")
    if executable is None:
        return {"name": "Service", "status": "FAIL", "detail": "systemctl is unavailable.",
                "hint": "The standard installer requires Linux with systemd."}
    try:
        result: subprocess.CompletedProcess[str] = subprocess.run(
            [executable, "--no-pager", "is-active", "musegadget.service"],
            capture_output=True, text=True, timeout=5, check=False,
        )
    except subprocess.TimeoutExpired:
        return {"name": "Service", "status": "FAIL", "detail": "systemctl timed out after 5 seconds.",
                "hint": "Inspect sudo systemctl status musegadget."}
    except OSError as exc:
        return {"name": "Service", "status": "FAIL",
                "detail": f"Could not run systemctl (OS error {exc.errno}).",
                "hint": "Inspect sudo systemctl status musegadget."}
    if result.returncode == 0 and result.stdout.strip() == "active":
        return {"name": "Service", "status": "OK", "detail": "musegadget.service is active.",
                "hint": "An active process does not confirm a connected Muse session."}
    return {"name": "Service", "status": "FAIL",
            "detail": f"musegadget.service is not active (systemctl exit {result.returncode}).",
            "hint": "Inspect sudo systemctl status musegadget and sudo journalctl -u musegadget -n 50."}


def check_bluetooth() -> CheckResult:
    """Check BlueZ's default controller; Bluetooth is needed for pairing, not normal operation."""
    executable: str | None = shutil.which("bluetoothctl")
    if executable is None:
        return {"name": "Bluetooth", "status": "WARN", "detail": "bluetoothctl is unavailable.",
                "hint": "Install BlueZ before pairing. An already-paired gadget can still connect."}
    try:
        result: subprocess.CompletedProcess[str] = subprocess.run(
            [executable, "show"], capture_output=True, text=True, timeout=5, check=False,
        )
    except subprocess.TimeoutExpired:
        return {"name": "Bluetooth", "status": "WARN",
                "detail": "bluetoothctl timed out after 5 seconds.",
                "hint": "Check sudo systemctl status bluetooth before pairing."}
    except OSError as exc:
        return {"name": "Bluetooth", "status": "WARN",
                "detail": f"Could not run bluetoothctl (OS error {exc.errno}).",
                "hint": "Check the BlueZ installation before pairing."}
    if result.returncode == 0 and any(
        line.strip() == "Powered: yes" for line in result.stdout.splitlines()
    ):
        return {"name": "Bluetooth", "status": "OK", "detail": "Default controller is powered on.",
                "hint": "Discovery is only open while musegadget pair is running."}
    return {"name": "Bluetooth", "status": "WARN",
            "detail": f"No powered default controller found (bluetoothctl exit {result.returncode}).",
            "hint": "Check bluetoothctl show and rfkill list before pairing; existing sessions use the network."}


def check_network() -> CheckResult:
    """Use the setup TCP probe; this does not test TLS, credentials, or a VM session."""
    if network.is_online(timeout=5.0):
        return {"name": "Muse API TCP", "status": "OK",
                "detail": f"{API_BASE} accepts a TCP connection on port 443.",
                "hint": "TLS, token validity, and the Muse VM connection are not tested."}
    return {"name": "Muse API TCP", "status": "FAIL",
            "detail": f"Could not connect to {API_BASE} on port 443.",
            "hint": "Check DNS, internet access, and outbound HTTPS firewall rules."}


def run_checks() -> tuple[CheckResult, ...]:
    """Collect independent checks, leaving device files and services unchanged."""
    if platform.system() != "Linux":
        return ({"name": "Platform", "status": "FAIL", "detail": "Doctor requires Linux.",
                 "hint": "Run sudo musegadget doctor on the Linux gadget itself."},)
    directory: Path = config.state_dir()
    return (check_pairing(directory), check_sdk_token(directory), check_service(),
            check_bluetooth(), check_network())


def format_check(result: CheckResult) -> str:
    """Render one check using only diagnostic details, never raw command output or tokens."""
    return f"[{result['status']}] {result['name']}: {result['detail']}\n  {result['hint']}"
