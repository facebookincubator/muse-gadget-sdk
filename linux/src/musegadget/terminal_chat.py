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

"""Unprivileged text chat through the running service's Unix socket."""
from __future__ import annotations

import asyncio
from collections import OrderedDict
import curses
import json
import os
import signal
import sys
import time

from musegadget import config
from musegadget.link_client import acknowledgement, printable
from musegadget.muse_turn import (AgentStatus, MuseTurn, Reply, _acknowledged_message_ids,
                                 _strip_controls, _update_reply)
from musegadget.terminal_chat_ui import PlainDisplay, TerminalDisplay

MAX_RECORD = 2 * 1024 * 1024  # JSON escaping can expand a 256 KiB upstream event.
MAX_INPUT = 64 * 1024
TURN_TIMEOUT = 15 * 60
MAX_QUEUED_MESSAGES = 16


class LiveUsers:
    """Live user messages, with bounded ID-based suppression of local echoes."""

    def __init__(self, session_id=None):
        self.session_id = session_id
        self.seen = OrderedDict()
        self.pending = []
        self.sending = False
        self.sequence = None

    def remember(self, identifier):
        self.seen[identifier] = None
        self.seen.move_to_end(identifier)
        while len(self.seen) > 256:
            self.seen.popitem(last=False)

    def feed(self, event):
        payload = event.get('payload')
        if (event.get('type') != 'event' or event.get('event') != 'message.user'
                or not isinstance(payload, dict) or payload.get('role') not in (None, 'user')):
            return []
        session = payload.get('session_id') or event.get('session_id')
        if self.session_id and session and session != self.session_id:
            return []
        seq = event.get('seq')
        if isinstance(seq, int):
            if self.sequence is not None and seq <= self.sequence:
                return []
            self.sequence = seq
        identifier = payload.get('message_id') or event.get('message_id') or payload.get('id')
        if (not isinstance(identifier, str) or not identifier or len(identifier) > 512
                or identifier in self.seen or payload.get('display_text_ready') is False):
            return []
        text = payload.get('display_text')
        if not isinstance(text, str):
            text = payload.get('content')
        if not isinstance(text, str):
            text = payload.get('text')
        if not isinstance(text, str):
            return []
        text = _strip_controls(text)
        if len(text.encode('utf-8')) > MAX_INPUT:
            raise ValueError('Muse returned more than 64 KiB of user message text')
        if not text:
            return []
        if self.sending:
            # The acknowledgement supplies IDs; matching text could hide an
            # unrelated phone/web message with the same words.
            if len(self.pending) >= 64:
                raise ValueError('Too many user messages arrived before the send acknowledgement')
            self.pending.append((identifier, text))
            return []
        self.remember(identifier)
        return [text]

    def finish_send(self, response=None):
        if response is not None:
            for identifier in _acknowledged_message_ids(response):
                self.remember(identifier)
        self.sending = False
        pending, self.pending = self.pending, []
        messages = []
        for identifier, text in pending:
            if identifier not in self.seen:
                self.remember(identifier)
                messages.append(text)
        return messages


