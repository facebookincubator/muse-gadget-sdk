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

"""What the panel's touches may reach, checked in the source:

- A phone's app (the BLE touch sink) gets only the touches on what it may see: a UI stream's picture, or the face
  with nothing over it. Settings, its keyboard, the passcode and the menu never reach it, and the log never says
  where a touch was.
- One tap on the card's Confirm pairs (a first setup or a re-setup, any tap); a tap elsewhere ends the prompt. The
  update card's Confirm takes a finger only, in a release build.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"


def body(text: str, head: str) -> str:
    i = text.index(head)
    return text[i:text.index("\n}\n", i)]


class ThePhoneGetsOnlyTheFacesTouches(unittest.TestCase):
    def test_the_sink_asks_first(self) -> None:
        touch = (MUSE / "muse_touch.c").read_text()
        feed = body(touch, "static void feed(const lv_indev_data_t *d)\n{")
        self.assertLess(feed.index("if (!ui_stream() && shareable && !shareable()) {"), feed.index("sink->sample("))
        ui = (MUSE / "muse_ui.c").read_text()
        self.assertIn("muse_touch_set_shareable(touch_shareable);", ui)
        share = body(ui, "static bool touch_shareable(void)\n{")
        for part in ("muse_ui_face_active()", "!muse_menu_is_open()", "!muse_passcode_ui_covering()",
                     "!muse_passcode_ui_locked_face()"):
            self.assertIn(part, share)

    def test_no_coordinates_in_the_log(self) -> None:
        ble = (ROOT / "components" / "muse_gadget_ble" / "mg_ble.c").read_text()
        self.assertNotIn('"touch %s at %u,%u"', ble)
        self.assertIn('ESP_LOGI(TAG, "touch %s", names[ev.action]);', ble)


class OneTapOnConfirmPairsAndATapElsewhereDoesNothing(unittest.TestCase):
    """A setup or a re-setup is confirmed by one tap on the card's Confirm (a generous hit area), whatever the tap (a
    finger or a bench press) and however long; a tap anywhere else does nothing, and the back gesture (a swipe) ends
    the prompt ("Not now"). The update card's Confirm still takes a finger only in a release build."""

    def test_the_confirm_paths(self) -> None:
        ui = (MUSE / "muse_ui.c").read_text()
        pair = body(ui, "static void on_pair_ok(lv_event_t *e)\n{")
        self.assertIn("if (lv_event_get_code(e) == LV_EVENT_PRESSED && s_pair_ok) {\n"
                      "        (void)muse_link_confirm_key_press();", pair)
        self.assertNotIn("touch_confirms", pair)
        # A generous hit area on the Confirm control; a tap elsewhere does nothing; the back gesture (a swipe)
        # dismisses.
        self.assertIn("lv_obj_set_ext_click_area(s_pair_code, on ? (s_w < s_h ? s_w : s_h) / 10 : 0);", ui)
        anyp = body(ui, "static void on_any_press(lv_event_t *e)\n{")
        self.assertNotIn("muse_link_confirm_dismiss", anyp)
        swipe = body(ui, "static void on_swipe_click(lv_event_t *e)\n{")
        self.assertIn("if (muse_link_state() == MUSE_LINK_CONFIRM && muse_link_confirm_dismiss()) {", swipe)
        # The card: Confirm on every touch board, a first setup or a re-setup; no hold anywhere.
        self.assertIn('strlcpy(hint, "Tap Confirm to pair", sizeof(hint));\n                ok = true;', ui)
        for gone in ("Hold 3 s", "muse_link_confirm_hold", "s_pair_ok_pressed_us"):
            self.assertNotIn(gone, ui)
        # The update card's Confirm (not a setup's): a finger only, in a release build.
        ok = body(ui, "static bool touch_confirms(void)\n{")
        self.assertIn("#if MUSE_NO_PERSON\n    return true;\n#else\n    return muse_touch_real();", ok)
        self.assertIn("if (!touch_confirms()) {\n            ESP_LOGW(TAG, \"update prompt: a real press is needed\");", anyp)


if __name__ == "__main__":
    unittest.main()
