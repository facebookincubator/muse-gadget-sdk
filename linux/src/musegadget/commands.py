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

"""Commands the device's owner adds without changing this package.

Each ``<name>.json`` in the commands directory declares one command, named
after the file. ``pump.set.json``::

    {
      "description": "Switch the garden pump on or off.",
      "exec": ["/usr/local/bin/pump"],
      "required": {"on": {"type": "boolean", "description": "true for on."}},
      "timeout_ms": 10000
    }

``description``, ``required`` and ``optional`` are registered with the Muse
as they are, like the built-in ``COMMAND_SPECS``. When the Muse invokes the
command, ``exec`` runs as the run-as account, the way ``system.run`` does,
with no shell. Its declared parameters arrive as one JSON object on stdin. A
JSON object on stdout becomes the result; any other output is returned as
``{"output": ...}``. A non-zero exit, or running past ``timeout_ms``
(default 30 seconds), is an error.

The directory is read once, when the service starts. A file with a problem is
logged and skipped; it never stops the service.
"""

from __future__ import annotations

import json
import logging
import os
import re
import stat
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger(__name__)

DEFAULT_TIMEOUT_MS = 30_000
MAX_TIMEOUT_MS = 600_000
# The Muse waits this much longer than the command may run, so a timeout
# reaches it as an error rather than as a lost invoke.
TIMEOUT_GRACE_MS = 5_000
MAX_FILE_BYTES = 64 * 1024
# Names look like the built-ins ("sensors.read"). Prefixes the SDK and the
# Muse use themselves are reserved.
_NAME_RE = re.compile(r"[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+")
MAX_NAME_LEN = 64
RESERVED_PREFIXES = ("system.", "file.", "device.", "link.")
PARAM_TYPES = {
    "string": (str,),
    "integer": (int,),
    "number": (int, float),
    "boolean": (bool,),
    "object": (dict,),
    "array": (list,),
}
_KEYS = {"description", "exec", "required", "optional", "timeout_ms"}


class InvalidCommand(Exception):
    pass


@dataclass(frozen=True)
class DropInCommand:
    name: str
    description: str
    argv: tuple
    required: dict
    optional: dict
    timeout_ms: int

    @property
    def spec(self) -> dict:
        """The entry for ``commands_v2`` in ``link.register``."""
        return {
            "description": self.description,
            "required": self.required,
            "optional": self.optional,
            "timeout_ms": self.timeout_ms + TIMEOUT_GRACE_MS,
        }

    def check_params(self, params: dict) -> dict:
        """The declared parameters from ``params``, or InvalidCommand."""
        for name in self.required:
            if name not in params:
                raise InvalidCommand(f"{name} is required")
        checked = {}
        for name, spec in {**self.required, **self.optional}.items():
            if name not in params:
                continue
            value = params[name]
            types = PARAM_TYPES[spec["type"]]
            # bool is an int in Python, but not in JSON.
            if not isinstance(value, types) or (isinstance(value, bool) and bool not in types):
                article = "an" if spec["type"][0] in "aeiou" else "a"
                raise InvalidCommand(f"{name} must be {article} {spec['type']}")
            checked[name] = value
        return checked


def parse(name: str, data: dict) -> DropInCommand:
    """Validate one command file's contents. Raises InvalidCommand."""
    if len(name) > MAX_NAME_LEN or not _NAME_RE.fullmatch(name):
        raise InvalidCommand(
            "the file name must be the command name, like sensors.read.json "
            "(lowercase words joined by dots)")
    if name.startswith(RESERVED_PREFIXES):
        raise InvalidCommand(f"names starting with {', '.join(RESERVED_PREFIXES)} are reserved")
    if not isinstance(data, dict):
        raise InvalidCommand("the file must hold a JSON object")
    unknown = set(data) - _KEYS
    if unknown:
        raise InvalidCommand(f"unknown keys: {', '.join(sorted(unknown))}")
    description = data.get("description")
    if not isinstance(description, str) or not description.strip():
        raise InvalidCommand("description is required")
    argv = data.get("exec")
    if (not isinstance(argv, list) or not argv
            or not all(isinstance(arg, str) and arg for arg in argv)):
        raise InvalidCommand("exec must be a list of strings: the program and its arguments")
    if not os.path.isabs(argv[0]):
        raise InvalidCommand("exec must start with the program's absolute path")
    required = _params(data.get("required", {}), "required")
    optional = _params(data.get("optional", {}), "optional")
    both = set(required) & set(optional)
    if both:
        raise InvalidCommand(f"both required and optional: {', '.join(sorted(both))}")
    timeout_ms = data.get("timeout_ms", DEFAULT_TIMEOUT_MS)
    if (not isinstance(timeout_ms, int) or isinstance(timeout_ms, bool)
            or not 0 < timeout_ms <= MAX_TIMEOUT_MS):
        raise InvalidCommand(f"timeout_ms must be an integer from 1 to {MAX_TIMEOUT_MS}")
    return DropInCommand(name, description, tuple(argv), required, optional, timeout_ms)


