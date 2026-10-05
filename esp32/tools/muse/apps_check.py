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

"""Checks apps, the app library and the pet on a board, over its serial console.

  tools/muse/apps_check.py [--port PORT] [--board watcher] [--quick]

Runs the commands the agent would (app.*, pet.*, script.*), drives pages and
events, redefines and updates apps in loops while watching memory, and
restarts the board to see what's kept. --quick skips the restarts and the
long loops. Leaves the board as it found it, apart from the pet having been
looked after. Prints PASS/FAIL lines; exits 1 on any failure.
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hw  # noqa: E402
import ports  # noqa: E402

LIBRARY = ["apps", "clock", "timer", "camera", "sound", "light", "system", "dice"]


class Board:
    def __init__(self, port, log=None):
        self.port = port
        self.s = None
        self.log = open(log, "a") if log else None
        self.lines = []   # the board's log lines, for checks that read them
        self.open()

    def open(self):
        self.s = hw.open_port(self.port)
        time.sleep(0.5)
        self.s.write(b"\n")   # opening the port can garble the first line
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    def run(self, command, params=None, timeout=40):
        rid = f"ck-{time.monotonic():.6f}"
        hw.send(self.s, json.dumps({"id": rid, "command": command, "params": params or {}}, separators=(",", ":")))
        buf = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            buf += self.s.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                if line.startswith("@hw ") and rid in line:
                    try:
                        return json.loads(line[4:]).get("result") or {}
                    except ValueError:
                        continue
                if line:
                    self.lines.append(line)
                if self.log and line:
                    self.log.write(line + "\n")
                    self.log.flush()
        return {"ok": False, "error": {"code": "timeout"}}

    def restart(self):
        self.run("device.reboot")
        self.close()
        time.sleep(20)   # boot, Wi-Fi, and the scripts' 5 s autostart delay
        self.open()


failures = 0


def check(name, ok, detail=""):
    global failures
    failures += 0 if ok else 1
    print(("PASS " if ok else "FAIL ") + name + (f"  [{detail}]" if detail else ""), flush=True)
    return ok


def payload(r):
    return r.get("payload", {}) if r.get("ok") else {}


def memory(b):
    return payload(b.run("device.status")).get("memory", {})


def pages(b):
    return {a["id"]: a for a in payload(b.run("app.list")).get("apps", [])}


def events(b, after, wait_ms=1500):
    r = b.run("input.read", {"after": after, "wait_ms": wait_ms})
    return payload(r).get("events", [])


def last_seq(b):
    return payload(b.run("input.read", {"wait_ms": 0})).get("last_seq", 0)


EVERYTHING = {
    "id": "zz_check", "title": "Check", "order": 99, "bg": "#101418",
    "children": [
        {"type": "label", "id": "l", "text": "hello", "font": 28, "color": "white", "icon": "ok"},
        {"type": "row", "id": "r", "gap": 6, "children": [
            {"type": "button", "id": "b", "text": "Go", "icon": "play", "bg": "green"},
            {"type": "switch", "id": "sw", "on": True},
            {"type": "checkbox", "id": "cb", "text": "yes", "on": False},
            {"type": "led", "id": "led", "color": "cyan", "on": True, "brightness": 200},
            {"type": "spinner", "id": "sp", "w": 30, "h": 30, "color": "orange"}]},
        {"type": "slider", "id": "s", "min": 0, "max": 10, "value": 3, "live": True},
        {"type": "bar", "id": "bar", "min": 0, "max": 100, "value": 60},
        {"type": "arc", "id": "a", "w": 80, "h": 80, "min": 0, "max": 100, "value": 40, "knob": False},
        {"type": "chart", "id": "ch", "h": 50, "min": 0, "max": 100, "points": 10,
         "series": [{"color": "cyan", "values": [1, 50, 20]}, {"color": "red", "values": [5, 5]}]},
        {"type": "roller", "id": "ro", "options": ["a", "b", "c"], "rows": 2},
        {"type": "dropdown", "id": "dd", "options": "x\ny"},
        {"type": "line", "id": "li", "points": [[0, 0], [40, 10], [80, 0]], "width": 3, "color": "yellow"},
        {"type": "canvas", "id": "cv", "w": 120, "h": 40, "fill": "#202020", "draw": [
            {"op": "rect", "x": 2, "y": 2, "w": 30, "h": 30, "color": "red", "fill": True},
            {"op": "circle", "x": 60, "y": 20, "r": 15, "color": "blue"},
            {"op": "arc", "x": 95, "y": 20, "r": 15, "start": 0, "end": 270, "width": 3},
            {"op": "text", "x": 2, "y": 2, "text": "hi", "font": 14}]},
        {"type": "qr", "id": "qr", "text": "https://example.com", "size": 60},
        {"type": "table", "id": "t", "rows": [["a", 1], ["b", 2.5]], "col_w": [60, 60]},
        {"type": "input", "id": "in", "placeholder": "type"},
        {"type": "image", "id": "im", "w": 40, "h": 40},
    ],
}


def check_library(b):
    lib = payload(b.run("app.library"))
    ex = {e["id"]: e for e in lib.get("examples", [])}
    check("app.library lists the 8 examples", sorted(ex) == sorted(LIBRARY), str(sorted(ex)))
    check("the examples are installed", all(e.get("installed") for e in ex.values()),
          str([i for i, e in ex.items() if not e.get("installed")]))
    check("each says what it shows", all(len(e.get("about", "")) > 40 for e in ex.values()))
    e = payload(b.run("app.example", {"id": "clock"}))
    check("app.example: page and script", e.get("app", {}).get("id") == "clock" and "on(\"ui\"" in e.get("script", ""))
    g = payload(b.run("app.get", {"app": "clock"}))
    check("app.get: the kept page and its script", g.get("app", {}).get("id") == "clock" and g.get("script") == "clock")
    src = payload(b.run("script.read", {"name": "clock"}))
    check("script.read: the source, starting at boot", src.get("source") == e.get("script") and src.get("autostart"))
    check("app.example of nothing", b.run("app.example", {"id": "nope"}).get("error", {}).get("code") == "not_found")
    check("script.read of nothing", b.run("script.read", {"name": "nope"}).get("error", {}).get("code") == "not_found")


def check_pages(b):
    p = pages(b)
    check("app.list: built-in pages first", [i for i in p if p[i].get("builtin")] == ["face", "pet", "settings"])
    check("app.list: the library's apps", all(i in p for i in LIBRARY))
    seq = last_seq(b)
    for page in ["pet", "settings", "apps", "clock", "pet", "system", "face"]:
        r = b.run("app.show", {"app": page})
        time.sleep(1.2)   # the slide
        p = pages(b).get(page, {})
        check(f"app.show {page}", r.get("ok") and p.get("shown") and p.get("on_screen", True), str(p))
    ev = [e for e in events(b, seq) if e["type"] == "ui"]
    shown = [e["app"] for e in ev if e["event"] == "show"]
    hidden = [e["app"] for e in ev if e["event"] == "hide"]
    check("show and hide events for apps", "apps" in shown and "clock" in shown and "apps" in hidden, str(shown))
    check("app.show of nothing", b.run("app.show", {"app": "nope"}).get("error", {}).get("code") == "not_found")


def check_camera(b):
    """The camera works, alone and as the camera app's live video."""
    seen = len(b.lines)
    r = b.run("camera.capture", {"resolution": "240x240"}, 30)
    check("camera.capture", r.get("ok") and payload(r).get("width") == 240, json.dumps(r)[:160])
    b.run("app.show", {"app": "camera"})
    time.sleep(9)   # the camera starts, then streams into the page
    logs = payload(b.run("script.logs", {"name": "camera"})).get("lines", [])
    b.run("app.show", {"app": "face"})
    time.sleep(1)
    new = b.lines[seen:]
    bad = [l for l in new if any(k in l for k in ("didn't start", "spi_master", "not a baseline JPEG", "camera failed"))]
    frames = [l for l in new if "link.camera: live:" in l]
    check("the camera app streams", frames and not bad, (bad or frames or ["no frames"])[0][:160])
    check("the camera app's script is fine", not any("error" in l for l in logs), str(logs)[:160])
    r = b.run("camera.stop")
    check("camera.stop", r.get("ok") and not payload(r).get("preview"))


