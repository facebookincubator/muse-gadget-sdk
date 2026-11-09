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

import asyncio
import curses
import io
from collections import deque

import pytest

from musegadget.muse_turn import Reply
from musegadget.terminal_chat_ui import PlainDisplay, TerminalDisplay, _wrap


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
    # Exercise both wheel directions even on Python 3.9, which lacks BUTTON5.
    monkeypatch.setattr(curses, 'BUTTON4_PRESSED', 1 << 16, raising=False)
    monkeypatch.setattr(curses, 'BUTTON5_PRESSED', 1 << 21, raising=False)
    monkeypatch.setattr(curses, 'initscr', lambda: screen)
    monkeypatch.setattr(curses, 'mousemask', lambda mask: (mask, 0))
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


def test_clear_removes_transcript_and_resets_scroll_without_interrupting_reply(display):
    for i in range(20):
        display.user(str(i))
    display.reply(Reply('r', text='Hello'), '')
    display.scroll = 3
    display.draft, display.cursor = 'draft', 2
    display.status('Muse is responding')
    display.clear()
    assert not display.entries
    assert display.scroll == 0
    assert display.screen.rows == {6: 'User: draft', 7: 'Muse is responding'}
    assert display.screen.cursor == (6, 8)
    display.draw()  # Old entries must not reappear on redraw.
    assert not display.entries
    display.reply(Reply('r', text='Hello again', done=True), '')
    assert list(display.entries) == [('r', 'Muse: Hello again')]


@pytest.mark.parametrize('tty,term,clears', [(True, 'xterm', True),
                                           (True, 'dumb', False),
                                           (False, 'xterm', False)])
def test_plain_clear_uses_terminal_controls_only_for_capable_terminals(monkeypatch, tty, term, clears):
    output = io.StringIO()
    monkeypatch.setattr(output, 'isatty', lambda: tty)
    monkeypatch.setenv('TERM', term)
    view = PlainDisplay(output)
    view.reply(Reply('r', text='Hello'), '\nMuse: Hello')
    view.clear()
    if clears:
        assert output.getvalue().endswith('\x1b[2J\x1b[H')
        assert view.at_start
    else:
        assert '\x1b' not in output.getvalue()
        assert 'Screen clearing is unavailable' in output.getvalue()


def test_mouse_wheel_scrolls_transcript_and_preserves_draft(display, monkeypatch):
    for i in range(20):
        display.user(str(i))
    display.draft, display.cursor = 'unfinished message', 4
    mouse_events = deque([
        (0, 10, 2, 0, display.wheel_up),
        (0, 10, 2, 0, curses.BUTTON1_PRESSED),  # Clicks do not edit or scroll.
        (0, 10, 2, 0, display.wheel_down),
    ])
    monkeypatch.setattr(curses, 'getmouse', mouse_events.popleft)

    async def read_one_mouse_event():
        display.screen.keys.append(curses.KEY_MOUSE)
        task = asyncio.create_task(display.readline(65536))
        try:
            await asyncio.sleep(0.05)
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    asyncio.run(read_one_mouse_event())
    assert display.scroll == 3
    assert display.screen.rows[0] == 'User: 11'
    assert (display.draft, display.cursor) == ('unfinished message', 4)
    asyncio.run(read_one_mouse_event())
    assert display.scroll == 3
    asyncio.run(read_one_mouse_event())
    assert display.scroll == 0
    assert display.screen.rows[5] == 'User: 19'
    assert (display.draft, display.cursor) == ('unfinished message', 4)

    # Both ends clamp to the retained transcript, even with repeated gestures.
    monkeypatch.setattr(curses, 'getmouse', lambda: (0, 0, 0, 0, display.wheel_up))
    display.screen.keys.extend([curses.KEY_MOUSE] * 30 + ['\n'])
    assert asyncio.run(display.readline(65536)) == b'unfinished message\n'
    display.draw()
    assert display.scroll == 14
    assert display.screen.rows[0] == 'User: 0'
    monkeypatch.setattr(curses, 'getmouse', lambda: (0, 0, 0, 0, display.wheel_down))
    display.screen.keys.extend([curses.KEY_MOUSE] * 30 + ['\n'])
    asyncio.run(display.readline(65536))
    assert display.scroll == 0


def test_mouse_errors_leave_keyboard_scrolling_available(display, monkeypatch):
    def unavailable(*args):
        raise curses.error()
    monkeypatch.setattr(curses, 'mousemask', unavailable)
    monkeypatch.setattr(curses, 'getmouse', unavailable)
    view = TerminalDisplay()
    try:
        for i in range(20):
            view.user(str(i))
        view.screen.keys.extend([curses.KEY_MOUSE, curses.KEY_PPAGE, '\n'])
        assert asyncio.run(view.readline(65536)) == b'\n'
        assert view.scroll == 6
    finally:
        view.close()


def test_mouse_mask_is_restored_on_exit(display, monkeypatch):
    masks = []
    def mousemask(mask):
        masks.append(mask)
        return mask, curses.BUTTON1_PRESSED
    monkeypatch.setattr(curses, 'mousemask', mousemask)
    view = TerminalDisplay()
    view.close()
    assert masks == [view.wheel_up | view.wheel_down, curses.BUTTON1_PRESSED]


def test_missing_wheel_constants_preserve_keyboard_support(display, monkeypatch):
    with monkeypatch.context() as patch:
        patch.delattr(curses, 'BUTTON5_PRESSED', raising=False)
        def unexpected(mask):
            pytest.fail('mouse capture enabled without both wheel directions')
        patch.setattr(curses, 'mousemask', unexpected)
        view = TerminalDisplay()
        try:
            assert view.mouse_mask is None
        finally:
            view.close()


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


