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
"""Build check: the talk button's pin is never reconfigured after gpio-keys armed it.

  check_button_pin.py <src dir>

gpio-keys configures sw0 and arms its trigger at POST_KERNEL; reconfiguring
the pin later removes that trigger in Zephyr's nRF GPIO driver, and every
press after it is lost. So every gpio_pin_configure_dt() on a gpio_dt_spec
taken from DT_ALIAS(sw0) must sit under `if (!MG_SW0_GPIO_KEYS)` (mg_button.h),
except in off_now(), which arms the System OFF wake right before powering
off. Exit 1, naming the file and line, otherwise.
"""
import pathlib
import re
import sys

SPEC = re.compile(r"struct\s+gpio_dt_spec\s+(\w+)\s*=\s*GPIO_DT_SPEC_GET\(\s*DT_ALIAS\(sw0\)")
FUNC = re.compile(r"^\w[\w\s\*]*\b(\w+)\s*\([^;]*$")


def bad_sites(path: pathlib.Path) -> list:
    lines = path.read_text().splitlines()
    names = {m.group(1) for m in SPEC.finditer("\n".join(lines))}
    bad, func = [], None
    for i, line in enumerate(lines):
        m = FUNC.match(line)
        if m:
            func = m.group(1)
        for name in names:
            if re.search(r"gpio_pin_configure_dt\(\s*&" + name + r"\b", line):
                prev = next((lines[j] for j in range(i - 1, -1, -1) if lines[j].strip()), "")
                if func != "off_now" and "if (!MG_SW0_GPIO_KEYS)" not in prev:
                    bad.append(f"{path.name}:{i + 1}: {line.strip()}")
    return bad


def main():
    src = pathlib.Path(sys.argv[1])
    bad = [b for p in sorted(src.glob("*.c")) for b in bad_sites(p)]
    for b in bad:
        print("check_button_pin: sw0 reconfigured where gpio-keys owns it:", b)
    if bad:
        sys.exit(1)
    print("check_button_pin: ok")


if __name__ == "__main__":
    main()
