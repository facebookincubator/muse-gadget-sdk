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

"""UI input (components/muse_gadget_ble/mg_ui.c) on the host: hid_map parsing,
the offset map, the HID report map (parsed here into its reports), the
reports and the relative pointer's park-and-step moves."""

from __future__ import annotations

import json
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from test_mg_ble import MG, PROTOCOLS, ROOT, compile_and_run


def parse_report_map(desc: bytes) -> dict:
    """A small HID report descriptor parser: per report ID, its application usage
    (page, usage) and the input / feature report sizes in bits."""
    globals_ = {"size": 0, "count": 0, "id": 0, "page": 0, "lmin": 0, "lmax": 0}
    stack: list[dict] = []
    reports: dict = {}
    app = None
    usages: list[tuple[int, int]] = []
    i = 0
    while i < len(desc):
        b = desc[i]
        size = (0, 1, 2, 4)[b & 3]
        data = int.from_bytes(desc[i + 1:i + 1 + size], "little")
        sdata = data - (1 << (8 * size)) if size and data >= 1 << (8 * size - 1) else data
        tag, typ = b >> 4, (b >> 2) & 3
        i += 1 + size
        if typ == 1:   # global
            if tag == 0:
                globals_["page"] = data
            elif tag == 1:
                globals_["lmin"] = sdata
            elif tag == 2:
                globals_["lmax"] = sdata if globals_["lmin"] < 0 else data
            elif tag == 7:
                globals_["size"] = data
            elif tag == 8:
                globals_["id"] = data
            elif tag == 9:
                globals_["count"] = data
            elif tag == 10:
                stack.append(dict(globals_))
            elif tag == 11:
                globals_ = stack.pop()
        elif typ == 2:   # local
            if tag == 0:
                usages.append((globals_["page"], data))
        else:            # main
            if tag == 10:   # collection
                if data == 1 and usages:
                    app = usages[0]
            if tag in (8, 11):   # input, feature
                r = reports.setdefault(globals_["id"], {"app": app, "input": 0, "feature": 0, "fields": []})
                kind = "input" if tag == 8 else "feature"
                r[kind] += globals_["size"] * globals_["count"]
                if not data & 1:   # data, not constant
                    r["fields"].append({"kind": kind, "usages": list(usages), "size": globals_["size"],
                                        "count": globals_["count"], "lmin": globals_["lmin"],
                                        "lmax": globals_["lmax"], "relative": bool(data & 4)})
            usages = []
    return reports


class MgUiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        out = compile_and_run(cls(), [ROOT / "tests/mg_ui_harness.c", MG / "mg_ui.c"], [],
                              [MG, MG / "include", PROTOCOLS])
        cls.out = {}
        cls.maps = {}
        cls.dropped = {}
        for line in out.splitlines():
            obj = json.loads(line)
            if "map" in obj:
                cls.maps[obj["map"]] = obj
            else:
                dropped = obj.pop("dropped", None)
                if dropped is not None:
                    cls.dropped.update(dict.fromkeys(obj, dropped))   # each report line's own count
                cls.out.update(obj)


    def test_offset_map(self) -> None:
        # Square 1 x 0.5 of the screen from y = -0.25: panel row 40 is off the
        # screen's top (dropped), row 350 lands 0.4 down the screen.
        top, mid, sx, sy = self.out["offset_pt"]
        self.assertEqual((top, mid), (0, 1))
        self.assertAlmostEqual(sx, 0.5, delta=0.002)
        self.assertAlmostEqual(sy, -0.25 + (351 / 466) * 0.5, delta=0.002)
        reports = self.out["offset"]
        self.assertEqual(len(reports), 2)   # only the lower tap: down and up
        self.assertEqual(self.out["offset_parse"], 0)   # past 65535 is fine with the offset flag

    def test_no_touch_leaves_a_passcode_entry(self) -> None:
        # mgcommands.h, UI input: while a passcode keypad or any passcode entry is on the gadget's screen no touch
        # event (and so no HID report) carries a touch. A touch under way ends at its last point before (up at 30,40),
        # nothing goes out while it's up, a finger still down after it closes stays out until it lifts, then a new
        # touch is a down.
        down, move, up = 0, 1, 2
        self.assertEqual(self.out["touch_held"], [[down, 10, 20], [move, 30, 40], [up, 30, 40], [], [], [], [], [],
                                                  [], [down, 200, 210], [up, 200, 210]])
        ble = (MG / "mg_ble.c").read_text()
        touch = ble[ble.index("void mg_ble_touch(bool pressed, uint16_t x, uint16_t y)"):]
        touch = touch[:touch.index("\n}\n")]
        self.assertIn("bool held = s_platform.touch_private && s_platform.touch_private();", touch)
        self.assertIn("mg_touch_feed_held(&s_touch, held, pressed, x, y, now, &ev)", touch)
        self.assertNotIn("mg_touch_feed(&s_touch", touch)
        glue = (ROOT / "main" / "mg_glue.c").read_text()
        self.assertIn("platform.touch_private = muse_touch_private;", glue)
        mt = (ROOT / "components" / "muse" / "muse_touch.c").read_text()
        self.assertIn("return muse_passcode_locked() || muse_passcode_ui_covering();", mt)
        self.assertEqual(mt.count("hold_back();"), 2)   # LVGL's read and the video view's poll
        self.assertIn("i <= steps && !passcode_owns(); i++)", mt)   # an injected touch stops at one too

    def test_consumer_key(self) -> None:
        self.assertEqual(self.out["consumer"], [[4, 0x24, 0x02], [4, 0, 0]])


    def test_hid_map_parsing(self) -> None:
        m = self.maps
        self.assertEqual((m["off"]["err"], m["off"]["mode"]), (0, 0))
        d = m["digitizer"]
        self.assertEqual((d["err"], d["mode"], d["x"], d["y"], d["w"], d["h"], d["flags"]),
                         (0, 1, 0x4000, 0x2000, 0x8000, 0x6000, 1))
        r = m["rel"]
        self.assertEqual((r["err"], r["mode"], r["rel_w"], r["rel_h"]), (0, 3, 1000, 2160))
        self.assertEqual(m["short"]["err"], 2)        # invalid_length
        self.assertEqual(m["empty"]["err"], 2)
        self.assertEqual(m["bad_mode"]["err"], 3)     # invalid_value
        self.assertEqual(m["past_edge"]["err"], 3)
        self.assertEqual(m["rel_no_cal"]["err"], 2)
        self.assertEqual(m["rel_too_far"]["err"], 3)   # invalid_value: a park and steps the queue can't hold


    def test_report_map(self) -> None:
        reports = parse_report_map(bytes(self.out["report_map"]))
        self.assertEqual(set(reports), {1, 2, 3, 4})
        # 4: consumer control, one 16-bit usage (AC Back 0x224, AC Home 0x223).
        self.assertEqual(reports[4]["app"], (0x0C, 0x01))
        self.assertEqual(reports[4]["input"], 16)
        # 1: Digitizers / Touch Screen, 7-byte input, 1-byte feature (contact count maximum).
        dig = reports[1]
        self.assertEqual(dig["app"], (0x0D, 0x04))
        self.assertEqual((dig["input"], dig["feature"]), (7 * 8, 8))
        usages = [u for f in dig["fields"] for u in f["usages"]]
        for u in ((0x0D, 0x42), (0x0D, 0x32), (0x0D, 0x51), (0x01, 0x30), (0x01, 0x31), (0x0D, 0x54), (0x0D, 0x55)):
            self.assertIn(u, usages)
        xy = next(f for f in dig["fields"] if (0x01, 0x30) in f["usages"])
        self.assertEqual((xy["lmin"], xy["lmax"], xy["size"]), (0, 32767, 16))
        # 2: an absolute mouse, 5 bytes: three buttons and X, Y 0..32767.
        self.assertEqual(reports[2]["app"], (0x01, 0x02))
        self.assertEqual((reports[2]["input"], reports[2]["feature"]), (5 * 8, 0))
        axy = next(f for f in reports[2]["fields"] if (0x01, 0x30) in f["usages"])
        self.assertEqual((axy["lmax"], axy["relative"]), (32767, False))
        # 3: a relative mouse, 4 bytes: buttons, X, Y and the wheel, -127..127.
        self.assertEqual(reports[3]["app"], (0x01, 0x02))
        self.assertEqual(reports[3]["input"], 4 * 8)
        rel = next(f for f in reports[3]["fields"] if (0x01, 0x30) in f["usages"])
        self.assertEqual((rel["lmin"], rel["lmax"], rel["relative"]), (-127, 127, True))
        self.assertIn((0x01, 0x38), rel["usages"])

    def test_digitizer_reports(self) -> None:
        reps = self.out["digitizer"]
        self.assertEqual(len(reps), 3)
        down, move, up = reps
        self.assertEqual(down[:3], [1, 3, 0])            # tip + in range, contact 0
        x = down[3] | down[4] << 8
        self.assertAlmostEqual(x / 32767, 0.501, delta=0.002)
        self.assertEqual(down[7], 1)                      # contact count
        self.assertEqual(up[1], 0)                        # lifted: tip and in range clear
        self.assertEqual(up[3:7], move[3:7])              # where it was last

    def test_digitizer_same_spot_twice(self) -> None:
        # The kernel drops a position that repeats; Android then takes the new contact as (0, 0).
        d1, u1, d2, u2 = self.out["digitizer_twice"]
        p = lambda r: (r[3] | r[4] << 8, r[5] | r[6] << 8)
        # Both axes of the second down differ from where the first contact was, by one count.
        self.assertEqual(p(u1), p(d1))
        self.assertEqual(abs(p(d2)[0] - p(d1)[0]), 1)
        self.assertEqual(abs(p(d2)[1] - p(d1)[1]), 1)
        self.assertEqual(p(u2), p(d2))

    def test_hid_service_at_connect(self) -> None:
        # Only a bonded peer sees a saved HID service at connect; anyone else
        # (an iPhone asked to pair on every connection) waits for its own hid_map.
        self.assertEqual(self.out["show_at_connect"], [0, 0, 0, 1])

    def test_mouse_abs_reports(self) -> None:
        reps = self.out["mouse_abs"]
        self.assertEqual([r[1] for r in reps], [0, 1, 1, 0])   # there first, press, drag, release
        self.assertEqual(reps[0][2:], reps[1][2:])

    def test_mouse_rel_park_and_steps(self) -> None:
        reps = self.out["mouse_rel"]
        def signed(b: int) -> int:
            return b - 256 if b > 127 else b
        park = [r for r in reps if signed(r[2]) == -127 and signed(r[3]) == -127]
        self.assertGreaterEqual(len(park) * 127, 2000)   # enough to cross the whole screen (2000 counts down)
        self.assertEqual(reps[:len(park)], park)          # parking comes first
        rest = reps[len(park):]
        for r in rest:
            self.assertLessEqual(abs(signed(r[2])), 8)
            self.assertLessEqual(abs(signed(r[3])), 8)
        # Up to the press: the walk to the centre in counts, button up.
        press = next(i for i, r in enumerate(rest) if r[1] == 1)
        walk = rest[:press]
        self.assertTrue(all(r[1] == 0 for r in walk))
        sx = sum(signed(r[2]) for r in walk)
        sy = sum(signed(r[3]) for r in walk)
        self.assertEqual(sx, round(((2 * 233 + 1) / 466) / 2 * 1000))
        self.assertEqual(sy, round(((2 * 233 + 1) / 466) / 2 * 2000))
        self.assertEqual(rest[press][2:4], [0, 0])
        # The drag right keeps the button down, then the release.
        drag = rest[press + 1:-1]
        self.assertTrue(drag and all(r[1] == 1 for r in drag))
        self.assertEqual(sum(signed(r[2]) for r in drag), round((2 * 300 + 1) / 466 / 2 * 1000) - sx)
        self.assertEqual(rest[-1][1:4], [0, 0, 0])

    def test_mouse_rel_big_screen_fits(self) -> None:
        reps = self.out["mouse_rel_far"]
        self.assertEqual(self.dropped["mouse_rel_far"], 0)
        self.assertLessEqual(len(reps), 512)
        self.assertEqual(reps[-1][1], 1)   # it got as far as the press

    def test_off_sends_nothing(self) -> None:
        self.assertEqual(self.out["off"], [])


