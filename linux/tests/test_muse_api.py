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

import http.client
import io
import json
from types import SimpleNamespace

import pytest

from musegadget import muse_api

TOKENS = {"access_token": "new-a", "refresh_token": "new-r"}


@pytest.fixture
def sent(monkeypatch):
    bodies: list[dict] = []

    def urlopen(req, timeout):
        bodies.append((req.get_header("Authorization"), json.loads(req.data)))
        return io.BytesIO(json.dumps(TOKENS).encode())

    monkeypatch.setattr(muse_api.urllib.request, "urlopen", urlopen)
    return bodies


def test_refresh_sends_only_the_device_id_without_a_key(sent):
    assert muse_api.refresh_device_token("r", "homelink-abcdef") == (TOKENS, 200)
    assert sent == [("Bearer hatch_refresh:r", {"device_id": "homelink-abcdef"})]


def test_refresh_sends_the_sdk_token(sent):
    muse_api.refresh_device_token("r", "homelink-abcdef", sdk_token="mgst_token")
    assert sent == [("Bearer hatch_refresh:r", {"device_id": "homelink-abcdef", "sdk_token": "mgst_token"})]



def test_refresh_never_presents_the_access_token_or_doubles_the_prefix(sent):
    for stored in ("hatch_refresh:raw-r", "raw-r"):
        sent.clear()
        assert muse_api.refresh_device_token(stored, "homelink-abcdef") == (TOKENS, 200)
        assert [header for header, _ in sent] == ["Bearer hatch_refresh:raw-r"]


def test_a_rejected_refresh_reports_401(monkeypatch):
    def urlopen(req, timeout):
        raise muse_api.urllib.error.HTTPError(req.full_url, 401, "", {}, io.BytesIO(b""))

    monkeypatch.setattr(muse_api.urllib.request, "urlopen", urlopen)
    assert muse_api.refresh_device_token("r", "homelink-abcdef") == (None, 401)


@pytest.mark.parametrize("operation", ["fetch", "refresh"])
def test_short_http_body_is_a_retryable_failure(monkeypatch, operation):
    def urlopen(req, timeout):
        wire = b'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n{"partial":'
        sock = SimpleNamespace(makefile=lambda mode: io.BytesIO(wire))
        response = http.client.HTTPResponse(sock)
        response.begin()
        return response

    monkeypatch.setattr(muse_api.urllib.request, "urlopen", urlopen)
    if operation == "fetch":
        assert muse_api.fetch_vms_with_status("a") == ([], None)
    else:
        assert muse_api.refresh_device_token("r", "homelink-abcdef") == (None, None)
