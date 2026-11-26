#!/usr/bin/env python3
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

# Live call only.

"""Reference for mgcommands.h (Live call subtitles), and its test vectors.

Python standard library only. It encodes and parses every mg_command_subtitle
message, splits a reply's text into words and words messages the way the
phone apps do, and models the device's reveal in both timing domains. Check
the vectors next to this file:

    python3 protocols/test-vectors/mg_subtitle_ref.py

and regenerate them (the inputs are fixed, so the output never changes):

    python3 protocols/test-vectors/mg_subtitle_ref.py --write

Apps check their encoder and splitter against mg-subtitle-v1.json (messages,
splits); devices check their parser and reveal against it (messages, reveal).
"""

import json
import sys
from pathlib import Path

VECTORS = Path(__file__).with_name("mg-subtitle-v1.json")

# From mgcommands.h.
MG_COMMAND_SUBTITLE = 0x87
REQUEST_CAPABILITIES, CAPABILITIES, STATE, WORDS, REPLY_END, CLEAR, CLOCK = range(7)
VERSION = 1
FLAG_NO_SPEAKER, FLAG_WANTS_AUDIO_TOO, FLAG_FRAME_TIMING = 0x01, 0x02, 0x04
FLAG_MS_TIMING, FLAG_WORD_REVEAL, FLAG_LATIN1_ONLY = 0x08, 0x10, 0x20
TIMING_FRAME, TIMING_MS = 0, 1
WORDS_FINAL, WORDS_REPLACE = 0x01, 0x02
CLEAR_CALL_ENDED, CLEAR_INTERRUPTED, CLEAR_MODE_OFF, CLEAR_NEW_REPLY = range(4)
SHOW_NOW = 0xFFFFFFFF
WORDS_HEADER = 6          # reply id, flags, offset, count (after command, sub-command)
WORD_HEADER = 5           # show_at, length
MAX_WORD_BYTES = 255
MIN_WORDS_BYTES = WORDS_HEADER + WORD_HEADER + 1
SPACE = b" \t\n\r"


def le16(v: int) -> bytes:
    return (v & 0xFFFF).to_bytes(2, "little")


def le32(v: int) -> bytes:
    return (v & 0xFFFFFFFF).to_bytes(4, "little")


def msg(sub: int, payload: bytes = b"") -> bytes:
    return bytes([MG_COMMAND_SUBTITLE, sub]) + payload


# Encoders: one per message.

def request_capabilities() -> bytes:
    return msg(REQUEST_CAPABILITIES)


def capabilities(flags: int, lines: int, chars_per_line: int, max_words_bytes: int) -> bytes:
    return msg(CAPABILITIES, bytes([VERSION, flags, lines, chars_per_line]) + le16(max_words_bytes))


def state(active: bool, timing: int) -> bytes:
    return msg(STATE, bytes([1 if active else 0, timing]))


def words(reply_id: int, flags: int, offset: int, cues) -> bytes:
    """cues: [(show_at, utf8 bytes)]."""
    assert 1 <= len(cues) <= 255
    out = le16(reply_id) + bytes([flags]) + le16(offset) + bytes([len(cues)])
    for show_at, text in cues:
        assert 1 <= len(text) <= MAX_WORD_BYTES
        out += le32(show_at) + bytes([len(text)]) + text
    return msg(WORDS, out)


def reply_end(reply_id: int, show_at: int) -> bytes:
    return msg(REPLY_END, le16(reply_id) + le32(show_at))


def clear(reply_id: int, reason: int) -> bytes:
    return msg(CLEAR, le16(reply_id) + bytes([reason]))


def clock(reply_id: int, playout_ms: int) -> bytes:
    return msg(CLOCK, le16(reply_id) + le32(playout_ms))


# Parser: what a device (or, for capabilities, an app) reads.

