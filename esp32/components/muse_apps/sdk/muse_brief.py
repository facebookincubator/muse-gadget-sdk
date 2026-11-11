#!/usr/bin/env python3
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

"""Keeps MUSE.md's copies of gadget.h, build.py and hello.c in step with the files.

    python3 components/muse_apps/sdk/muse_brief.py           rewrite them
    python3 components/muse_apps/sdk/muse_brief.py --check   exit 1 if any is stale, or the guide is too large

MUSE.md is the guide you send Muse so it can build apps for a gadget; it carries
the three files whole, since Muse can't read the SDK. Edit its prose by hand and
the files where they live (gadget.h is generated: components/muse_apps/sdk/api/gen_api.py), then run
this. It goes to Muse as one typed message (tools/muse/chat.py --file), which a
board takes up to MAX_BYTES of.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BRIEF = os.path.join(HERE, "MUSE.md")
FILES = {"gadget.h": "c", "build.py": "python", "hello.c": "c"}
MAX_BYTES = 192 * 1024 - 1  # a typed message over the console: CHAT_MAX in components/muse/muse_input.c


def render(text):
    for name, lang in FILES.items():
        source = "examples/hello.c" if name == "hello.c" else name
        body = open(os.path.join(HERE, source)).read().rstrip("\n")
        if name == "hello.c":
            # The guide delivers hello.c beside gadget.h, not inside examples/.
            body = body.replace('#include "../gadget.h"', '#include "gadget.h"')
            body = body.replace("Build from the SDK folder:\n * python3 build.py examples/hello.c",
                                "Build: python3 build.py hello.c")
        block = "<!-- BEGIN %s -->\n```%s\n%s\n```\n<!-- END %s -->" % (name, lang, body, name)
        pattern = re.compile(r"<!-- BEGIN %s -->.*?<!-- END %s -->" % (re.escape(name), re.escape(name)), re.S)
        if not pattern.search(text):
            sys.exit("MUSE.md has no BEGIN/END markers for " + name)
        text = pattern.sub(lambda _: block, text)
    return text


def main():
    text = open(BRIEF).read()
    new = render(text)
    size = len(new.encode("utf-8"))
    too_large = size > MAX_BYTES
    if too_large:
        print("components/muse_apps/sdk/MUSE.md is %d bytes: a board sends at most %d in one message" % (size, MAX_BYTES), file=sys.stderr)
    if "--check" in sys.argv:
        if new != text:
            print("components/muse_apps/sdk/MUSE.md is stale: run python3 components/muse_apps/sdk/muse_brief.py", file=sys.stderr)
            return 1
        return 1 if too_large else 0
    if new != text:
        open(BRIEF, "w").write(new)
        print("updated components/muse_apps/sdk/MUSE.md (%d KB)" % (size // 1024))
    return 1 if too_large else 0


if __name__ == "__main__":
    sys.exit(main())
