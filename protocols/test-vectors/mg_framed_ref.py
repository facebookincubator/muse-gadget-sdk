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

"""Reference device for framed Data (mgcommands.h, mg_data_type_framed), and its test vectors.

Python standard library only. A device that lists mg_stream_video_capability_flag_framed with a video stream and audio
playback sharing the Data characteristic: Control writes (change_data_type, the streams' start, stop, buffer requests,
flush, video_anchor) and Data writes, in the order they arrive. Check the vectors next to this file:

    python3 protocols/test-vectors/mg_framed_ref.py

and regenerate them (the inputs are fixed, so the output never changes):

    python3 protocols/test-vectors/mg_framed_ref.py --write

A device checks itself against mg-framed-v1.json: given each exchange's writes in order, it notifies exactly what the
step lists and ends each step in the state it lists (the counters a device keeps; a firmware harness compares the
ones it has). A client checks its framing against the same writes.
"""

import hashlib
import json
import struct
import sys
from pathlib import Path

VECTORS = Path(__file__).with_name("mg-framed-v1.json")

# From mgcommands.h.
MG_COMMAND_CHANGE_DATA_TYPE = 8
MG_COMMAND_STREAM_AUDIO = 29
MG_COMMAND_STREAM_VIDEO = 0x84
START_STREAMING, STOP_STREAMING, REQUEST_BUFFER_UPDATE, BUFFER_UPDATE, FLUSH, VIDEO_ANCHOR = 2, 3, 4, 5, 6, 8
BUFFER_KEEP, BUFFER_DROP = 0, 1
AUDIO_LC3, VIDEO_H264, VIDEO_VP8, FRAMED = 19, 24, 25, 30
FLAG_FRAMED = 0x02
VIDEO_FRAME_MAGIC, VIDEO_FRAME_HEADER_LEN, VIDEO_FRAME_KEYFRAME = 0x56, 8, 0x01
MAX_ATT_VALUE = 512

# The reference device.
MTU = 100                 # MG_MIN_ATT_MTU: a write is at most 97 bytes
AUDIO_BUFFER = 4096
LC3_FRAME = 40            # 16 kHz, 10 ms, 32 kb/s
VIDEO_BUFFER = 16384
UNKNOWN_TYPE = 0xEE       # no mg_data_type_t


def max_write(mtu: int) -> int:
    return min(mtu - 3, MAX_ATT_VALUE)


def le16(v: int) -> bytes:
    return struct.pack("<H", v)


def le32(v: int) -> bytes:
    return struct.pack("<I", v)


def det(label: str, n: int) -> bytes:
    out, i = b"", 0
    while len(out) < n:
        out += hashlib.sha256(f"{label}/{i}".encode()).digest()
        i += 1
    return out[:n]


def change_data_type(t: int) -> bytes:
    return bytes([MG_COMMAND_CHANGE_DATA_TYPE, t])


def audio_start() -> bytes:
    """LC3 at 16 kHz, 32 kb/s, LC3_FRAME-byte frames (parameters in increasing id order)."""
    return bytes([MG_COMMAND_STREAM_AUDIO, START_STREAMING, 4, 1, AUDIO_LC3, 2]) + le32(16000) + bytes([3]) + le16(32) \
        + bytes([5]) + le16(LC3_FRAME)


def video_start(codec: int = VIDEO_H264) -> bytes:
    return bytes([MG_COMMAND_STREAM_VIDEO, START_STREAMING, 3, 1, codec, 2]) + le16(240) + bytes([3]) + le16(240)


def stop(command: int, action: int = BUFFER_DROP) -> bytes:
    return bytes([command, STOP_STREAMING, action])


def request_buffer(command: int) -> bytes:
    return bytes([command, REQUEST_BUFFER_UPDATE])


def buffer_update(command: int, available: int, total: int) -> bytes:
    return bytes([command, BUFFER_UPDATE]) + le32(available) + le32(total)


def flush() -> bytes:
    return bytes([MG_COMMAND_STREAM_AUDIO, FLUSH])


def video_anchor(frame: int) -> bytes:
    return bytes([MG_COMMAND_STREAM_AUDIO, VIDEO_ANCHOR]) + le32(frame)


START_CODE = b"\x00\x00\x00\x01"