def check_define(b):
    r = b.run("app.define", {"app": EVERYTHING})
    check("app.define: every widget type", r.get("ok") and payload(r).get("kept"), json.dumps(r)[:200])
    r = b.run("app.update", {"app": "zz_check", "set": {
        "l": {"text": "changed", "color": "#ff0"}, "s": {"value": 9}, "a": {"value": 90}, "ch": {"push": [70, 10]},
        "t": {"rows": [["c", 3]]}, "cv": {"draw": [{"op": "line", "points": [[0, 0], [100, 30]], "width": 2}]},
        "ro": {"selected": 2}, "bar": {"value": 5}, "led": {"on": False}, "qr": {"text": "new"}, "nope": {"text": "x"}}})
    check("app.update of every kind (unknown ids skipped)", r.get("ok"), json.dumps(r)[:200])
    b.run("app.show", {"app": "zz_check"})
    time.sleep(1)
    check("the page shows", pages(b).get("zz_check", {}).get("shown"))
    for bad, why in [({"id": "Bad!"}, "id"), ({"id": "pet"}, "built-in"),
                     ({"id": "x1", "children": [{"type": "nope"}]}, "unknown type"),
                     ({"id": "x2", "children": [{"type": "box"}] * 120}, "too many widgets")]:
        r = b.run("app.define", {"app": bad})
        check(f"app.define refuses: {why}", not r.get("ok"), r.get("error", {}).get("message", ""))
    r = b.run("app.remove", {"app": "zz_check"})
    check("app.remove", r.get("ok") and "zz_check" not in pages(b))
    b.run("app.show", {"app": "face"})


