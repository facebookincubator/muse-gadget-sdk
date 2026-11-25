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

"""Tell the Muse, once, that the previous run of the service crashed.

``watch_run`` wraps the service's whole run and keeps a small record in the
state directory. A run that ends in an exception, or that vanishes while the
machine stays up (killed, out of memory), leaves the record behind; the next
run turns it into one ``link.error_report`` message (protocols/ERROR_REPORT.md)
and the service sends it after it registers.

The report names the exception's type and where it was raised (file name, line
and function). It never carries the exception's message, a path, a log line or
anything else that could name the device, its network or its owner.
"""

from __future__ import annotations

import contextlib
import logging
import os
import time
import traceback
from pathlib import Path
from typing import Iterator

from musegadget import config

log = logging.getLogger(__name__)

RECORD_FILE = "error_report.json"
# "0" turns crash reports off: nothing is recorded and nothing is sent.
ENABLE_ENV = "MUSEGADGET_ERROR_REPORT"
SCHEMA_VERSION = 1
MAX_VERSION_CHARS = 31
MAX_CAUSE_CHARS = 47
MAX_DETAIL_CHARS = 95

_BOOT_ID = Path("/proc/sys/kernel/random/boot_id")
_started = time.monotonic()


def enabled() -> bool:
    return os.environ.get(ENABLE_ENV) != "0"


@contextlib.contextmanager
def watch_run(version: str, directory: Path | None = None) -> Iterator[None]:
    """Wrap the service's run: an exception that escapes it is a crash.

    Stopping on a signal, ``KeyboardInterrupt`` and ``SystemExit`` are clean
    exits.
    """
    if not enabled():
        yield
        return
    _begin_run(version, directory)
    crashed = False
    try:
        yield
    except Exception as exc:
        crashed = True
        _record_exception(exc, directory)
        raise
    finally:
        if not crashed:
            _end_run(directory)


def pending_message(directory: Path | None = None) -> dict | None:
    """The ``link.error_report`` message waiting to be sent, if any."""
    if not enabled():
        return None
    report = _load(directory).get("pending")
    if not isinstance(report, dict):
        return None
    return {"method": "link.error_report", "report": report}


def mark_sent(directory: Path | None = None) -> None:
    saved = _load(directory)
    if saved.pop("pending", None) is not None:
        _save(saved, directory)


def _begin_run(version: str, directory: Path | None) -> None:
    global _started
    _started = time.monotonic()
    saved = _load(directory)
    previous = saved.get("run")
    reason = _how_it_ended(previous)
    count = 0
    if reason is not None:
        count = _count(previous) + 1
        report: dict = {
            "schema_version": SCHEMA_VERSION,
            "kind": "crash",
            "reason": reason,
            "consecutive_count": count,
        }
        if isinstance(previous.get("version"), str) and previous["version"]:
            report["version"] = _printable(previous["version"], MAX_VERSION_CHARS)
        if reason == "unhandled_exception":
            uptime = previous.get("uptime_seconds")
            if type(uptime) is int and uptime >= 0:
                report["uptime_seconds"] = uptime
            report["fault"] = previous["fault"]
        saved["pending"] = report
        log.warning("the previous run ended in %s (%d in a row)", reason, count)
    saved["run"] = {
        "boot_id": _boot_id(),
        "pid": os.getpid(),
        "version": version,
        "count": count,
    }
    _save(saved, directory)


def _how_it_ended(run: object) -> str | None:
    """Why the run that left this record counts as a crash; None if it doesn't."""
    if not isinstance(run, dict):
        return None
    if isinstance(run.get("fault"), dict):
        return "unhandled_exception"
    # No exception was recorded, so the process was killed or the machine went
    # down. Only the first is a crash, and only the boot id tells them apart.
    boot_id = _boot_id()
    pid = run.get("pid")
    if (not boot_id or run.get("boot_id") != boot_id
            or type(pid) is not int or pid <= 0):
        return None
    if pid != os.getpid() and _is_running(pid):
        return None     # another service is still running from this state
    return "unclean_exit"


def _record_exception(exc: Exception, directory: Path | None) -> None:
    saved = _load(directory)
    run = saved.get("run")
    if not isinstance(run, dict):
        return
    fault = {"cause": _printable(type(exc).__name__, MAX_CAUSE_CHARS)}
    frames = traceback.extract_tb(exc.__traceback__)
    if frames:
        raised = frames[-1]
        fault["detail"] = _printable(
            f"{Path(raised.filename).name}:{raised.lineno} in {raised.name}",
            MAX_DETAIL_CHARS,
        )
    run["fault"] = fault
    run["uptime_seconds"] = max(0, int(time.monotonic() - _started))
    _save(saved, directory)


def _end_run(directory: Path | None) -> None:
    saved = _load(directory)
    if saved.pop("run", None) is None:
        return
    if saved:
        _save(saved, directory)
    else:
        try:
            config.delete_json(RECORD_FILE, directory)
        except OSError as exc:
            log.warning("could not clear the crash record: %s", type(exc).__name__)


def _count(run: dict) -> int:
    count = run.get("count")
    return count if type(count) is int and count >= 0 else 0


def _printable(text: str, limit: int) -> str:
    return "".join(c if " " <= c <= "~" else " " for c in text)[:limit]


def _boot_id() -> str:
    try:
        return _BOOT_ID.read_text(encoding="ascii").strip()
    except (OSError, ValueError):
        return ""


def _is_running(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except OSError:
        return True     # it exists but belongs to another account
    return True


def _load(directory: Path | None) -> dict:
    try:
        return config.load_json(RECORD_FILE, directory) or {}
    except OSError:
        return {}


def _save(saved: dict, directory: Path | None) -> None:
    try:
        config.save_json(RECORD_FILE, saved, directory)
    except OSError as exc:
        log.warning("could not save the crash record: %s", type(exc).__name__)