def access_unit(n: int, size: int, keyframe: bool) -> bytes:
    """A size-byte access unit in Annex B form, as the header requires: start codes, and on a keyframe an SPS and a
    PPS NAL before the IDR slice (a non-IDR slice otherwise). The NAL bodies are zero-free stand-in bytes (no start
    code emulation), structurally valid but not decodable: the vectors test framing and counts, not the decoder."""
    head = (START_CODE + b"\x67\x42\xc0" + START_CODE + b"\x68\xce" + START_CODE + b"\x65") if keyframe \
        else (START_CODE + b"\x41")
    assert size >= len(head) + 1
    return head + bytes(b or 0x01 for b in det(f"framed video {n}", size - len(head)))


def video_frame(n: int, size: int, keyframe: bool = False) -> bytes:
    """A frame header and a size-byte Annex B access unit."""
    return bytes([VIDEO_FRAME_MAGIC, VIDEO_FRAME_KEYFRAME if keyframe else 0]) + le16(n) + le32(size) \
        + access_unit(n, size, keyframe)


def lc3_frames(label: str, n: int) -> bytes:
    return det(f"framed audio {label}", n * LC3_FRAME)


class Device:
    """The device side of Data while a video stream and audio playback may both be open.

    Plain mode: Data goes to the selected stream (change_data_type selects it; a type that isn't the audio stream's
    selects video). With one stream open, Data goes to it; when the client ends the selected stream, Data goes to the
    other; when the device ends it, Data for it is discarded until the next change_data_type.
    Framed mode (change_data_type(framed) on a device that lists the flag): each write's first byte names the stream,
    exactly; credits and counts exclude it; a write with no stream data, or naming no open stream, is discarded and
    counted, and nothing else changes. It ends with a change_data_type to another type, when both streams have ended,
    or at disconnect.
    Audio writes are whole frames: one that isn't, or doesn't fit, is dropped whole (its bytes still count in the
    received total). Video is a byte stream of frames (header + access unit), each decoded and shown when complete."""

    def __init__(self, flags: int = FLAG_FRAMED):
        self.flags = flags
        self.discarded = 0        # Data writes that went to no stream
        self.dropped = 0          # audio writes dropped whole
        self.reset()

    def reset(self):
        """A new connection: no streams, plain mode, nothing selected."""
        self.audio = self.video = None   # {"held", "total"} / {"buf", "total", "shown"}
        self.framed = False
        self.selected = None             # "audio" / "video" in plain mode
        self.discarding = False          # plain mode: the device ended the selected stream
        self.anchor = None               # (frame, audio offset): the next audio Data's first byte goes with frame
        self.anchor_pending = None
        self.gap_start = None            # audio offset of the first byte held since the last gap (a reply's first)

    # ---- Control ----
    def control(self, frame: bytes) -> list:
        cmd = frame[0]
        if cmd == MG_COMMAND_CHANGE_DATA_TYPE:
            return self.on_change_data_type(frame[1])
        if cmd == MG_COMMAND_STREAM_AUDIO:
            return self.on_audio(frame[1], frame[2:])
        if cmd == MG_COMMAND_STREAM_VIDEO:
            return self.on_video(frame[1], frame[2:])
        return []

    def on_change_data_type(self, t: int) -> list:
        if t == FRAMED and self.flags & FLAG_FRAMED:
            self.framed = True   # a repeat is a no-op
        else:
            self.framed = False
            self.selected = "audio" if t == AUDIO_LC3 or (self.audio and t == self.audio["codec"]) else "video"
            self.discarding = False
        return []   # never answered

    def audio_update(self, available: int) -> bytes:
        """buffer_update for audio; its received total plus available bytes is the credit granted."""
        self.audio["grant"] = self.audio["total"] + available
        return buffer_update(MG_COMMAND_STREAM_AUDIO, available, self.audio["total"])

    def on_audio(self, sub: int, p: bytes) -> list:
        if sub == START_STREAMING:
            self.audio = {"held": 0, "total": 0, "codec": p[2]}   # codec: the first parameter (id 1)
            self.selected = self.selected or "audio"
            return [self.audio_update(AUDIO_BUFFER)]
        if sub == STOP_STREAMING and self.audio is not None:
            self.end("audio", by_client=True)
            return [stop(MG_COMMAND_STREAM_AUDIO, BUFFER_DROP)]   # nothing played here: dropped at once
        if sub == REQUEST_BUFFER_UPDATE and self.audio is not None:
            return [self.audio_update(AUDIO_BUFFER - self.audio["held"])]
        if sub == FLUSH and self.audio is not None:
            self.audio["held"] = 0
            self.anchor_pending = self.gap_start = None   # a flush drops a pending anchor
            return [self.audio_update(AUDIO_BUFFER)]
        if sub == VIDEO_ANCHOR and self.audio is not None and self.video is not None:
            frame = struct.unpack("<I", p[:4])[0]
            if self.audio["held"] and self.gap_start is not None:
                self.anchor, self.anchor_pending = (frame, self.gap_start), None   # late: names the reply's first
            else:
                self.anchor_pending = frame
            return []
        return []

    def on_video(self, sub: int, p: bytes) -> list:
        if sub == START_STREAMING:
            self.video = {"buf": bytearray(), "total": 0, "shown": 0, "codec": p[2]}
            self.selected = self.selected or "video"
            self.anchor_pending = None
            return [buffer_update(MG_COMMAND_STREAM_VIDEO, VIDEO_BUFFER, 0)]
        if sub == STOP_STREAMING and self.video is not None:
            self.end("video", by_client=True)
            return []   # the device never answers the client's
        if sub == REQUEST_BUFFER_UPDATE and self.video is not None:
            return [buffer_update(MG_COMMAND_STREAM_VIDEO, VIDEO_BUFFER - len(self.video["buf"]), self.video["total"])]
        return []

    def end(self, which: str, by_client: bool):
        setattr(self, which, None)
        if which == "video":
            self.anchor_pending = None
        other = "video" if which == "audio" else "audio"
        if getattr(self, other) is None:
            self.framed = False   # both ended
            self.selected, self.discarding = None, False
        elif self.selected == which:
            if by_client:
                self.selected = other
            else:
                self.discarding = True

    # ---- device events ----
    def event(self, what: str) -> list:
        if what == "device_ends_audio" and self.audio is not None:
            self.end("audio", by_client=False)
            return [stop(MG_COMMAND_STREAM_AUDIO, BUFFER_DROP)]
        if what == "device_ends_video" and self.video is not None:
            self.end("video", by_client=False)
            return [stop(MG_COMMAND_STREAM_VIDEO, BUFFER_DROP)]
        if what == "audio_plays" and self.audio is not None:
            self.audio["held"] = 0   # the device plays out everything it holds
        if what == "disconnect":
            self.reset()
        return []

    def past_credit(self, a: dict) -> bool:
        return a["total"] > a["grant"]

    # ---- Data ----
    def data(self, write: bytes) -> list:
        if self.framed:
            target, payload = self.framed_target(write)
        else:
            target, payload = self.plain_target(), write
        if target is None:
            self.discarded += 1
        elif target == "audio":
            self.audio_bytes(payload)
        else:
            self.video_bytes(payload)
        return []

    def framed_target(self, write: bytes):
        if len(write) < 2:
            return None, b""   # no stream data
        t, payload = write[0], write[1:]
        if self.audio is not None and t == self.audio["codec"]:
            return "audio", payload
        if self.video is not None and t == self.video["codec"]:
            return "video", payload
        return None, payload

    def plain_target(self):
        if self.discarding:
            return None
        if self.selected == "audio" and self.audio is not None:
            return "audio"
        if self.selected == "video" and self.video is not None:
            return "video"
        return None

    def audio_bytes(self, payload: bytes):
        a = self.audio
        a["total"] += len(payload)
        if not payload or len(payload) % LC3_FRAME or a["held"] + len(payload) > AUDIO_BUFFER \
                or self.past_credit(a):
            self.dropped += 1   # not whole frames, doesn't fit, or past the credit granted: dropped whole
            return
        if not a["held"]:
            self.gap_start = a["total"] - len(payload)   # this write starts after a gap
        if self.anchor_pending is not None:
            self.anchor = (self.anchor_pending, a["total"] - len(payload))
            self.anchor_pending = None
        a["held"] += len(payload)

    def video_bytes(self, payload: bytes):
        v = self.video
        v["total"] += len(payload)
        v["buf"].extend(payload)
        while len(v["buf"]) >= VIDEO_FRAME_HEADER_LEN:
            n = struct.unpack("<I", bytes(v["buf"][4:8]))[0]
            if len(v["buf"]) < VIDEO_FRAME_HEADER_LEN + n:
                break
            del v["buf"][:VIDEO_FRAME_HEADER_LEN + n]
            v["shown"] += 1

    def state(self) -> dict:
        return {"framed": self.framed,
                "audio": None if self.audio is None else [self.audio["held"], self.audio["total"]],
                "video": None if self.video is None else [len(self.video["buf"]), self.video["total"],
                                                          self.video["shown"]],
                "discarded": self.discarded, "dropped": self.dropped,
                "anchor": None if self.anchor is None else list(self.anchor)}