def parse(data: bytes):
    """Returns a dict of the message's fields, or None for a malformed one."""
    if len(data) < 2 or data[0] != MG_COMMAND_SUBTITLE:
        return None
    sub, body = data[1], data[2:]

    def u16(i):
        return int.from_bytes(body[i:i + 2], "little")

    def u32(i):
        return int.from_bytes(body[i:i + 4], "little")

    if sub == REQUEST_CAPABILITIES:
        return {"type": "request_capabilities"} if not body else None
    if sub == CAPABILITIES:
        if len(body) < 6 or body[0] != VERSION:
            return None
        return {"type": "capabilities", "version": body[0], "flags": body[1], "lines": body[2],
                "chars_per_line": body[3], "max_words_bytes": u16(4)}
    if sub == STATE:
        if len(body) != 2 or body[0] > 1 or body[1] > TIMING_MS:
            return None
        return {"type": "state", "active": body[0] == 1, "timing": body[1]}
    if sub == WORDS:
        if len(body) < WORDS_HEADER:
            return None
        count, i, cues = body[5], WORDS_HEADER, []
        for _ in range(count):
            if i + WORD_HEADER > len(body):
                return None
            show_at, n = u32(i), body[i + 4]
            text = body[i + WORD_HEADER:i + WORD_HEADER + n]
            if n == 0 or len(text) != n:
                return None
            cues.append({"show_at": show_at, "text": text.decode("utf-8")})
            i += WORD_HEADER + n
        if i != len(body) or count == 0:
            return None
        return {"type": "words", "reply_id": u16(0), "flags": body[2], "offset": u16(3), "words": cues}
    if sub == REPLY_END:
        return {"type": "reply_end", "reply_id": u16(0), "show_at": u32(2)} if len(body) == 6 else None
    if sub == CLEAR:
        if len(body) != 3 or body[2] > CLEAR_NEW_REPLY:
            return None
        return {"type": "clear", "reply_id": u16(0), "reason": body[2]}
    if sub == CLOCK:
        return {"type": "clock", "reply_id": u16(0), "playout_ms": u32(2)} if len(body) == 6 else None
    return None


# The phone side: text into words, words into messages.

def max_word_bytes(max_words_bytes: int) -> int:
    return max(4, min(MAX_WORD_BYTES, max_words_bytes - WORDS_HEADER - WORD_HEADER))


def split_words(utf8: bytes, max_word: int = MAX_WORD_BYTES):
    """Each word with the whitespace after it, as (byte offset, bytes). A word
    longer than max_word goes out in pieces cut at code point boundaries."""
    out, start, i, saw_space = [], 0, 0, False
    while i < len(utf8):
        space = utf8[i] in SPACE
        if saw_space and not space:
            out += _pieces(utf8, start, i, max_word)
            start, saw_space = i, False
        saw_space = saw_space or space
        i += 1
    if start < len(utf8):
        out += _pieces(utf8, start, len(utf8), max_word)
    return out


def _pieces(utf8: bytes, start: int, end: int, max_word: int):
    out = []
    while end - start > max_word:
        cut = start + max_word
        while cut > start and (utf8[cut] & 0xC0) == 0x80:
            cut -= 1
        out.append((start, utf8[start:cut]))
        start = cut
    out.append((start, utf8[start:end]))
    return out


def split_messages(reply_id: int, flags: int, offset: int, cues, max_words_bytes: int):
    """cues: [(show_at, bytes)] in reply order, starting at byte offset `offset`.
    Each message holds as many whole cues as fit max_words_bytes (counted from
    the reply id, after the command and sub-command) and at most 255; a single
    cue always goes even if it is larger. REPLACE stays on the first message
    only and FINAL on the last only. Each message's offset is its first word's."""
    budget = max(MIN_WORDS_BYTES, max_words_bytes)
    batches, current, size = [], [], WORDS_HEADER
    for cue in cues:
        cue_size = WORD_HEADER + len(cue[1])
        if current and (size + cue_size > budget or len(current) == 255):
            batches.append(current)
            current, size = [], WORDS_HEADER
        current.append(cue)
        size += cue_size
    if current:
        batches.append(current)
    out = []
    for n, batch in enumerate(batches):
        f = flags
        if n > 0:
            f &= ~WORDS_REPLACE
        if n < len(batches) - 1:
            f &= ~WORDS_FINAL
        out.append(words(reply_id, f, offset, batch))
        offset += sum(len(t) for _, t in batch)
    return out


# The device side: what is revealed.

