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

"""Muse Gadget Guard: a three-valued check in front of the executor.

Every command Muse sends a Linux gadget passes through ``Guard.check``
before it runs:

1. Layer 1 (``policy``): deterministic rules that need no context.
2. Layer 2 (``scope``): whether the call stays inside what the user
   actually asked for. Only evaluated when a request is supplied; when
   it is not, the ledger records ``not_evaluated`` for this layer.

The combined decision is ``allow``, ``confirm`` or ``block`` (block beats
confirm beats allow). Every decision is appended to a tamper-evident
ledger (``ledger``). The guard is opt-in: an ``Executor`` built without
one behaves exactly as before.

This guard does not make an agent safe. It is one layer, alongside
least-privilege accounts, sandboxing and human confirmation, and it will
miss things — obfuscated shell commands in particular can slip past the
Layer 1 string rules. What it adds is a decision point and evidence:
every call, its decision, and a chain that shows if the record changed.
"""

from __future__ import annotations

import logging
import os
from dataclasses import dataclass

from command_guard import policy as policy_module
from command_guard.ledger import Ledger, summarize_params
from command_guard.policy import Policy, load_policy, worse
from command_guard.scope import ScopeChecker

log = logging.getLogger(__name__)

DEFAULT_LEDGER_PATH = "/var/lib/musegadget/guard-ledger.jsonl"


@dataclass(frozen=True)
class GuardContext:
    """Context a caller may attach to a check. ``request`` is the user's
    own wording, when the caller has it; the current gadget protocol does
    not pass it to the executor, so it is usually None."""
    request: str | None = None


@dataclass(frozen=True)
class Decision:
    action: str              # allow / confirm / block
    reason: str
    layer1: str              # layer 1 action
    layer2: str              # evaluated action, or "not_evaluated"


class Guard:
    def __init__(self, policy: Policy | None = None, ledger: Ledger | None = None,
                 home: str = "", scope_checker: ScopeChecker | None = None) -> None:
        self.home = home or os.path.expanduser("~")
        self.policy = policy or load_policy(home=self.home)
        self.ledger = ledger
        self.scope_checker = scope_checker or ScopeChecker(home=self.home)

    def check(self, command: str, params: dict,
              request: str | None = None,
              context: GuardContext | None = None) -> Decision:
        if request is None and context is not None:
            request = context.request
        params = params or {}

        layer1 = policy_module.evaluate(self.policy, command, params)
        scope = self.scope_checker.check(request, command, params)

        action = layer1.action
        reason = layer1.reason
        layer2_label = scope.status if scope.action is None else scope.action
        if scope.action is not None:
            combined = worse(action, scope.action)
            if combined != action:
                action = combined
                reason = scope.reason
            elif scope.action != "allow":
                reason = f"{reason}; scope: {scope.reason}"

        decision = Decision(action=action, reason=reason,
                            layer1=layer1.action, layer2=layer2_label)
        if self.ledger is not None:
            try:
                self.ledger.append(
                    command=command,
                    params_summary=summarize_params(command, params),
                    layer1=decision.layer1,
                    layer2=decision.layer2,
                    final=decision.action,
                    reason=decision.reason,
                )
            except OSError as exc:  # a full disk must not wedge the gadget
                log.warning("guard ledger append failed: %s", exc)
        return decision


def build_guard(home: str = "") -> Guard:
    """Build a guard from the environment, for the service entry point.

    MUSEGADGET_GUARD_POLICY: path to a policy.json (default: the one next
    to this package). MUSEGADGET_GUARD_LEDGER: ledger path (default:
    /var/lib/musegadget/guard-ledger.jsonl).
    """
    home = home or os.path.expanduser("~")
    policy = load_policy(path=os.environ.get("MUSEGADGET_GUARD_POLICY") or None, home=home)
    ledger_path = os.environ.get("MUSEGADGET_GUARD_LEDGER") or DEFAULT_LEDGER_PATH
    return Guard(policy=policy, ledger=Ledger(ledger_path), home=home)
