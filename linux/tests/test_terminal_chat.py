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

import asyncio
import io
import json
import tempfile
from pathlib import Path

import pytest

from musegadget import config, terminal_chat
from musegadget.executor import Account, Executor
from musegadget.identity import Identity
from musegadget.noise import ApplicationResponse, BodyChunk, ServiceFrame
from musegadget.service import Service
from musegadget.terminal_chat import LiveReplies, LiveUsers, run_chat
from test_link_client import registered_session
from test_muse_turn import event
from test_service import run_with_socket


def test_live_replies_include_phone_activity_and_final_corrections():
    view = LiveReplies("chat")
    assert view.feed(event("delta.text_append", "r1", parent="phone", text="Hel\x1b")) == "\nMuse: Hel"
    assert view.feed(event("delta.text_append", "r1", text="lo")) == "lo"
    assert view.feed(event("delta.message_done", "r1", content="Hello!")) == "!\n"
    assert view.feed(event("delta.message_done", "r1", content="Hello!")) == ""
    assert view.feed(event("message.assistant", "r1", display_text="Corrected")) == "\nMuse: Corrected\n"
    assert view.feed(event("message.assistant", "r2", role="user", content="user")) == ""
    assert view.feed(event("message.assistant", "r3", session="elsewhere", content="other")) == ""


def test_lone_surrogates_cannot_reach_the_terminal_as_raw_bytes():
    # "\udc9d" would be written as a raw C1 byte by a surrogateescape stream.
    view = LiveReplies("chat")
    assert view.feed(event("message.assistant", "r1", content="a\udc9d52;c;eA==\udc9cb")) == "\nMuse: a52;c;eA==b\n"
    users = LiveUsers("chat")
    shown = users.feed(event("message.user", "u9", content="hi\ud800"))
    assert shown and all("\ud800" not in str(item) for item in shown)
    output = io.StringIO()
    from musegadget.terminal_chat_ui import PlainDisplay
    PlainDisplay(output).status(terminal_chat._strip_controls("Muse: \udc9dx"))
    assert "\udc9d" not in output.getvalue()


def test_live_replies_deduplicate_sequences_and_bound_storage():
    view = LiveReplies()
    first = event("delta.text_append", "r", text="a")
    first["seq"] = 1
    assert view.feed(first) == "\nMuse: a"
    assert view.feed(first) == ""
    for i in range(100):
        view.feed(event("message.assistant", str(i), content="x"))
    assert len(view.messages) == 32
    with pytest.raises(ValueError, match="64 KiB"):
        view.feed(event("message.assistant", "large", content="x" * 65537))


def test_live_replies_keep_their_order_apart_from_user_events():
    view = LiveReplies("chat")
    user = event("message.user", "u1", content="hi")
    user["seq"] = 2000
    assert view.feed(user) == ""
    reply = event("delta.text_append", "r1", parent="u1", text="Hello")
    reply["seq"] = 1147
    assert view.feed(reply) == "\nMuse: Hello"


def test_an_answer_row_below_live_sequence_is_shown():
    view = LiveReplies("chat")
    delta = event("delta.text_append", "r1", parent="u1", text="Streamed")
    delta["seq"] = 1150
    assert view.feed(delta) == "\nMuse: Streamed"
    # A complete answer row, with no deltas, numbered in the chat's own order.
    row = event("message.assistant", "r2", parent="u2", content="Row answer")
    row["seq"] = 977
    assert view.feed(row) == "\nMuse: Row answer\n"
    assert view.feed(row) == ""


def test_live_users_include_remote_messages_and_suppress_local_echoes_by_id():
    users = LiveUsers('chat')
    assert users.feed(event('message.user', 'web', display_text='Web\x1b message')) == ['Web message']
    assert users.feed(event('message.user', 'web', content='duplicate')) == []
    users.sending = True
    assert users.feed(event('message.user', 'local', content='same words')) == []
    assert users.feed(event('message.user', 'phone', content='same words')) == []
    assert users.finish_send({'result': {'message_id': 'local'}}) == ['same words']
    assert users.feed(event('message.user', 'local', content='same words')) == []
    assert users.feed(event('message.user', 'other', session='elsewhere', content='skip')) == []
    assert users.feed(event('message.user', 'bad-role', role='assistant', content='skip')) == []
    assert users.feed(event('message.user', None, content='skip')) == []
    assert users.feed(event('message.user', 'incomplete', display_text_ready=False, content='wait')) == []
    assert users.feed(event('message.user', 'incomplete', display_text='Ready')) == ['Ready']


