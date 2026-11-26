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

"""Gadget rules that fail closed, checked in the source:

- A gadget set up with no proof key fails closed. mg_proto's owner_ok refuses it (its run is in
  test_mg_session, "owner fails closed"); mg_ble counts Link setup's marker as set up, so Bluetooth updates stay
  gated; and the Wi-Fi gadget's boot neither promotes a pair without the marker (a setup cut short between its
  tokens and its key) nor keeps one set up with no key: both start over, as the BLE-only gadget's boot does.
- A build without the offline queue erases the mg_queue partition at a setup's forget, so an earlier image's
  clips don't stay for the next owner.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = (ROOT / "main" / "app.c").read_text()
MG = ROOT / "components" / "muse_gadget_ble"


def fn(src: str, head: str) -> str:
    body = src[src.index(head):]
    return body[:body.index("\n}\n")]


class SetUpWithNoKeyFailsClosed(unittest.TestCase):
    def test_the_owner_check_and_set_up(self) -> None:
        proto = (MG / "mg_proto.c").read_text()
        own = fn(proto, "static bool owner_ok(const mg_proto_t *p)")
        self.assertIn("if (have_key(p)) {\n        return p->proof.state == MG_PROOF_MATCHED;", own)
        self.assertIn("return !(proof_supported(p) && p->ops.set_up && p->ops.set_up(p->ops.ctx));", own)
        ble = (MG / "mg_ble.c").read_text()
        changed = fn(ble, "void mg_ble_setup_changed(void)")
        self.assertIn("s_set_up = (s_platform.proof_key && s_platform.proof_key(k)) || (s_platform.set_up && "
                      "s_platform.set_up());", changed)
        self.assertIn("ops.set_up = op_set_up;", ble)
        # Bluetooth updates don't follow it: set up or not, one prompted press (test_mg_smp GlueMayChange).
        self.assertIn("mg_smp_gate(safe, s_platform.safe_updates_open, ok, SMP_RELEASE)", ble)
        glue = (ROOT / "main" / "mg_glue.c").read_text()
        self.assertEqual(glue.count(".set_up = set_up_or_unreadable,"), 2)   # SAFE and normal

    def test_the_boot_never_keeps_set_up_with_no_key(self) -> None:
        self.assertIn("#if CONFIG_MUSE_GADGET_BLE_AUDIO\n#define GADGET_PROOF 1\n#else\n#define GADGET_PROOF 0", APP)
        # The legacy pair (no marker) is promoted only where there's no proof key to have.
        self.assertIn("if (!setup_complete && provisioned && have_wifi && !wifi_skipped && !GADGET_PROOF) {", APP)
        boot = APP[APP.index("    xTaskCreate(ota_verify_task, \"ota_verify\", 4096, NULL, 4, NULL);\n\n    bool setup_complete = config_setup_complete();"):]
        boot = boot[:boot.index("bool skip_boot_scan = false;")]
        clear = boot.index('if (GADGET_PROOF && setup_complete && config_key_lookup("mg_proof_k") == CONFIG_KEY_NOT_FOUND) {')   # a read error never clears
        self.assertLess(clear, boot.index("// Restore endpoints only after saved setup has been validated or repaired."))
        tail = boot[clear:]
        self.assertLess(tail.index("config_clear_setup()"), tail.index("setup_complete = false;"))
        # The commit stores the key before the marker, which goes last (commit_setup).
        commit = fn(APP, "static bool commit_setup(void) {")
        self.assertLess(commit.index('config_set_str("mg_proof_k", hex)'), commit.index("config_mark_setup_complete()"))


class AnUnreadableKeyOrMarkerFailsClosed(unittest.TestCase):
    """An NVS read error (or a malformed key) is never "no key": no proof matches, and set_up() says
    set up, so the owner check fails closed. The marker's own row is link_config_store_harness's
    test_setup_marker_fails_closed."""

    def test_the_glue(self) -> None:
        glue = (ROOT / "main" / "mg_glue.c").read_text()
        key = glue[glue.index("static bool proof_key(uint8_t k[MG_TOKEN_PROOF_KEY_LEN]) {"):]
        key = key[:key.index("\n}\n")]
        self.assertIn('if (!ok && config_key_lookup(config_reprovision_open() ? "rp_k" : "mg_proof_k") != '
                      "CONFIG_KEY_NOT_FOUND\n        && !atomic_exchange(&s_key_unreadable, true)) {", key)
        self.assertIn("if (!ok) mg_token_proof_wipe(k, MG_TOKEN_PROOF_KEY_LEN);", glue)
        self.assertEqual(glue.count(".set_up = set_up_or_unreadable,"), 2)   # SAFE and normal
        su = glue[glue.index("static bool set_up_or_unreadable(void) {"):]
        su = su[:su.index("\n}\n")]
        self.assertLess(su.index("if (config_setup_complete_or_unreadable()) {"), su.index("bool have = proof_key(k);"))
        self.assertIn("return have || atomic_load(&s_key_unreadable);", su)
        self.assertIn("mg_token_proof_wipe(k, sizeof(k));", su)
        store = (ROOT / "main" / "config_store.c").read_text()
        f = store[store.index("bool config_setup_complete_or_unreadable(void) {"):]
        f = f[:f.index("\n}\n")]
        self.assertIn("case CONFIG_KEY_NOT_FOUND: return false;", f)
        self.assertIn("default: return true;", f)


class SafeRefusesTheMuseGattOnThePublicAddress(unittest.TestCase):
    def test_both_platforms_take_the_refusal(self) -> None:
        # The public address refusal is SAFE's (ble_server_muse_gatt_refused is s_safe_identity && s_safe_public),
        # but only the normal platform wired it; SAFE's musegadgets service took writes on the public address.
        glue = (ROOT / "main" / "mg_glue.c").read_text()
        safe = glue[glue.index("static void start_safe(void) {"):]
        safe = safe[:safe.index("};")]
        self.assertIn(".muse_gatt_refused = ble_server_muse_gatt_refused,", safe)
        self.assertEqual(glue.count(".muse_gatt_refused = ble_server_muse_gatt_refused,"), 2)
        ble = (ROOT / "components" / "muse_gadget_ble" / "mg_ble.c").read_text()
        self.assertIn("if (s_platform.muse_gatt_refused && s_platform.muse_gatt_refused()) {", ble)


class ALateRevocationNeverUnpairsANewerPair(unittest.TestCase):
    def test_the_revocation_compares_the_pair(self) -> None:
        # A Wi-Fi refresh of the older pair revoked after the device session saved a newer one must not unpair the
        # newer one. The store's half is link_token_pair_lock_harness's "the newer pair stays".
        f = fn(APP, "static bool ensure_access_token_ready_with_gate_held(")
        rev = f[f.index("if (rc == VM_API_ERR_REVOKED) {"):]
        self.assertIn("bool same = refresh_token_unchanged_locked(refresh);", rev)
        self.assertIn('if (same) handle_device_token_revoked_unlocked("device token refresh");', rev)
        self.assertLess(rev.index("session_tokens_lock_take();"), rev.index("bool same = "))
        self.assertLess(rev.index("if (same) handle_device_token_revoked_unlocked"), rev.index("session_tokens_lock_give();"))
        self.assertIn("access_expires_from(tokens.expires_in_s, tokens.server_date_s), refresh);", f)


class ThePairLocksCantFailToAllocate(unittest.TestCase):
    def test_static_mutexes(self) -> None:
        # A NULL lock (an allocation failure) made take/give no-ops, ending the pair's locking silently.
        run = APP[APP.index("void app_run(void) {"):]
        run = run[:run.index("if (!s_operation_gate ||")]
        for lock in ("s_auth_lock = xSemaphoreCreateMutexStatic(&auth_lock_buf);",
                     "s_setup_window_lock = xSemaphoreCreateMutexStatic(&setup_window_lock_buf);",
                     "s_session_tokens_lock = xSemaphoreCreateMutexStatic(&session_tokens_lock_buf);"):
            self.assertIn(lock, run)
        self.assertNotIn("xSemaphoreCreateMutex()", run)


class AFirstAccountTokenKeepsItsParts(unittest.TestCase):
    def test_the_parts_follow_on_one_connection(self) -> None:
        # On an unset gadget, a token's first part (hatch.token) made it set up, so the gate refused the
        # rest (hatch.token+): a 181-character token kept only 180 (a wireless line; the wired console skips the
        # window).
        src = (ROOT / "components" / "muse" / "muse_ble.c").read_text()
        run = src[src.index("static void run_command(char *cmd, muse_line_origin_t origin)"):]
        run = run[:run.index("\n}\n")]
        self.assertLess(run.index("bool was_set_up = set_up();"), run.index("muse_ble_gate_allows(cmd, was_set_up,"))
        self.assertIn('bool part = s_token_parts && !strcmp(cmd, "hatch.token+");', run)
        self.assertLess(run.index("s_token_parts = false;"), run.index("muse_ble_gate_allows("))   # any command ends them
        self.assertIn("} else if (!was_set_up || part) {\n            s_token_parts = true;", run)
        disc = src[src.index("case BLE_GAP_EVENT_DISCONNECT:"):]
        self.assertIn("s_token_parts = false;", disc[:disc.index("break;")])


class ARelayedServerUnpairResetsTheGadget(unittest.TestCase):
    def test_the_ble_start_wires_the_status(self) -> None:
        # noise_ctrl_ble_start() never set the status callback, so a BLE-only gadget dropped ws_unpaired.
        app = (ROOT / "main" / "app.c").read_text()
        self.assertLess(app.index("noise_ctrl_set_status_cb(on_ble_session_status);"), app.index("if (!noise_ctrl_ble_start("))
        cb = app[app.index("static void on_ble_session_status(const char *status) {"):]
        cb = cb[:cb.index("\n}\n")]
        self.assertIn('if (strcmp(status, "ws_unpaired") != 0) return;', cb)
        self.assertIn('xTaskCreate(ws_unpaired_task, "ws_unpair", 4096, NULL, 4, NULL)', cb)
        self.assertIn("atomic_store_explicit(&s_unpair_pending, true, memory_order_release);", cb)   # keeps Wi-Fi
        ble_only = app[app.index("noise_ctrl_set_status_cb(on_ble_session_status);"):]
        self.assertIn('reset_setup_from_control("deferred unpair", SETUP_RESET_KEEP_WIFI);', ble_only[:4000])
        nc = (ROOT / "main" / "noise_control.cpp").read_text()
        self.assertIn('extern "C" void noise_ctrl_set_status_cb(noise_ctrl_status_cb on_status) {\n    s_status_cb = on_status;', nc)


class ReachabilityOnlyWhereTheBuildAsks(unittest.TestCase):
    def test_the_setters_follow_the_switch(self) -> None:
        # The BLE-only boots set both reachability checks whatever MUSE_BOOTGUARD_BLE_REACHABLE said, so a build
        # with it off still restarted for being out of reach.
        src = (ROOT / "components" / "muse_restart" / "muse_bootguard.c").read_text()
        for head, field in (("void muse_bootguard_set_still_reachable(bool (*reachable_now)(void))", "s_still_reachable"),
                            ("void muse_bootguard_set_reachable(bool (*reachable)(void))", "s_reachable")):
            f = src[src.index(head):]
            f = f[:f.index("\n}\n")]
            self.assertLess(f.index("#if CONFIG_MUSE_BOOTGUARD_BLE_REACHABLE"), f.index(field + " = "))
            self.assertIn("#else", f)


class TheBootGuardsRecordIsChecked(unittest.TestCase):
    def test_set_erase_and_commit_are_checked(self) -> None:
        # keep_off(), kept_off()'s erase and muse_bootguard_reenable() reported success without checking NVS's
        # set/erase/commit.
        src = (ROOT / "components" / "muse_restart" / "muse_bootguard.c").read_text()
        keep = src[src.index("static void keep_off(void)"):]
        keep = keep[:keep.index("\n}\n")]
        self.assertIn("if (nvs_set_blob(h, NVS_OFF_KEY, esp_app_get_description()->app_elf_sha256, 8) != ESP_OK\n"
                      "        || nvs_commit(h) != ESP_OK) {", keep)
        kept = src[src.index("static bool kept_off(void)"):]
        kept = kept[:kept.index("\n}\n")]
        self.assertIn("if (nvs_erase_key(h, NVS_OFF_KEY) != ESP_OK || nvs_commit(h) != ESP_OK) {", kept)
        re = src[src.index("bool muse_bootguard_reenable_from(bool trusted)"):]   # the body (reenable() calls it trusted)
        re = re[:re.index("\n}\n")]
        self.assertIn("bool record_gone = erased == ESP_ERR_NVS_NOT_FOUND || (erased == ESP_OK && committed);", re)
        self.assertLess(re.index("muse_bootguard_rec_stable(&s_rec);"), re.index("if (!record_gone) {"))


class NoSecondProtoInit(unittest.TestCase):
    def test_the_player_fallback(self) -> None:
        # With no memory for the player, mg_ble_init() called mg_proto_init() again, wiping what the first
        # allocated (the credentials session) while the worker ran on it.
        ble = (MG / "mg_ble.c").read_text()
        self.assertEqual(ble.count("mg_proto_init(&s_proto, &cfg, &ops, &settings);"), 1)
        fb = ble[ble.index('ESP_LOGE(TAG, "no memory for the player");'):]
        fb = fb[:fb.index("\n    }\n")]
        self.assertIn("mg_lock();\n        mg_proto_drop_play(&s_proto);\n        mg_unlock();", fb)
        proto = (MG / "mg_proto.c").read_text()
        self.assertIn("void mg_proto_drop_play(mg_proto_t *p)\n{\n    p->cfg.play = NULL;", proto)


class NoQueueBuildsEraseOldClips(unittest.TestCase):
    def test_forget_erases_the_partition(self) -> None:
        ble = (MG / "mg_ble.c").read_text()
        forget = fn(ble, "bool mg_ble_queue_forget(const char *why)")
        alt = forget[forget.index("#else"):]
        self.assertIn('esp_partition_find_first(ESP_PARTITION_TYPE_DATA, QUEUE_PART_SUBTYPE, "mg_queue")', alt)
        self.assertIn("return stale_queue_wipe_start(part, why);", alt)
        # Durable: the mark before the wipe, cleared only once every sector erased, and a no-queue boot replays a
        # marked wipe (a queue image's mount_queue already does).
        start = fn(ble, "static bool stale_queue_wipe_start(const esp_partition_t *part, const char *why)")
        self.assertLess(start.index("bool marked = queue_wipe_mark((uint32_t)part->size);"),
                        start.index("xTaskCreate(stale_queue_wipe_task,"))
        self.assertEqual(ble.count("stale_queue_wipe_task"), 2)   # defined, started from stale_queue_wipe_start only
        self.assertEqual(ble.count("stale_queue_wipe_start("), 3)   # defined; the queue-less forget; the boot's replay
        task = fn(ble, "static void stale_queue_wipe_task(void *arg)")
        self.assertIn("} else {\n        queue_wipe_mark(0);", task)
        init = ble[ble.index("    mount_queue();\n#else"):]
        init = init[:init.index("#endif")]
        self.assertIn("if (queue_wipe_marked()) {", init)
        self.assertIn('stale_queue_wipe_start(part, "a wipe the last boot left unfinished");', init)
        self.assertLess(ble.index("static uint32_t queue_wipe_marked(void)"),
                        ble.index("#if CONFIG_MUSE_GADGET_BLE_QUEUE\n/*\n * The wipe at"))
        self.assertIn("for (size_t off = 0; off + sector <= part->size; off += sector) {", task)
        self.assertIn("esp_partition_erase_range(part, off, sector)", task)
        self.assertTrue(re.search(r"#if !CONFIG_MUSE_GADGET_BLE_QUEUE\nstatic void stale_queue_wipe_task", ble))


class DiscoverOfferedOnlyWhereItsHandled(unittest.TestCase):
    def test_same_condition(self) -> None:
        # A BLE-only build with the tunnel option on must not offer a command its handler isn't built for.
        nc = (ROOT / "main" / "noise_control.cpp").read_text()
        cond = "#if CONFIG_HOMEHUB_TUNNEL && !CONFIG_MUSE_GADGET_BLE_STANDALONE"
        self.assertIn(cond + "\n    // Offered only where app.c handles it", nc)
        self.assertIn(cond + '\n    if (strcmp(command, "device.discover") == 0) {', APP)


class TheEndOfLinkControlEndsTheSession(unittest.TestCase):
    def test_end_body_on_control(self) -> None:
        # Over the phone's relay no socket closes, so the end of the control stream has to end the session itself.
        nc = (ROOT / "main" / "noise_control.cpp").read_text()
        d = fn(nc, "static bool dispatch_inbound_frame(")
        self.assertIn("if (frame.stream_id == CTRL_STREAM_ID && frame.body_chunk.end_body) {", d)
        self.assertIn("if (frame.response.status == 200 && frame.stream_id == CTRL_STREAM_ID && frame.response.end_body) {", d)
        self.assertEqual(d.count('ESP_LOGW(TAG, "server ended the control stream'), 2)


class ARejectedRegistrationIsAnswered(unittest.TestCase):
    def test_no_longer_outstanding(self) -> None:
        # A link.register the VM rejected must not keep the BLE session busy forever.
        nc = (ROOT / "main" / "noise_control.cpp").read_text()
        self.assertIn("s_register_rejected = true;   /* answered: nothing waits for it any more */", nc)
        self.assertIn("return link.active && ((!s_register_acked && !s_register_rejected) || link.control_tx.json"
                      " || link.ping_pending);", nc)
        self.assertEqual(nc.count("    s_register_rejected = false;"), 2)   # both transports' new registration
        # Readiness and the tunnel still wait for an ack only.
        self.assertIn("if (s_register_acked && !s_req_ready.exchange(true)) {", nc)


class AnOwnerChangesCleanupSurvivesACut(unittest.TestCase):
    def test_marked_in_the_journal_and_replayed(self) -> None:
        # A power cut between a re-setup's commit and its owner cleanup must not keep the old passcode and face.
        commit = fn(APP, "static bool commit_setup(void) {")
        self.assertLess(commit.index('if (resetup && !config_set_str(OWNER_WIPE_KEY, "1")) return false;'),
                        commit.index("if (resetup && !config_reprovision_commit()) return false;"))
        self.assertLess(commit.index('bool forgot = passcode_forget("Link setup");'),
                        commit.index("s_owner_wipe_retire = forgot && links;   // the mark goes once the cleanup is verified"))
        # A committed owner change also forgets the apps, the bonds and the hand-set Muse server/token; never on a
        # rollback (after the commit point); a failed erase keeps the mark.
        self.assertLess(commit.index("if (resetup && !config_reprovision_commit()) return false;"),
                        commit.index('bool links = owner_change_forget("Link setup", true);'))
        self.assertLess(commit.index('bool links = owner_change_forget("Link setup", true);'),
                        commit.index("s_owner_wipe_retire = forgot && links;"))
        forget = fn(APP, "static bool owner_change_forget(const char *why, bool keep_phone) {")
        self.assertIn("#if CONFIG_MUSE_WASM_APPS\n    muse_apps_forget_all();\n#endif\n"
                      "    if (!keep_phone || !ble_server_forget_bonds_but_connected()) ble_server_forget_bonds();\n"
                      "#if CONFIG_MUSE_ENABLED\n    bool ok = muse_settings_forget_server();", forget)
        # An unpair and a reset still forget every bond (forget_owner_links is unchanged).
        self.assertIn("ble_server_forget_bonds();\n#if CONFIG_MUSE_ENABLED\n    return muse_settings_forget_server();",
                      fn(APP, "static bool forget_owner_links(void) {"))
        self.assertEqual(APP.count("owner_change_forget(\""), 2)   # the commit and the replay
        self.assertNotIn("config_erase_key(OWNER_WIPE_KEY)", commit)   # retired only once verified
        # The mark retires only once the face's erase worked and the passcode store is clean; the tick loops retire
        # one whose passcode save was still pending.
        ret = fn(APP, "static void owner_wipe_retire(void) {")
        self.assertLess(ret.index("if (!muse_passcode_store_clean()) return;"),
                        ret.index("(void)config_erase_key(OWNER_WIPE_KEY);"))
        self.assertEqual(APP.count("owner_wipe_retire();   /* a cleanup whose passcode save was still pending */")
                         + APP.count("owner_wipe_retire();   // a cleanup whose passcode save was still pending"), 2)
        pc = (ROOT / "components" / "muse" / "muse_passcode.c").read_text()
        self.assertIn("bool clean = !s_p.dirty;   /* a failed store stays dirty (store()) */", pc)
        self.assertIn("if (!muse_passcode_platform_save(&s_p.rec)) {\n        if (s_store_failed_ms < 0)", pc)
        self.assertIn("return erased;", fn((ROOT / "main" / "mg_face.c").read_text(), "bool mg_face_forget(const char *why)\n{"))
        run = APP[APP.index("void app_run(void) {"):]
        recover = 'if (config_reprovision_recover() == CONFIG_RECOVER_RESTORED) (void)config_erase_key("mg_q_forget");'
        self.assertLess(run.index("bool owner_wipe = config_key_lookup(OWNER_WIPE_KEY) == CONFIG_KEY_FOUND;"),
                        run.index(recover))
        self.assertLess(run.index("bool rolled_back = config_reprovision_open();"), run.index(recover))
        self.assertIn("if (owner_wipe && !rolled_back) {\n        s_owner_wipe_due = true;", run)
        rp = fn(APP, "static void owner_wipe_replay(void) {")
        self.assertIn('bool links = owner_change_forget("an owner change cut short", false);   // no phone to keep at boot\n'
                      '    s_owner_wipe_retire = forgot && links;', rp)
        self.assertLess(rp.index('passcode_forget("an owner change cut short");'), rp.index("owner_wipe_retire();"))
        self.assertNotIn("config_erase_key(OWNER_WIPE_KEY)", rp)
        self.assertEqual(APP.count("owner_wipe_replay();"), 2)   # the BLE-only loop and the Wi-Fi path
        # A commit that failed after the mark (the journal rolled back) never leaves it for the next boot.
        fail = fn(APP, "static void setup_fail_for_session(const char *stage, const char *status,")
        self.assertLess(fail.index("bool restored = config_reprovision_open() && config_reprovision_rollback();"),
                        fail.index("(void)config_erase_key(OWNER_WIPE_KEY);"))


if __name__ == "__main__":
    unittest.main()
