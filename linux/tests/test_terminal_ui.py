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

import asyncio
import curses
from collections import deque

import pytest

from musegadget.muse_turn import Reply
from musegadget.terminal_ui import TerminalDisplay, _wrap


class Screen:
    def __init__(self):
        self.size = (8, 40)
        self.rows = {}
        self.styles = {}
        self.cursor = None
        self.keys = deque()
        self.keypad_enabled = False

    def keypad(self, enabled):
        self.keypad_enabled = enabled

    def nodelay(self, enabled):
        pass

    def getmaxyx(self):
        return self.size

    def erase(self):
        self.rows.clear()
        self.styles.clear()

    def addstr(self, row, column, text, style):
        assert 0 <= row < self.size[0]
        previous = self.rows.get(row, '')
        self.rows[row] = previous.ljust(column)[:column] + text + previous[column + len(text):]
        self.styles[row, column] = style

    def move(self, row, column):
        assert 0 <= row < self.size[0]
        assert 0 <= column < self.size[1]
        self.cursor = (row, column)

    def refresh(self):
        pass

    def get_wch(self):
        if not self.keys:
            raise curses.error()
        return self.keys.popleft()


@pytest.fixture
def display(monkeypatch):
    screen = Screen()
    monkeypatch.setattr(curses, 'initscr', lambda: screen)
    for name in ('noecho', 'cbreak', 'nocbreak', 'echo', 'endwin'):
        monkeypatch.setattr(curses, name, lambda: None)
    def no_colors():
        raise curses.error()
    monkeypatch.setattr(curses, 'start_color', no_colors)
    view = TerminalDisplay()
    yield view
    view.close()
    assert not screen.keypad_enabled


def test_status_and_reply_updates_preserve_draft_and_transcript(display):
    display.user('hello')
    display.draft, display.cursor = 'a follow-up', 5
    display.reply(Reply('r', text='Hel'), '')
    for i in range(30):
        display.status('Working ' + str(i))
    display.reply(Reply('r', text='Hello!', done=True), '')
    assert list(display.entries) == [(None, 'User: hello'), ('r', 'Muse: Hello!')]
    assert display.screen.rows[7] == 'Working 29'
    assert display.screen.rows[6] == 'User: a follow-up'
    assert display.screen.cursor == (6, 11)
    assert display.screen.styles[7, 0] == curses.A_DIM
    assert display.screen.styles[6, 0] == curses.A_BOLD
    assert display.screen.styles[6, 6] == 0
    assert display.cursor == 5
    # Corrections update the original reply even after a follow-up is entered.
    display.user('another question')
    display.reply(Reply('r', text='Corrected', done=True), '')
    assert len(display.entries) == 3
    assert display.entries[1] == ('r', 'Muse: Corrected')


def test_scrolling_resize_and_bounded_transcript(display):
    for i in range(520):
        display.user(str(i))
    assert len(display.entries) == 512
    display.screen.keys.extend([curses.KEY_PPAGE, '\n'])
    asyncio.run(display.readline(65536))
    display.draw()
    assert display.screen.rows[0] == 'User: 508'
    display.screen.size = (5, 16)
    display.draw()
    assert 3 in display.screen.rows and 4 in display.screen.rows
    display.screen.keys.extend([curses.KEY_NPAGE, curses.KEY_NPAGE, '\n'])
    asyncio.run(display.readline(65536))
    display.draw()
    assert display.screen.rows[2] == 'User: 519'


def test_editor_unicode_delete_and_exit(display):
    display.screen.keys.extend(['a', '界', 'b', curses.KEY_LEFT,
                                curses.KEY_BACKSPACE, 'é', curses.KEY_HOME,
                                curses.KEY_DC, curses.KEY_END, '\n'])
    assert asyncio.run(display.readline(65536)) == 'éb\n'.encode()
    assert display.draft == ''
    display.screen.keys.append('\x04')
    assert asyncio.run(display.readline(65536)) == b''
    display.screen.keys.append('\x03')
    assert asyncio.run(display.readline(65536)) == b'/quit\n'


def test_draft_survives_async_progress(display):
    async def scenario():
        display.screen.keys.extend('partial')
        reader = asyncio.create_task(display.readline(65536))
        await asyncio.sleep(0.05)
        display.status('Muse is responding')
        display.reply(Reply('r', text='Hello'), '')
        assert display.draft == 'partial'
        display.screen.keys.extend(' input\n')
        assert await reader == b'partial input\n'
    asyncio.run(scenario())


def test_editor_bounds_utf8_bytes(display):
    display.screen.keys.extend(['é', 'é', 'é', '\n'])
    assert asyncio.run(display.readline(5)) == 'éé\n'.encode()
    assert 'shorten' in display.status_text


