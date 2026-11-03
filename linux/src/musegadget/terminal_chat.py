# Copyright (c) Eric Chen
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

"""Unprivileged text chat through the running service's Unix socket."""
from __future__ import annotations

import asyncio
from collections import OrderedDict
import json
import sys
import time

from musegadget import config
from musegadget.link_client import acknowledgement
from musegadget.muse_turn import AgentStatus, MuseTurn, Reply, _update_reply

MAX_RECORD = 2 * 1024 * 1024  # JSON escaping can expand a 256 KiB upstream event.
MAX_INPUT = 64 * 1024
TURN_TIMEOUT = 15 * 60
MAX_QUEUED_MESSAGES = 16


class LiveReplies:
    """A bounded view of all assistant replies on one subscription."""

    def __init__(self, session_id=None):
        self.session_id = session_id
        self.messages = OrderedDict()
        self.sequence = None
        self.current = None

    def feed(self, event):
        payload = event.get("payload")
        if event.get("type") != "event" or not isinstance(payload, dict):
            return ""
        session = payload.get("session_id") or event.get("session_id")
        if self.session_id and session and session != self.session_id:
            return ""
        seq = event.get("seq")
        if isinstance(seq, int):
            if self.sequence is not None and seq <= self.sequence:
                return ""
            self.sequence = seq
        name = event.get("event")
        if name not in ("delta.message_start", "delta.text_append", "delta.message_done", "message.assistant"):
            return ""
        if payload.get("role") not in (None, "assistant"):
            return ""
        identifier = payload.get("message_id") or event.get("message_id") or payload.get("id")
        if not isinstance(identifier, str) or not identifier or len(identifier) > 512:
            return ""
        previous = self.messages.pop(identifier, Reply(identifier))
        reply = _update_reply(previous, name, payload)
        self.messages[identifier] = reply
        while len(self.messages) > 32:
            self.messages.popitem(last=False)
        if reply == previous:
            return ""
        output = ""
        if reply.text != previous.text:
            if self.current != identifier:
                output = "\nMuse: " + reply.text
            elif reply.text.startswith(previous.text):
                output = reply.text[len(previous.text):]
            else:
                output = "\nMuse (updated): " + reply.text
            self.current = identifier
        if reply.done and (not previous.done or reply.text != previous.text):
            output += "\n"
            self.current = None
        return output


async def _record(reader):
    line = await reader.readline()
    if not line:
        raise ConnectionError("Muse chat connection closed; no messages were resent")
    value = json.loads(line)
    if not isinstance(value, dict):
        raise ValueError("invalid response from the musegadget service")
    if value.get("type") == "error" or value.get("ok") is False:
        raise ConnectionError(value.get("error") or "Muse chat request failed")
    return value


async def _send(path, message, session_id):
    reader, writer = await asyncio.open_unix_connection(str(path), limit=MAX_RECORD)
    try:
        request = {"message": message}
        if session_id:
            request["session_id"] = session_id
        writer.write(json.dumps(request).encode() + b"\n")
        await writer.drain()
        return acknowledgement(await _record(reader))
    finally:
        writer.close()
        await writer.wait_closed()