def test_live_users_read_the_text_field_too():
    users = LiveUsers('chat')
    assert users.feed(event('message.user', 'remote-1', text='hello\x1b')) == ['hello']
    users.sending = True
    assert users.feed(event('message.user', 'local', text='typed here')) == []
    assert users.feed(event('message.user', 'phone', text='from the phone')) == []
    # The local echo stays hidden; the phone's message is shown.
    assert users.finish_send({'result': {'message_id': 'local'}}) == ['from the phone']
    assert users.feed(event('message.user', 'local', text='typed here')) == []


def test_live_users_bound_text_storage_and_sequences():
    users = LiveUsers()
    item = event('message.user', 'first', content='hello')
    item['seq'] = 2
    assert users.feed(item) == ['hello']
    item = event('message.user', 'stale', content='skip')
    item['seq'] = 1
    assert users.feed(item) == []
    for i in range(300):
        users.feed(event('message.user', str(i), content='x'))
    assert len(users.seen) == 256
    with pytest.raises(ValueError, match='64 KiB'):
        users.feed(event('message.user', 'huge', content='é' * 32769))
    users.sending = True
    for i in range(64):
        assert users.feed(event('message.user', 'pending-' + str(i), content='x')) == []
    with pytest.raises(ValueError, match='Too many user messages'):
        users.feed(event('message.user', 'overflow', content='x'))
    assert len(users.finish_send()) == 64
    assert users.pending == []


@pytest.mark.parametrize("exit_line", [b"/quit\n", b"/exit\n", b"", None])
@pytest.mark.parametrize("session_id", [None, "chat"])
def test_repl_uses_service_transport_and_receives_before_ack(monkeypatch, exit_line, session_id):
    async def scenario():
        session, vm, stop, session_task = await registered_session()
        with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
            path = Path(tmp) / "chat.sock"
            monkeypatch.setenv(config.SOCKET_ENV, str(path))
            service = Service(Identity("02:00:00:00:00:01"), Executor(Account.current()))
            service._current = session
            server = await service.serve_local(path)
            stdin = asyncio.StreamReader()
            output = io.StringIO()
            client = asyncio.create_task(run_chat(session_id, input_reader=stdin, output=output))
            try:
                subscription = await vm.next_frame()
                assert subscription.value.path == "/chat/subscribe"
                assert json.loads(subscription.value.body) == ({"session_id": session_id} if session_id else {})
                # No terminal send until subscription readiness.
                stdin.feed_data(b"/clear\nhello\n")
                assert not client.done()
                await vm.send_frame(ServiceFrame.response(subscription.stream_id, ApplicationResponse(
                    status=200, end_body=False)))
                request = await vm.next_frame()
                assert request.value.path == "/chat/stream"
                assert json.loads(request.value.body)["message"] == "hello"
                for item in [event('message.user', 'u', display_text='hello'),
                             event('message.user', 'web', display_text='Web question'),
                             event("delta.text_append", "r", parent="u", text="Early reply"),
                             event("delta.message_done", "r", parent="u", content="Early reply"),
                             event("message.assistant", "phone-reply", parent="phone", content="Phone reply")]:
                    await vm.send_frame(ServiceFrame.body_chunk(subscription.stream_id, BodyChunk(
                        data=json.dumps(item).encode() + b"\n")))
                await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
                    status=200, body=b'{"message_id":"u","session_id":"chat"}', end_body=True)))
                for item in [event('message.user', 'u', display_text='hello'),
                             event('message.user', 'phone', content='Phone question'),
                             event('message.assistant', 'after', content='After ack')]:
                    await vm.send_frame(ServiceFrame.body_chunk(subscription.stream_id, BodyChunk(
                        data=json.dumps(item).encode() + b'\n')))
                while ('After ack' not in output.getvalue()
                       or 'User: Web question' not in output.getvalue()):
                    await asyncio.sleep(0.01)
                assert "Early reply" in output.getvalue()
                assert 'Phone reply' in output.getvalue()
                assert 'User: Phone question' in output.getvalue()
                assert output.getvalue().count('User: hello') == 1
                assert 'Screen clearing is unavailable' in output.getvalue()
                assert 'User: /clear' not in output.getvalue()
                if exit_line is None:
                    client.cancel()
                    with pytest.raises(asyncio.CancelledError):
                        await client
                else:
                    stdin.feed_data(exit_line)
                    stdin.feed_eof()
                    await client
                # The command session stays registered after terminal exit.
                assert session.registered.is_set()
                while session._streams:
                    # Only the control stream remains outside _streams.
                    await asyncio.sleep(0.01)
            finally:
                client.cancel()
                await asyncio.gather(client, return_exceptions=True)
                server.close()
                await server.wait_closed()
                stop.set()
                await session_task

    asyncio.run(asyncio.wait_for(scenario(), 5))