def test_wrap_counts_wide_and_combining_characters():
    assert list(_wrap('a界bé\u0301c\nend', 4)) == ['a界b', 'é\u0301c', 'end']


def test_message_continuations_align_after_labels_on_resize(display):
    display.screen.size = (12, 15)
    display.user('12345678more\nnext')
    display.reply(Reply('r', text='a界bé\u0301cdeZ\nlast'), '')
    assert [display.screen.rows[i] for i in range(6)] == [
        'User: 12345678', '      more', '      next',
        'Muse: a界bé\u0301cde', '      Z', '      last']
    display.screen.size = (12, 21)
    display.draw()
    assert display.screen.rows[0] == 'User: 12345678more'
    assert display.screen.rows[1] == '      next'
    # Very narrow terminals still render without reserving an impossible indent.
    display.screen.size = (4, 5)
    display.draw()
    assert all(len(line) <= 4 for line in display.screen.rows.values())


def test_speaker_colors_use_terminal_background(display, monkeypatch):
    pairs = []
    monkeypatch.setattr(curses, 'start_color', lambda: None)
    monkeypatch.setattr(curses, 'use_default_colors', lambda: None)
    monkeypatch.setattr(curses, 'init_pair', lambda *args: pairs.append(args))
    monkeypatch.setattr(curses, 'color_pair', lambda n: n << 8)
    view = TerminalDisplay()
    try:
        view.user('hello')
        view.reply(Reply('r', text='reply'), '')
        assert pairs == [(1, curses.COLOR_CYAN, -1), (2, curses.COLOR_MAGENTA, -1)]
        assert view.screen.styles[0, 0] == curses.A_BOLD | (1 << 8)
        assert view.screen.styles[1, 0] == curses.A_BOLD | (2 << 8)
        assert view.screen.styles[0, 6] == view.screen.styles[1, 6] == 0
    finally:
        view.close()


@pytest.mark.parametrize('ending', ['quit', 'eof', 'interrupt'])
def test_real_terminal_chat_restores_echo_and_line_input(ending):
    """Exercise curses and run_chat together, with real terminal line discipline."""
    import os
    import pty
    import re
    import select
    import signal
    import subprocess
    import sys
    import termios
    import time

    script = '''
import asyncio, json, os, tempfile
from musegadget import config
from musegadget.terminal_chat import run_chat

async def main():
    peers = set()
    async def handle(reader, writer):
        peers.add(writer)
        try:
            request = json.loads(await reader.readline())
            if request.get('op') == 'subscribe_chat':
                writer.write(b'{"type":"ready"}\\n')
                await writer.drain()
                await reader.read()
            else:
                writer.write(b'{"ok":true,"status":200,"response":{"message_id":"u"}}\\n')
                await writer.drain()
                for peer in peers - {writer}:
                    record = {'type': 'chat_event', 'event': {
                        'type': 'event', 'event': 'message.assistant',
                        'payload': {'message_id': 'r', 'content': 'Hello'}}}
                    peer.write(json.dumps(record).encode() + b'\\n')
                    await peer.drain()
        finally:
            peers.discard(writer)
            writer.close()
            await writer.wait_closed()
    with tempfile.TemporaryDirectory(dir='/tmp') as tmp:
        path = tmp + '/chat.sock'
        os.environ[config.SOCKET_ENV] = path
        server = await asyncio.start_unix_server(handle, path)
        try:
            await run_chat()
        finally:
            server.close()
            await server.wait_closed()
asyncio.run(main())
'''
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    process = subprocess.Popen([sys.executable, '-c', script], stdin=slave,
                               stdout=slave, stderr=slave,
                               env=dict(os.environ, TERM='xterm-256color'))
    captured = bytearray()

    def wait_for(text):
        deadline = time.monotonic() + 5
        # Curses changes both SGR colors and character sets between label/body.
        while text not in re.sub(rb'\x1b(?:\[[0-?]*[ -/]*[@-~]|[()][0-2A-Z])',
                                 b'', captured):
            assert time.monotonic() < deadline, captured.decode(errors='replace')
            if select.select([master], [], [], 0.1)[0]:
                captured.extend(os.read(master, 65536))
            assert process.poll() is None, captured.decode(errors='replace')

    try:
        wait_for(b'Muse is ready')
        os.write(master, b'hello\n')
        wait_for(b'Muse: Hello')
        if ending == 'interrupt':
            process.send_signal(signal.SIGINT)
        else:
            os.write(master, b'/quit\n' if ending == 'quit' else b'\x04')
        process.wait(timeout=5)
        assert process.returncode in (0, -signal.SIGINT)
        restored = termios.tcgetattr(slave)
        assert restored == original
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)