class Recorder:
    """The client's side: Control writes, Data writes (framed or plain), device events."""

    def __init__(self, name: str, note: str, device=None):
        self.dev = device or Device()
        self.ex = {"name": name, "mtu": MTU, "note": note, "steps": []}

    def step(self, kind: str, value, notify: list, note: str = None):
        st = {kind: value.hex() if isinstance(value, bytes) else value, "notify": [f.hex() for f in notify]}
        if note:
            st["note"] = note
        st["state"] = self.dev.state()
        self.ex["steps"].append(st)

    def control(self, frame: bytes, note: str = None):
        self.step("control", frame, self.dev.control(frame), note)

    def write(self, data: bytes, note: str = None):
        assert len(data) <= max_write(MTU)
        self.step("data", data, self.dev.data(data), note)

    def event(self, what: str, note: str = None):
        self.step("event", what, self.dev.event(what), note)

    def framed_video(self, frame: bytes, note: str = None, codec: int = VIDEO_H264):
        room = max_write(MTU) - 1   # a write's stream data is at most max write - 1
        for i in range(0, len(frame), room):
            self.write(bytes([codec]) + frame[i:i + room], note if i == 0 else None)

    def plain_video(self, frame: bytes, note: str = None):
        room = max_write(MTU)
        for i in range(0, len(frame), room):
            self.write(frame[i:i + room], note if i == 0 else None)

    def framed_audio(self, frames: bytes, note: str = None):
        self.write(bytes([AUDIO_LC3]) + frames, note)


