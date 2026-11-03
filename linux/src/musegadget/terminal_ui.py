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

"""Compact terminal chat, with a persistent status and input row."""
from __future__ import annotations

import asyncio
from collections import deque
import curses
import unicodedata


def _width(char):
    if unicodedata.combining(char):
        return 0
    return 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1


def _wrap(text, width):
    """Wrap by terminal cells, preserving explicit newlines."""
    line, cells = "", 0
    for char in text.expandtabs(4):
        size = _width(char)
        if char == "\n":
            yield line
            line, cells = "", 0
            continue
        if cells + size > width:
            yield line
            line, cells = "", 0
        line += char
        cells += size
    yield line


class PlainDisplay:
    """Readable output for pipes and terminals without cursor support."""

    def __init__(self, output):
        self.output = output
        self.at_start = True

    def write(self, text):
        if text.startswith("\n") and self.at_start:
            text = text[1:]
        self.output.write(text)
        self.output.flush()
        if text:
            self.at_start = text.endswith("\n")

    def status(self, text):
        self.write("\n[" + text + "]\n")

    def user(self, text):
        self.write("\nUser: " + text + "\n")

    def reply(self, reply, rendered):
        self.write(rendered)

    def close(self):
        pass


class TerminalDisplay:
    """A bounded transcript above two fixed rows; curses owns terminal echo."""

    def __init__(self):
        self.screen = None
        self.entries = deque(maxlen=512)
        self.status_text = "Muse is ready · /quit to exit"
        self.draft = ""
        self.cursor = 0
        self.scroll = 0
        self.notice = []
        self.dirty = True
        self.size = None
        self.user_style = curses.A_BOLD
        self.muse_style = curses.A_BOLD
        try:
            self.screen = curses.initscr()
            curses.noecho()
            curses.cbreak()
            self.screen.keypad(True)
            self.screen.nodelay(True)
            try:
                curses.start_color()
                curses.use_default_colors()
                curses.init_pair(1, curses.COLOR_CYAN, -1)
                curses.init_pair(2, curses.COLOR_MAGENTA, -1)
                self.user_style |= curses.color_pair(1)
                self.muse_style |= curses.color_pair(2)
            except curses.error:
                pass  # Keep bold labels when default-background colors aren't supported.
        except BaseException:
            self.close()
            raise

    def write(self, text):
        text = text.strip()
        if text:
            self.entries.append((None, text))
            # Preserve exceptional exit notices after the full-screen view closes.
            self.notice.append(text)
            self.notice = self.notice[-8:]
        self.dirty = True
        self.draw()

    def status(self, text):
        self.status_text = text
        self.dirty = True
        self.draw()

    def user(self, text):
        self.entries.append((None, "User: " + text))
        self.scroll = 0
        self.dirty = True
        self.draw()

    def reply(self, reply, rendered):
        for i, (identifier, _) in enumerate(self.entries):
            if identifier == reply.message_id:
                self.entries[i] = (identifier, "Muse: " + reply.text)
                break
        else:
            self.entries.append((reply.message_id, "Muse: " + reply.text))
        self.dirty = True
        self.draw()

    def draw(self):
        rows, columns = self.screen.getmaxyx()
        if not self.dirty and self.size == (rows, columns):
            return
        self.dirty = False
        self.size = (rows, columns)
        # Leave the last column free, avoiding curses' bottom-right-cell error.
        width = max(1, columns - 1)
        height = max(0, rows - 2)
        lines = deque(maxlen=8192)
        for _, text in self.entries:
            label = text[:6] if text.startswith(('User: ', 'Muse: ')) else ''
            style = self.user_style if label == 'User: ' else self.muse_style
            if label and width > len(label):
                for i, line in enumerate(_wrap(text[6:], width - 6)):
                    lines.append((label + line if i == 0 else ' ' * 6 + line,
                                  style if i == 0 else 0))
            else:
                lines.extend((line, 0) for line in _wrap(text, width))
        self.scroll = min(self.scroll, max(0, len(lines) - height))
        end = len(lines) - self.scroll
        visible = list(lines)[max(0, end - height):end]
        self.screen.erase()

        def put(row, text, style=0, column=0):
            if 0 <= row < rows:
                try:
                    self.screen.addstr(row, column, next(_wrap(text, width - column)), style)
                except curses.error:
                    pass  # Resize may race a draw.

        for row, (line, style) in enumerate(visible):
            if style:
                put(row, line[:6], style)
                put(row, line[6:], column=6)
            else:
                put(row, line)
        put(rows - 1, self.status_text, curses.A_DIM)
        # Horizontally scroll long input while keeping its insertion point visible.
        start = self.cursor
        cells = 0
        while start and cells + _width(self.draft[start - 1]) < max(1, width - 6):
            start -= 1
            cells += _width(self.draft[start])
        input_row = rows - 2
        if width > 6:
            put(input_row, 'User: ', self.user_style)
            put(input_row, self.draft[start:], column=6)
        else:
            put(input_row, 'User: ' + self.draft[start:], self.user_style)
        try:
            self.screen.move(input_row, min(width - 1, 6 + cells))
            self.screen.refresh()
        except curses.error:
            pass

    async def readline(self, limit):
        while True:
            self.draw()
            try:
                key = self.screen.get_wch()
            except curses.error:
                await asyncio.sleep(0.03)
                continue
            self.dirty = True
            if key in ("\n", "\r", curses.KEY_ENTER):
                result = self.draft.encode("utf-8") + b"\n"
                self.draft, self.cursor = "", 0
                return result
            if key == "\x04" and not self.draft:
                return b""
            if key == "\x03":
                return b"/quit\n"
            if key in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                if self.cursor:
                    self.draft = self.draft[:self.cursor - 1] + self.draft[self.cursor:]
                    self.cursor -= 1
            elif key == curses.KEY_DC:
                self.draft = self.draft[:self.cursor] + self.draft[self.cursor + 1:]
            elif key == curses.KEY_LEFT:
                self.cursor = max(0, self.cursor - 1)
            elif key == curses.KEY_RIGHT:
                self.cursor = min(len(self.draft), self.cursor + 1)
            elif key in (curses.KEY_HOME, "\x01"):
                self.cursor = 0
            elif key in (curses.KEY_END, "\x05"):
                self.cursor = len(self.draft)
            elif key == "\x15":
                self.draft = self.draft[self.cursor:]
                self.cursor = 0
            elif key in (curses.KEY_PPAGE, curses.KEY_NPAGE):
                step = max(1, self.screen.getmaxyx()[0] - 2)
                self.scroll = max(0, self.scroll + (step if key == curses.KEY_PPAGE else -step))
            elif isinstance(key, str) and key.isprintable():
                if len(self.draft.encode("utf-8")) + len(key.encode("utf-8")) >= limit:
                    self.status("Message exceeds 64 KiB; shorten it before sending")
                    continue
                self.draft = self.draft[:self.cursor] + key + self.draft[self.cursor:]
                self.cursor += 1

    def close(self):
        if self.screen is not None:
            try:
                self.screen.keypad(False)
                curses.nocbreak()
                curses.echo()
            finally:
                curses.endwin()
                self.screen = None