async def run_chat(session_id=None, *, input_reader=None, output=None):
    """Run until user exit or a connection error; never open credential files."""
    output = output or sys.stdout
    path = config.socket_path()
    reader, writer = await asyncio.open_unix_connection(str(path), limit=MAX_RECORD)
    tasks = []
    messages = asyncio.Queue(maxsize=MAX_QUEUED_MESSAGES)
    sending = False
    submitted = 0
    unconfirmed = False
    status = None
    stdin_transport = None
    turn = None
    started = 0.0
    view = LiveReplies(session_id)

    def show(text):
        output.write(text)
        output.flush()

    def show_status(text):
        nonlocal status
        if text != status:
            status = text
            show("\n[" + text + "]\n")

    def progress(events):
        for item in events:
            if isinstance(item, AgentStatus):
                text = " ".join((item.activity_text or item.activity_code or "Working").split())
                show_status("Muse: " + text[:200])

    try:
        request = {"op": "subscribe_chat"}
        if session_id:
            request["session_id"] = session_id
        writer.write(json.dumps(request).encode() + b"\n")
        await writer.drain()
        ready = await asyncio.wait_for(_record(reader), 90)
        if ready.get("type") != "ready":
            raise ConnectionError("Service does not support terminal chat; update and restart it")
        if input_reader is None:
            input_reader = asyncio.StreamReader(limit=MAX_INPUT)
            stdin_transport, _ = await asyncio.get_running_loop().connect_read_pipe(
                lambda: asyncio.StreamReaderProtocol(input_reader), sys.stdin)
        show("Chatting with your Muse. /quit, Ctrl-C or EOF exits.\n> ")

        async def receive():
            while True:
                record = await _record(reader)
                if record.get("type") != "chat_event" or not isinstance(record.get("event"), dict):
                    raise ValueError("invalid chat event from the musegadget service")
                event = record["event"]
                if turn is not None:
                    progress(turn.feed(event))
                rendered = view.feed(event)
                if rendered:
                    if turn is not None:
                        show_status("Muse is responding")
                    show(rendered)

        async def send():
            nonlocal turn, started, sending, submitted, unconfirmed
            while True:
                message = await messages.get()
                sending = True
                try:
                    follow_up = turn is not None
                    if follow_up:
                        turn.expect_follow_up()
                    else:
                        turn = MuseTurn(session_id)
                    started = time.monotonic()
                    unconfirmed = True
                    response = await asyncio.wait_for(_send(path, message, session_id), 90)
                    unconfirmed = False
                    submitted += 1
                    show("\n[Message submitted.]\n")
                    show_status("Waiting for Muse…")
                    progress(turn.acknowledge(response, follow_up=follow_up))
                    view.session_id = turn.session_id
                finally:
                    sending = False
                    messages.task_done()

        async def inputs():
            while True:
                raw = await input_reader.readline()
                if not raw:
                    return
                if len(raw) > MAX_INPUT:
                    raise ValueError("terminal message exceeds 64 KiB")
                message = raw.decode("utf-8").strip()
                if message == "/quit":
                    return
                if not message:
                    continue
                try:
                    messages.put_nowait(message)
                except asyncio.QueueFull:
                    show("\n[Message not accepted: send queue is full. Try again shortly.]\n")
                else:
                    show_status("Waiting for Muse…")
                    show("> ")

        async def completion():
            nonlocal turn
            while True:
                await asyncio.sleep(0.1)
                if turn is not None:
                    if time.monotonic() - started > TURN_TIMEOUT:
                        raise TimeoutError("Muse reply timed out; the message may still be running")
                    if not sending and messages.empty() and turn.complete(time.monotonic()):
                        turn = None
                        show_status("Muse is ready")
                        show("\n> ")

        tasks.extend([asyncio.create_task(receive()), asyncio.create_task(inputs()),
                      asyncio.create_task(completion()), asyncio.create_task(send())])
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    finally:
        if messages.qsize():
            show("\n[" + str(messages.qsize()) + " queued message(s) were not sent.]\n")
        if unconfirmed:
            show("\n[The last send was not confirmed; Muse may have received it. It will not be resent.]\n")
        if submitted:
            show("\n[Submitted messages remain with Muse.]\n")
        show_status("Chat closed")
        pending = tasks
        for task in pending:
            task.cancel()
        await asyncio.gather(*pending, return_exceptions=True)
        if stdin_transport is not None:
            stdin_transport.close()
        writer.close()
        await writer.wait_closed()


def command(args):
    try:
        asyncio.run(run_chat(args.session_id))
    except KeyboardInterrupt:
        print()
        return 0
    except (OSError, ValueError, asyncio.TimeoutError) as exc:
        print(f"Could not continue Muse chat: {exc}", file=sys.stderr)
        return 1
    return 0