@pytest.mark.parametrize("payload", [{"op": "subscribe_chat"},
                                     {"op": "subscribe_chat", "session_id": "../invalid"}])
def test_subscription_errors_are_typed(payload):
    async def check(service, path):
        reader, writer = await asyncio.open_unix_connection(str(path))
        writer.write(json.dumps(payload).encode() + b"\n")
        await writer.drain()
        result = json.loads(await reader.readline())
        assert result["type"] == "error"
        assert result["error"]
        assert await reader.read() == b""
        writer.close()
        await writer.wait_closed()
    run_with_socket(check)


def test_subscription_disconnect_releases_upstream():
    async def check(service, path):
        closed = asyncio.Event()

        class Session:
            registered_at = 1

            async def subscribe_chat(self, session_id, *, opened):
                opened.set()
                try:
                    await asyncio.Event().wait()
                    yield {}
                finally:
                    closed.set()

        service._current = Session()
        reader, writer = await asyncio.open_unix_connection(str(path))
        writer.write(b'{"op":"subscribe_chat"}\n')
        await writer.drain()
        assert json.loads(await reader.readline()) == {"type": "ready"}
        writer.close()
        await writer.wait_closed()
        await asyncio.wait_for(closed.wait(), 1)
    run_with_socket(check)


def test_subscription_ready_precedes_an_event_arriving_with_headers():
    async def scenario():
        service = Service(Identity("02:00:00:00:00:01"), Executor(Account.current()))
        reader = asyncio.StreamReader()
        closed = asyncio.Event()
        item = event("message.assistant", "r", content="Immediate reply")

        class Session:
            registered_at = 1

            async def subscribe_chat(self, session_id, *, opened):
                try:
                    opened.set()
                    yield item
                    await asyncio.Event().wait()
                finally:
                    closed.set()

        class Writer:
            def __init__(self):
                self.records = []
                self.was_closed = False

            def write(self, raw):
                self.records.append(json.loads(raw))

            async def drain(self):
                await asyncio.sleep(0)
                if len(self.records) == 2:
                    reader.feed_eof()

            def close(self):
                self.was_closed = True

        service._current = Session()
        writer = Writer()
        await asyncio.wait_for(service._local_subscribe({}, reader, writer), 1)
        assert writer.records == [
            {"type": "ready"}, {"type": "chat_event", "event": item},
        ]
        assert closed.is_set()
        assert writer.was_closed

    asyncio.run(scenario())


def test_cli_chat_never_reads_pairing(monkeypatch):
    from musegadget import cli
    async def chat(session_id):
        assert session_id == "side"
    def forbidden(*args, **kwargs):
        pytest.fail("terminal chat read identity or pairing state")
    monkeypatch.setattr(terminal_chat, "run_chat", chat)
    monkeypatch.setattr(config, "load_json", forbidden)
    monkeypatch.setattr(cli.identity, "load_or_create", forbidden)
    assert cli.main(["chat", "--session-id", "side"]) == 0


