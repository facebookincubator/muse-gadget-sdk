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

"""send-user-msg --wait: the local socket's wait and the CLI that prints the answer."""

from __future__ import annotations

import asyncio
import json
import socket
import tempfile
import threading
from pathlib import Path

import pytest

from musegadget import cli, config, service as service_module
from musegadget.link_client import HttpStreamError
from musegadget.noise import ApplicationResponse, BodyChunk, Header, ServiceFrame
from musegadget.service import send_and_wait

from test_link_client import bounded_scenario, registered_session
from test_service import ask, run_with_socket


def event(name, message_id=None, *, parent=None, session="main", **payload):
    data = {"session_id": session, **payload}
    if message_id is not None:
        data["message_id"] = message_id
    if parent is not None:
        data["parent_message_id"] = parent
    return {"type": "event", "event": name, "payload": data}


# The request's own message, which the answer follows.
NOTE = {"type": "event", "event": "message.user",
        "payload": {"session_id": "main", "message_id": "u1", "role": "user", "text": "what is 2 + 2?"}}
# How Muse answers on the main chat: no reply_to_message_id, a start and a done
# without a parent, and text deltas that name the reply itself as their parent.
ANSWER = [
    NOTE,
    event("agent.status", activity_code="thinking"),
    event("delta.message_start", "r1"),
    event("delta.text_append", "r1", parent="r1", text="Four"),
    event("delta.text_append", "r1", parent="r1", text="."),
    event("delta.message_done", "r1", display_text="Four."),
    event("task.status", status="completed"),
    event("agent.status", activity_code="idle"),
]
ACK = {"ok": True, "status": 200, "response": {"message_id": "u1", "session_id": "main"}}


class ChatSession:
    """A LinkSession stand-in: a subscription fed from a queue, and sends that record order."""

    registered_at = 1.0

    def __init__(self, *, refuse: int | None = None, refuse_once: bool = False,
                 ack: dict = ACK) -> None:
        self.events: asyncio.Queue = asyncio.Queue()
        self.log: list = []
        self.refuse = refuse
        self.refuse_once = refuse_once
        self.ack = ack
        self.open = 0

    async def subscribe_chat(self, session_id=None, *, opened=None):
        self.log.append(("subscribe", session_id))
        if self.refuse:
            status, self.refuse = self.refuse, (None if self.refuse_once else self.refuse)
            raise HttpStreamError(status, "/chat/subscribe")
        self.open += 1
        opened.set()
        try:
            while True:
                item = await self.events.get()
                if item is None:
                    return
                yield item
        finally:
            self.open -= 1

    async def send_chat(self, message, session_id=None):
        self.log.append(("send", message, session_id, self.open))
        return self.ack

    def answer(self, events) -> None:
        for item in events:
            self.events.put_nowait(item)


def send_of(session, message="what is 2 + 2?", session_id=None):
    return lambda: session.send_chat(message, session_id)


def run(coro):
    return asyncio.run(bounded_scenario(coro))


def test_answer_on_the_main_chat_is_returned_once_muse_finishes():
    async def scenario():
        session = ChatSession()
        session.answer(ANSWER)
        result = await send_and_wait(session, send_of(session), None, 5)
        assert result == {**ACK, "reply": "Four."}
        # Subscribed first, and the message went only once the subscription was open.
        assert session.log == [("subscribe", None), ("send", "what is 2 + 2?", None, 1)]
        assert session.open == 0  # the subscription is closed afterwards

    run(scenario())


def test_reply_to_another_message_is_not_the_answer():
    async def scenario():
        session = ChatSession()
        session.answer([
            NOTE,
            event("delta.message_start", "other", parent="someone-else"),
            event("delta.text_append", "other", parent="other", text="Not yours"),
            event("delta.message_done", "other", display_text="Not yours"),
            event("message.assistant", "elsewhere", session="side", display_text="Wrong chat"),
            *ANSWER[1:],
        ])
        result = await send_and_wait(session, send_of(session), None, 5)
        assert result["reply"] == "Four."

    run(scenario())


