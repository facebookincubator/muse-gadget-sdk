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

"""Tests don't depend on the trust store of the machine that runs them."""

from __future__ import annotations

import pytest

from musegadget import tls


@pytest.fixture(autouse=True)
def _trust_store_is_clean(request, monkeypatch):
    if request.module.__name__.endswith("test_tls"):
        return      # test_tls checks the trust store itself
    monkeypatch.setattr(tls, "trust_store_problem", lambda: None)