@pytest.mark.parametrize("failure", ["refused", "transport"])
def test_subscription_failure_ends_client_without_resending(failure):
    async def check(service, path):
        closed = asyncio.Event()

        class Session:
            registered_at = 1

            async def subscribe_chat(self, session_id, *, opened):
                try:
                    if failure == "refused":
                        from musegadget.link_client import HttpStreamError
                        raise HttpStreamError(403, "/chat/subscribe")
                    opened.set()
                    await asyncio.Event().wait()
                    yield {}
                finally:
                    closed.set()

        service._current = Session()
        reader, writer = await asyncio.open_unix_connection(str(path))
        writer.write(b'{"op":"subscribe_chat"}\n')
        await writer.drain()
        if failure == "transport":
            assert json.loads(await reader.readline())["type"] == "ready"
            service._current = None
        result = json.loads(await asyncio.wait_for(reader.readline(), 1))
        assert result["type"] == "error"
        assert await reader.read() == b""
        await asyncio.wait_for(closed.wait(), 1)
        writer.close()
        await writer.wait_closed()
    run_with_socket(check)


def test_slow_local_reader_releases_upstream(monkeypatch):
    from musegadget import service as service_module
    monkeypatch.setattr(service_module, "LOCAL_WRITE_TIMEOUT_S", 0.01)

    async def scenario():
        service = Service(Identity("02:00:00:00:00:01"), Executor(Account.current()))
        closed = asyncio.Event()

        class Session:
            registered_at = 1

            async def subscribe_chat(self, session_id, *, opened):
                opened.set()
                try:
                    yield event("message.assistant", "r", content="reply")
                    await asyncio.Event().wait()
                finally:
                    closed.set()

        class Writer:
            records = []
            was_closed = False

            def write(self, raw):
                self.records.append(json.loads(raw))

            async def drain(self):
                await asyncio.Event().wait()

            def close(self):
                self.was_closed = True

        service._current = Session()
        writer = Writer()
        await asyncio.wait_for(service._local_subscribe({}, asyncio.StreamReader(), writer), 1)
        assert closed.is_set()
        assert writer.was_closed
        assert writer.records[0] == {"type": "ready"}
    asyncio.run(scenario())


def test_rejected_terminal_send_is_reported_and_connections_close(monkeypatch):
    async def scenario():
        sent = []
        closed = asyncio.Event()

        async def handle(reader, writer):
            try:
                request = json.loads(await reader.readline())
                if request.get("op") == "subscribe_chat":
                    writer.write(b'{"type":"ready"}\n')
                    await writer.drain()
                    await reader.read()
                    closed.set()
                else:
                    sent.append(request)
                    writer.write(b'{"ok":false,"status":403,"response":{}}\n')
                    await writer.drain()
            finally:
                writer.close()
                await writer.wait_closed()

        with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
            path = Path(tmp) / "chat.sock"
            monkeypatch.setenv(config.SOCKET_ENV, str(path))
            server = await asyncio.start_unix_server(handle, str(path))
            stdin = asyncio.StreamReader()
            stdin.feed_data(b"hello\n")
            try:
                with pytest.raises(ConnectionError):
                    await run_chat(input_reader=stdin, output=io.StringIO())
                await asyncio.wait_for(closed.wait(), 1)
                assert sent == [{"message": "hello"}]
            finally:
                server.close()
                await server.wait_closed()
    asyncio.run(asyncio.wait_for(scenario(), 2))