def _params(value, key: str) -> dict:
    if not isinstance(value, dict):
        raise InvalidCommand(f"{key} must be an object of parameters")
    for name, spec in value.items():
        if (not isinstance(spec, dict) or not isinstance(spec.get("type"), str)
                or spec["type"] not in PARAM_TYPES
                or not isinstance(spec.get("description"), str)
                or set(spec) != {"type", "description"}):
            raise InvalidCommand(
                f"{key}.{name} must be {{\"type\": ..., \"description\": ...}} with type one of "
                f"{', '.join(PARAM_TYPES)}")
    return value


def load(directory: Path) -> list:
    """The valid commands in ``directory``, sorted by name.

    A missing directory has none. Problems with a file are logged, and the
    file is skipped.
    """
    try:
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except FileNotFoundError:
        return []
    except OSError as exc:
        log.warning("can't read commands from %s: %s", directory, type(exc).__name__)
        return []
    try:
        st = os.fstat(fd)
        if os.geteuid() == 0 and (st.st_uid != 0 or st.st_mode & 0o022):
            log.warning("skipping commands directory %s: must be owned by root "
                        "and writable only by root", directory)
            return []
        paths = [directory / name for name in sorted(os.listdir(fd)) if name.endswith(".json")]
        commands = []
        for path in paths:
            try:
                commands.append(_load_file(path, directory_fd=fd))
            except InvalidCommand as exc:
                log.warning("skipping %s: %s", path, exc)
            except Exception as exc:
                # Isolate only this definition. Exception text may contain its secrets;
                # BaseException (including cancellation/KeyboardInterrupt) propagates.
                log.warning("skipping %s: %s while loading definition", path, type(exc).__name__)
        if commands:
            log.info("commands from %s: %s", directory, ", ".join(c.name for c in commands))
        return commands
    except OSError as exc:
        log.warning("can't read commands from %s: %s", directory, type(exc).__name__)
        return []
    finally:
        os.close(fd)


def _load_file(path: Path, directory_fd: int | None = None) -> DropInCommand:
    try:
        # No symlinks, and no blocking on a FIFO. The checks below are on the
        # open file, so it can't be swapped between checking and reading.
        fd = os.open(path if directory_fd is None else path.name,
                     os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory_fd)
        with os.fdopen(fd, "rb") as f:
            st = os.fstat(f.fileno())
            if not stat.S_ISREG(st.st_mode):
                raise InvalidCommand("not a regular file")
            # Like sudoers.d: only root decides what the service offers the Muse.
            if os.geteuid() == 0 and (st.st_uid != 0 or st.st_mode & 0o022):
                raise InvalidCommand("must be owned by root and writable only by root")
            raw = f.read(MAX_FILE_BYTES + 1)
        if len(raw) > MAX_FILE_BYTES:
            raise InvalidCommand(f"larger than {MAX_FILE_BYTES} bytes")
        data = json.loads(raw.decode("utf-8"))
    except OSError as exc:
        raise InvalidCommand(f"can't read it: {type(exc).__name__}") from exc
    except (ValueError, RecursionError) as exc:
        raise InvalidCommand(f"not valid JSON: {type(exc).__name__}") from exc
    return parse(path.name[:-len(".json")], data)
