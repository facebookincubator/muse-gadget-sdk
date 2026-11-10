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

"""Opt-in command guard for the Linux Muse gadget SDK (example).

See README.md in this directory. Standard library only; the Interlock
scope model is an optional adapter (see scope.py). The guard is wired in by
subclassing ``Service`` and overriding ``run_command`` — see
``GuardedService`` in ``service.py``. The core SDK is unchanged.

Names are loaded lazily so ``python -m command_guard.ledger`` and
``python -m command_guard.demo`` run without importing the service layer.
"""

_GUARD_NAMES = {"Decision", "Guard", "GuardContext", "build_guard"}
_SERVICE_NAMES = {"GuardedService", "run_guarded_service"}

__all__ = sorted(_GUARD_NAMES | _SERVICE_NAMES)


def __getattr__(name):
    if name in _GUARD_NAMES:
        from command_guard import guard as _guard

        return getattr(_guard, name)
    if name in _SERVICE_NAMES:
        from command_guard import service as _service

        return getattr(_service, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
