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

"""What a board did on its last run on battery.

    python3 tools/muse/power.py [--port PORT]              the report
    python3 tools/muse/power.py --mah 450                  and the average current, for a 450 mAh cell
    python3 tools/muse/power.py --json                     the board's "@power" JSON
    python3 tools/muse/power.py --reset                    start a new measurement

A measurement starts when USB is unplugged and stops when it comes back, so:
unplug, leave the board be (a few hours: the fuel gauge moves in 1% steps),
plug it back in, run this. The board keeps the last measurement across a
reset, so one that restarts when its port opens, or that crashed on battery,
still has it. components/muse/muse_battery.h has the details. Needs pyserial.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import chat  # noqa: E402

# By the strongest power lock held. The clock in each depends on the screen: with it
# on, the CPU stays at full speed throughout; resting, idle is the crystal clock.
MODES = (("SLEEP", "light sleep allowed"), ("APB_MIN", "idle"), ("APB_MAX", "a peripheral busy"),
         ("CPU_MAX", "a core busy"))


def duration(secs):
    h, m = divmod(int(secs) // 60, 60)
    return f"{h} h {m} min" if h else f"{m} min {int(secs) % 60} s"


def memory(heap):
    """Internal RAM, from the JSON's "heap": what Wi-Fi, BLE and the tunnel have to work with."""
    kb = lambda n: f"{n / 1024:.1f} KB"
    mem = (f"Memory    internal RAM {kb(heap['free'])} free (largest block {kb(heap['largest'])}), "
           f"{kb(heap['min'])} lowest since boot")
    if heap.get("resting_min") is not None:
        mem += f", {kb(heap['resting_min'])} lowest while resting"
    if "cpu_pd" in heap:
        mem += (f"\n          retention-capable {kb(heap['retention_free'])} free (largest block "
                f"{kb(heap['retention_largest'])}); CPU {'powers down' if heap['cpu_pd'] else 'stays powered'}"
                " in light sleep")
    return mem


def report(p, mah=None):
    if not p.get("started"):
        text = "No measurement yet: unplug USB, let the board run on battery, then plug it back in."
        return text + ("\n" + memory(p["heap"]) if p.get("heap") else "")
    secs = p["secs"]
    lines = [("On battery for %s, still measuring." if p["running"] else
              "Ran on battery for %s; stopped when USB came back.") % duration(secs)]
    if p.get("boot") not in (None, "power on", "usb", "jtag") and p.get("uptime", 0) <= secs + 60:
        lines.append(f"The board restarted ({p['boot']}) on battery; this counts from then.")
    if p.get("battery_boots", 0) > 1:
        lines.append(f"Booted {p['battery_boots']} times on battery since USB power was last seen.")

    (pct0, pct1), (mv0, mv1) = p["battery_pct"], p["battery_mv"]
    batt = f"Battery   {pct0}% -> {pct1}%"
    if mv0 and mv1:
        batt += f"   {mv0 / 1000:.2f} V -> {mv1 / 1000:.2f} V"
    used = pct0 - pct1
    if used > 0 and secs >= 600:
        rate = used * 3600 / secs
        batt += f"   {rate:.1f}%/h, about {100 / rate:.0f} h from full"
        if mah:
            batt += f", {mah * rate / 100:.0f} mA average"
    elif secs < 3600:
        batt += "   (too soon to tell: the gauge moves in 1% steps)"
    lines.append(batt)

    lines.append(f"Screen    off {p['screen_off']}% of the time; resting (display stopped, CPU free to sleep) "
                 f"{p['resting']}%")
    chip = []
    if p.get("slept") is not None:
        rate = p["sleeps"] / secs if secs else 0
        chip.append(f"in light sleep {p['slept']}%, {p['sleeps']} wakes ({rate:.1f}/s)")
    if p.get("busy") is not None:
        chip.append(f"a core busy {p['busy']}%")
    if chip:
        lines.append("Chip      " + "; ".join(chip))
    if "modes" in p:
        lines.append("Modes     " + ", ".join(f"{p['modes'].get(k, 0)}% {what}" for k, what in MODES))
        lines.append(f"          {p.get('sleep_rejects', 0)} light sleeps refused (a wake-up already pending)")
    if p.get("heap"):
        lines.append(memory(p["heap"]))
    locks = sorted((l for l in p.get("locks", []) if l["held"] or l["taken"]), key=lambda l: -l["held"])
    if locks:
        lines.append("Power locks held (each one keeps the chip out of light sleep; rtos0/1 = a CPU core busy):")
        lines += [f"  {l['name']:<16}{l['type']:<15}{l['held']:>6}%  taken {l['taken']}" for l in locks]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port")
    ap.add_argument("--json", action="store_true", help="print the board's JSON")
    ap.add_argument("--reset", action="store_true", help="start a new measurement")
    ap.add_argument("--mah", type=float, help="the battery's capacity, for an average current")
    args = ap.parse_args()
    try:
        with chat.Board(args.port or chat.pick_port()) as board:
            p, saved = ask(board, "power.reset" if args.reset else "power")
            if p is None and not saved:
                raise chat.BoardError("The board doesn't answer: is its firmware older than the power command?")
    except chat.BoardError as e:
        sys.exit(str(e))
    if args.json:
        print(json.dumps({"saved": saved, "now": p} if saved else p, indent=2))
    elif saved and not p:
        print("Saved before the board last restarted:\n" + report(saved, args.mah))
    elif saved and not (p.get("started") and p.get("uptime", 0) > p["secs"]):
        # The board reset since this boot's measurement began, or it has none: the saved one is the run.
        print("Saved before the board last restarted:\n" + report(saved, args.mah))
        if p.get("started"):
            print("\nSince then:\n" + report(p, args.mah))
    else:
        print(report(p, args.mah))


def ask(board, cmd):
    """The board's @power JSON and the @power.saved it prints first, if any. A board that
    restarts when its port opens prints its saved one as it boots, then gets the question
    again; one that takes no commands (the Watcher) answers only with that."""
    saved = None
    for attempt in range(2):
        board.ser.write(b"\n")   # ends any half line
        board.write_line(cmd)
        deadline = time.monotonic() + 3.0
        booted = False
        while True:
            line = board.read_line(deadline)
            if line is None:
                break
            if attempt == 0 and not booted and any(m in line for m in chat.BOOT_MARKS):
                booted = True
                deadline = time.monotonic() + 5.0   # to boot
                continue
            for kind in ("@power.saved", "@power"):
                at = line.find(kind + " {")
                if at >= 0:
                    try:
                        f = json.loads(line[at + len(kind) + 1:])
                    except ValueError:
                        break
                    if kind == "@power":
                        return f, saved
                    saved = f
                    break
        if not booted:
            break
    return None, saved


if __name__ == "__main__":
    main()