def test_an_earlier_answer_still_arriving_is_not_taken_for_this_one():
    async def scenario():
        session = ChatSession()
        session.answer([
            # Muse finishing something asked earlier, before this message appears.
            event("message.assistant", "old", display_text="Yesterday's answer"),
            event("task.status", status="completed"),
        ])
        waiting = asyncio.ensure_future(send_and_wait(session, send_of(session), None, 5))
        await asyncio.sleep(0.3)
        assert not waiting.done()
        session.answer(ANSWER)
        assert (await waiting)["reply"] == "Four."

    run(scenario())


def test_a_reply_naming_the_message_counts_wherever_it_falls():
    async def scenario():
        session = ChatSession()
        session.answer([
            event("message.assistant", "r1", parent="u1", display_text="Named."),
            event("task.status", status="completed"),
        ])
        result = await send_and_wait(session, send_of(session), None, 5)
        assert result["reply"] == "Named."

    run(scenario())


def test_a_reply_chained_to_a_named_reply_counts_too():
    async def scenario():
        session = ChatSession()
        session.answer([
            event("message.assistant", "r1", parent="u1", display_text="Let me check."),
            event("message.assistant", "r2", parent="r1", display_text="It is four."),
            event("task.status", parent="u1", status="completed"),
        ])
        result = await send_and_wait(session, send_of(session), None, 5)
        assert result["reply"] == "Let me check.\n\nIt is four."

    run(scenario())


def test_a_message_in_another_chat_does_not_end_the_answer():
    async def scenario():
        session = ChatSession()
        session.answer([
            NOTE,
            {"type": "event", "event": "message.user",
             "payload": {"session_id": "side", "message_id": "s1", "role": "user"}},
            *ANSWER[1:],
        ])
        result = await send_and_wait(session, send_of(session), None, 5)
        assert result["reply"] == "Four."

    run(scenario())


def test_a_message_sent_from_elsewhere_ends_the_answer_but_not_a_reply_under_way():
    async def scenario():
        session = ChatSession()
        session.answer([
            NOTE,
            event("delta.message_start", "r1"),
            event("delta.text_append", "r1", parent="r1", text="Fo"),
            {"type": "event", "event": "message.user",
             "payload": {"session_id": "main", "message_id": "app-1", "role": "user"}},
            event("delta.message_start", "app-answer"),
            event("delta.message_done", "app-answer", display_text="For the app"),
            event("delta.text_append", "r1", parent="r1", text="ur."),
            event("delta.message_done", "r1", display_text="Four."),
        ])
        result = await send_and_wait(session, send_of(session), None, 5)
        assert result["reply"] == "Four."

    run(scenario())


def test_a_blank_reply_waits_for_text_that_comes_after_the_task_ends():
    async def scenario():
        session = ChatSession()
        session.answer([
            NOTE,
            event("delta.message_start", "r1"),
            event("delta.message_done", "r1", display_text=""),
            event("task.status", status="completed"),
        ])
        waiting = asyncio.ensure_future(send_and_wait(session, send_of(session), None, 5))
        await asyncio.sleep(0.3)
        assert not waiting.done()
        session.answer([event("message.assistant", "r1", display_text="Late text.")])
        assert (await waiting)["reply"] == "Late text."

    run(scenario())


def test_a_repeated_seq_is_dropped():
    async def scenario():
        session = ChatSession()
        rows = [dict(item, seq=number) for number, item in enumerate([
            NOTE,
            event("delta.message_start", "r1"),
            event("delta.text_append", "r1", parent="r1", text="Four"),
            event("delta.text_append", "r1", parent="r1", text="."),
            event("delta.message_done", "r1"),  # no final text: the deltas are the answer
            event("task.status", status="completed"),
        ], 1)]
        session.answer(rows[:3] + rows[2:3] + rows[3:])  # a delta sent twice
        result = await send_and_wait(session, send_of(session), None, 5)
        assert result["reply"] == "Four."

    run(scenario())