def check_pet(b):
    st = payload(b.run("pet.status"))
    check("pet.status", all(k in st for k in ("food", "clean", "fun", "energy", "health", "needs")), json.dumps(st)[:160])
    # Unnamed, it shows the Muse's name, which pet.name mustn't pin: MUSE puts it back.
    old_name = st.get("name", "MUSE") if st.get("named", True) else "MUSE"
    r = payload(b.run("pet.name", {"name": "Testy 2"}))
    check("pet.name (in capitals)", r.get("name") == "TESTY 2")
    check("pet.name refuses odd names", not b.run("pet.name", {"name": "no/way"}).get("ok"))
    seq = last_seq(b)
    if st.get("asleep"):
        b.run("pet.care", {"action": "wake"})
    fed = [payload(b.run("pet.care", {"action": "feed"})) for _ in range(4)]
    full = st.get("food", 0) > 92
    check("feeding until it's full", (full or fed[0].get("done")) and fed[3].get("reason") == "FULL",
          " ".join(str(f.get("food")) for f in fed) + f" {fed[3].get('reason')}")
    check("a snack still goes down", payload(b.run("pet.care", {"action": "snack"})).get("done"))
    w = payload(b.run("pet.care", {"action": "wash"}))
    check("a bath", w.get("done") and w.get("clean") == 100 and w.get("poops") == 0)
    check("nothing to clean up", payload(b.run("pet.care", {"action": "clean"})).get("reason") == "ALL CLEAN")
    p = payload(b.run("pet.care", {"action": "play"}))
    check("play", p.get("done") or p.get("reason") in ("TOO TIRED", "TOO POORLY"), p.get("reason", ""))
    check("a stroke", payload(b.run("pet.care", {"action": "pet"})).get("done"))
    s = payload(b.run("pet.care", {"action": "sleep"}))
    check("lights out", s.get("done") and s.get("asleep"))
    check("no food while asleep", payload(b.run("pet.care", {"action": "feed"})).get("reason") == "ASLEEP")
    wk = payload(b.run("pet.care", {"action": "wake"}))
    check("waking it", wk.get("done") and not wk.get("asleep"))
    h = payload(b.run("pet.care", {"action": "heal"}))
    check("no medicine when well", h.get("done") or h.get("reason") == "NOT SICK", h.get("reason", ""))
    check("pet.care refuses nonsense", not b.run("pet.care", {"action": "juggle"}).get("ok"))
    ev = [e for e in events(b, seq, 500) if e["type"] == "pet"]
    whats = [e["what"] for e in ev]
    check("pet events for the care", ("feed" in whats or full) and "wash" in whats and "sleep" in whats, str(whats))
    b.run("app.show", {"app": "pet"})
    time.sleep(1.5)
    check("the pet's page shows", pages(b).get("pet", {}).get("shown"))
    b.run("app.show", {"app": "face"})
    r = payload(b.run("pet.name", {"name": old_name}))
    # Unnamed, only named is compared: the Muse's name may have arrived meanwhile.
    check("pet.name back as it was", r.get("named") == st.get("named")
          and (r.get("name") == st.get("name") or not st.get("named", True)), json.dumps(r)[:160])