def both_open(r: Recorder):
    r.control(video_start(), "video start_streaming (H.264): buffer_update")
    r.control(audio_start(), "audio start_streaming (LC3, 40-byte frames): buffer_update")
    r.control(change_data_type(FRAMED), "change_data_type(framed), once: every Data write now starts with a type byte")


def base_exchange() -> dict:
    r = Recorder("framed video and audio", "Both streams open, framed once, then interleaved typed writes: video split "
                 "anywhere at up to max write - 1 bytes of stream data, audio in whole frames. buffer_update's "
                 "received totals and the buffers exclude the type bytes.")
    both_open(r)
    r.framed_video(video_frame(1, 150, keyframe=True), "video frame 1 (158 bytes): two writes, 96 + 62 bytes of "
                   "stream data")
    r.framed_audio(lc3_frames("a", 2), "audio: 2 frames (80 bytes)")
    r.framed_video(video_frame(2, 60), "video frame 2 (68 bytes)")
    r.framed_audio(lc3_frames("b", 2), "audio: 2 more frames")
    r.control(request_buffer(MG_COMMAND_STREAM_AUDIO), "audio request_buffer_update: 160 received, 160 held")
    r.control(request_buffer(MG_COMMAND_STREAM_VIDEO), "video request_buffer_update: 226 received, both frames shown")
    return r.ex


def bad_writes_exchange() -> dict:
    r = Recorder("framed writes naming no open stream", "A write whose type byte names no open stream, or no "
                 "mg_data_type_t at all, is discarded and counted; the next valid typed write is taken (no discard "
                 "until the next change_data_type).")
    both_open(r)
    r.write(bytes([VIDEO_VP8]) + det("not this stream", 40), "a write typed VP8 while the video stream is H.264: "
            "discarded, counted")
    r.framed_audio(lc3_frames("c", 1), "the next audio write: taken")
    r.write(bytes([UNKNOWN_TYPE]) + det("unknown type", 40), "a write typed 0xee (no data type): discarded, counted")
    r.framed_video(video_frame(1, 40, keyframe=True), "the next video write: taken")
    r.control(request_buffer(MG_COMMAND_STREAM_AUDIO), "audio: 40 received (the discarded writes count nowhere)")
    r.control(request_buffer(MG_COMMAND_STREAM_VIDEO), "video: 48 received")
    return r.ex