def test_a_new_side_chat_is_created_by_the_message_then_followed():
    async def scenario():
        session = ChatSession(refuse=404, refuse_once=True,
                              ack={**ACK, "response": {"message_id": "u1", "session_id": "new-chat",
                                                       "is_thread": True}})
        session.answer([event(item["event"], session="new-chat",
                              **{k: v for k, v in item["payload"].items() if k != "session_id"})
                        for item in ANSWER[2:]])  # its message event came before the subscription
        result = await send_and_wait(session, send_of(session, session_id="new-chat"), "new-chat", 5)
        assert result["reply"] == "Four."
        assert [entry[0] for entry in session.log] == ["subscribe", "send", "subscribe"]

    run(scenario())


def test_a_stalled_transport_cannot_hold_the_caller_past_its_deadline():
    async def scenario():
        release = asyncio.Event()

        async def stubborn():
            while not release.is_set():
                try:
                    await asyncio.sleep(10)
                except asyncio.CancelledError:
                    continue  # like a send that finishes its frames before giving up

        class Stalled(ChatSession):
            async def send_chat(self, message, session_id=None):
                await stubborn()

            async def subscribe_chat(self, session_id=None, *, opened=None):
                opened.set()
                try:
                    await asyncio.sleep(3600)
                    yield {}
                finally:
                    await stubborn()  # a stream reset stuck behind the stalled send

        session = Stalled()
        loop = asyncio.get_running_loop()
        started = loop.time()
        result = await send_and_wait(session, send_of(session), None, 0.3)
        assert loop.time() - started < 0.3 + service_module.CLEANUP_S + 0.5
        assert result == {"ok": False, "sent": "unknown",
                          "error": "Muse did not acknowledge the message within 0.3 seconds"}
        release.set()

    asyncio.run(asyncio.wait_for(scenario(), 10))


def test_several_answer_messages_are_joined_and_controls_removed():
    async def scenario():
        session = ChatSession()
        session.answer([
            NOTE,
            event("delta.message_start", "r1"),
            event("delta.message_done", "r1", display_text="Let me check.\x1b[2J"),
            event("delta.message_start", "r2", parent="r1"),
            event("delta.message_done", "r2", parent="r1", display_text="It is 4.\n"),
            event("task.status", status="completed"),
        ])
        result = await send_and_wait(session, send_of(session), None, 5)
        assert result["reply"] == "Let me check.[2J\n\nIt is 4."

    run(scenario())


def test_an_unfinished_message_is_not_an_answer_yet():
    async def scenario():
        session = ChatSession()
        session.answer([
            NOTE,
            event("agent.status", activity_code="working"),
            event("message.assistant", "r1", display_text="", display_text_ready=False),
        ])
        waiting = asyncio.ensure_future(send_and_wait(session, send_of(session), None, 5))
        await asyncio.sleep(0.3)
        assert not waiting.done()
        session.answer([
            event("message.assistant", "r1", display_text="Done.", display_text_ready=True),
            event("task.status", status="completed"),
        ])
        assert (await waiting)["reply"] == "Done."

    run(scenario())


def test_no_answer_in_time_says_so_after_the_message_was_acknowledged():
    async def scenario():
        session = ChatSession()
        session.answer([NOTE, event("agent.status", activity_code="working")])
        result = await send_and_wait(session, send_of(session), None, 0.3)
        assert result == {**ACK, "reply": None, "detail": "no answer within 0.3 seconds"}

    run(scenario())


def test_wait_running_out_mid_answer_returns_what_arrived_as_partial():
    async def scenario():
        session = ChatSession()
        session.answer([
            NOTE,
            event("agent.status", activity_code="working"),
            event("delta.message_start", "r1"),
            event("delta.message_done", "r1", display_text="Looking into it."),
        ])
        result = await send_and_wait(session, send_of(session), None, 0.5)
        assert result["reply"] == "Looking into it." and result["partial"] is True

    run(scenario())