SCRIPT = r'''
app.define{app = {id = "zz_script", title = "Script", order = 98, children = {
  {type = "label", id = "n", text = "0"}, {type = "button", id = "b", text = "+"}}}}
local shows, pets = 0, 0
on("ui", function(e)
  if e.app ~= "zz_script" then return end
  if e.event == "show" then
    shows = shows + 1
    app.update{app = "zz_script", set = {n = {text = tostring(shows)}}}
    print("shown", shows)
  end
end)
on("pet", function(e) pets = pets + 1; print("pet", e.what) end)
print("ready")
'''


def check_scripts(b):
    r = b.run("script.install", {"name": "zz_script", "source": SCRIPT})
    check("a script that makes an app", r.get("ok") and payload(r).get("running"), json.dumps(r)[:200])
    time.sleep(1)
    check("its app is there", "zz_script" in pages(b))
    b.run("app.show", {"app": "zz_script"})
    time.sleep(1.5)
    b.run("pet.care", {"action": "pet"})
    time.sleep(1)
    logs = payload(b.run("script.logs", {"name": "zz_script"})).get("lines", [])
    check("it saw its page show (e.event is 'show')", "shown\t1" in logs, str(logs))
    check("it saw the pet", "pet\tpet" in logs, str(logs))
    b.run("app.show", {"app": "face"})
    r = b.run("script.delete", {"name": "zz_script"})
    time.sleep(0.5)
    check("deleting the script takes its app", r.get("ok") and "zz_script" not in pages(b))


def check_stress(b):
    before = memory(b)
    t0 = time.monotonic()
    for i in range(25):
        app = dict(EVERYTHING, id="zz_stress")
        r = b.run("app.define", {"app": app, "keep": False})
        if not r.get("ok"):
            check("redefining an app 25 times", False, json.dumps(r)[:200])
            break
    b.run("app.remove", {"app": "zz_stress"})
    ok = True
    b.run("app.define", {"app": {"id": "zz_stress", "keep": False, "children": [
        {"type": "label", "id": "l", "text": "0"}, {"type": "chart", "id": "c", "series": [{"values": []}]}]},
        "keep": False})
    for i in range(150):
        r = b.run("app.update", {"app": "zz_stress", "set": {"l": {"text": str(i)}, "c": {"push": i % 100}}}, 10)
        ok = ok and r.get("ok")
    check("150 updates in a row", ok, f"{time.monotonic() - t0:.1f} s so far")
    order = ["pet", "face", "settings", "face", "zz_stress", "clock", "face"]
    for i in range(35):
        b.run("app.show", {"app": order[i % len(order)]}, 10)
        time.sleep(0.15)
    b.run("app.show", {"app": "face"})
    b.run("app.remove", {"app": "zz_stress"})
    time.sleep(2)
    after = memory(b)
    st = payload(b.run("device.status"))
    dp = before.get("psram_free", 0) - after.get("psram_free", 0)
    di = before.get("internal_free", 0) - after.get("internal_free", 0)
    # The camera, TLS and scripts swing PSRAM by ~100 KB on their own.
    check("no PSRAM lost to the loops", dp < 160 * 1024, f"{dp // 1024} KB")
    check("no internal RAM lost to the loops", di < 2048, f"{di} B, lowest ever {after.get('internal_min')}")
    check("no restart through it all", st.get("boot_reason") == before_boot,
          f"{st.get('boot_reason', '')} {json.dumps(st.get('last_crash'))}")


