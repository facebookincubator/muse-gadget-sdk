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

"""Append-only, tamper-evident ledger of guard decisions.

One JSON object per line. Each row carries the previous row's hash, and
its own hash is ``sha256(prev_hash + canonical row body)`` where the body
is the row without the ``hash`` field, serialised with sorted keys. Edit
any earlier row and every later hash stops matching: ``verify`` fails.

Parameters are summarised, never stored raw: a ``file.write`` payload is
recorded as a byte length and a SHA-256 only, and strings that look like
tokens or secrets are masked.

Verify a ledger with:

    PYTHONPATH=linux/examples python3 -m command_guard.ledger verify <path>
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import sys
from datetime import datetime, timezone

GENESIS_HASH = "0" * 64

_SECRET_PATTERNS = [
    re.compile(r"mgst_[A-Za-z0-9_\-\.]+"),                      # SDK tokens
    re.compile(r"\bBearer\s+[A-Za-z0-9_\-\.]+", re.IGNORECASE),
    re.compile(r"((?:token|password|passwd|secret|api[_-]?key)\s*=\s*)[^\s;]+", re.IGNORECASE),
]


def mask_secrets(text: str) -> str:
    masked = text
    for pattern in _SECRET_PATTERNS:
        masked = pattern.sub(lambda m: (m.group(1) + "***") if m.lastindex else "***", masked)
    return masked


def summarize_params(command: str, params: dict) -> dict:
    """A ledger-safe summary of a command's parameters."""
    summary: dict = {}
    for key, value in (params or {}).items():
        if command == "file.write" and key == "data_b64" and isinstance(value, str):
            try:
                raw = base64.b64decode(value, validate=True)
                summary["data_len"] = len(raw)
                summary["data_sha256"] = hashlib.sha256(raw).hexdigest()
            except Exception:
                summary["data_len"] = len(value)
                summary["data_sha256"] = hashlib.sha256(value.encode()).hexdigest()
        elif isinstance(value, str):
            summary[key] = mask_secrets(value)
        elif isinstance(value, (int, float, bool)) or value is None:
            summary[key] = value
        else:
            summary[key] = mask_secrets(str(value))
    return summary


def _canonical(row_without_hash: dict) -> str:
    return json.dumps(row_without_hash, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def _row_hash(prev_hash: str, row_without_hash: dict) -> str:
    return hashlib.sha256((prev_hash + _canonical(row_without_hash)).encode("utf-8")).hexdigest()


class Ledger:
    """Append-only hash-chain ledger at ``path``."""

    def __init__(self, path: str) -> None:
        self.path = path

    def _last_hash(self) -> str:
        try:
            with open(self.path, encoding="utf-8") as fh:
                last = ""
                for line in fh:
                    if line.strip():
                        last = line
                if last:
                    return json.loads(last)["hash"]
        except (OSError, ValueError, KeyError):
            pass
        return GENESIS_HASH

    def append(self, *, command: str, params_summary: dict, layer1: str,
               layer2: str, final: str, reason: str) -> dict:
        prev_hash = self._last_hash()
        row = {
            "ts": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "command": command,
            "params_summary": params_summary,
            "layer1": layer1,
            "layer2": layer2,
            "final": final,
            "reason": reason,
            "prev_hash": prev_hash,
        }
        row["hash"] = _row_hash(prev_hash, row)
        directory = os.path.dirname(os.path.abspath(self.path))
        os.makedirs(directory, exist_ok=True)
        with open(self.path, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(row, ensure_ascii=False) + "\n")
        return row


def verify(path: str) -> tuple:
    """Return (ok, message). ok is False at the first broken link."""
    prev_hash = GENESIS_HASH
    count = 0
    try:
        fh = open(path, encoding="utf-8")
    except OSError as exc:
        return False, f"cannot read ledger: {exc}"
    with fh:
        for lineno, line in enumerate(fh, start=1):
            if not line.strip():
                continue
            count += 1
            try:
                row = json.loads(line)
                stored_hash = row.pop("hash")
            except (ValueError, KeyError, AttributeError):
                return False, f"line {lineno}: not a ledger row"
            if row.get("prev_hash") != prev_hash:
                return False, f"line {lineno}: prev_hash does not match the previous row"
            if _row_hash(prev_hash, row) != stored_hash:
                return False, f"line {lineno}: hash mismatch (row was modified)"
            prev_hash = stored_hash
    return True, f"ok: {count} row(s), chain intact"


def main(argv: list | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) == 2 and args[0] == "verify":
        ok, message = verify(args[1])
        print(("PASS: " if ok else "FAIL: ") + message)
        return 0 if ok else 1
    print("usage: python -m command_guard.ledger verify <ledger.jsonl>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
