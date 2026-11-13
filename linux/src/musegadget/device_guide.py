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

"""``device.guide``: the device's usage guide, in Markdown."""

from __future__ import annotations

import functools
import re
from importlib import resources

COMMAND = "device.guide"
MAX_BYTES = 4096
SPEC = {
    "description": "This device's usage guide (Markdown): what it is, its controls and limits.",
    "required": {},
    "optional": {},
}
_LEADING_COMMENT = re.compile(r"\A<!--.*?-->\n*", re.S)


@functools.lru_cache(maxsize=1)
def default_guide() -> str:
    """The Linux client's own guide, from ``data/guide.md`` without its license header."""
    text = resources.files("musegadget").joinpath("data/guide.md").read_text(encoding="utf-8")
    return check(_LEADING_COMMENT.sub("", text, count=1))


def check(guide: str) -> str:
    """``guide``, or ``ValueError`` if it is empty or over ``MAX_BYTES`` of UTF-8."""
    size = len(guide.encode("utf-8")) if isinstance(guide, str) else 0
    if not 0 < size <= MAX_BYTES:
        raise ValueError(f"device guide is {size} bytes; it must be 1 to {MAX_BYTES}")
    return guide


def payload(guide: str) -> dict:
    return {"format": "markdown", "guide": guide}