def check_restarts(b):
    b.run("app.define", {"app": {"id": "zz_kept", "title": "Kept", "children": [{"type": "label", "id": "l", "text": "still here"}]}})
    b.run("pet.name", {"name": "KEPT"})
    # A change to an example is the user's now: an update mustn't undo it.
    clock = payload(b.run("app.get", {"app": "clock"})).get("app")
    clock["title"] = "My Clock"
    b.run("app.define", {"app": clock})
    b.run("app.remove", {"app": "dice"})
    b.run("script.delete", {"name": "dice"})
    time.sleep(2)   # the saves are deferred
    b.restart()
    st = payload(b.run("device.status"))
    check("restarted cleanly", st.get("boot_reason") == "restart", st.get("boot_reason", ""))
    p = pages(b)
    check("a kept app comes back", "zz_kept" in p)
    check("the pet's name is kept", payload(b.run("pet.status")).get("name") == "KEPT")
    check("a changed example stays changed", p.get("clock", {}).get("title") == "My Clock")
    check("a removed example stays removed", "dice" not in p)
    lib = {e["id"]: e for e in payload(b.run("app.library")).get("examples", [])}
    check("app.library knows", lib.get("clock", {}).get("changed") and not lib.get("dice", {}).get("installed"))
    r = b.run("app.install", {"id": "dice"})
    r2 = b.run("app.install", {"id": "clock"})
    time.sleep(1)
    p = pages(b)
    check("app.install brings one back, running", payload(r).get("running") and "dice" in p)
    check("app.install undoes changes", payload(r2).get("running") and p.get("clock", {}).get("title") == "Clock")
    b.run("app.remove", {"app": "zz_kept"})
    b.run("pet.name", {"name": "MUSE"})


def main():
    global before_boot
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port")
    ap.add_argument("--board", default="watcher")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--log", help="save the board's log lines to this file")
    a = ap.parse_args()
    b = Board(a.port or ports.find(a.board), a.log)
    st = payload(b.run("device.status"))
    before_boot = st.get("boot_reason")
    check("device.status", bool(st), f"boot {st.get('boot_reason')}, battery {st.get('battery_percent')}%, "
          f"memory {st.get('memory')}")
    check("the battery reads on USB", st.get("battery_percent") is not None)
    if "last_crash" in st:
        print("NOTE last crash:", json.dumps(st["last_crash"]))
    global crash_seen
    crash_seen = st.get("last_crash")
    uptime = [st.get("uptime_s", 0)]

    def still_up(section):
        now = payload(b.run("device.status"))
        restarted = now.get("uptime_s", 0) < uptime[0]
        uptime[0] = now.get("uptime_s", 0)
        check(f"no restart during {section}", not restarted,
              f"{now.get('boot_reason')} {json.dumps(now.get('last_crash'))}" if restarted else "")

    for section, fn in [("library", check_library), ("pages", check_pages), ("camera", check_camera),
                        ("define", check_define),
                        ("pet", check_pet), ("scripts", check_scripts)] + \
            ([] if a.quick else [("stress", check_stress)]):
        fn(b)
        still_up(section)
    if not a.quick:
        check_restarts(b)
    b.run("app.show", {"app": "face"})
    b.close()
    print(f"{failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
