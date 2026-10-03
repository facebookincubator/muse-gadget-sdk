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

from __future__ import annotations

import json
import socket
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples"))
import pebble_ring_bridge as bridge  # noqa: E402

MULTIPART = (
    b"--XyZ\r\n"
    b'Content-Disposition: form-data; name="transcription"\r\n\r\n'
    b"Turn on the porch light\r\n"
    b"--XyZ\r\n"
    b'Content-Disposition: form-data; name="recordedAt"\r\n\r\n'
    b"1790403830159\r\n"
    b"--XyZ--\r\n"
)


def test_multipart_form_fields():
    fields = bridge.parse_body("multipart/form-data; boundary=XyZ", MULTIPART)
    assert fields == {"transcription": "Turn on the porch light", "recordedAt": "1790403830159"}
    assert bridge.transcription(fields) == "Turn on the porch light"


def test_json_and_plain_text():
    assert bridge.transcription(bridge.parse_body("application/json", b'{"text": " hi "}')) == "hi"
    assert bridge.transcription(bridge.parse_body("text/plain", b"hello")) == "hello"


@pytest.mark.parametrize("headers, fields", [
    ({"X-Pebble-Token": "s3"}, {}),
    ({"Authorization": "Bearer s3"}, {}),
    ({}, {"token": "s3"}),
])
def test_token_sources(headers, fields):
    assert bridge.presented_token(headers, fields) == "s3"


@pytest.fixture
def server(monkeypatch):
    sent = []
    monkeypatch.setattr(bridge, "send_user_msg", lambda text: (sent.append(text) or True, "Sent to your Muse."))
    bridge.Handler.secret = "s3"
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), bridge.Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{httpd.server_address[1]}", sent
    httpd.shutdown()


def post(url, body, headers):
    request = urllib.request.Request(url + "/ingest", data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as err:
        return err.code, json.loads(err.read())


def test_authorized_transcription_is_forwarded(server):
    url, sent = server
    status, body = post(url, MULTIPART, {
        "Content-Type": "multipart/form-data; boundary=XyZ", "X-Pebble-Token": "s3"})
    assert status == 200 and body["ok"]
    assert sent == ["Turn on the porch light [via Pebble Ring]"]


def test_wrong_token_is_rejected(server):
    url, sent = server
    status, _ = post(url, b'{"text": "hi"}', {"Content-Type": "application/json", "X-Pebble-Token": "no"})
    assert status == 401 and sent == []


def test_empty_transcription_is_rejected(server):
    url, sent = server
    status, _ = post(url, b'{"text": ""}', {"Content-Type": "application/json", "X-Pebble-Token": "s3"})
    assert status == 400 and sent == []


def test_non_ascii_token_is_rejected_not_crashed(server):
    url, sent = server
    status, _ = post(url, b'{"text": "hi"}', {"Content-Type": "application/json", "X-Pebble-Token": "s\xe9"})
    assert status == 401 and sent == []


def test_bad_content_length_gets_a_400(server):
    url, sent = server
    host, port = urllib.parse.urlsplit(url).netloc.split(":")
    with socket.create_connection((host, int(port)), timeout=5) as sock:
        sock.sendall(b"POST /ingest HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n"
                     b"Content-Type: application/json\r\nX-Pebble-Token: s3\r\n\r\n")
        status_line = sock.makefile("rb").readline()
    assert status_line.split()[1] == b"400" and sent == []
