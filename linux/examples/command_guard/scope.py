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

"""Layer 2: scope checking — is this call inside what the user asked for?

Content shields ask "is this command dangerous?". A scope check asks a
different question: "the user asked for X; the agent is about to do Y; is
Y more than X?" That is the question Interlock (github.com/NagaYu/interlock)
answers with a trained model. This adapter is the integration point:

- If a caller supplies a callable ``interlock_check(request, command,
  params) -> (action, reason)``, it is used.
- Otherwise a small deterministic heuristic (below) covers the clearest
  case: a destructive call whose target strictly contains, or is not, the
  path the request named. The heuristic is an MVP stand-in for the model,
  not a replacement for it; it is deliberately conservative and only
  speaks up on deletes, moves and overwrites.

The gadget protocol does not currently pass the user's request to the
executor, so ``request`` is usually None. In that case the result is
``not_evaluated`` — and the guard records exactly that in the ledger,
rather than silently treating the call as scope-checked.
"""

from __future__ import annotations

import os
import re
import shlex
from dataclasses import dataclass

NOT_EVALUATED = "not_evaluated"
EVALUATED = "evaluated"


@dataclass(frozen=True)
class ScopeResult:
    status: str          # NOT_EVALUATED or EVALUATED
    action: str | None   # allow / confirm / block, or None if not evaluated
    reason: str


def interlock_available() -> bool:
    try:
        import interlock  # noqa: F401
        return True
    except Exception:
        return False


_PATH_IN_TEXT = re.compile(r"(~)?/[^\s'\";,]+")


def _paths_in(text: str, home: str) -> list:
    found = []
    for match in _PATH_IN_TEXT.finditer(text or ""):
        raw = match.group(0).rstrip(").,")
        if raw.startswith("~"):
            raw = home.rstrip("/") + raw[1:]
        found.append(os.path.normpath(raw))
    return found


def _tokens(command: str) -> list:
    try:
        return shlex.split(command)
    except ValueError:
        return command.split()


def _action_targets(command_name: str, params: dict, home: str) -> tuple:
    """(is_destructive, [target paths]) for one call."""
    params = params or {}
    if command_name in ("file.read", "file.write"):
        path = params.get("path")
        targets = [os.path.normpath(str(path))] if path else []
        # A write is not treated as destructive for scope purposes: saving
        # a file the user asked for is the normal case, and Layer 1 has
        # already fenced off sensitive and system paths.
        return (False, targets)
    if command_name == "system.run":
        tokens = _tokens(str(params.get("command") or ""))
        words = [os.path.basename(t) for t in tokens]
        destructive = any(w in ("rm", "mv", "rmdir") for w in words)
        targets = []
        for token in tokens:
            candidate = token.strip("'\"")
            if candidate.startswith(("~", "/")):
                if candidate == "~":
                    candidate = home
                elif candidate.startswith("~/"):
                    candidate = home.rstrip("/") + candidate[1:]
                targets.append(os.path.normpath(candidate))
        return (destructive, targets)
    return (False, [])


def _is_ancestor(ancestor: str, path: str) -> bool:
    return path.startswith(ancestor.rstrip("/") + "/")


class ScopeChecker:
    def __init__(self, home: str = "", interlock_check=None) -> None:
        self.home = home or os.path.expanduser("~")
        self.interlock_check = interlock_check

    def check(self, request: str | None, command: str, params: dict) -> ScopeResult:
        if request is None:
            return ScopeResult(NOT_EVALUATED, None,
                               "no user request available to check scope against")
        if self.interlock_check is not None:
            action, reason = self.interlock_check(request, command, params)
            return ScopeResult(EVALUATED, action, f"interlock: {reason}")

        destructive, targets = _action_targets(command, params, self.home)
        if not destructive or not targets:
            return ScopeResult(EVALUATED, "allow", "no destructive target to compare")
        requested = _paths_in(request, self.home)
        if not requested:
            return ScopeResult(EVALUATED, "confirm",
                               "destructive call, but the request names no path to "
                               "check it against")
        for target in targets:
            if target in requested:
                continue
            contained = [r for r in requested if _is_ancestor(target, r)]
            if contained:
                return ScopeResult(
                    EVALUATED, "block",
                    f"scope expansion: the request named {contained[0]}, but the "
                    f"call targets {target}, which contains it")
            return ScopeResult(
                EVALUATED, "confirm",
                f"target {target} is not the path named in the request "
                f"({requested[0]}); confirmation required")
        return ScopeResult(EVALUATED, "allow", "target matches the path named in the request")