def truncated_exchange() -> dict:
    r = Recorder("framed writes without whole stream data", "A write with no stream data (the type byte alone, or an "
                 "empty write) is discarded and counted and changes nothing. An audio write that isn't whole frames is "
                 "dropped whole; its bytes still count in audio's received total.")
    both_open(r)
    r.write(bytes([AUDIO_LC3]), "the audio type byte alone: discarded, counted, audio unchanged")
    r.write(b"", "an empty write: discarded, counted")
    r.framed_audio(lc3_frames("d", 1) + det("half a frame", 20), "audio: 60 bytes, a frame and a half: dropped whole, "
                   "60 counted in the total, nothing held")
    r.framed_audio(lc3_frames("e", 1), "the next whole frame: taken")
    r.control(request_buffer(MG_COMMAND_STREAM_AUDIO), "audio: 100 received, 40 held")
    return r.ex


def mode_changes_exchange() -> dict:
    r = Recorder("entering and leaving framed", "A repeated change_data_type(framed) is a no-op. When one stream ends, "
                 "framed stays until the client's change_data_type to the remaining type, then plain Data follows; "
                 "framed ends when both streams end, and at disconnect.")
    both_open(r)
    r.control(change_data_type(FRAMED), "change_data_type(framed) again: a no-op, still framed")
    r.framed_audio(lc3_frames("f", 1), "audio: taken")
    r.control(stop(MG_COMMAND_STREAM_VIDEO), "the client ends video (never answered): still framed")
    r.framed_video(video_frame(1, 20, keyframe=True), "a write still typed for video: discarded, counted")
    r.framed_audio(lc3_frames("g", 1), "audio: taken")
    r.control(change_data_type(AUDIO_LC3), "change_data_type(LC3): plain from here")
    r.write(lc3_frames("h", 1), "plain audio, no type byte: taken")
    r.control(video_start(), "video starts again: plain Data stays with audio")
    r.write(lc3_frames("i", 1), "plain audio: taken")
    r.control(change_data_type(FRAMED), "change_data_type(framed): framed again")
    r.event("device_ends_audio", "the device ends audio (its stop_streaming): still framed")
    r.framed_audio(lc3_frames("j", 1), "a write typed for audio: discarded, counted")
    r.framed_video(video_frame(2, 30, keyframe=True), "the next video write: taken (no discard until the next "
                   "change_data_type)")
    r.control(change_data_type(VIDEO_H264), "change_data_type(H.264): plain")
    r.plain_video(video_frame(3, 30), "plain video: taken")
    r.control(audio_start(), "audio starts again")
    r.control(change_data_type(FRAMED), "framed")
    r.control(stop(MG_COMMAND_STREAM_VIDEO), "the client ends video")
    r.control(stop(MG_COMMAND_STREAM_AUDIO), "the client ends audio: both ended, framed ends")
    r.control(audio_start(), "audio starts")
    r.write(lc3_frames("k", 1), "plain audio, no type byte, no change_data_type: taken")
    r.control(video_start(), "video starts")
    r.control(change_data_type(FRAMED), "framed")
    r.event("disconnect", "the link drops: framed ends with the connection")
    r.control(audio_start(), "a new connection: audio starts")
    r.write(lc3_frames("l", 1), "plain audio: taken")
    return r.ex


