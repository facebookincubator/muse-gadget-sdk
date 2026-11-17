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

"""Reaping adopted orphans when the service runs as PID 1.

In the container deployment the service's entrypoint ends in
``exec musegadget run``, so the daemon *is* PID 1. When a process's
parent exits first, the kernel reparents the orphan to PID 1, and only
PID 1 can reap it. The daemon spawns plenty of helpers (shells, browser
subprocesses) whose grandchildren can be orphaned this way; with nobody
reaping them they stay zombies forever — thousands over a long uptime.

A background thread here drains every exited child with
``waitpid(-1, WNOHANG)``. That races with :class:`subprocess.Popen`,
which expects to reap its own children and read their exit status, so
drained statuses are stashed by pid and :class:`ReapingPopen` consults
the stash before waiting itself. Exit codes therefore stay exact for
children the service spawned on purpose, while adopted orphans — whose
status nobody will ever ask for — are reaped instead of piling up.

The reaper only starts when this process is PID 1; anywhere else the
children's own init does this job and the stash is never consulted
because it is always empty.
"""

from __future__ import annotations

import logging
import os
import subprocess
import threading

log = logging.getLogger(__name__)

_POLL_S = 0.2
# Statuses of adopted orphans are never claimed, so the stash would grow
# by one small entry per orphan without a bound. Past this many entries
# the oldest are dropped; losing one only means a Popen whose child was
# reaped long before it waited falls back to Popen's own error path.
_MAX_STASHED = 4096

_lock = threading.Lock()
_statuses: dict[int, int] = {}


def _stash_locked(pid: int, status: int) -> None:
    if len(_statuses) >= _MAX_STASHED:
        _statuses.clear()
    _statuses[pid] = status


def _take_status(pid: int) -> int | None:
    with _lock:
        return _statuses.pop(pid, None)


def drain_children() -> int:
    """Reap every exited child, stashing exit statuses. Returns the count."""
    reaped = 0
    while True:
        # Reap and stash under the lock, so ReapingPopen never sees a child
        # gone from waitpid but not yet in the stash.
        with _lock:
            try:
                pid, status = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                return reaped
            except InterruptedError:
                continue
            if pid <= 0:
                return reaped
            _stash_locked(pid, status)
        reaped += 1


class ChildReaper:
    """Polls for exited children and reaps them into the status stash."""

    def __init__(self, interval: float = _POLL_S) -> None:
        self._interval = interval
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(
            target=self._run, name="musegadget-child-reaper", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2)
            self._thread = None

    def _run(self) -> None:
        while not self._stop.is_set():
            if drain_children():
                log.debug("reaped child processes")
            self._stop.wait(self._interval)


class ReapingPopen(subprocess.Popen):
    """A Popen whose exit status may already have been reaped and stashed."""

    def _try_wait(self, wait_flags):
        # Same contract as Popen._try_wait: a (pid, raw status) pair,
        # where pid is 0 while the child is still running. The caller
        # turns the status into returncode via _handle_exitstatus.
        status = _take_status(self.pid)
        if status is not None:
            return (self.pid, status)
        try:
            return os.waitpid(self.pid, wait_flags)
        except ChildProcessError:
            # The reaper got there first; it stashes under the lock.
            status = _take_status(self.pid)
            return (self.pid, 0 if status is None else status)

    def poll(self):
        # Popen.poll() calls waitpid itself rather than _try_wait, so it
        # needs the stash too; the executor polls its shells with it.
        with _lock:
            if self.returncode is None:
                status = _statuses.pop(self.pid, None)
                if status is not None:
                    self._handle_exitstatus(status)
            return super().poll()


def run(*popenargs, timeout=None, check=False, input=None,
        capture_output=False, **kwargs) -> subprocess.CompletedProcess:
    """Drop-in for subprocess.run that survives the reaper.

    subprocess.run builds a plain Popen, whose wait would miss a status
    the reaper already drained (and report exit code 0). Route through
    ReapingPopen instead, keeping subprocess.run's semantics for the
    arguments the service's callers use: timeout (kill, wait, and raise
    TimeoutExpired with the output captured so far), check, input and
    capture_output.
    """
    if capture_output:
        if kwargs.get("stdout") is not None or kwargs.get("stderr") is not None:
            raise ValueError("stdout and stderr arguments may not be used "
                             "with capture_output.")
        kwargs["stdout"] = subprocess.PIPE
        kwargs["stderr"] = subprocess.PIPE
    if input is not None:
        if kwargs.get("stdin") is not None:
            raise ValueError("stdin and input arguments may not both be used.")
        kwargs["stdin"] = subprocess.PIPE
    with ReapingPopen(*popenargs, **kwargs) as proc:
        try:
            stdout, stderr = proc.communicate(input=input, timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
            raise subprocess.TimeoutExpired(
                proc.args, timeout, output=stdout, stderr=stderr)
        except BaseException:
            proc.kill()
            proc.wait()
            raise
    result = subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr)
    if check and result.returncode:
        raise subprocess.CalledProcessError(
            result.returncode, result.args, output=stdout, stderr=stderr)
    return result


_reaper: ChildReaper | None = None


def start_child_reaper() -> bool:
    """Start the orphan reaper if this process is PID 1. Returns started."""
    global _reaper
    if os.getpid() != 1 or not hasattr(os, "waitpid"):
        return False
    if _reaper is None:
        _reaper = ChildReaper()
        _reaper.start()
        log.info("running as PID 1; reaping adopted orphan processes")
    return True
