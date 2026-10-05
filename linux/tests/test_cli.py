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

import argparse
import io
import json
import socket

from musegadget import cli


def test_send_user_msg_wait_option_requests_and_prints_reply(monkeypatch, capsys):
    class FakeSocket:
        timeout = None
        request = None

        def __init__(self, *_args):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def settimeout(self, timeout):
            self.timeout = timeout

        def connect(self, _path):
            pass

        def sendall(self, request):
            self.request = json.loads(request)

        def makefile(self, _mode):
            return io.BytesIO(b'{"ok":true,"response":"Muse says: 3\\u001b[31m\\n"}\n')

    fake = FakeSocket()
    monkeypatch.setattr(socket, "socket", lambda *_args: fake)
    monkeypatch.setattr(cli.config, "socket_path", lambda: "/tmp/musegadget.sock")

    result = cli.cmd_send_user_msg(argparse.Namespace(
        message=["What is the square root of 9?"],
        session_id=None,
        wait=True,
    ))

    assert result == 0
    assert fake.timeout == 190
    assert fake.request == {"message": "What is the square root of 9?", "wait_for_reply": True}
    assert capsys.readouterr().out == "Muse says: 3\n\n"


def test_sanitize_reply_strips_control_and_format_characters():
    assert cli.sanitize_reply("first\r\nsecond\t\u202e!") == "first\nsecond!"


def test_send_user_msg_help_has_wait_flag_and_no_ask_command(capsys):
    try:
        cli.main(["send-user-msg", "--help"])
    except SystemExit as exc:
        assert exc.code == 0
    help_text = capsys.readouterr().out
    assert "--wait" in help_text

    try:
        cli.main(["ask", "hello"])
    except SystemExit as exc:
        assert exc.code == 2
    assert "invalid choice: 'ask'" in capsys.readouterr().err