def test_task_finished_without_text_is_an_empty_answer():
    async def scenario():
        session = ChatSession()
        session.answer([NOTE, event("task.status", status="completed")])
        result = await send_and_wait(session, send_of(session), None, 10)
        assert result["reply"] == ""

    run(scenario())


def test_refused_subscription_sends_nothing():
    async def scenario():
        session = ChatSession(refuse=403)
        result = await send_and_wait(session, send_of(session), None, 5)
        assert result == {"ok": False, "error": "could not follow Muse's replies (HTTP 403); "
                                                "the message was not sent"}
        assert session.log == [("subscribe", None)]

    run(scenario())


def test_subscription_ending_mid_wait_is_reported():
    async def scenario():
        session = ChatSession()
        session.answer([NOTE, event("agent.status", activity_code="working"), None])
        result = await send_and_wait(session, send_of(session), None, 5)
        assert result == {**ACK, "reply": None, "detail": "Muse closed the reply stream"}

    run(scenario())


def test_rejected_message_is_returned_as_is():
    async def scenario():
        rejected = {"ok": False, "status": 503, "response": None}
        session = ChatSession(ack=rejected)
        assert await send_and_wait(session, send_of(session), None, 5) == rejected
        assert session.open == 0

    run(scenario())


def test_answer_over_the_real_session_and_its_subscription():
    async def scenario():
        session, vm, stop, task = await registered_session()
        waiting = asyncio.ensure_future(send_and_wait(
            session, lambda: session.send_chat("what is 2 + 2?", "side-1"), "side-1", 5))
        subscribe = await vm.next_frame()
        assert subscribe.value.path == "/chat/subscribe"
        assert json.loads(subscribe.value.body) == {"session_id": "side-1"}
        posted = asyncio.ensure_future(vm.next_frame())
        await asyncio.sleep(0.2)
        assert not posted.done()  # nothing is sent until the subscription is open
        await vm.send_frame(ServiceFrame.response(subscribe.stream_id, ApplicationResponse(
            status=200, headers=[Header("Content-Type", "application/x-ndjson")], end_body=False,
        )))
        post = await posted
        assert post.value.path == "/chat/stream"
        # An answer that starts streaming before the acknowledgement still counts.
        events = [event(e["event"], session="side-1",
                        **{k: v for k, v in e["payload"].items() if k != "session_id"})
                  for e in ANSWER]
        await vm.send_frame(ServiceFrame.body_chunk(subscribe.stream_id, BodyChunk(
            data=b"".join(json.dumps(e).encode() + b"\n" for e in events[:3]))))
        ack = {"message_id": "u1", "session_id": "side-1", "is_thread": True}
        await vm.send_frame(ServiceFrame.response(post.stream_id, ApplicationResponse(
            status=200, body=json.dumps(ack).encode(), end_body=True)))
        await vm.send_frame(ServiceFrame.body_chunk(subscribe.stream_id, BodyChunk(
            data=b"".join(json.dumps(e).encode() + b"\n" for e in events[3:]))))
        result = await waiting
        assert result == {"ok": True, "status": 200, "response": ack, "reply": "Four."}
        stop.set()
        await task

    run(scenario())


def test_waits_on_one_chat_take_turns_and_a_queued_one_gives_up_unsent():
    async def check(service, path):
        session = ChatSession()
        service._current = session
        first = asyncio.ensure_future(ask(path, b'{"message": "first", "wait": 5}\n'))
        while len(session.log) < 2:
            await asyncio.sleep(0.01)
        second = asyncio.ensure_future(ask(path, b'{"message": "second", "wait": 1}\n'))
        reply = await second
        assert reply == {"ok": False, "error": "another request on this chat was still waiting "
                                               "for its answer; this message was not sent"}
        assert [entry[1] for entry in session.log if entry[0] == "send"] == ["first"]
        session.answer(ANSWER)
        assert (await first)["reply"] == "Four."
        assert service._reply_waits == {}

    run_with_socket(check)


