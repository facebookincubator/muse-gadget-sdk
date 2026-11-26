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

"""The build check for Live call and subtitles: every wireless read that prints the owner's call or subtitle content.

On this firmware the only wireless way out for it is the Nordic UART log mirror (a log line whose tag is on
CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS); no Control or GATT read returns call metadata or subtitle text (live_call and
subtitle messages are the client's writes; subtitle capabilities and the NUS status line are status). scan() finds
every mirrored log line in the call and subtitle sources; call_owner_reads.txt lists them."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIST = Path(__file__).with_name("call_owner_reads.txt")

# Files whose every mirrored line is call or subtitle content (call state, timeline, who started or ended it,
# timings, subtitle state and reply ids), and files where only the call's lines are (the line's tag or text says
# call).
WHOLE = ["components/muse/muse_call.c", "components/muse/muse_call_at.c", "components/muse/muse_call_stats.c",
         "components/muse/muse_subs.c", "components/muse_gadget_ble/mg_lct.c", "components/muse_gadget_ble/mg_lct_fmt.c"]
CALL_LINES = ["components/muse/muse_input.c", "components/muse/muse_video.c", "components/muse_gadget_ble/mg_ble.c",
              "components/muse/muse_voice.c"]

LOG = re.compile(r'ESP_LOG[IWED]\(\s*([A-Za-z_][A-Za-z_0-9]*|"[^"]+")\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)')


def mirrored(tag: str, tags: str) -> bool:
    if tags.strip() == "*":
        return True
    return any(p.strip() and tag.startswith(p.strip()) for p in tags.split(","))


def default_tags() -> str:
    kc = (ROOT / "components/muse_gadget_ble/Kconfig").read_text()
    opt = kc[kc.index("config MUSE_GADGET_BLE_NUS_LOG_TAGS"):]
    tags = re.search(r'default "([^"]*)"', opt).group(1)
    # Overlays may widen it (muse_video on the video builds): every value any overlay sets counts.
    for f in (ROOT / "devices").glob("sdkconfig.*"):
        for m in re.finditer(r'^CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS="([^"]*)"', f.read_text(), re.M):
            tags += "," + m.group(1)
    return tags


def scan() -> list[str]:
    tags = default_tags()
    out = []
    for rel in WHOLE + CALL_LINES:
        path = ROOT / rel
        if not path.exists():
            continue
        src = path.read_text()
        names = dict(re.findall(r'static const char \*(?:const )?(\w+)\s*=\s*"([^"]+)"', src))
        for m in LOG.finditer(src):
            t = m.group(1)
            tag = names.get(t, t.strip('"'))
            fmt = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2)))
            if not mirrored(tag, tags):
                continue
            if rel in CALL_LINES and not (tag in ("mg.call", "mg.lct", "mg.subs") or re.search(r"\bcall\b|subtitle", fmt)):
                continue
            out.append(f"{rel} | {tag} | {fmt}")
    return sorted(set(out))


if __name__ == "__main__":
    print("\n".join(scan()))
