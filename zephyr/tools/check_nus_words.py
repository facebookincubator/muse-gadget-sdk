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
"""Build check: every Nordic UART word mg_nus.c dispatches is classified.

  check_nus_words.py src/mg_nus.c src/mg_nus_policy.h

Every literal run_cmd compares the line with (strcmp(cmd, "...")) must be in
mg_nus_policy.h's mg_nus_words[] table with its class (read, change or
bench mode; a bench-mode word only inside #if defined(CONFIG_MG_BENCH_MODE));
run_cmd must not compare a prefix (strncmp on cmd) and must call
mg_nus_word_locked() before its first comparison (a word missing from a lock
list ships open, and a word dispatched before the gate skips it). Exit 1,
naming the word, otherwise.
"""
import re
import sys


def inside_bench_mode(text, pos):
    """True when pos sits inside an #if defined(CONFIG_MG_BENCH_MODE) ... #endif (nesting counted)."""
    stack = []
    for m in re.finditer(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$", text[:pos], re.M):
        kw, rest = m.group(1), m.group(2)
        if kw in ("if", "ifdef", "ifndef"):
            stack.append("CONFIG_MG_BENCH_MODE" in rest and kw != "ifndef" and "!" not in rest)
        elif kw in ("elif", "else") and stack:
            stack[-1] = False
        elif kw == "endif" and stack:
            stack.pop()
    return any(stack)


def main():
    nus, policy = open(sys.argv[1]).read(), open(sys.argv[2]).read()
    table = dict(re.findall(r'\{"([^"]+)",\s*MG_NUS_(READ|CHANGE|BENCH_MODE)\}', policy))
    m = re.search(r"static void run_cmd\(const char \*cmd\)\n\{(.*?)\n\}\n", nus, re.S)
    if not m:
        sys.exit("check_nus_words: run_cmd not found in mg_nus.c")
    body = m.group(1)
    bad = []
    gate = body.find("mg_nus_word_locked(cmd")
    first = body.find("strcmp(cmd")
    if gate < 0 or (first >= 0 and first < gate):
        bad.append("run_cmd dispatches a word before mg_nus_word_locked()")
    if re.search(r"strncmp\(\s*cmd", body) or re.search(r"cmd\[0\]\s*==", body):
        bad.append("run_cmd compares a prefix of the line (words are matched whole)")
    for w in re.findall(r'strcmp\(\s*cmd\s*,\s*"([^"]*)"\s*\)', body):
        if w not in table:
            bad.append(f'the word "{w}" isn\'t in mg_nus_words[] (classify it: read or change)')
    # A bench-mode word exists only in a CONFIG_MG_BENCH_MODE image, in the table and in
    # run_cmd alike.
    for w, cls in table.items():
        if cls != "BENCH_MODE":
            continue
        for where, text in (("mg_nus_words[]", policy), ("run_cmd", body)):
            needle = ('{"%s",' % w) if where == "mg_nus_words[]" else ('strcmp(cmd, "%s")' % w)
            for m in re.finditer(re.escape(needle), text):
                if not inside_bench_mode(text, m.start()):
                    bad.append(f'the bench-mode word "{w}" is in {where} outside '
                               "#if defined(CONFIG_MG_BENCH_MODE)")
    if bad:
        print("check_nus_words: refused:", file=sys.stderr)
        for b in bad:
            print("  " + b, file=sys.stderr)
        return 1
    print(f"check_nus_words: {len(table)} words classified; the gate runs before every one; "
          "bench-mode words only in bench-mode images")
    return 0


if __name__ == "__main__":
    sys.exit(main())
