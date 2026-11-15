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

"""Compact terminal chat, with a persistent status and input row."""
from __future__ import annotations

import asyncio
from collections import deque
import curses
import os
import unicodedata


def _width(char):
    if unicodedata.combining(char):
        return 0
    return 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1


def _wrap_positions(text, width):
    """Yield (source offset, line) with word breaks measured in terminal cells."""
    width = max(1, width)
    start = 0
    while start < len(text):
        end, cells = start, 0
        while end < len(text) and text[end] != "\n":
            size = _width(text[end])
            if cells + size > width:
                break
            cells += size
            end += 1
        following = end
        if end < len(text) and text[end] == "\n":
            following += 1
        elif end < len(text):
            # Prefer the last separator, including one just beyond a full row.
            split = end
            while split > start and not text[split].isspace():
                split -= 1
            if split > start and text[start:split].strip():
                end = split
                following = split
                while following < len(text) and text[following] == " ":
                    following += 1
            elif end == start:
                # A wide glyph cannot fit in a one-cell terminal. Consume it
                # without asking curses to draw outside the available cells.
                following += 1
        yield start, text[start:end]
        start = following
    if not text or text.endswith("\n"):
        yield len(text), ""


def _wrap(text, width):
    """Wrap whole words by terminal cells, preserving explicit newlines."""
    for _, line in _wrap_positions(text.expandtabs(4), width):
        yield line


class PlainDisplay:
    """Readable output for pipes and terminals without cursor support."""

    def __init__(self, output):
        self.output = output
        self.at_start = True
        self.interrupted = False

    def write(self, text):
        self._write(text)
        # A later reply chunk must not continue unlabeled after this line.
        self.interrupted = True

    def _write(self, text):
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
        if self.interrupted and rendered and not rendered.startswith("\n"):
            rendered = "\nMuse: " + reply.text + ("\n" if rendered.endswith("\n") else "")
        self.interrupted = False
        self._write(rendered)

    def clear(self):
        if self.output.isatty() and os.environ.get("TERM", "dumb") != "dumb":
            self.output.write("\x1b[2J\x1b[H")
            self.output.flush()
            self.at_start = True
        else:
            self.status("Screen clearing is unavailable for plain-text output")

    def close(self):
        pass


class TerminalDisplay:
    """A bounded transcript above a growing editor and a fixed status row."""

    def __init__(self):
        self.screen = None
        self.entries = deque(maxlen=512)
        self.status_text = "Muse is ready · /quit or /exit · /clear"
        self.draft = ""
        self.cursor = 0
        self.scroll = 0
        self.mouse_mask = None
        self.wheel_up = getattr(curses, "BUTTON4_PRESSED", 0)
        self.wheel_down = getattr(curses, "BUTTON5_PRESSED", 0)
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
            if self.wheel_up and self.wheel_down:
                try:
                    _, self.mouse_mask = curses.mousemask(self.wheel_up | self.wheel_down)
                except curses.error:
                    pass  # Keyboard scrolling remains available without mouse support.
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

    def clear(self):
        self.entries.clear()
        self.scroll = 0
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
        indent = 6 if width > 6 else 0
        input_width = width - indent
        draft_lines = list(_wrap_positions(self.draft, input_width))
        cursor_line = 0
        for i, (offset, _) in enumerate(draft_lines):
            if offset <= self.cursor:
                cursor_line = i
        offset, line = draft_lines[cursor_line]
        cursor_cells = sum(_width(c) for c in line[:self.cursor - offset])
        # Reserve a cell for the insertion cursor after a completely full row.
        if cursor_cells >= input_width:
            if cursor_line == len(draft_lines) - 1:
                draft_lines.append((self.cursor, ""))
            cursor_line += 1
            cursor_cells = 0
        input_height = min(len(draft_lines), max(1, rows - 2))
        input_start = min(max(0, cursor_line - input_height + 1),
                          len(draft_lines) - input_height)
        input_row = max(0, rows - 1 - input_height)
        height = input_row
        self.transcript_height = height
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
        for i, (_, line) in enumerate(draft_lines[input_start:input_start + input_height]):
            row = input_row + i
            if indent:
                put(row, 'User: ' if input_start + i == 0 else ' ' * indent,
                    self.user_style)
                put(row, line, column=indent)
            else:
                put(row, line)
        try:
            self.screen.move(input_row + cursor_line - input_start,
                             min(width - 1, indent + cursor_cells))
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
            elif key == curses.KEY_MOUSE:
                try:
                    _, _, _, _, buttons = curses.getmouse()
                except curses.error:
                    continue
                if buttons & self.wheel_up:
                    self.scroll += 3
                elif buttons & self.wheel_down:
                    self.scroll = max(0, self.scroll - 3)
            elif key in (curses.KEY_PPAGE, curses.KEY_NPAGE):
                step = max(1, self.transcript_height)
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
                if self.mouse_mask is not None:
                    try:
                        curses.mousemask(self.mouse_mask)
                    except curses.error:
                        pass
                    self.mouse_mask = None
                self.screen.keypad(False)
                curses.nocbreak()
                curses.echo()
            finally:
                curses.endwin()
                self.screen = None