@pytest.mark.parametrize(('text', 'width', 'expected'), [
    ('hello world again', 8, ['hello', 'world', 'again']),
    ('hello world', 5, ['hello', 'world']),
    ('one two\n\nthree four\n', 9, ['one two', '', 'three', 'four', '']),
    ('abcdefghijk', 4, ['abcd', 'efgh', 'ijk']),
    ('界界 next e\u0301cho', 7, ['界界', 'next', 'e\u0301cho']),
])
def test_wrap_prefers_whole_words(text, width, expected):
    assert list(_wrap(text, width)) == expected


def test_transcript_wraps_words_and_preserves_hanging_indent(display):
    display.screen.size = (12, 21)
    display.user('hello wonderful world')
    display.reply(Reply('r', text='a response with words'), '')
    assert [display.screen.rows[i] for i in range(5)] == [
        'User: hello', '      wonderful', '      world',
        'Muse: a response', '      with words']


def test_draft_grows_to_six_rows_and_shrinks_after_submission(display):
    display.screen.size = (12, 21)
    display.user('earlier')
    display.draft = ' '.join(['hello world'] * 6)
    display.cursor = len(display.draft)
    display.dirty = True
    display.draw()
    assert [display.screen.rows[i] for i in range(5, 11)] == [
        'User: hello world', *['      hello world'] * 5]
    assert display.screen.cursor == (10, 17)
    display.status('Muse is responding')
    assert display.screen.rows[11] == 'Muse is responding'
    assert display.screen.rows[0] == 'User: earlier'
    submitted = display.draft
    display.screen.keys.append('\n')
    assert asyncio.run(display.readline(65536)) == (submitted + '\n').encode()
    display.draw()
    assert display.screen.rows[10] == 'User: '
    assert display.screen.cursor == (10, 6)


def test_tall_draft_scrolls_vertically_with_cursor_and_resize(display):
    display.screen.size = (6, 21)
    display.draft = ' '.join('word%02d' % i for i in range(20))
    display.cursor = len(display.draft)
    display.dirty = True
    display.draw()
    assert display.screen.rows[4] == '      word18 word19'
    assert display.screen.cursor == (4, 19)
    display.cursor = 0
    display.dirty = True
    display.draw()
    assert display.screen.rows[1] == 'User: word00 word01'
    assert display.screen.cursor == (1, 6)
    display.screen.size = (10, 40)
    display.draw()
    assert display.screen.rows[4].startswith('User: word00')
    assert display.screen.cursor == (4, 6)
    assert display.draft.endswith('word19')



def test_editing_across_wrapped_rows_preserves_message(display):
    async def scenario():
        display.screen.size = (8, 21)
        display.screen.keys.extend('hello wonderful world')
        reader = asyncio.create_task(display.readline(65536))
        await asyncio.sleep(0.05)
        assert display.screen.rows[4] == 'User: hello'
        assert display.screen.rows[5] == '      wonderful'
        assert display.screen.rows[6] == '      world'
        display.status('Muse is responding')
        display.screen.keys.extend([curses.KEY_HOME, curses.KEY_RIGHT,
                                    curses.KEY_DC, 'a', curses.KEY_END,
                                    curses.KEY_BACKSPACE, '!'])
        await asyncio.sleep(0.05)
        display.screen.size = (8, 40)
        display.draw()
        assert display.screen.rows[6] == 'User: hallo wonderful worl!'
        display.screen.keys.append('\n')
        assert await reader == b'hallo wonderful worl!\n'
    asyncio.run(scenario())

def test_full_width_and_unicode_draft_cursor(display):
    display.screen.size = (8, 15)
    display.draft = '界界界界'
    display.cursor = len(display.draft)
    display.dirty = True
    display.draw()
    assert display.screen.rows[5] == 'User: 界界界界'
    assert display.screen.rows[6] == '      '
    assert display.screen.cursor == (6, 6)
    display.draft = '界界 e\u0301cho world'
    display.cursor = 6  # after two letters and the combining accent in echo
    display.dirty = True
    display.draw()
    assert display.screen.cursor == (5, 8)


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


@pytest.mark.parametrize('ending', ['quit', 'exit', 'eof', 'interrupt'])
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
from musegadget.terminal_chat import run_until_interrupted

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
            await run_until_interrupted()
        finally:
            server.close()
            await server.wait_closed()
try:
    asyncio.run(main())
except asyncio.CancelledError:
    pass  # Ctrl-C ends the chat by cancellation, as in the CLI.
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
            os.write(master, ('/' + ending + '\n').encode() if ending in ('quit', 'exit') else b'\x04')
        # Keep reading: a small pty buffer (macOS) would otherwise block the
        # child's final redraw before it can restore the terminal and exit.
        deadline = time.monotonic() + 5
        while process.poll() is None:
            assert time.monotonic() < deadline, captured.decode(errors='replace')
            if select.select([master], [], [], 0.1)[0]:
                try:
                    captured.extend(os.read(master, 65536))
                except OSError:
                    break
        process.wait(timeout=5)
        assert process.returncode in (0, -signal.SIGINT)
        restored = termios.tcgetattr(slave)
        # PENDIN is kernel bookkeeping (macOS sets it on leaving raw input).
        pendin = getattr(termios, 'PENDIN', 0)
        restored[3] &= ~pendin
        original[3] &= ~pendin
        assert restored == original
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)