class Device:
    """Holds one reply's words by byte offset and reveals them as they come
    due: in the frame domain when a shown frame's 32-bit number reaches
    show_at, in the ms domain when the reply's clock reaches it. SHOW_NOW is
    due at once. Frame numbers come from mg_video_frame_header (16 bits),
    extended by counting wraps over every received frame from the stream's
    start; a frame is shown with the number its receipt gave it (showing it
    is not another receipt, so a picture shown behind the newest received
    one counts no wrap); comparisons are serial (32-bit wrap safe)."""

    def __init__(self):
        self.timing = TIMING_FRAME
        self.active = False     # subtitle mode: only state 1 turns it on
        self.ignored = 0        # words / reply_end / clock received outside subtitle mode
        self.reply = None
        self.cues = {}          # offset -> (show_at, text bytes)
        self.revealed = set()   # offsets of words already shown
        self.ext = None         # extended number of the last received frame
        self.last16 = None
        self.received = {}      # 16-bit number -> extended number, as its latest receipt gave it
        self.shown = None       # extended number of the last shown frame
        self.clock_ms = None
        self.ended_at = None
        self.locked = False     # the passcode lock screen is up: nothing draws over it

    def _due(self, show_at: int) -> bool:
        if show_at == SHOW_NOW:
            return True
        now = self.shown if self.timing == TIMING_FRAME else self.clock_ms
        if now is None:
            return False
        return ((now - show_at) & 0xFFFFFFFF) < 0x80000000

    def text(self) -> str:
        """The revealed text: the longest run of words from the reply's start
        that are each due or already revealed. A revealed word stays revealed
        until a replace or clear removes it. Locked, nothing shows: the words
        wait for the unlock, and a reply whose end comes while locked is
        dropped."""
        if self.locked:
            if self.ended_at is not None and self._due(self.ended_at):
                self._drop()
            return ""
        out = b""
        for offset in sorted(self.cues):
            show_at, text = self.cues[offset]
            if offset not in self.revealed and not self._due(show_at):
                break
            self.revealed.add(offset)
            out += text
        return out.decode("utf-8")

    def frame_received(self, number16: int):
        """A frame header received (decoded or dropped): the only thing that extends the count."""
        if self.last16 is None:
            self.ext = number16
        else:
            self.ext = (self.ext + ((number16 - self.last16) & 0xFFFF)) & 0xFFFFFFFF
        self.last16 = number16
        self.received[number16] = self.ext
        return self.ext

    def frame_shown(self, number16: int):
        """A frame received earlier is shown: its number is the one its receipt gave it."""
        assert number16 in self.received, f"frame {number16} shown but never received"
        self.shown = self.received[number16]

    def stream_start(self):
        self.ext = self.last16 = self.shown = None
        self.received = {}

    def apply(self, data: bytes):
        m = parse(data)
        assert m is not None, data.hex()
        kind = m["type"]
        if kind in ("words", "reply_end", "clock") and not self.active:
            self.ignored += 1   # outside subtitle mode: ignored (and counted), never shown
            return
        if kind == "state":
            self.timing = m["timing"]
            self.active = m["active"]
            if not m["active"]:
                self._drop()
        elif kind == "words":
            if m["reply_id"] != self.reply:
                self._drop()
                self.reply = m["reply_id"]
            offset = m["offset"]
            if m["flags"] & WORDS_REPLACE:
                for k in [k for k in self.cues if k >= offset]:
                    del self.cues[k]
                    self.revealed.discard(k)
            for cue in m["words"]:
                text = cue["text"].encode("utf-8")
                self.cues[offset] = (cue["show_at"], text)
                offset += len(text)
        elif kind == "clear":
            if m["reply_id"] in (0, self.reply):
                self._drop()
        elif kind == "clock":
            if m["reply_id"] == self.reply:
                self.clock_ms = m["playout_ms"]
        elif kind == "reply_end":
            if m["reply_id"] == self.reply:
                self.ended_at = m["show_at"]

    def _drop(self):
        self.reply, self.cues, self.clock_ms, self.ended_at = None, {}, None, None
        self.revealed = set()


# The vectors.

def cue_list(cues):
    return [(at, t.encode("utf-8")) for at, t in cues]