@pytest.mark.parametrize("session_id", [None, "chat"])
@pytest.mark.parametrize("combined", [True, False])
def test_follow_ups_are_ordered_and_progress_is_shown(monkeypatch, session_id, combined):
    async def scenario():
        session, vm, stop, session_task = await registered_session()
        with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
            path = Path(tmp) / "chat.sock"
            monkeypatch.setenv(config.SOCKET_ENV, str(path))
            service = Service(Identity("02:00:00:00:00:01"), Executor(Account.current()))
            service._current = session
            server = await service.serve_local(path)
            stdin, output = asyncio.StreamReader(), io.StringIO()
            client = asyncio.create_task(run_chat(session_id, input_reader=stdin, output=output))

            async def emit(item):
                await vm.send_frame(ServiceFrame.body_chunk(subscription.stream_id, BodyChunk(
                    data=json.dumps(item).encode() + b"\n")))

            async def wait_text(text):
                while text not in output.getvalue():
                    assert not client.done()
                    await asyncio.sleep(0.01)

            try:
                subscription = await vm.next_frame()
                await vm.send_frame(ServiceFrame.response(subscription.stream_id, ApplicationResponse(
                    status=200, end_body=False)))
                stdin.feed_data(b"dinner\nvegetarian\nfor two\n")
                for i, text in enumerate(["dinner", "vegetarian", "for two"]):
                    request = await vm.next_frame()
                    assert request.value.path == "/chat/stream"
                    body = json.loads(request.value.body)
                    assert body["message"] == text
                    assert body.get("session_id") == session_id
                    # A reply/finish before ACK must not discard queued follow-ups.
                    if not combined or i == 2:
                        await emit(event("delta.message_done", "r" + str(i),
                                         parent="u" + str(i), content="Answer " + str(i)))
                    await emit(event("task.status", parent="u" + str(i), status="completed"))
                    if i == 2:
                        await asyncio.sleep(0.15)
                        assert "Muse is ready" not in output.getvalue()
                    await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
                        status=200, body=json.dumps({"message_id": "u" + str(i),
                                                    "session_id": "chat"}).encode(), end_body=True)))
                await wait_text("Answer 2")
                await wait_text("Muse is ready")
                assert output.getvalue().count("User: ") == 3
                assert "Message submitted." not in output.getvalue()
                assert "Waiting for Muse" in output.getvalue()
                # A new turn receives public progress, but not unrelated status.
                stdin.feed_data(b"dessert\n")
                request = await vm.next_frame()
                await emit(event("agent.status", parent="u3", activity_code="working",
                                 activity_text="Planning\x1b dessert"))
                await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
                    status=200, body=b'{"message_id":"u3","session_id":"chat"}', end_body=True)))
                await wait_text("Planning dessert")
                await emit(event("agent.status", parent="phone", activity_code="working",
                                 activity_text="Unrelated activity"))
                await emit(event("delta.text_append", "r3", parent="u3", text="Dessert"))
                await wait_text("Dessert")
                stdin.feed_data(b"coffee too\n")
                request = await vm.next_frame()
                assert json.loads(request.value.body)["message"] == "coffee too"
                # Earlier completion cannot finish a follow-up awaiting ACK.
                await emit(event("delta.message_done", "r3", parent="u3", content="Dessert"))
                await emit(event("task.status", parent="u3", status="completed"))
                await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
                    status=200, body=b'{"message_id":"u4","session_id":"chat"}', end_body=True)))
                await emit(event("delta.message_done", "r4", parent="u4", content="Coffee"))
                await emit(event("task.status", parent="u4", status="completed"))
                await wait_text("Coffee")
                assert "Unrelated activity" not in output.getvalue()
                stdin.feed_data(b"/quit\n")
                await client
                assert session.registered.is_set()
            finally:
                client.cancel()
                await asyncio.gather(client, return_exceptions=True)
                server.close()
                await server.wait_closed()
                stop.set()
                await session_task
    asyncio.run(asyncio.wait_for(scenario(), 8))


