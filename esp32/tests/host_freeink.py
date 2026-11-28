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

"""The pinned FreeInk source for the e-ink host tests (components/muse/idf_component.yml's freeink-sdk).

The tests find it by themselves: FREEINK_SOURCE_DIR if set, else this tree's managed_components/freeink-sdk (a
reTerminal Sticky configure fetches it). Without one a test that needs it fails, saying how to get it."""

import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UI = "libs/ui/FreeInkUI"


def pinned_revision():
    manifest = (ROOT / "components/muse/idf_component.yml").read_text()
    m = re.search(r"freeink-sdk:\s+git:.*?\s+version: ([0-9a-f]{40})", manifest)
    if not m:
        raise AssertionError("components/muse/idf_component.yml has no pinned freeink-sdk version")
    return m.group(1)


def freeink_dir():
    """The FreeInk checkout; the test fails, naming how to get it, when there's none."""
    d = Path(os.environ.get("FREEINK_SOURCE_DIR") or ROOT / "managed_components/freeink-sdk")
    if (d / UI / "include/FreeInkApp.h").is_file():
        return d
    try:
        pin = pinned_revision()
    except AssertionError:
        pin = "the commit components/muse/idf_component.yml pins"
    raise AssertionError(
        f"The pinned FreeInk source isn't in {d}: configure the reTerminal Sticky once in esp32/ "
        f"(tools/board.sh reterminal-sticky build), or set FREEINK_SOURCE_DIR to a checkout of Free-Ink/freeink-sdk "
        f"at {pin}")
