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

"""Tests for the PID 1 orphan reaper (musegadget.reaper)."""

from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import time

import pytest

from musegadget import reaper

pytestmark = pytest.mark.skipif(
    not hasattr(os, "waitpid"), reason="waitpid is POSIX-only")


@pytest.fixture()
def running_reaper():
    drain = reaper.ChildReaper(interval=0.01)
    drain.start()
    try:
        yield drain
    finally:
        drain.stop()


def test_reaping_popen_keeps_exit_codes_while_reaper_drains(running_reaper):
    for _ in range(10):
        proc = reaper.ReapingPopen(
            [sys.executable, "-c", "import sys; sys.exit(3)"])
        assert proc.wait() == 3
    proc = reaper.ReapingPopen([sys.executable, "-c", "pass"])
    assert proc.wait() == 0


def test_reaping_popen_reads_status_the_reaper_already_drained(running_reaper):
    proc = reaper.ReapingPopen(
        [sys.executable, "-c", "import sys; sys.exit(7)"])
    # Give the reaper time to reap the child into the stash, so wait()
    # can only learn the exit code from the stash.
    deadline = time.monotonic() + 5
    while proc.pid not in reaper._statuses and time.monotonic() < deadline:
        time.sleep(0.01)
    assert proc.wait() == 7


def test_reaping_popen_poll_keeps_exit_codes_while_reaper_drains(running_reaper):
    # The executor waits for its shells with poll(), which bypasses _try_wait.
    for _ in range(20):
        proc = reaper.ReapingPopen(["sh", "-c", "exit 3"])
        while proc.poll() is None:
            time.sleep(0.02)
        assert proc.returncode == 3


def test_reaping_popen_ignores_a_stale_status_for_a_reused_pid(monkeypatch):
    # Pretend an earlier process with the new child's pid left a status.
    spawn = subprocess.Popen._execute_child

    def spawn_with_stale_status(self, *args, **kwargs):
        spawn(self, *args, **kwargs)
        reaper._statuses[self.pid] = 9 << 8

    monkeypatch.setattr(subprocess.Popen, "_execute_child", spawn_with_stale_status)
    proc = reaper.ReapingPopen(["sh", "-c", "sleep 0.2; exit 3"])
    assert proc.poll() is None
    assert proc.wait() == 3


def test_run_helper_keeps_exit_code_and_output(running_reaper):
    result = reaper.run(
        [sys.executable, "-c", "import sys; sys.stdout.write('x'); sys.exit(5)"],
        capture_output=True, text=True, timeout=30)
    assert result.returncode == 5
    assert result.stdout == "x"


def test_run_helper_delivers_input(running_reaper):
    result = reaper.run(["cat"], input=b"request-body", capture_output=True, timeout=30)
    assert result.returncode == 0
    assert result.stdout == b"request-body"


def test_run_helper_raises_on_timeout(running_reaper):
    with pytest.raises(subprocess.TimeoutExpired):
        reaper.run([sys.executable, "-c", "import time; time.sleep(30)"],
                   capture_output=True, timeout=0.2)


def _set_child_subreaper(enabled: bool) -> bool:
    """Ask the kernel to reparent orphans to this process, like PID 1 gets."""
    PR_SET_CHILD_SUBREAPER = 36
    try:
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        return libc.prctl(PR_SET_CHILD_SUBREAPER, 1 if enabled else 0, 0, 0, 0) == 0
    except (OSError, AttributeError):
        return False


def _proc_state(pid: int) -> str | None:
    try:
        with open(f"/proc/{pid}/stat", "rb") as handle:
            return handle.read().rsplit(b")", 1)[1].split()[0].decode()
    except OSError:
        return None


def test_adopted_orphan_is_reaped_instead_of_staying_zombie():
    if sys.platform != "linux" or not _set_child_subreaper(True):
        pytest.skip("needs Linux child-subreaper support")
    read_fd, write_fd = os.pipe()
    try:
        child = os.fork()
        if child == 0:  # Intermediate parent: fork the orphan, then exit.
            grandchild = os.fork()
            if grandchild == 0:
                time.sleep(0.3)
                os._exit(0)
            os.write(write_fd, str(grandchild).encode())
            os._exit(0)
        os.close(write_fd)
        grandchild_pid = int(os.read(read_fd, 32).decode())
        os.waitpid(child, 0)
        # The grandchild exits and, with nowhere else to go, is adopted
        # here — a zombie until somebody reaps it.
        deadline = time.monotonic() + 5
        while _proc_state(grandchild_pid) != "Z" and time.monotonic() < deadline:
            time.sleep(0.02)
        assert _proc_state(grandchild_pid) == "Z"
        drain = reaper.ChildReaper(interval=0.01)
        drain.start()
        try:
            deadline = time.monotonic() + 5
            while _proc_state(grandchild_pid) is not None and time.monotonic() < deadline:
                time.sleep(0.02)
            assert _proc_state(grandchild_pid) is None
            assert reaper._take_status(grandchild_pid) == 0
        finally:
            drain.stop()
    finally:
        os.close(read_fd)
        _set_child_subreaper(False)


def test_start_child_reaper_only_when_pid1(monkeypatch):
    monkeypatch.setattr(os, "getpid", lambda: 4242)
    assert reaper.start_child_reaper() is False
    monkeypatch.setattr(os, "getpid", lambda: 1)
    try:
        assert reaper.start_child_reaper() is True
    finally:
        if reaper._reaper is not None:
            reaper._reaper.stop()
            reaper._reaper = None
