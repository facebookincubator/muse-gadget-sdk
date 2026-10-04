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

"""Persistent device state: identity and pairing credentials."""

from __future__ import annotations

import json
import os
import re
import tempfile
from pathlib import Path

STATE_DIR_ENV = "MUSEGADGET_STATE_DIR"
SOCKET_ENV = "MUSEGADGET_SOCKET"
DEFAULT_SOCKET = Path("/run/musegadget/musegadget.sock")
DEFAULT_STATE_DIR = Path("/var/lib/musegadget")
IDENTITY_FILE = "identity.json"
PAIRING_FILE = "pairing.json"
SDK_TOKEN_ENV = "MUSEGADGET_SDK_TOKEN"
SDK_TOKEN_FILE = "sdk_token"
# mgst_ plus 43 canonical base64url characters, as issued by gadgets.muse.ai.
_SDK_TOKEN = re.compile(r"mgst_[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]")


def state_dir() -> Path:
    return Path(os.environ.get(STATE_DIR_ENV) or DEFAULT_STATE_DIR)


def socket_path() -> Path:
    """Local socket where programs on this device hand messages to the service."""
    return Path(os.environ.get(SOCKET_ENV) or DEFAULT_SOCKET)


def sdk_token(directory: Path | None = None) -> str | None:
    """The SDK token from $MUSEGADGET_SDK_TOKEN or the state dir, if any.

    Raises ValueError for a token that gadgets.muse.ai could not have issued.
    """
    token = os.environ.get(SDK_TOKEN_ENV)
    if not token:
        try:
            token = ((directory or state_dir()) / SDK_TOKEN_FILE).read_text(encoding="utf-8")
        except OSError:
            return None
    token = token.strip()
    if not token:
        return None
    return validate_sdk_token(token)


def validate_sdk_token(token: str) -> str:
    token = token.strip()
    if not _SDK_TOKEN.fullmatch(token):
        raise ValueError("the SDK token is not valid; copy it again from gadgets.muse.ai")
    return token


def save_sdk_token(token: str, directory: Path | None = None) -> None:
    """Validate and atomically save a token without exposing it in arguments."""
    token = validate_sdk_token(token)
    directory = directory or state_dir()
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    directory.chmod(0o700)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=directory,
                                     prefix=".sdk_token.", delete=False) as temp:
        temporary = Path(temp.name)
        try:
            temp.write(token + "\n")
            temp.flush()
            os.fsync(temp.fileno())
            os.replace(temporary, directory / SDK_TOKEN_FILE)
        finally:
            temporary.unlink(missing_ok=True)


def load_json(name: str, directory: Path | None = None) -> dict | None:
    path = (directory or state_dir()) / name
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


def save_json(name: str, data: dict, directory: Path | None = None) -> None:
    """Atomically write ``data``, readable only by the owner.

    A crash leaves either the old file or the new one, never a partial file.
    """
    directory = directory or state_dir()
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    path = directory / name
    tmp = path.with_suffix(path.suffix + ".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def delete_json(name: str, directory: Path | None = None) -> None:
    try:
        ((directory or state_dir()) / name).unlink()
    except FileNotFoundError:
        pass