class LiveReplies:
    """A bounded view of all assistant replies on one subscription."""

    def __init__(self, session_id=None):
        self.session_id = session_id
        self.messages = OrderedDict()
        self.sequence = None
        self.row_sequence = None
        self.current = None
        self.last_reply = None

    def feed(self, event):
        payload = event.get("payload")
        if event.get("type") != "event" or not isinstance(payload, dict):
            return ""
        session = payload.get("session_id") or event.get("session_id")
        if self.session_id and session and session != self.session_id:
            return ""
        name = event.get("event")
        if name not in ("delta.message_start", "delta.text_append", "delta.message_done", "message.assistant"):
            return ""
        # Message rows (message.user, message.assistant) carry the chat's own
        # order, apart from live delta events, so each kind keeps its own.
        seq = event.get("seq")
        if isinstance(seq, int):
            last = self.row_sequence if name == "message.assistant" else self.sequence
            if last is not None and seq <= last:
                return ""
            if name == "message.assistant":
                self.row_sequence = seq
            else:
                self.sequence = seq
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
        self.last_reply = reply
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
    users = LiveUsers(session_id)
    display = PlainDisplay(output)

    def show(text):
        display.write(text)

    def show_status(text):
        nonlocal status
        if text != status:
            status = text
            display.status(text)

    def stop_tracking(text):
        # The turn only drives the status line; losing it never ends the chat.
        nonlocal turn
        turn = None
        show_status(text)

    def progress(events):
        for item in events:
            if isinstance(item, AgentStatus):
                text = _strip_controls(item.activity_text or item.activity_code or "Working")
                text = " ".join(text.split())
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
        if (input_reader is None and sys.stdin.isatty() and output is sys.stdout
                and output.isatty() and os.environ.get("TERM", "dumb") != "dumb"):
            try:
                display = TerminalDisplay()
            except curses.error:
                pass  # Missing terminal capabilities: retain the plain-text client.
            else:
                show_status("Muse is ready · /quit or /exit · /clear")
        if input_reader is None and isinstance(display, PlainDisplay):
            input_reader = asyncio.StreamReader(limit=MAX_INPUT)
            # The pipe transport makes stdin non-blocking (a terminal shares
            # that flag with the shell) and closes what it reads at EOF. Give
            # it a duplicate, so stdin stays open to restore the flag on exit.
            stdin_blocking = os.get_blocking(sys.stdin.fileno())
            pipe = os.fdopen(os.dup(sys.stdin.fileno()), "rb", buffering=0)
            try:
                stdin_transport, _ = await asyncio.get_running_loop().connect_read_pipe(
                    lambda: asyncio.StreamReaderProtocol(input_reader), pipe)
            except BaseException:
                pipe.close()
                raise
        if isinstance(display, PlainDisplay):
            show("Chatting with your Muse. /quit or /exit, Ctrl-C or EOF exits. /clear clears the screen.\n")

        async def receive():
            while True:
                record = await _record(reader)
                if record.get("type") != "chat_event" or not isinstance(record.get("event"), dict):
                    raise ValueError("invalid chat event from the musegadget service")
                event = record["event"]
                for text in users.feed(event):
                    view.current = None
                    display.user(text)
                if turn is not None:
                    try:
                        progress(turn.feed(event))
                    except ValueError:
                        stop_tracking("Lost track of Muse's progress; replies will appear as they arrive")
                rendered = view.feed(event)
                if rendered:
                    if turn is not None:
                        show_status("Muse is responding")
                    display.reply(view.last_reply, rendered)

        async def send():
            nonlocal turn, started, sending, submitted, unconfirmed
            while True:
                message = await messages.get()
                sending = True
                users.sending = True
                try:
                    follow_up = turn is not None
                    if follow_up:
                        turn.expect_follow_up()
                    else:
                        turn = MuseTurn(session_id)
                    current = turn
                    started = time.monotonic()
                    unconfirmed = True
                    response = await asyncio.wait_for(_send(path, message, session_id), 90)
                    unconfirmed = False
                    submitted += 1
                    for text in users.finish_send(response):
                        view.current = None
                        display.user(text)
                    show_status("Waiting for Muse…")
                    if turn is current:
                        try:
                            progress(current.acknowledge(response, follow_up=follow_up))
                        except ValueError:
                            stop_tracking("Lost track of Muse's progress; replies will appear as they arrive")
                        else:
                            view.session_id = current.session_id
                            users.session_id = current.session_id
                finally:
                    for text in users.finish_send():
                        view.current = None
                        display.user(text)
                    sending = False
                    messages.task_done()

        async def inputs():
            while True:
                if isinstance(display, TerminalDisplay):
                    raw = await display.readline(MAX_INPUT)
                else:
                    raw = await input_reader.readline()
                if not raw:
                    return
                if len(raw) > MAX_INPUT:
                    raise ValueError("terminal message exceeds 64 KiB")
                message = raw.decode("utf-8").strip()
                if message in ("/quit", "/exit"):
                    return
                if message == "/clear":
                    display.clear()
                    view.current = None
                    continue
                if not message:
                    continue
                try:
                    messages.put_nowait(message)
                except asyncio.QueueFull:
                    show("\n[Message not accepted: send queue is full. Try again shortly.]\n")
                else:
                    view.current = None
                    display.user(_strip_controls(message))
                    show_status("Waiting for Muse…")

        async def completion():
            nonlocal turn
            while True:
                await asyncio.sleep(0.1)
                if turn is not None:
                    if not sending and time.monotonic() - started > TURN_TIMEOUT:
                        # Stop tracking progress, but keep the chat open:
                        # replies still appear whenever they arrive.
                        stop_tracking("Still no sign Muse finished; replies will appear as they arrive")
                        continue
                    if not sending and messages.empty() and turn.complete(time.monotonic()):
                        turn = None
                        show_status("Muse is ready")

        tasks.extend([asyncio.create_task(receive()), asyncio.create_task(inputs()),
                      asyncio.create_task(completion()), asyncio.create_task(send())])
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        try:
            if messages.qsize():
                show("\n[" + str(messages.qsize()) + " queued message(s) were not sent.]\n")
            if unconfirmed:
                show("\n[The last send was not confirmed; Muse may have received it. It will not be resent.]\n")
            if submitted:
                show("\n[Submitted messages remain with Muse.]\n")
            show_status("Chat closed")
        finally:
            try:
                display.close()
            finally:
                if stdin_transport is not None:
                    try:
                        os.set_blocking(sys.stdin.fileno(), stdin_blocking)
                    except (OSError, ValueError):
                        pass
                    stdin_transport.close()
                writer.close()
                await writer.wait_closed()
        if isinstance(display, TerminalDisplay):
            for notice in display.notice:
                output.write(notice + "\n")
            output.flush()


async def run_until_interrupted(session_id=None):
    """run_chat, ending through cancellation on Ctrl-C so its cleanup always runs.

    A KeyboardInterrupt raised inside the event loop (Python before 3.11) can
    strand tasks and hang cleanup. A second Ctrl-C interrupts as usual.
    """
    loop = asyncio.get_running_loop()
    task = asyncio.current_task()

    def interrupt():
        loop.remove_signal_handler(signal.SIGINT)
        task.cancel()

    loop.add_signal_handler(signal.SIGINT, interrupt)
    try:
        await run_chat(session_id)
    finally:
        loop.remove_signal_handler(signal.SIGINT)


def command(args):
    try:
        asyncio.run(run_until_interrupted(args.session_id))
    except (KeyboardInterrupt, asyncio.CancelledError):
        print()
        return 0
    except (OSError, ValueError, asyncio.TimeoutError) as exc:
        # The text can come from Muse (a stream reset reason): keep controls out.
        print(f"Could not continue Muse chat: {printable(str(exc))}", file=sys.stderr)
        return 1
    return 0