def flush_anchor_exchange() -> dict:
    r = Recorder("flush and video_anchor in order with framed Data", "flush and video_anchor apply in order with the "
                 "Data around them: audio written before a flush is discarded, after it plays; an anchor binds the "
                 "next audio write's first byte (a video write between doesn't take it), except that one arriving "
                 "while its reply's first audio (the first Data after a gap) is still unplayed names that first "
                 "sample; a flush drops a pending anchor.")
    both_open(r)
    r.framed_audio(lc3_frames("m", 2), "audio: 80 bytes held")
    r.control(flush(), "flush: the buffer emptied at once, buffer_update (the total keeps running)")
    r.control(video_anchor(7), "video_anchor(7) with no audio held: pending")
    r.framed_video(video_frame(1, 30, keyframe=True), "a video write: not the anchor's")
    r.framed_audio(lc3_frames("n", 1), "the next audio write (a reply's first, after the gap): its first byte "
                   "(audio offset 80) goes with picture 7")
    r.control(video_anchor(9), "video_anchor(9) while that first audio is still unplayed: names it (offset 80)")
    r.framed_audio(lc3_frames("o", 1), "more audio: held behind it, no anchor change")
    r.control(flush(), "flush: the buffer emptied")
    r.control(video_anchor(11), "video_anchor(11) with no audio held: pending")
    r.control(flush(), "flush: drops the pending anchor")
    r.framed_audio(lc3_frames("p", 1), "audio: plays at once, no new anchor")
    return r.ex


def older_client_exchange() -> dict:
    r = Recorder("plain change_data_type on a device that lists framed", "A client that doesn't use framed switches "
                 "with change_data_type before each run of writes, with no type bytes, as before.")
    r.control(video_start(), "video start_streaming")
    r.control(audio_start(), "audio start_streaming")
    r.control(change_data_type(VIDEO_H264), "change_data_type(H.264)")
    r.plain_video(video_frame(1, 150, keyframe=True), "video frame 1 (158 bytes), plain: 97 + 61")
    r.control(change_data_type(AUDIO_LC3), "change_data_type(LC3)")
    r.write(lc3_frames("q", 2), "audio: 2 frames")
    r.control(change_data_type(VIDEO_H264), "change_data_type(H.264)")
    r.plain_video(video_frame(2, 40), "video frame 2")
    r.control(request_buffer(MG_COMMAND_STREAM_AUDIO), "audio: 80 received")
    r.control(request_buffer(MG_COMMAND_STREAM_VIDEO), "video: 206 received, both shown")
    return r.ex


