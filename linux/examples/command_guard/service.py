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

"""A ``Service`` subclass that guards every command Muse invokes.

This is the whole integration. The ``musegadget`` package exposes exactly
the seam a gadget needs (see "Building a gadget" in ``linux/AGENTS.md``):
``Service.run_command(name, params, timeout_ms)`` handles each command from
Muse and, by default, calls the ``Executor``. ``GuardedService`` overrides
only that one method — it runs the guard first, and lets the command
through to the base implementation only on an ``allow``. Nothing in the
core SDK changes.

``run_command`` runs in a worker thread and commands can run concurrently;
the guard's ledger is locked accordingly (see ``ledger.py``).

Run it exactly like ``musegadget run``, but through this example:

    MUSEGADGET_GUARD_POLICY=/path/policy.json \\
    MUSEGADGET_GUARD_LEDGER=/var/lib/musegadget/guard-ledger.jsonl \\
    sudo PYTHONPATH=linux/examples python3 -m command_guard.service run --run-as pi

or from your own code:

    from command_guard.service import GuardedService
    from command_guard.guard import build_guard
    service = GuardedService(identity=..., executor=..., guard=build_guard(home))
    await service.run()
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import signal
import sys
from dataclasses import dataclass

from musegadget import __version__, config, identity as identity_mod
from musegadget.executor import Account, Executor, error
from musegadget.service import Service

from command_guard.guard import Guard, build_guard

log = logging.getLogger("command_guard")


@dataclass
class GuardedService(Service):
    """A ``Service`` that checks every command through a ``Guard`` first.

    ``guard=None`` behaves exactly like the base ``Service`` — the guard is
    opt-in. Being a dataclass subclass, it just adds one field with a
    default; everything else (reconnect loop, token rotation, local socket)
    is inherited unchanged.
    """

    guard: Guard | None = None

    def run_command(self, name: str, params: dict, timeout_ms: int | None = None) -> dict:
        """Guard one command, then delegate to the base executor on allow.

        The guard never raises (see ``Guard.check``); the extra try/except
        here is a final belt-and-braces guarantee that Muse always receives
        a result dict and never an exception from this thread.
        """
        guard = self.guard
        if guard is None:
            return super().run_command(name, params, timeout_ms)
        try:
            decision = guard.check(name, params or {})
            if decision.action != "allow":
                return error(f"guard: {decision.action}: {decision.reason}")
            return super().run_command(name, params, timeout_ms)
        except Exception as exc:  # noqa: BLE001 - never surface an exception to Muse
            log.exception("guarded run_command failed; failing closed")
            return error(f"guard: block: internal error ({type(exc).__name__})")


def run_guarded_service(identity, executor: Executor, guard: Guard | None,
                        sdk_token: str | None = None) -> None:
    """Run a :class:`GuardedService` the way ``service.run_service`` runs a
    plain one: install signal handlers, start the local socket, serve."""

    async def _main() -> None:
        service = GuardedService(identity=identity, executor=executor,
                                 guard=guard, sdk_token=sdk_token)
        loop = asyncio.get_running_loop()
        for signum in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(signum, service.stop)
        server = await service.serve_local(config.socket_path())
        try:
            await service.run()
        finally:
            server.close()

    asyncio.run(_main())


def _resolve_account(run_as: str | None) -> Account:
    if os.geteuid() == 0:
        if not run_as:
            raise SystemExit("Pass --run-as ACCOUNT (the account whose permissions "
                             "commands get).")
        try:
            return Account.lookup(run_as)
        except KeyError:
            raise SystemExit(f"Account {run_as!r} does not exist.")
    return Account.current()


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="command_guard.service",
        description="Run the Muse gadget service with the command guard in front.")
    parser.add_argument("command", nargs="?", default="run", choices=["run"])
    parser.add_argument(
        "--run-as",
        default=os.environ.get("MUSEGADGET_RUN_AS") or os.environ.get("SUDO_USER"),
        help="account whose permissions commands run with (default: the account that ran sudo)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    account = _resolve_account(args.run_as)
    try:
        sdk_token = config.sdk_token()
    except ValueError as exc:
        log.warning("running without an SDK token: %s", exc)
        sdk_token = None

    guard = build_guard(home=account.home)
    log.info("musegadget %s (command guard enabled): commands run as %s",
             __version__, account.name)
    run_guarded_service(identity_mod.load_or_create(), Executor(account), guard, sdk_token)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