@pytest.mark.parametrize("ending", ["quit", "eof", "cancel", "disconnect", "reject"])
def test_pending_queue_is_bounded_and_exit_reports_unsent(monkeypatch, ending):
    async def check(service, path):
        ready, release = asyncio.Event(), asyncio.Event()
        started, cancelled = asyncio.Event(), asyncio.Event()

        class Session:
            registered_at = 1

            async def subscribe_chat(self, session_id, *, opened):
                opened.set()
                ready.set()
                await release.wait()
                if ending == "reject":
                    await asyncio.Event().wait()
                if False:
                    yield {}

        async def send(*args):
            started.set()
            try:
                await release.wait()
                if ending == "reject":
                    raise ConnectionError("request rejected")
                await asyncio.Event().wait()
            finally:
                cancelled.set()

        service._current = Session()
        monkeypatch.setenv(config.SOCKET_ENV, str(path))
        monkeypatch.setattr(terminal_chat, "_send", send)
        monkeypatch.setattr(terminal_chat, "MAX_QUEUED_MESSAGES", 2)
        stdin, output = asyncio.StreamReader(), io.StringIO()
        client = asyncio.create_task(run_chat(input_reader=stdin, output=output))
        try:
            await ready.wait()
            stdin.feed_data(b"first\n")
            await started.wait()
            stdin.feed_data(b"second\nthird\nfourth\n")
            while "queue is full" not in output.getvalue():
                await asyncio.sleep(0.01)
            if ending == "quit":
                stdin.feed_data(b"/quit\n")
                await client
            elif ending == "eof":
                stdin.feed_eof()
                await client
            elif ending == "cancel":
                client.cancel()
                with pytest.raises(asyncio.CancelledError):
                    await client
            else:
                release.set()
                with pytest.raises(ConnectionError):
                    await client
            assert "2 queued message(s) were not sent" in output.getvalue()
            assert "may have received it" in output.getvalue()
            assert "Chat closed" in output.getvalue()
            assert cancelled.is_set()
        finally:
            client.cancel()
            await asyncio.gather(client, return_exceptions=True)
    async def bounded_check(service, path):
        await asyncio.wait_for(check(service, path), 3)
    run_with_socket(bounded_check)


def test_unfinished_turn_stops_tracking_but_keeps_the_chat_open(monkeypatch):
    async def check(service, path):
        events, ready = asyncio.Queue(), asyncio.Event()

        class Session:
            registered_at = 1

            async def subscribe_chat(self, session_id, *, opened):
                opened.set()
                ready.set()
                while True:
                    yield await events.get()

        async def send(*args):
            return {"message_id": "u1", "session_id": "chat"}

        service._current = Session()
        monkeypatch.setenv(config.SOCKET_ENV, str(path))
        monkeypatch.setattr(terminal_chat, "_send", send)
        monkeypatch.setattr(terminal_chat, "TURN_TIMEOUT", 0.3)
        stdin, output = asyncio.StreamReader(), io.StringIO()
        client = asyncio.create_task(run_chat(input_reader=stdin, output=output))

        async def wait_text(text):
            while text not in output.getvalue():
                assert not client.done(), output.getvalue()
                await asyncio.sleep(0.01)

        try:
            await ready.wait()
            stdin.feed_data(b"long task\n")
            await wait_text("Waiting for Muse")
            # Muse stays busy past the timeout: the chat must stay open.
            await events.put(event("agent.status", parent="u1", activity_code="working",
                                   activity_text="Working"))
            await wait_text("replies will appear as they arrive")
            await asyncio.sleep(0.2)
            assert not client.done()
            await events.put(event("delta.message_done", "r1", parent="u1", content="Done at last"))
            await wait_text("Done at last")
            stdin.feed_data(b"/quit\n")
            await client
        finally:
            client.cancel()
            await asyncio.gather(client, return_exceptions=True)

    async def bounded_check(service, path):
        await asyncio.wait_for(check(service, path), 5)
    run_with_socket(bounded_check)


def test_other_commands_never_load_the_terminal_ui():
    import subprocess
    import sys
    code = ("import sys\n"
            "from musegadget import cli\n"
            "try:\n"
            "    cli.main(['--help'])\n"
            "except SystemExit:\n"
            "    pass\n"
            "assert 'curses' not in sys.modules, 'curses loaded'\n"
            "assert 'musegadget.terminal_chat' not in sys.modules\n")
    result = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def _scripted_chat(monkeypatch, path, send):
    events, ready = asyncio.Queue(), asyncio.Event()

    class Session:
        registered_at = 1

        async def subscribe_chat(self, session_id, *, opened):
            opened.set()
            ready.set()
            while True:
                yield await events.get()

    monkeypatch.setenv(config.SOCKET_ENV, str(path))
    monkeypatch.setattr(terminal_chat, "_send", send)
    return Session(), events, ready


