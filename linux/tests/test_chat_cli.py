# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
import argparse
import io
import json
import socket
import sys

from musegadget.cli import cmd_chat


def test_unicode_prompt_fits_local_socket_limit_and_ndjson_is_preserved(monkeypatch, capsys):
    prompt = "🌟" * 8000  # The Mac composer's 32 KB UTF-8 limit.
    sent = []

    class FakeSocket:
        def __enter__(self): return self
        def __exit__(self, *args): pass
        def settimeout(self, value): pass
        def connect(self, path): pass
        def sendall(self, data):
            assert len(data) < 64 * 1024  # Same bound as the service's reader.
            sent.append(json.loads(data))
        def makefile(self, mode):
            return io.BytesIO(b'{"type":"reply","message_id":"r","text":"Hello","complete":true}\n{"type":"done"}\n')

    monkeypatch.setattr(socket, "socket", lambda *args: FakeSocket())
    monkeypatch.setattr(sys, "stdin", io.StringIO(prompt))
    args = argparse.Namespace(message=["-"], session_id="side-1", json=True)
    assert cmd_chat(args) == 0
    assert sent == [{"message": prompt, "stream": True, "session_id": "side-1"}]
    output = [json.loads(line) for line in capsys.readouterr().out.splitlines()]
    assert output[0]["text"] == "Hello" and output[-1]["type"] == "done"
