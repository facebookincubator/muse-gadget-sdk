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

"""File reads and chunked writes, run as the command account.

Invoked as ``python -m musegadget.fileops`` in a child process started as the
account that runs commands, so file access is limited by that account's
permissions rather than the service's. Takes a JSON request on stdin and
prints a JSON result.

Writes land in a hidden partial file next to the target and publish it
atomically once the final chunk arrives and its SHA-256 matches. Without
overwrite, publication refuses any existing destination directory entry.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import stat
import sys

MAX_CHUNK_BYTES = 64 * 1024


class FileOpError(Exception):
    pass


def _partial_path(path: str) -> str:
    head, tail = os.path.split(path)
    return os.path.join(head, f".{tail}.musegadget-partial")


def _require_absolute(path: object) -> str:
    if not isinstance(path, str) or not os.path.isabs(path):
        raise FileOpError("path must be an absolute path")
    return path


def read(request: dict) -> dict:
    path = _require_absolute(request.get("path"))
    offset = int(request.get("offset") or 0)
    limit = min(int(request.get("limit") or MAX_CHUNK_BYTES), MAX_CHUNK_BYTES)
    if offset < 0 or limit <= 0:
        raise FileOpError("offset must be >= 0 and limit > 0")
    with open(path, "rb") as f:
        size = os.fstat(f.fileno()).st_size
        f.seek(offset)
        data = f.read(limit)
    next_offset = offset + len(data)
    return {
        "path": path,
        "size": size,
        "offset": offset,
        "next_offset": next_offset,
        "eof": next_offset >= size,
        "data_b64": base64.b64encode(data).decode("ascii"),
    }


def write(request: dict) -> dict:
    path = _require_absolute(request.get("path"))
    offset = int(request.get("offset") or 0)
    data = base64.b64decode(request.get("data_b64") or "", validate=True)
    if len(data) > MAX_CHUNK_BYTES:
        raise FileOpError(f"chunk larger than {MAX_CHUNK_BYTES} bytes")
    partial = _partial_path(path)
    if offset == 0:
        if request.get("create_parents"):
            os.makedirs(os.path.dirname(path), exist_ok=True)
        # Keep the content private while it arrives: the final mode, which
        # may be wider, is applied just before the rename.
        fd = os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.fchmod(fd, 0o600)  # a partial left by an earlier write keeps its mode
    else:
        try:
            fd = os.open(partial, os.O_WRONLY)
        except FileNotFoundError:
            raise FileOpError("no write in progress; start at offset 0") from None
    with os.fdopen(fd, "wb") as f:
        size = f.seek(0, os.SEEK_END)
        if offset != size:
            raise FileOpError(f"expected offset {size}, got {offset}")
        f.write(data)
        written = f.tell()
    if not request.get("final"):
        return {"path": path, "received": written, "complete": False}

    digest = hashlib.sha256()
    with open(partial, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    expected = request.get("sha256")
    if expected and expected.lower() != digest.hexdigest():
        os.unlink(partial)
        raise FileOpError("sha256 mismatch; write discarded")
    # Keep the permission bits of the file being replaced, such as 0600 or
    # 0755, but not its setuid, setgid or sticky bits. A new file gets 0644,
    # less the umask.
    try:
        mode = stat.S_IMODE(os.stat(path).st_mode) & 0o777
    except OSError:
        umask = os.umask(0)
        os.umask(umask)
        mode = 0o644 & ~umask
    os.chmod(partial, mode)
    if request.get("overwrite", True):
        os.replace(partial, path)
    else:
        # Both names are in the same directory. link() atomically refuses an
        # existing entry (including a dangling symlink); an exists()/rename()
        # pair would let another writer's file be replaced between the calls.
        try:
            os.link(partial, path)
        except FileExistsError:
            raise FileOpError("file exists and overwrite is false") from None
        finally:
            os.unlink(partial)
    return {"path": path, "size": written, "sha256": digest.hexdigest(), "complete": True}


OPERATIONS = {"read": read, "write": write}


def main() -> int:
    try:
        request = json.load(sys.stdin)
        result = {"ok": True, "payload": OPERATIONS[request["op"]](request)}
    except (FileOpError, OSError, ValueError, KeyError) as exc:
        result = {"ok": False, "error": str(exc) or type(exc).__name__}
    json.dump(result, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