def test_plain_output_strips_status_controls_and_relabels_interrupted_replies(monkeypatch):
    async def check(service, path):
        async def send(*args):
            return {"message_id": "u1", "session_id": "chat"}

        service._current, events, ready = _scripted_chat(monkeypatch, path, send)
        stdin, output = asyncio.StreamReader(), io.StringIO()
        client = asyncio.create_task(run_chat(input_reader=stdin, output=output))

        async def wait_text(text):
            while text not in output.getvalue():
                assert not client.done(), output.getvalue()
                await asyncio.sleep(0.01)

        try:
            await ready.wait()
            stdin.feed_data(b"hi\n")
            await wait_text("Waiting for Muse")
            await events.put(event("delta.text_append", "r1", parent="u1", text="Hello"))
            await wait_text("Muse: Hello")
            # A status code with no text, carrying a clipboard-writing escape.
            await events.put(event("agent.status", parent="u1",
                                   activity_code="\x1b]52;c;b3duZWQ=\x07"))
            await wait_text("]52;c;b3duZWQ=")
            await events.put(event("delta.text_append", "r1", parent="u1", text=" world"))
            await wait_text("Muse: Hello world")
            stdin.feed_data(b"/quit\n")
            await client
            assert "\x1b" not in output.getvalue() and "\x07" not in output.getvalue()
        finally:
            client.cancel()
            await asyncio.gather(client, return_exceptions=True)

    async def bounded_check(service, path):
        await asyncio.wait_for(check(service, path), 5)
    run_with_socket(bounded_check)


def test_streaming_during_an_unacknowledged_follow_up_keeps_the_chat_open(monkeypatch):
    async def check(service, path):
        release = asyncio.Event()
        sends = []

        async def send(path, message, session_id):
            sends.append(message)
            if len(sends) == 2:
                await release.wait()
            return {"message_id": "u" + str(len(sends)), "session_id": "chat"}

        service._current, events, ready = _scripted_chat(monkeypatch, path, send)
        stdin, output = asyncio.StreamReader(), io.StringIO()
        client = asyncio.create_task(run_chat(input_reader=stdin, output=output))

        async def wait_text(text):
            while text not in output.getvalue():
                assert not client.done(), output.getvalue()
                await asyncio.sleep(0.01)

        try:
            await ready.wait()
            stdin.feed_data(b"first\n")
            await events.put(event("delta.text_append", "r1", parent="u1", text="a"))
            await wait_text("Muse: a")
            stdin.feed_data(b"second\n")
            while len(sends) < 2:
                await asyncio.sleep(0.01)
            # More streamed chunks than the turn holds before an acknowledgement.
            for _ in range(80):
                await events.put(event("delta.text_append", "r1", parent="u1", text="b"))
            await wait_text("a" + "b" * 80)
            release.set()
            await events.put(event("delta.message_done", "r2", parent="u2", content="Second answer"))
            await wait_text("Second answer")
            assert not client.done()
            stdin.feed_data(b"/quit\n")
            await client
        finally:
            client.cancel()
            await asyncio.gather(client, return_exceptions=True)

    async def bounded_check(service, path):
        await asyncio.wait_for(check(service, path), 5)
    run_with_socket(bounded_check)


def test_ctrl_c_cancels_the_chat_so_cleanup_runs(monkeypatch):
    import os
    import signal
    cleaned = []

    async def chat(session_id):
        try:
            os.kill(os.getpid(), signal.SIGINT)
            await asyncio.sleep(5)
        finally:
            cleaned.append(session_id)

    monkeypatch.setattr(terminal_chat, "run_chat", chat)
    # Not a KeyboardInterrupt raised inside the loop: the chat is cancelled.
    with pytest.raises(asyncio.CancelledError):
        asyncio.run(asyncio.wait_for(terminal_chat.run_until_interrupted("side"), 3))
    assert cleaned == ["side"]
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler


def test_subscription_cleanup_is_bounded_when_the_upstream_close_stalls(monkeypatch):
    from musegadget import service as service_module
    monkeypatch.setattr(service_module, "CLEANUP_S", 0.1)

    async def scenario():
        service = Service(Identity("02:00:00:00:00:01"), Executor(Account.current()))
        release = asyncio.Event()

        class Session:
            registered_at = 1

            async def subscribe_chat(self, session_id, *, opened):
                opened.set()
                try:
                    await asyncio.Event().wait()
                    yield {}
                finally:
                    # Like a reset waiting out a stalled transport write.
                    while not release.is_set():
                        try:
                            await asyncio.shield(release.wait())
                        except asyncio.CancelledError:
                            pass

        class Writer:
            records = []
            was_closed = False

            def write(self, raw):
                self.records.append(json.loads(raw))

            async def drain(self):
                pass

            def close(self):
                self.was_closed = True

        service._current = Session()
        reader, writer = asyncio.StreamReader(), Writer()
        handler = asyncio.create_task(service._local_subscribe({}, reader, writer))
        while not writer.records:
            await asyncio.sleep(0.01)
        reader.feed_eof()  # The local client disconnects.
        try:
            await asyncio.wait({handler}, timeout=1)
            assert handler.done() and writer.was_closed
        finally:
            release.set()
            await asyncio.wait({handler}, timeout=1)
    asyncio.run(scenario())


def test_event_write_timeout_ends_the_subscription_when_the_upstream_close_stalls(monkeypatch):
    from musegadget import service as service_module
    monkeypatch.setattr(service_module, "LOCAL_WRITE_TIMEOUT_S", 0.05)
    monkeypatch.setattr(service_module, "CLEANUP_S", 0.1)

    async def scenario():
        service = Service(Identity("02:00:00:00:00:01"), Executor(Account.current()))
        release = asyncio.Event()

        class Session:
            registered_at = 1

            async def subscribe_chat(self, session_id, *, opened):
                opened.set()
                try:
                    yield event("message.assistant", "r", content="reply")
                    await asyncio.Event().wait()
                finally:
                    # Like a reset waiting out a stalled transport write.
                    while not release.is_set():
                        try:
                            await asyncio.shield(release.wait())
                        except asyncio.CancelledError:
                            pass

        class Writer:
            records = []
            was_closed = False

            def write(self, raw):
                self.records.append(json.loads(raw))

            async def drain(self):
                # The ready record drains; the client then stops reading.
                if len(self.records) > 1:
                    await asyncio.Event().wait()

            def close(self):
                self.was_closed = True

        service._current = Session()
        writer = Writer()
        # The local client stays connected (no EOF) throughout.
        handler = asyncio.create_task(service._local_subscribe({}, asyncio.StreamReader(), writer))
        try:
            await asyncio.wait({handler}, timeout=1)
            assert handler.done() and writer.was_closed
            assert writer.records[0] == {"type": "ready"}
            assert writer.records[1]["type"] == "chat_event"
        finally:
            release.set()
            await asyncio.wait({handler}, timeout=1)
    asyncio.run(scenario())


def test_chat_errors_from_muse_cannot_reach_the_terminal_raw(monkeypatch, capsys):
    async def chat(session_id):
        raise ConnectionError("stream reset: \x1b]52;c;b3duZWQ=\x07")

    monkeypatch.setattr(terminal_chat, "run_chat", chat)
    assert terminal_chat.command(type("Args", (), {"session_id": None})()) == 1
    err = capsys.readouterr().err
    assert "\x1b" not in err and "\x07" not in err
    assert "]52;c;b3duZWQ=" in err


@pytest.mark.parametrize("ending", ["quit", "eof"])
def test_plain_mode_restores_blocking_stdin(monkeypatch, ending):
    import os
    import sys

    async def check(service, path):
        async def send(*args):
            return {"message_id": "u1", "session_id": "chat"}

        service._current, events, ready = _scripted_chat(monkeypatch, path, send)
        read_fd, write_fd = os.pipe()
        shared = os.dup(read_fd)  # Same open file, like a shell's terminal.
        stdin = os.fdopen(read_fd, "rb", buffering=0)
        monkeypatch.setattr(sys, "stdin", stdin)
        try:
            assert os.get_blocking(shared)
            if ending == "quit":
                os.write(write_fd, b"/quit\n")
            else:
                os.close(write_fd)  # EOF: the transport closes what it reads.
            await asyncio.wait_for(run_chat(output=io.StringIO()), 3)
            assert os.get_blocking(shared)
            assert not stdin.closed
        finally:
            os.close(shared)
            if ending == "quit":
                os.close(write_fd)
            stdin.close()

    run_with_socket(check)