class MgHidBuildTest(unittest.TestCase):
    def test_hid_service_per_connection(self) -> None:
        # The HID service's visibility is decided per connection, before
        # mg_hogp applies it and before any discovery: by the peer's bond and
        # the saved flag, never by the saved map alone. A bonded host the
        # controller didn't resolve at connect gets it at encryption.
        ble = (MG / "mg_ble.c").read_text()
        at = ble.index("hid_visibility_for(ev->connect.conn_handle);")
        self.assertLess(at, ble.index("if (mg_hogp_gap_event(ev))"), "decided before mg_hogp applies it")
        body = ble[ble.index("static void hid_visibility_for("):ble.index("static void hid_show_on_encryption(")]
        self.assertIn("mg_hogp_peer_bonded(conn)", body)
        self.assertIn("mg_hid_show_at_connect(bonded, s_hid_saved_shown)", body)
        self.assertIn("mg_hogp_set_visible(show)", body)
        self.assertIn("ev->type == BLE_GAP_EVENT_ENC_CHANGE && ev->enc_change.status == 0) {\n        hid_show_on_encryption();", ble)
        self.assertNotIn("hid_hide_if_unbonded", ble)

    def test_bond_store_and_host_stack(self) -> None:
        # NimBLE's peer-address records outlive their bonds (IDF v6.0.1), and
        # ble_store_util_status_rr() answers their overflow by unpairing the
        # oldest bond, the phone that has just paired when it's the only one:
        # mg_hogp's own policy must stand in front of it.
        hogp = (MG / "mg_hogp.c").read_text()
        self.assertIn("store_status_cb = store_status;", hogp)
        self.assertNotIn("store_status_cb = ble_store_util_status_rr", hogp)
        self.assertRegex(hogp, r"BLE_STORE_OBJ_TYPE_PEER_ADDR\)\s*\{[^}]*return BLE_HS_ESTORE_CAP;")
        # Every overlay that turns HID on gives the host task room for
        # pairing's key exchange and our GAP events' logging (4 KB overflowed).
        overlays = [p for p in (ROOT / "devices").glob("sdkconfig.*")
                    if re.search(r"^CONFIG_MUSE_GADGET_BLE_(HID|VIDEO)=y", p.read_text(), re.M)]
        self.assertTrue(overlays)
        for p in overlays:
            m = re.search(r"^CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=(\d+)", p.read_text(), re.M)
            self.assertTrue(m and int(m.group(1)) >= 6144, p.name)


if __name__ == "__main__":
    unittest.main()