def test_a_queued_wait_runs_once_the_earlier_one_is_answered():
    async def check(service, path):
        session = ChatSession()
        service._current = session
        first = asyncio.ensure_future(ask(path, b'{"message": "first", "wait": 5}\n'))
        while len(session.log) < 2:
            await asyncio.sleep(0.01)
        second = asyncio.ensure_future(ask(path, b'{"message": "second", "wait": 5}\n'))
        await asyncio.sleep(0.2)
        assert [entry[1] for entry in session.log if entry[0] == "send"] == ["first"]
        session.answer(ANSWER)
        assert (await first)["reply"] == "Four."
        while len(session.log) < 4:
            await asyncio.sleep(0.01)
        assert session.log[-1] == ("send", "second", None, 1)
        session.answer(ANSWER)
        assert (await second)["reply"] == "Four."

    run_with_socket(check)


@pytest.mark.parametrize("wait", [0, 0.5, 601, "30", True, None])
def test_local_socket_refuses_a_bad_wait(wait):
    async def check(service, path):
        service._current = session = ChatSession()
        request = {"message": "hi", "wait": wait}
        reply = await ask(path, json.dumps(request).encode() + b"\n")
        if wait is None:
            # null is the same as no wait: just send.
            assert reply == ACK
        else:
            assert reply == {"ok": False, "error": "wait must be 1 to 600 seconds"}
            assert session.log == []

    run_with_socket(check)


@pytest.fixture
def local_reply(monkeypatch):
    """A one-shot local socket that records the request and answers with ``reply[0]``."""
    received, reply = [], [{"ok": True}]
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
                    conn.sendall(json.dumps(reply[0]).encode() + b"\n")

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        monkeypatch.setenv(config.SOCKET_ENV, str(path))
        yield received, reply
        if thread.is_alive():
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as wake:
                wake.connect(str(path))
        thread.join(5)
        server.close()


@pytest.mark.parametrize("answer, code, out, err", [
    ({**ACK, "reply": "Four."}, 0, "Four.\n", ""),
    ({**ACK, "reply": ""}, 0, "", "Muse finished without a text answer.\n"),
    ({**ACK, "reply": None, "detail": "no answer within 30 seconds"}, 3, "",
     "Sent to your Muse, but got no answer: no answer within 30 seconds\n"),
    ({**ACK, "reply": "Looking.", "partial": True, "detail": "no answer within 30 seconds"}, 3,
     "Looking.\n", "Muse was still answering: no answer within 30 seconds\n"),
    ({"ok": False, "error": "not connected to the Muse"}, 1, "",
     "Not delivered: not connected to the Muse\n"),
    ({"ok": False, "sent": "unknown", "error": "Muse did not acknowledge the message within 30 seconds"},
     1, "", "May not have been delivered: Muse did not acknowledge the message within 30 seconds\n"),
])
def test_cli_prints_the_answer(local_reply, capsys, answer, code, out, err):
    received, reply = local_reply
    reply[0] = answer
    assert cli.main(["send-user-msg", "--wait", "30", "what", "is", "2", "+", "2?"]) == code
    assert received == [{"message": "what is 2 + 2?", "wait": 30}]
    assert capsys.readouterr() == (out, err)


@pytest.mark.parametrize("wait", ["0", "601", "1.5", "soon"])
def test_cli_refuses_a_bad_wait_before_sending(local_reply, capsys, wait):
    received, _ = local_reply
    with pytest.raises(SystemExit) as exit_info:
        cli.main(["send-user-msg", "--wait", wait, "hi"])
    assert exit_info.value.code == 2
    assert "from 1 to 600" in capsys.readouterr().err
    assert received == []
