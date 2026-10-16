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

import base64
import json
import os
import select
import subprocess
import sys
from pathlib import Path

import pytest

from musegadget import fileops

# Source-owned inert wrapper pauses the real publication primitive. No production
# algorithm is copied: all checks, chunking, checksum and mode logic are fileops.write.
CHILD = """import json, sys
from musegadget import fileops
request = json.loads(sys.stdin.readline())
replace, link = fileops.os.replace, fileops.os.link

def pause(publish):
    def wrapped(source, destination):
        print("READY", flush=True)
        assert sys.stdin.readline().strip() == "publish"
        return publish(source, destination)
    return wrapped

fileops.os.replace = pause(replace)
fileops.os.link = pause(link)
try:
    result = {"ok": True, "payload": fileops.write(request)}
except (fileops.FileOpError, OSError, ValueError) as exc:
    result = {"ok": False, "error": type(exc).__name__}
print(json.dumps(result), flush=True)
"""


def test_overwrite_false_preserves_a_destination_created_before_publication(tmp_path):
    target = tmp_path / "destination"
    params = {"path": str(target), "data_b64": base64.b64encode(b"new contents").decode(),
              "final": True, "overwrite": False}
    env = {"PATH": "/usr/bin:/bin", "PYTHONPATH": str(Path(fileops.__file__).parent.parent)}
    child = subprocess.Popen(
        [sys.executable, "-c", CHILD], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, env=env,
    )
    try:
        child.stdin.write(json.dumps(params) + "\n")
        child.stdin.flush()
        ready, _, _ = select.select([child.stdout], [], [], 5)
        assert ready, "child never reached publication barrier"
        assert child.stdout.readline().strip() == "READY"
        # Another process creates the destination just before the actual OS
        # publication, with deterministic IPC rather than timing a race.
        target.write_bytes(b"concurrent contents")
        child.stdin.write("publish\n")
        child.stdin.flush()
        output, stderr = child.communicate(timeout=5)
        assert child.returncode == 0, stderr
        result = json.loads(output)
        assert target.read_bytes() == b"concurrent contents", result
        assert result["ok"] is False, result
        assert not (tmp_path / ".destination.musegadget-partial").exists()
    finally:
        if child.poll() is None:
            child.kill()
            child.wait(timeout=5)


def request_for(path, **fields):
    return {"path": str(path), "data_b64": base64.b64encode(b"contents").decode(),
            "final": True, "overwrite": False, **fields}


def test_no_overwrite_creates_new_file(tmp_path):
    target = tmp_path / "new"
    result = fileops.write(request_for(target))
    assert result["complete"]
    assert target.read_bytes() == b"contents"
    assert not (tmp_path / ".new.musegadget-partial").exists()


@pytest.mark.parametrize("entry", ["file", "directory", "dangling", "symlink"])
def test_no_overwrite_refuses_existing_directory_entries(tmp_path, entry):
    target = tmp_path / "existing"
    if entry == "file":
        target.write_bytes(b"original")
    elif entry == "directory":
        target.mkdir()
    else:
        referent = tmp_path / "referent"
        if entry == "symlink":
            referent.write_bytes(b"original")
        target.symlink_to(referent)
    with pytest.raises(fileops.FileOpError, match="overwrite is false"):
        fileops.write(request_for(target))
    assert os.path.lexists(target)
    assert not (tmp_path / ".existing.musegadget-partial").exists()
    if entry == "file":
        assert target.read_bytes() == b"original"
    if entry in ("symlink", "dangling"):
        assert target.is_symlink()


def test_no_overwrite_publication_failure_discards_partial(tmp_path, monkeypatch):
    def unsupported(*args):
        raise OSError("hard links unavailable")
    monkeypatch.setattr(fileops.os, "link", unsupported)
    target = tmp_path / "new"
    with pytest.raises(OSError, match="hard links unavailable"):
        fileops.write(request_for(target))
    assert not target.exists()
    assert not (tmp_path / ".new.musegadget-partial").exists()