def build():
    messages = []

    def add(name, data, note=None):
        entry = {"name": name, "hex": data.hex(), "fields": parse(data)}
        if note:
            entry["note"] = note
        messages.append(entry)

    add("request_capabilities", request_capabilities())
    add("capabilities_mosaico", capabilities(FLAG_FRAME_TIMING | FLAG_MS_TIMING | FLAG_WORD_REVEAL, 2, 26, 480))
    add("capabilities_no_speaker", capabilities(FLAG_NO_SPEAKER | FLAG_MS_TIMING | FLAG_LATIN1_ONLY, 2, 16, 180))
    add("state_on_frames", state(True, TIMING_FRAME))
    add("state_on_ms", state(True, TIMING_MS))
    add("state_off", state(False, TIMING_FRAME))
    add("words_frames", words(1, 0, 0, cue_list([(120, "Hello "), (124, "there. ")])))
    add("words_ms_final", words(2, WORDS_FINAL, 0, cue_list([(0, "Good "), (280, "morning.")])))
    add("words_show_now", words(3, 0, 0, cue_list([(SHOW_NOW, "Now ")])),
        "show_at 0xFFFFFFFF: shown as soon as it arrives, in either domain")
    multibyte = ["café ", "日本 ", "👋 "]
    add("words_multibyte", words(4, 0, 6, cue_list(list(zip((10, 12, 14), multibyte)))),
        "offset counts UTF-8 bytes: " + ", ".join(f'"{t}" is {len(t.encode("utf-8"))}' for t in multibyte))
    add("words_replace", words(5, WORDS_REPLACE, 7, cue_list([(30, "world! ")])),
        "drops every word of reply 5 from byte 7 on, then adds these")
    add("words_reply_id_max", words(0xFFFF, WORDS_FINAL, 0x1234, cue_list([(0x7FFFFFFF, "end.")])))
    add("words_frame_wrap", words(6, 0, 0, cue_list([(0x00010002, "after "), (0x00010003, "wrap ")])),
        "32-bit frame numbers past the 16-bit header's wrap")
    add("reply_end_frames", reply_end(1, 140))
    add("reply_end_ms", reply_end(2, 1200))
    add("clear_all_call_ended", clear(0, CLEAR_CALL_ENDED), "reply id 0: every reply")
    add("clear_interrupted", clear(7, CLEAR_INTERRUPTED))
    add("clear_mode_off", clear(0, CLEAR_MODE_OFF))
    add("clear_new_reply", clear(8, CLEAR_NEW_REPLY))
    add("clock", clock(2, 1000))

    malformed = [
        {"name": "words_count_zero", "hex": msg(WORDS, le16(1) + b"\x00" + le16(0) + b"\x00").hex()},
        {"name": "words_truncated", "hex": words(1, 0, 0, cue_list([(1, "abc")]))[:-1].hex()},
        {"name": "words_trailing_byte", "hex": (words(1, 0, 0, cue_list([(1, "abc")])) + b"\x00").hex()},
        {"name": "words_length_zero", "hex": msg(WORDS, le16(1) + b"\x00" + le16(0) + b"\x01" + le32(1) + b"\x00").hex()},
        {"name": "capabilities_version_2", "hex": msg(CAPABILITIES, bytes([2, 0, 2, 16]) + le16(180)).hex()},
        {"name": "state_bad_timing", "hex": msg(STATE, b"\x01\x02").hex()},
        {"name": "state_active_2", "hex": msg(STATE, b"\x02\x00").hex(),
         "note": "active is 0 or 1; a device answers invalid_value"},
        {"name": "clear_bad_reason", "hex": msg(CLEAR, le16(0) + b"\x04").hex()},
        {"name": "unknown_sub_command", "hex": msg(7).hex()},
    ]
    for m in malformed:
        assert parse(bytes.fromhex(m["hex"])) is None, m["name"]

    splits = []

    def add_split(name, text, max_words_bytes, times, flags=WORDS_FINAL, offset=0, reply_id=1):
        utf8 = text.encode("utf-8")
        ws = split_words(utf8[offset:], max_word_bytes(max_words_bytes))
        assert len(ws) == len(times), (name, len(ws))
        cues = [(at, w) for at, (_, w) in zip(times, ws)]
        out = split_messages(reply_id, flags, offset, cues, max_words_bytes)
        # Every message keeps to its own budget (counted from the reply id): the smallest budget a cut word
        # always fits is WORDS_HEADER + WORD_HEADER + 4.
        assert all(len(m) - 2 <= max_words_bytes for m in out), (name, max_words_bytes)
        splits.append({
            "name": name, "text": text, "from_offset": offset, "reply_id": reply_id, "flags": flags,
            "max_words_bytes": max_words_bytes, "show_at": times,
            "words": [{"offset": offset + o, "text": w.decode("utf-8")} for o, w in ws],
            "messages": [m.hex() for m in out],
        })

    add_split("one_message", "The Moon is moving away from Earth.", 180, [100, 102, 104, 106, 108, 110, 112])
    add_split("split_across_messages", "Octopuses have three hearts and blue blood, which carries copper.", 40,
              [10 * i for i in range(10)])
    add_split("multibyte_words", "Ça va? 日本語 は 🙂 fine.", 180, [1, 2, 3, 4, 5, 6])
    add_split("whitespace_kinds", "one\ttwo\nthree\r\nfour  five", 180, [1, 2, 3, 4, 5])
    add_split("long_word_cut_at_code_points", "é" * 20 + " end", 30, [5, 6, 7, 8])
    add_split("replace_from_offset", "Hello there, world.", 180, [50, 52], flags=WORDS_REPLACE, offset=7, reply_id=9)
    add_split("not_final_yet", "Light from the Sun takes", 180, [0, 300, 600, 900, 1200], flags=0, reply_id=3)
    add_split("tiny_budget", "a b c", WORDS_HEADER + WORD_HEADER + 4, [1, 2, 3])

    reveal = []

    def add_reveal(name, timing, steps, start=True):
        """start: the case opens with state(True, timing), listed as its first step (subtitle mode begins with
        state 1); start=False lists every message itself."""
        dev, out = Device(), []
        ends = {}   # reply id -> the reply's text received so far, in bytes (every words message, mode on or off)
        if start:
            dev.apply(state(True, timing))
            out.append({"message": state(True, timing).hex(), "text": dev.text()})
        for step in steps:
            kind = step[0]
            if kind == "frame":   # a frame received and shown as it came: two steps
                dev.frame_received(step[1])
                out.append({"frame_received": step[1], "text": dev.text()})
                if dev.locked:
                    out[-1]["locked"] = True
                kind = "shown"
            if kind == "msg":
                m = parse(step[1])
                if m["type"] == "words":
                    # The wire examples are a reply's own bytes: each words message starts where the reply's text
                    # so far ends, or (replace) inside it.
                    end = ends.get(m["reply_id"], 0)
                    if m["flags"] & WORDS_REPLACE:
                        assert m["offset"] <= end, (name, m)
                    else:
                        assert m["offset"] == end, (name, m["offset"], end)
                    ends[m["reply_id"]] = m["offset"] + sum(len(w["text"].encode("utf-8")) for w in m["words"])
                dev.apply(step[1])
                out.append({"message": step[1].hex(), "text": dev.text()})
            elif kind == "shown":
                dev.frame_shown(step[1])
                out.append({"frame_shown": step[1], "text": dev.text()})
            elif kind == "received":
                dev.frame_received(step[1])
                out.append({"frame_received": step[1], "text": dev.text()})
            elif kind == "stream_start":
                dev.stream_start()
                out.append({"stream_start": True, "text": dev.text()})
            elif kind in ("lock", "unlock"):
                dev.locked = kind == "lock"   # the user's own lock and unlock; no subtitle message changes it
                out.append({kind: True, "text": dev.text()})
            if dev.locked:
                out[-1]["locked"] = True
        reveal.append({"name": name, "timing": timing, "steps": out})

    add_reveal("frames_basic", TIMING_FRAME, [
        ("frame", 98),
        ("msg", words(1, 0, 0, cue_list([(100, "The "), (103, "Sun "), (106, "is ")]))),
        ("frame", 99), ("frame", 100), ("frame", 102), ("frame", 103), ("frame", 107),
        ("msg", words(1, WORDS_FINAL, 11, cue_list([(105, "a "), (110, "star.")]))),
        ("frame", 109), ("frame", 110),
        ("msg", reply_end(1, 125)),
        ("msg", clear(1, CLEAR_NEW_REPLY)),
    ])
    add_reveal("frames_received_but_dropped_still_count", TIMING_FRAME, [
        ("frame", 10),
        ("msg", words(2, 0, 0, cue_list([(12, "one "), (13, "two ")]))),
        ("received", 11), ("received", 12), ("frame", 13),
    ])
    add_reveal("frames_wrap", TIMING_FRAME, [
        ("frame", 0xFFFE),
        ("msg", words(3, 0, 0, cue_list([(0xFFFF, "before "), (0x00010001, "after ")]))),
        ("frame", 0xFFFF), ("frame", 0x0000), ("frame", 0x0001),
    ])
    add_reveal("shown_behind_the_newest_received", TIMING_FRAME, [
        # The panel shows a picture behind the newest frame received (decoding lags): it keeps the number its
        # receipt gave it, and showing it counts no wrap.
        ("frame", 10),
        ("msg", words(7, 0, 0, cue_list([(11, "next "), (13, "word ")]))),
        ("received", 11), ("received", 12), ("received", 13),
        ("shown", 11), ("shown", 12), ("shown", 13),
    ])
    add_reveal("show_now_and_past", TIMING_FRAME, [
        ("frame", 500),
        ("msg", words(4, 0, 0, cue_list([(SHOW_NOW, "now "), (450, "late "), (600, "later ")]))),
        ("frame", 600),
    ])
    add_reveal("replace_and_barge_in", TIMING_FRAME, [
        ("frame", 20),
        ("msg", words(5, 0, 0, cue_list([(20, "Hello "), (21, "there "), (22, "friend ")]))),
        ("frame", 22),
        ("msg", words(5, WORDS_REPLACE, 6, cue_list([(23, "world ")]))),
        ("frame", 23),
        ("msg", clear(0, CLEAR_INTERRUPTED)),
    ])
    add_reveal("stream_restart_resets_frames", TIMING_FRAME, [
        ("frame", 900),
        ("msg", words(6, 0, 0, cue_list([(901, "Rain "), (903, "falls ")]))),
        ("frame", 901),
        ("stream_start",), ("frame", 0),
        ("msg", words(6, WORDS_REPLACE, 5, cue_list([(2, "falls ")]))),
        ("frame", 1), ("frame", 2),
    ])
    add_reveal("ms_clock", TIMING_MS, [
        ("msg", words(7, 0, 0, cue_list([(0, "Water "), (400, "boils "), (900, "at "), (1300, "100 ")]))),
        ("msg", clock(7, 0)), ("msg", clock(7, 1000)), ("msg", clock(7, 2000)),
        ("msg", clock(8, 3000)),
        ("msg", reply_end(7, 1700)),
    ])
    add_reveal("clock_is_per_reply", TIMING_MS, [
        # A new reply drops the old one's clock with it: its words wait for their own reply's clock.
        ("msg", words(30, 0, 0, cue_list([(0, "First ")]))),
        ("msg", clock(30, 9000)),
        ("msg", words(31, 0, 0, cue_list([(500, "Second ")]))),
        ("msg", clock(31, 400)),
        ("msg", clock(31, 600)),
    ])
    add_reveal("words_before_state_ignored", TIMING_MS, [
        # Only state 1 turns subtitle mode on; words, reply_end and clock outside it
        # are ignored (counted), and state 0 drops everything and ends the mode again.
        ("msg", words(12, 0, 0, cue_list([(SHOW_NOW, "unasked ")]))),
        ("msg", clock(12, 500)),
        ("msg", reply_end(12, 900)),
        ("msg", state(True, TIMING_MS)),
        ("msg", words(13, 0, 0, cue_list([(SHOW_NOW, "asked ")]))),
        ("msg", state(False, TIMING_MS)),
        ("msg", words(13, 0, 6, cue_list([(SHOW_NOW, "after ")]))),
        ("msg", state(True, TIMING_MS)),
        ("msg", words(14, 0, 0, cue_list([(SHOW_NOW, "again ")]))),
    ], start=False)
    add_reveal("locked_waits_for_unlock", TIMING_FRAME, [
        # A locked gadget draws nothing over its lock screen; state 1 and words
        # neither wake nor unlock it. The words show once the user unlocks.
        ("lock",),
        ("msg", state(True, TIMING_FRAME)),
        ("frame", 100),
        ("msg", words(20, 0, 0, cue_list([(100, "Shown "), (102, "after "), (104, "unlock ")]))),
        ("frame", 102),
        ("unlock",),
        ("frame", 104),
    ], start=False)
    add_reveal("locked_reply_ends_first", TIMING_FRAME, [
        # The reply ends (its reply_end moment comes) while locked: dropped,
        # nothing shows after the unlock.
        ("lock",),
        ("msg", state(True, TIMING_FRAME)),
        ("frame", 200),
        ("msg", words(21, WORDS_FINAL, 0, cue_list([(200, "Never "), (201, "seen ")]))),
        ("msg", reply_end(21, 203)),
        ("frame", 201), ("frame", 203),
        ("unlock",),
    ], start=False)
    add_reveal("new_reply_replaces_old", TIMING_MS, [
        ("msg", words(10, 0, 0, cue_list([(SHOW_NOW, "first ")]))),
        ("msg", words(11, 0, 0, cue_list([(SHOW_NOW, "second ")]))),
        ("msg", clear(10, CLEAR_NEW_REPLY)),
        ("msg", clear(11, CLEAR_MODE_OFF)),
    ])

    return {
        "description": (
            "Live call subtitles (protocols/mgcommands.h, Live call subtitles; Live call only). "
            "Hex strings are whole Control payloads, command byte first. messages: each encodes to hex and "
            "parses to fields; malformed: each must be rejected. splits: the reply text from from_offset, split "
            "into words (each with the whitespace after it, byte offsets, a word longer than "
            "max(4, min(255, max_words_bytes - 11)) bytes cut at code point boundaries), each word given the "
            "listed show_at, then packed into the listed words messages (max_words_bytes counts from the reply "
            "id; at most 255 words; REPLACE on the first message only, FINAL on the last only). reveal: a device "
            "model; after each step the revealed text must equal text. Frame numbers are mg_video_frame_header's "
            "16 bits, extended to 32 by counting wraps over every received frame since the stream started: "
            "frame_received is a frame header received (decoded or dropped), the only step that extends the count, "
            "and frame_shown shows a frame received earlier with the number its receipt gave it (showing is not "
            "another receipt). A word is revealed once a shown frame reaches show_at (serial comparison), or the "
            "reply's clock "
            "does in the ms domain. Subtitle mode begins with state 1 (each reveal case lists it first): words, "
            "reply_end and clock received outside subtitle mode are ignored (counted), and state 0 drops everything "
            "and ends the mode. lock and unlock are the user's own passcode lock (locked: true on every step while "
            "it is up): nothing shows over it and no subtitle message wakes or unlocks it; the words wait for the "
            "unlock, and a reply whose reply_end moment comes while locked is dropped. Generated by "
            "mg_subtitle_ref.py."),
        "version": 1,
        "command": MG_COMMAND_SUBTITLE,
        "show_now": SHOW_NOW,
        "messages": messages,
        "malformed": malformed,
        "splits": splits,
        "reveal": reveal,
    }


def main() -> int:
    vectors = build()
    text = json.dumps(vectors, indent=1, ensure_ascii=False) + "\n"
    if "--write" in sys.argv:
        VECTORS.write_text(text, encoding="utf-8")
        print(f"wrote {VECTORS.name}: {len(vectors['messages'])} messages, {len(vectors['splits'])} splits, "
              f"{len(vectors['reveal'])} reveal cases")
        return 0
    if not VECTORS.exists() or VECTORS.read_text(encoding="utf-8") != text:
        print(f"{VECTORS.name} is out of date: run with --write", file=sys.stderr)
        return 1
    print(f"{VECTORS.name} matches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
