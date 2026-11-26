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
"""Build check: no log line formats a secret.

  check_log_secrets.py <dir> [<dir> ...]

Every log line can reach the Nordic UART log mirror, so in the C files under
each directory: no hex-buffer log macro (LOG_HEXDUMP_*, a %*ph format), and no
%s / %.*s / %p argument that is a bare variable or member named like a secret
(token, key, K, secret, nonce, MAC, seal, password, credentials). Lengths,
literals and calls (a "held"/"none" string) are fine. Exit 1, naming each line.
This app has no caption or reply text to log.
"""
import pathlib
import re
import sys

SECRET = re.compile(r"(^|[._>])(sdk_)?(token|tok|key|k|secret|nonce|mac|hmac|seal|password|"
                    r"cred|creds|credentials|access|refresh|psk)(_buf|_raw)?$", re.I)
CALL = re.compile(r"\b(LOG_(?:INF|WRN|ERR|DBG|HEXDUMP_\w+)|printk|snprintk)\s*\(")


def split_args(s):
    out, depth, cur, q = [], 0, "", None
    for ch in s:
        if q:
            cur += ch
            if ch == q and not cur.endswith("\\" + q):
                q = None
            continue
        if ch in "\"'":
            q = ch
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
            continue
        cur += ch
    out.append(cur.strip())
    return out


def scan(path):
    bad = []
    t = path.read_text()
    for m in CALL.finditer(t):
        i, depth = m.end(), 1
        while i < len(t) and depth:
            depth += {"(": 1, ")": -1}.get(t[i], 0)
            i += 1
        args = split_args(t[m.end():i - 1])
        line = t.count("\n", 0, m.start()) + 1
        if m.group(1).startswith("LOG_HEXDUMP"):
            bad.append(f"{path}:{line}: hex-buffer log macro")
            continue
        fmt = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', args[0] if m.group(1) != "snprintk" else
                                 (args[2] if len(args) > 2 else "")))
        if "%*ph" in fmt:
            bad.append(f"{path}:{line}: %*ph hex buffer")
        if not re.search(r"%(\.\*)?[sp]", fmt):
            continue
        for a in args[1:]:
            a = a.strip().lstrip("&*(").rstrip(")")
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*((\.|->)[A-Za-z_][A-Za-z0-9_]*)*", a) and SECRET.search(a):
                bad.append(f"{path}:{line}: formats '{a}'")
    return bad


def main():
    bad = []
    for d in sys.argv[1:]:
        for f in sorted(pathlib.Path(d).rglob("*.c")):
            bad += scan(f)
    if bad:
        sys.exit("check_log_secrets:\n  " + "\n  ".join(bad))
    print("check_log_secrets: no secret formatted and no hex buffer in any log line")


if __name__ == "__main__":
    main()