def past_credit_exchange() -> dict:
    r = Recorder("audio past the credit granted", "Data past the credit the device granted (its latest "
                 "buffer_update's received total plus available bytes) is dropped and counted in the received "
                 "total, never buffered, even where the buffer has room. Plain Data, audio alone.")
    r.control(audio_start(), "audio start_streaming: buffer_update, 4096 available (the credit: 4096 bytes)")
    for i in range(AUDIO_BUFFER // (2 * LC3_FRAME)):
        r.write(lc3_frames(f"credit {i}", 2), "audio, 2 frames a write, up to 4080 bytes" if i == 0 else None)
    r.event("audio_plays", "the device plays out what it holds: its buffer is empty, no buffer_update yet")
    r.write(lc3_frames("past", 2), "2 more frames (to 4160): past the 4096-byte credit, dropped whole and counted, "
            "though the buffer has room")
    r.control(request_buffer(MG_COMMAND_STREAM_AUDIO), "request_buffer_update: 4096 available, 4160 received (the "
              "credit now runs to 8256)")
    r.write(lc3_frames("after", 2), "2 frames: taken")
    return r.ex


EXCHANGES = (base_exchange, bad_writes_exchange, truncated_exchange, mode_changes_exchange, flush_anchor_exchange,
             older_client_exchange, past_credit_exchange)


def build() -> dict:
    return {
        "description": "Framed Data (mgcommands.h, mg_data_type_framed; mg_stream_video_capability_flag_framed). Each "
                       "step is a Control write (control), a Data write (data) or an event on the device "
                       "(device_ends_audio, device_ends_video: its own stop_streaming; audio_plays: it plays out all the"
                       " audio it holds; disconnect), in arrival order, with the Control notifications it causes and the"
                       " device's state after it: framed, audio [held bytes, received total], video [bytes of an "
                       "incomplete frame, received total, frames shown], discarded (Data writes that went to no stream),"
                       " dropped (audio writes dropped whole) and anchor [frame, audio byte offset] of the last "
                       "video_anchor taken. Every exchange starts on a new connection to the device below. Video access "
                       "units are in Annex B form (start codes; SPS, PPS and IDR NALs on keyframes) with stand-in NAL "
                       "bodies: structurally valid, not decodable, so replay with the decoder stubbed (a frame counts as"
                       " shown when its access unit is complete). Generated by mg_framed_ref.py.",
        "version": 1,
        "device": {"capability_flags": FLAG_FRAMED, "audio_codec": AUDIO_LC3, "lc3_frame_bytes": LC3_FRAME,
                   "audio_buffer": AUDIO_BUFFER, "video_codec": VIDEO_H264, "video_buffer": VIDEO_BUFFER,
                   "lists": ["stream_audio flush", "stream_audio video_sync"]},
        "exchanges": [f() for f in EXCHANGES],
    }


def render() -> str:
    return json.dumps(build(), indent=2, ensure_ascii=False) + "\n"


def replay(ex: dict, device: Device) -> list:
    """The steps a device gets wrong, as (index, what)."""
    bad = []
    for i, st in enumerate(ex["steps"]):
        if "control" in st:
            got = device.control(bytes.fromhex(st["control"]))
        elif "data" in st:
            data = bytes.fromhex(st["data"])
            if len(data) > max_write(ex["mtu"]):
                bad.append((i, "a write over max write"))
            got = device.data(data)
        else:
            got = device.event(st["event"])
        if [f.hex() for f in got] != st["notify"]:
            bad.append((i, "notify"))
        if device.state() != st["state"]:
            bad.append((i, "state"))
    return bad


# Devices that get framed wrong; each must fail some exchange.
class CountsTypeByte(Device):
    """Counts the type byte in credits and received totals."""

    def framed_target(self, write: bytes):
        target, payload = super().framed_target(write)
        return target, (write if target else payload)


class DiscardsUntilChange(Device):
    """Keeps plain mode's discard-until-the-next-change_data_type rule in framed mode."""

    def end(self, which: str, by_client: bool):
        super().end(which, by_client)
        if not by_client and self.framed:
            self.discarding = True

    def framed_target(self, write: bytes):
        return (None, b"") if self.discarding else super().framed_target(write)


class AnyTypeIsVideo(Device):
    """Takes any framed type that isn't audio's as video, as plain mode selects."""

    def framed_target(self, write: bytes):
        if len(write) >= 2 and self.video is not None and not (self.audio and write[0] == self.audio["codec"]):
            return "video", write[1:]
        return super().framed_target(write)


class ToggleOnRepeat(Device):
    """Leaves framed on a repeated change_data_type(framed)."""

    def on_change_data_type(self, t: int) -> list:
        if t == FRAMED and self.framed:
            self.framed = False
            return []
        return super().on_change_data_type(t)


class StrictLateAnchor(Device):
    """Binds an anchor that arrives while its reply's first audio is unplayed to the next audio write instead."""

    def on_audio(self, sub: int, p: bytes) -> list:
        if sub == VIDEO_ANCHOR and self.audio is not None and self.video is not None:
            self.anchor_pending = struct.unpack("<I", p[:4])[0]
            return []
        return super().on_audio(sub, p)


class BuffersPastCredit(Device):
    """Takes audio past the credit it granted while the buffer has room."""

    def past_credit(self, a: dict) -> bool:
        return False


MUTANTS = (CountsTypeByte, DiscardsUntilChange, AnyTypeIsVideo, ToggleOnRepeat, StrictLateAnchor, BuffersPastCredit)


def check() -> list:
    data = json.loads(VECTORS.read_text(encoding="utf-8"))
    bad = []
    for ex in data["exchanges"]:
        for i, what in replay(ex, Device(data["device"]["capability_flags"])):
            bad.append(f"{ex['name']} step {i}: {what}")
    for mutant in MUTANTS:
        if not any(replay(ex, mutant()) for ex in data["exchanges"]):
            bad.append(f"{mutant.__name__} passes every exchange")
    if VECTORS.read_text(encoding="utf-8") != render():
        bad.append("file differs from the generator's output")
    return bad


def main() -> int:
    if sys.argv[1:] == ["--write"]:
        VECTORS.write_text(render(), encoding="utf-8")
        print(f"wrote {VECTORS}")
        return 0
    if sys.argv[1:]:
        print(__doc__)
        return 2
    bad = check()
    for b in bad:
        print(f"FAIL {b}")
    if not bad:
        data = json.loads(VECTORS.read_text(encoding="utf-8"))
        print(f"OK {VECTORS.name}: {len(data['exchanges'])} exchanges, "
              f"{sum(len(e['steps']) for e in data['exchanges'])} steps, {len(MUTANTS)} wrong devices caught")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
