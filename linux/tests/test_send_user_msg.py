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

"""``musegadget send-user-msg``, against a stand-in for the local socket."""

from __future__ import annotations

import base64
import builtins
import json
import os
import socket
import tempfile
import threading
from pathlib import Path

import pytest

from musegadget import cli, config
from musegadget.link_client import MAX_IMAGE_BYTES


@pytest.fixture
def service(monkeypatch):
    """A one-shot local socket that records the request and says ok."""
    received = []
    # AF_UNIX paths are short on macOS, so avoid pytest's long tmp_path.
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        path = Path(tmp) / "mg.sock"
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(path))
        server.listen(1)
        server.settimeout(10)

        def serve():
            try:
                conn, _ = server.accept()
            except OSError:
                return
            with conn:
                line = conn.makefile("rb").readline()
                if line:
                    received.append(json.loads(line))
                    conn.sendall(b'{"ok": true}\n')

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        monkeypatch.setenv(config.SOCKET_ENV, str(path))
        yield received
        if thread.is_alive():  # nothing connected; wake the accept
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as wake:
                wake.connect(str(path))
        thread.join(5)
        server.close()


def refuse_to_open(monkeypatch, path):
    real_open = builtins.open

    def guarded(file, *args, **kwargs):
        assert str(file) != str(path), "the image was read"
        return real_open(file, *args, **kwargs)

    monkeypatch.setattr(builtins, "open", guarded)


def test_image_goes_to_the_service_with_its_type_and_name(tmp_path, service):
    picture = tmp_path / "Porch.JPG"
    picture.write_bytes(b"\xff\xd8\xff" + bytes(MAX_IMAGE_BYTES - 3))
    assert cli.main(["send-user-msg", "--image", str(picture), "who", "is", "it?"]) == 0
    request, = service
    assert request["message"] == "who is it?"
    image = request["image"]
    assert (image["filename"], image["mime_type"]) == ("Porch.JPG", "image/jpeg")
    assert base64.b64decode(image["data_base64"]) == picture.read_bytes()


def test_oversized_image_is_refused_before_it_is_read(tmp_path, monkeypatch, capsys, service):
    picture = tmp_path / "porch.png"
    with open(picture, "wb") as f:
        f.truncate(MAX_IMAGE_BYTES + 1)
    refuse_to_open(monkeypatch, picture)
    assert cli.main(["send-user-msg", "--image", str(picture), "hi"]) == 1
    assert capsys.readouterr().err == (
        "porch.png is 2,097,153 bytes; images must be 2 MiB (2,097,152 bytes) or less\n")
    assert service == []


@pytest.mark.parametrize("name", ["notes.txt", "porch", "scan.heic"])
def test_non_image_is_refused_before_it_is_read(tmp_path, monkeypatch, capsys, service, name):
    path = tmp_path / name
    path.write_bytes(b"\xff\xd8\xff")
    refuse_to_open(monkeypatch, path)
    assert cli.main(["send-user-msg", "--image", str(path), "hi"]) == 1
    assert capsys.readouterr().err == f"{name} is not a JPEG, PNG, GIF or WebP image\n"
    assert service == []


def test_fifo_is_refused_instead_of_waiting_for_a_writer(tmp_path, monkeypatch, capsys, service):
    fifo = tmp_path / "camera.jpg"
    os.mkfifo(fifo)
    refuse_to_open(monkeypatch, fifo)
    assert cli.main(["send-user-msg", "--image", str(fifo), "hi"]) == 1
    assert capsys.readouterr().err == "camera.jpg is not a regular file\n"
    assert service == []


def test_empty_or_missing_image_is_refused(tmp_path, capsys, service):
    empty = tmp_path / "empty.gif"
    empty.write_bytes(b"")
    assert cli.main(["send-user-msg", "--image", str(empty), "hi"]) == 1
    assert capsys.readouterr().err == "empty.gif is empty\n"
    assert cli.main(["send-user-msg", "--image", str(tmp_path / "gone.webp"), "hi"]) == 1
    assert "Could not read" in capsys.readouterr().err
    assert service == []
