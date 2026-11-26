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
"""Only the owner's phone reads the owner's content on a release build (protocols/mgcommands.h, Token proof).

The build check: every wireless read on ESP32 that returns the owner's content, each with its gate. A new read of
the owner's content goes on this list with its gate (and a host row), or the list fails. Test, bench and automation
builds are unchanged; identity and status reads stay open; touch events stay open.
"""

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MG = ROOT / "components" / "muse_gadget_ble"
VECTORS = ROOT.parent / "protocols" / "test-vectors" / "mg-session-v1.json"
RELEASE = "#if !CONFIG_MUSE_GADGET_AUTOMATION && !CONFIG_MUSE_GADGET_TEST_BUILD"


def body(src: str, head: str) -> str:
    i = src.index(head)
    return src[i:src.index("\n}\n", i)]


class EveryOwnerContentRead(unittest.TestCase):
    # (where, what): the wireless reads that print the owner's content, each proven-only on a set-up release gadget.
    READS = [
        ("mg_proto.c get_unix_time", "the local time and offset"),
        ("mg_proto.c get_settings client_name / client_app", "the phone's name and app"),
        ("mg_ble.c the Nordic UART log mirror", "every log line (session_image's mg.image paths, mg.voice, ...)"),
        ("mg_unlock.c mg_unlock_owner_word: !IN voice?, !IN img?", "voice activity, the shown image's VM path"),
        ("factory_test.c the button notify", "the gadget's presses"),
    ]

    def test_the_list(self) -> None:
        self.assertEqual(len(self.READS), 5)

    def test_get_unix_time(self) -> None:
        proto = (MG / "mg_proto.c").read_text()
        self.assertIn("if (a[0] == mg_device_action_get_unix_time && !proof_input_ok(p)) {\n", proto)

    def test_get_settings_client_fields(self) -> None:
        proto = (MG / "mg_proto.c").read_text()
        fn = body(proto, "static void cmd_get_settings(mg_proto_t *p, const uint8_t *a, size_t n)")
        # The filter is in both loops, at any parameter count (a count cap would let a long request through).
        self.assertIn("bool skip = !proof_input_ok(p);", fn)
        self.assertEqual(fn.count("if (skip && client_setting(a[i])) {"), 3)
        self.assertNotIn("sizeof(kept)", fn)
        self.assertNotIn("n <=", fn)
        h = (ROOT / "tests" / "mg_proto_harness.c").read_text()
        self.assertIn("owner reads, 70 parameters", h)   # the row that would catch a count cap
        self.assertIn("send_error(p, mg_command_get_settings, mg_error_code_proof_required);", fn)
        ble = (MG / "mg_ble.c").read_text()
        self.assertIn("    cfg.proof_inputs = true;", ble)   # the release build's switch

    def test_the_log_mirror(self) -> None:
        ble = (MG / "mg_ble.c").read_text()
        self.assertIn("s_nus_owner = !NUS_MIRROR_RELEASE || !s_set_up || mg_proto_proof_matched(&s_proto);", ble)
        self.assertIn("if (s_nus_sub && s_nus_ring && (s_nus_owner || (reply && reply == xTaskGetCurrentTaskHandle()))) {",
                      body(ble, "static int nus_vprintf("))
        # Identity and status stay open: only status, version, guard? and ota? reply to an unproven central, from the
        # handler's own task; a new link starts with an empty ring.
        acc = body(ble, "static int nus_access(")
        self.assertIn('bool open_read = !strcmp(line, "status") || !strcmp(line, "version") || !strcmp(line, "guard?") ||\n'
                      '                         !strcmp(line, "ota?");', acc)
        self.assertIn("s_nus_reply_task = NULL;", acc)
        self.assertIn("nus_ring_reset();   /* nothing queued for an earlier link goes to this one */", ble)
        self.assertIn("s_nus_owner = !NUS_MIRROR_RELEASE || !s_set_up;   /* a new link waits", ble)
        self.assertIn("#if CONFIG_MUSE_GADGET_AUTOMATION || CONFIG_MUSE_GADGET_TEST_BUILD\n#define NUS_MIRROR_RELEASE false",
                      ble)

    def test_the_owner_words(self) -> None:
        unlock = (MG / "mg_unlock.c").read_text()
        self.assertIn('return !strcmp(line, "!IN voice?") || !strcmp(line, "!IN img?");', unlock)
        self.assertIn("return !mg_unlock_owner_word(line) || !release || !set_up || proven;", unlock)
        ble = (MG / "mg_ble.c").read_text()
        cmd = body(ble, "static void nus_command(char *line)\n{")
        self.assertIn("bool owner = mg_unlock_owner_ok_in(line, mg_proto_proof_matched(&s_proto), s_set_up, true);", cmd)
        self.assertLess(cmd.index(RELEASE), cmd.index("mg_unlock_owner_ok_in("))

    def test_the_factory_press(self) -> None:
        # A set-up release gadget's presses are notified only while the factory service is open (unset, or the
        # owner's setup window; an unreadable store counts as set up); its device info is identity and stays open.
        ft = (ROOT / "main" / "factory_test.c").read_text()
        fn = body(ft, "void factory_test_on_button_press(void) {")
        self.assertLess(fn.index("if (!allowed_now()) {"), fn.index("ble_gatts_notify_custom("))
        self.assertIn(RELEASE, fn)
        self.assertIn("config_setup_complete_or_unreadable() || config_is_provisioned()",
                      body(ft, "static bool allowed_now(void) {"))
        self.assertNotIn("allowed_now()", body(ft, "static int ft_devinfo_access_cb("))

    def test_touch_events_stay_open(self) -> None:
        # Not the owner's content: touch_enable and the touch events answer any central (hid_map and hid_key keep
        # their proof gates).
        proto = (MG / "mg_proto.c").read_text()
        en = proto[proto.index("    case mg_ui_input_touch_enable:"):proto.index("    case mg_ui_input_hid_map: {")]
        self.assertNotIn("proof_required", en)
        self.assertNotIn("owner_ok", en)
        self.assertIn("return p->connected && p->touch_enabled;",
                      body(proto, "bool mg_proto_touch_enabled(const mg_proto_t *p)"))
        self.assertIn("if (!p->touch_enabled || !mg_proto_ready(p)) {",
                      body(proto, "bool mg_proto_touch(mg_proto_t *p, const mg_touch_event_t *ev, uint32_t time_ms, uint8_t id)"))

    def test_the_settings_vector_is_the_harness(self) -> None:
        ex = [e for e in json.loads(VECTORS.read_text())["settings"]["exchanges"]
              if e["name"] == "the owner's content without the proof, release build"][0]
        h = (ROOT / "tests" / "mg_proto_harness.c").read_text()
        fn = body(h, "static void test_owner_reads(void)")
        for st in ex["steps"]:
            if "write" in st:
                self.assertIn(f'"{st["write"]}"', fn, st.get("note"))
                for n in st["notify"]:
                    self.assertIn(f'"{n}"', fn, st.get("note"))


if __name__ == "__main__":
    unittest.main()
