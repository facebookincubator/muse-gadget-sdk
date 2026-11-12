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

"""Outputs for Muse's video: H.264 files, pipes and players.

A video call delivers H.264 access units (Annex-B, each starting with a start
code). ``VideoStream`` writes them from a worker thread, so a slow file, pipe
or player never blocks the call. When it falls behind it drops whole units and
then skips ahead to the next keyframe, so the decoder never sees a broken
reference chain; ``needs_keyframe`` tells the call to ask Muse for one.
``VideoPlayer`` feeds the stream to mpv, ffplay or any command that reads raw
H.264 on standard input.
"""

from __future__ import annotations

import logging
import os
import queue
import shutil
import subprocess
import threading
import time

log = logging.getLogger(__name__)

VIDEO_FPS = 25  # Muse paces call video at one frame per 40 ms.


def is_keyframe(unit: bytes) -> bool:
    return any(nal and nal[0] & 0x1F == 5 for nal in unit.split(b"\x00\x00\x00\x01"))


class VideoStream:
    """Writes H.264 access units to a binary file descriptor from a worker thread.

    ``write_unit`` never blocks. After a write error (a closed player, a full
    disk) ``error`` keeps the exception and ``write_unit`` raises it.
    """

    def __init__(self, fd: int, *, max_pending: int = 8, close_fd: bool = False) -> None:
        if max_pending < 1:
            raise ValueError("max_pending must be positive")
        self._fd, self._close_fd = fd, close_fd
        self._pending = queue.Queue(maxsize=max_pending)
        self.written = 0
        self.dropped = 0
        self.error = None
        self.needs_keyframe = True
        self._closed = False
        self._fd_lock = threading.Lock()
        self._thread = threading.Thread(target=self._run, name="musegadget-video", daemon=True)
        self._thread.start()

    def write_unit(self, unit: bytes) -> None:
        if self.error is not None:
            raise self.error
        if self._closed:
            return
        if self.needs_keyframe:
            if not is_keyframe(unit):
                self.dropped += 1
                return
            self.needs_keyframe = False
        try:
            self._pending.put_nowait(unit)
        except queue.Full:
            # Units already queued still decode; this one and its successors
            # would not, so wait for the next keyframe.
            self.dropped += 1
            self.needs_keyframe = True

    def _write(self, data: bytes) -> None:
        view = memoryview(data)
        while view:
            view = view[os.write(self._fd, view):]

    def _run(self) -> None:
        try:
            while True:
                unit = self._pending.get()
                if unit is None:
                    return
                if self.error is not None:
                    self.dropped += 1
                    continue
                try:
                    self._write(unit)
                    self.written += 1
                except OSError as exc:
                    self.error = exc
                    self.dropped += 1
                    log.warning("call video output stopped (%s)", type(exc).__name__)
        finally:
            if self._closed:
                self._close_own_fd()

    def _close_own_fd(self) -> None:
        with self._fd_lock:
            if not self._close_fd or self._fd < 0:
                return
            fd, self._fd = self._fd, -1
        try:
            os.close(fd)
        except OSError:
            log.warning("could not close call video output")

    def close(self, timeout: float = 2.0) -> bool:
        """Finish queued units within ``timeout``; False if the worker is still writing."""
        deadline = time.monotonic() + timeout
        if not self._closed:
            self._closed = True
            while True:
                try:
                    self._pending.put(None, timeout=max(0.0, deadline - time.monotonic()))
                    break
                except queue.Full:
                    try:
                        self._pending.get_nowait()
                        self.dropped += 1
                    except queue.Empty:
                        pass
        self._thread.join(max(0.0, deadline - time.monotonic()))
        finished = not self._thread.is_alive()
        if finished:
            self._close_own_fd()
        return finished


def player_command(name: str = "auto") -> list:
    """The command line that shows raw H.264 from standard input.

    ffplay probes as little as possible so the first frame shows at once. It
    must not use "-fflags nobuffer": that discards the packets read while
    probing, which hold the stream's parameter sets and its only keyframe.
    """
    players = {
        "mpv": ["mpv", "--really-quiet", "--no-cache", "--untimed", "--profile=low-latency",
                "--demuxer-lavf-format=h264", f"--container-fps-override={VIDEO_FPS}",
                "--title=Muse", "-"],
        "ffplay": ["ffplay", "-loglevel", "error", "-probesize", "32", "-analyzeduration", "0",
                   "-framedrop", "-window_title", "Muse", "-f", "h264", "-framerate", str(VIDEO_FPS),
                   "-i", "-"],
    }
    for candidate in (("mpv", "ffplay") if name == "auto" else (name,)):
        if candidate in players and shutil.which(candidate):
            return players[candidate]
    if name == "auto":
        raise FileNotFoundError("no video player found; install mpv or ffplay, or pass a command")
    raise FileNotFoundError(f"video player {name!r} is not installed")


class VideoPlayer:
    """Runs a player that reads raw H.264 on standard input and streams units to it."""

    def __init__(self, command: list) -> None:
        self._process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            fd = os.dup(self._process.stdin.fileno())
            try:
                self._stream = VideoStream(fd, close_fd=True)
            except BaseException:
                os.close(fd)
                raise
        except BaseException:
            self._process.kill()
            self._process.wait()
            raise
        finally:
            self._process.stdin.close()

    @property
    def dropped(self) -> int:
        return self._stream.dropped

    @property
    def needs_keyframe(self) -> bool:
        return self._stream.needs_keyframe

    def write_unit(self, unit: bytes) -> None:
        self._stream.write_unit(unit)

    def close(self) -> None:
        # A paused or frozen player stops reading: stop it rather than wait,
        # which also ends the worker's blocked write with a broken pipe.
        if not self._stream.close(timeout=1.0):
            self._process.terminate()
            self._stream.close(timeout=2.0)
        try:
            self._process.wait(1)
        except subprocess.TimeoutExpired:
            self._process.terminate()
            try:
                self._process.wait(2)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait()
        if not self._stream.close(timeout=1.0):
            log.warning("call video output did not stop")


class VideoOutputs:
    """Sends every unit to several outputs.

    An output that raises stops receiving units (it is closed with the rest at
    the end of the call); the others keep going.
    ``write_unit`` raises only when no output is left.
    """

    def __init__(self, outputs: list) -> None:
        self.outputs = list(outputs)
        self.failed = []

    @property
    def dropped(self) -> int:
        return sum(getattr(output, "dropped", 0) for output in self.outputs + self.failed)

    @property
    def needs_keyframe(self) -> bool:
        return any(getattr(output, "needs_keyframe", False) for output in self.outputs)

    def write_unit(self, unit: bytes) -> None:
        for output in list(self.outputs):
            try:
                output.write_unit(unit)
            except Exception as exc:
                log.warning("a call video output stopped (%s)", type(exc).__name__)
                self.outputs.remove(output)
                self.failed.append(output)
        if not self.outputs:
            raise ConnectionError("no call video output is left")

    def close(self) -> None:
        for output in self.outputs + self.failed:
            try:
                output.close()
            except Exception:
                log.warning("could not close a call video output")
