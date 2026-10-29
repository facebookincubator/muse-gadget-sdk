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
scope model is an optional adapter (see scope.py).

Names are loaded lazily so `python -m command_guard.ledger` and
`python -m command_guard.demo` run without import warnings.
"""

__all__ = ["Decision", "Guard", "GuardContext", "build_guard"]


def __getattr__(name):
    if name in __all__:
        from command_guard import guard as _guard

        return getattr(_guard, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
