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

"""Gadget setup over Muse Link (protocols/README.md): provision_v2 with Wi-Fi
optional or absent (main/ble_server.c), the token-only commit with its
rollback (main/app.c), and a BLE-only gadget's boot recovery. The real
blocks run on the host with storage, BLE and tasks replaced by fakes."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_link_unpair_storage_contract import _function_body  # noqa: E402
import tsan_run

ROOT = Path(__file__).resolve().parents[1]
MG = ROOT / "components/muse_gadget_ble"
PROTOCOLS = ROOT.parent / "protocols"
VECTORS = json.loads((PROTOCOLS / "test-vectors/mg-token-proof-v1.json").read_text(encoding="utf-8"))


def _between(source: str, start: str, end: str) -> str:
    i = source.index(start)
    return source[i:source.index(end, i)]


PROVISION_V2 = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "link_pairing.h"
#include "network_proxy.h"
#define mbedtls_platform_zeroize(p, n) memset((p), 0, (n))
#define cJSON_GetObjectItemCaseSensitive cJSON_GetObjectItem
/* A network's proxy (network_proxy.c and its own tests): none here. */
bool network_proxy_parse(const cJSON *object, network_proxy_config_t *out) {
    (void)object; memset(out, 0, sizeof(*out)); return true;
}
void network_proxy_wipe(void *data, unsigned len) { memset(data, 0, len); }
#ifndef cJSON_IsBool
#define cJSON_IsBool(x) (cJSON_IsTrue(x) || cJSON_IsFalse(x))
#endif
#define pdPASS 1
static int task_fails;
static void *last_task;
#define xTaskCreate(fn, name, stack, arg, prio, handle) \
    (task_fails ? 0 : (last_task = (arg), (void)(fn), 1))
#define helper_task_create xTaskCreate
#define PROVISION_STACK 8192
static char status[64];
static void ble_server_send_status(const char *s) { snprintf(status, sizeof(status), "%s", s); }
static void ble_server_send_pairing_status(const char *s, uint32_t g) { (void)g; ble_server_send_status(s); }
static void ble_server_disconnect_pairing_session(uint32_t g) { (void)g; }
static link_wifi_mode_t mode;
link_wifi_mode_t link_pairing_wifi_mode(void) { return mode; }
uint32_t link_pairing_mark_provisioning_active(void) { return 7; }
static void provision_task(void *arg) { (void)arg; }
#define TAG "ble"
#define ESP_LOGW(tag, ...) ((void)(tag))
static bool set_up;
static bool config_setup_complete(void) { return set_up; }
@TYPES@
static void provision_v2(const char *json) {
    cJSON *root = cJSON_ParseWithLength(json, strlen(json));
    assert(root);
    bool decrypted = true;
    const char *act = "provision_v2";
    status[0] = 0;
    last_task = NULL;
    if (0) {
@BRANCH@
    }
    delete_command_json(root, decrypted);
}
static provision_args_t *taken(void) { return (provision_args_t *)last_task; }
static void release(void) {
    provision_args_t *a = taken();
    assert(a);
    free(a->ssid); free(a->password); free(a->access_token); free(a->refresh_token);
    free(a->username); free(a->ota_url); free(a->api_url); free(a->api_url_v2); free(a->noise_host);
    free(a);
    last_task = NULL;
}
#define TOKENS "\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"device\""
#define EXTRA ",\"username\":\"u\",\"api_url_v2\":\"https://api\",\"noise_host\":\"noise\",\"ota_url\":\"https://ota\""
static void refused(const char *json, const char *why) {
    provision_v2(json);
    if (strcmp(status, why) != 0 || taken()) {
        fprintf(stderr, "mode %d: %s -> '%s' (task %p), want %s\n", mode, json, status, last_task, why);
        abort();
    }
}
int main(void) {
    /* Required (every build without gadget support): as before. */
    mode = LINK_WIFI_REQUIRED;
    provision_v2("{\"ssid\":\"home\",\"password\":\"pw\"," TOKENS EXTRA "}");
    assert(!status[0] && taken() && !strcmp(taken()->ssid, "home") && !strcmp(taken()->ota_url, "https://ota"));
    assert(!strcmp(taken()->username, "u") && taken()->session_generation == 7);
    release();
    /* Setting a set-up gadget up again never takes an image to install: ota_url ignored. */
    set_up = true;
    provision_v2("{\"ssid\":\"home\",\"password\":\"pw\"," TOKENS EXTRA "}");
    assert(!status[0] && taken() && (!taken()->ota_url || !*taken()->ota_url));
    release();
    set_up = false;
    refused("{" TOKENS "}", "error_missing_credentials");
    refused("{\"ssid\":\"\",\"password\":\"\"," TOKENS "}", "error_missing_credentials");
    refused("{\"ssid\":\"home\"," TOKENS "}", "error_missing_credentials");

    /* Optional: no ssid and no password is token-only; an ssid is the Wi-Fi flow. */
    mode = LINK_WIFI_OPTIONAL;
    const char *token_only[] = {
        "{" TOKENS EXTRA "}",
        "{\"ssid\":\"\",\"password\":\"\"," TOKENS EXTRA "}",
        "{\"ssid\":\"\"," TOKENS EXTRA "}",
    };
    for (size_t i = 0; i < sizeof(token_only) / sizeof(token_only[0]); i++) {
        provision_v2(token_only[i]);
        provision_args_t *a = taken();
        assert(!status[0] && a && !a->ssid && !a->password && !strcmp(a->access_token, "a"));
        assert(!strcmp(a->refresh_token, "r") && !a->ota_url);          /* OTA ignored */
        assert(!strcmp(a->username, "u") && !strcmp(a->api_url_v2, "https://api")
               && !strcmp(a->noise_host, "noise"));                      /* kept for later Wi-Fi */
        release();
    }
    provision_v2("{\"ssid\":\"home\",\"password\":\"pw\"," TOKENS EXTRA "}");
    assert(!status[0] && taken() && !strcmp(taken()->ssid, "home") && taken()->ota_url);
    release();
    refused("{\"ssid\":\"home\"," TOKENS "}", "error_missing_credentials");
    refused("{\"password\":\"pw\"," TOKENS "}", "error_missing_credentials");
    refused("{\"ssid\":null," TOKENS "}", "error_missing_credentials");
    refused("{\"access_token\":\"a\",\"token_type\":\"device\"}", "error_missing_credentials");
    refused("{\"access_token\":\"a\",\"refresh_token\":\"r\"}", "error_missing_credentials");
    refused("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"user\"}", "error_missing_credentials");

    /* None: an ssid is refused, endpoints and username ignored. */
    mode = LINK_WIFI_NONE;
    provision_v2("{\"ssid\":\"\",\"password\":\"\"," TOKENS EXTRA "}");
    provision_args_t *a = taken();
    assert(!status[0] && a && !a->ssid && !a->ota_url && !a->username && !a->api_url_v2 && !a->noise_host);
    release();
    refused("{\"ssid\":\"home\",\"password\":\"pw\"," TOKENS "}", "error_wifi_unsupported");
    refused("{\"password\":\"pw\"," TOKENS "}", "error_wifi_unsupported");
    refused("{\"ssid\":\"home\"}", "error_missing_credentials");   /* the tokens come first */

    /* A provisioning task that can't start. */
    task_fails = 1;
    refused("{" TOKENS "}", "error_operation_in_progress");
    printf("PASS provision_v2\n");
    return 0;
}
"""

TOKEN_ONLY = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link_pairing.h"
#include "mg_token_proof.h"
#define CONFIG_MUSE_GADGET_BLE_AUDIO 1
#define TAG "app"
static void log_sink(const char *fmt, ...) { (void)fmt; }
#define ESP_LOGI(tag, ...) log_sink(__VA_ARGS__)
#define ESP_LOGW(tag, ...) log_sink(__VA_ARGS__)
#define ESP_LOGE(tag, ...) log_sink(__VA_ARGS__)
#define LED_STATE_AUTH_OK 0
typedef enum { CONFIG_KEY_LOOKUP_ERROR = -1, CONFIG_KEY_NOT_FOUND = 0, CONFIG_KEY_FOUND = 1 } config_key_lookup_t;

/* NVS: a small string store with one key that can be made to fail. */
static char keys[32][16], vals[32][128];
static const char *fail_key;
static int find(const char *k) {
    for (int i = 0; i < 32; i++) if (keys[i][0] && !strcmp(keys[i], k)) return i;
    return -1;
}
static bool config_set_str(const char *k, const char *v) {
    if (fail_key && !strcmp(fail_key, k)) return false;
    int i = find(k);
    for (int j = 0; i < 0 && j < 32; j++) if (!keys[j][0]) i = j;
    assert(i >= 0 && strlen(v) < sizeof(vals[0]));
    snprintf(keys[i], sizeof(keys[i]), "%s", k);
    snprintf(vals[i], sizeof(vals[i]), "%s", v);
    return true;
}
static bool config_erase_key(const char *k) {
    if (fail_key && !strcmp(fail_key, k)) return false;
    int i = find(k);
    if (i >= 0) keys[i][0] = 0;
    return true;
}
static const char *get(const char *k) { int i = find(k); return i < 0 ? NULL : vals[i]; }
static config_key_lookup_t config_key_lookup(const char *k) { return find(k) < 0 ? CONFIG_KEY_NOT_FOUND : CONFIG_KEY_FOUND; }
/* The re-provisioning journal (config_store.c, tested on its own against
 * link_reprovision_journal_harness.c): here a snapshot of the store. */
static char saved_keys[32][16], saved_vals[32][128];
static bool journal_open;
static const char *journal_fails;   /* "begin" or "commit" */
static bool config_reprovision_open(void) { return journal_open; }
static bool config_reprovision_begin(void) {
    if (journal_fails && !strcmp(journal_fails, "begin")) return false;
    memcpy(saved_keys, keys, sizeof(keys));
    memcpy(saved_vals, vals, sizeof(vals));
    journal_open = true;
    return true;
}
static bool config_reprovision_commit(void) {
    if (journal_fails && !strcmp(journal_fails, "commit")) return false;
    journal_open = false;
    return true;
}
static bool config_reprovision_rollback(void) {
    if (journal_open) {
        memcpy(keys, saved_keys, sizeof(keys));
        memcpy(vals, saved_vals, sizeof(vals));
    }
    journal_open = false;
    return true;
}
static bool config_clear_setup(void) { memset(keys, 0, sizeof(keys)); journal_open = false; return true; }
/* The device session's token updates are held from a re-setup's begin to its
 * end (app.c: reprovision_begin_holding_session_tokens()). */
static bool tokens_held;
static bool reprovision_begin_holding_session_tokens(void) {
    tokens_held = config_reprovision_begin();
    return tokens_held;
}
static void session_tokens_release(void) { tokens_held = false; }
#define OWNER_WIPE_KEY "owner_wipe"   /* app.c: an owner change's cleanup, marked before the commit */
/* A failed re-setup's servers back in RAM (tests/test_link_setup_failure_servers.py): not this harness's. */
static void disconnect_vm_transports(void) {}
static void endpoints_from_store(void) {}
static bool config_mark_setup_complete(void) { return config_set_str("setup_complete", "1"); }
static bool config_setup_complete(void) { return get("setup_complete") && !strcmp(get("setup_complete"), "1"); }
static bool config_mark_wifi_skipped(void) { return config_set_str("setup_wifi", "skipped"); }
static bool config_wifi_skipped(void) { return get("setup_wifi") && !strcmp(get("setup_wifi"), "skipped"); }
static bool config_is_provisioned(void) { return get("access_token") != NULL; }
static int count(void) { int n = 0; for (int i = 0; i < 32; i++) n += keys[i][0] != 0; return n; }

/* Link and the app around the commit. */
static link_wifi_mode_t mode;
static bool session_valid = true;
static int stops, setup_changes, fresh;
static char events[8][64];
static int nevents;
static void record(const char *what) {
    assert(nevents < 8);
    snprintf(events[nevents++], sizeof(events[0]), "%s%s", what, config_setup_complete() ? "+committed" : "");
}
link_wifi_mode_t link_pairing_wifi_mode(void) { return mode; }
bool link_pairing_mgcommands(void) { return true; }
bool link_pairing_commit_provisioning(uint32_t g, bool (*commit)(void)) { return g != 0 && session_valid && commit(); }
/* The id device_info offers (a new setup's, identity.h) and the one in use:
 * the stored one, as identity.c reads it. */
static const char *identity_setup_node_id(void) { return "@NODE@"; }
static int refreshes, noise_node_sets;
static bool identity_refresh_node_id(void) { refreshes++; return true; }
static const char *identity_node_id(void) { return get("node_id") ? get("node_id") : "@NODE@"; }
static void noise_ctrl_set_node_id(const char *node) { assert(!strcmp(node, identity_node_id())); noise_node_sets++; }
static int disconnects;
static void ble_server_disconnect_pairing_session(uint32_t g) { assert(g != 0); disconnects++; }
static void identity_note_paired_sdk_token(void) { config_set_str("pair_sdk_hint", "mgst_hint"); }
static void setup_fail_for_session(const char *stage, const char *status, uint32_t g) {
    (void)stage; assert(g != 0);
@FAIL_RESTORE@
    bool cleared = restored || config_clear_setup();   /* as the app's: setup_wipe_to_clean() */
    char what[64];
    snprintf(what, sizeof(what), "fail:%s", cleared ? status : "error_storage");
    record(what);
}
static bool require_provisioning_pairing_session(uint32_t g) {
    if (session_valid) return true;
    setup_fail_for_session("pairing", "auth_failed", g);
    return false;
}
static void auth_lock_take(void) {}
static void auth_lock_give(void) {}
static void note_access_token_fresh(void) { fresh++; }
static void store_pairing_username_unlocked(const char *u) {
    if (u && *u) config_set_str("username", u); else config_erase_key("username");
}
static void setup_stage_set(const char *s) { (void)s; }
static void ui_set_status(const char *s) { (void)s; }
static void led_status_set_state(int s) { (void)s; }
static bool changed_while_held;
static void mg_glue_setup_changed(void) { setup_changes++; changed_while_held = tokens_held; }
static void ble_server_send_pairing_status(const char *s, uint32_t g) { assert(g != 0); record(s); }
static void stop_setup_ble(const char *why) { (void)why; stops++; }
/* The passcode lock goes with a completed setup (app.c), and the clips with their mark. */
static int passcode_forgets;
static bool passcode_forget(const char *why) { (void)why; passcode_forgets++; (void)config_erase_key("mg_q_forget"); return true; }
static bool s_owner_wipe_retire;   /* app.c: the mark retires once the cleanup is verified */
/* A committed owner change's apps, bonds and hand-set Muse server/token (app.c: owner_change_forget()). */
static int owner_forgets;
static bool owner_forget_fails, owner_forget_kept_phone;
static bool owner_change_forget(const char *why, bool keep_phone) {
    (void)why;
    owner_forgets++;
    owner_forget_kept_phone = keep_phone;
    return !owner_forget_fails;
}
static void owner_wipe_retire(void) { if (s_owner_wipe_retire) { s_owner_wipe_retire = false; config_erase_key(OWNER_WIPE_KEY); } }

@COMMIT@

static void provision_token_only_with_gate_held(const char *access_token, const char *refresh_token,
                                                const char *username, const char *api_url,
                                                const char *api_url_v2, const char *noise_host,
                                                uint32_t session_generation) {
@TOKEN_ONLY@
}
static bool standalone_boot(void) {
@STANDALONE@
    return setup_complete;
}
/* on_provision's start on a set-up gadget: the journal opens before anything
 * is written. */
static bool provision_gate(uint32_t session_generation) {
@GATE@
    return true;
done:
    return false;
}
static void reset(void) {
    config_clear_setup();
    fail_key = NULL;
    journal_fails = NULL;
    session_valid = true;
    stops = setup_changes = fresh = nevents = disconnects = passcode_forgets = owner_forgets = 0;
    owner_forget_fails = false;
    tokens_held = false;
    s_consumed_session = 0;   /* a fresh boot */
}
static void run(void);
static char k2_hex[65];
static void run2(void) {
    assert(provision_gate(10));
    provision_token_only_with_gate_held("second-access", "second-refresh", "", "", "", "", 10);
}
/* Set up with the first token pair; then a second setup on the set-up gadget. */
static void set_up_first(link_wifi_mode_t m) {
    mode = m;
    reset();
    run();
    assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed") && !journal_open);
    assert(owner_forgets == 0);   /* a first setup changes no owner */
    stops = setup_changes = fresh = nevents = 0;
}
static void assert_first_setup(const char *when) {
    if (!get("access_token") || strcmp(get("access_token"), "@ACCESS@") || strcmp(get("refresh_token"), "refresh-token")
        || strcmp(get("mg_proof_k"), "@K@") || !get("node_id") || strcmp(get("node_id"), "@NODE@")
        || !config_setup_complete() || !config_wifi_skipped() || journal_open || tokens_held) {
        fprintf(stderr, "%s: the first setup is gone\n", when);
        abort();
    }
}
static void run(void) {
    provision_token_only_with_gate_held("@ACCESS@", "refresh-token", "user@example.com", "https://old",
                                        "https://api", "noise.example", 9);
}
int main(void) {
    /* Optional Wi-Fi: everything in one commit, auth_ok only after it. */
    mode = LINK_WIFI_OPTIONAL;
    reset();
    run();
    assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed"));
    assert(!strcmp(get("access_token"), "@ACCESS@") && !strcmp(get("refresh_token"), "refresh-token"));
    assert(!strcmp(get("mg_proof_k"), "@K@"));
    assert(config_wifi_skipped() && config_setup_complete() && !get("ssid"));
    assert(!strcmp(get("username"), "user@example.com") && !strcmp(get("api_url_v2"), "https://api")
           && !strcmp(get("noise_host"), "noise.example") && !strcmp(get("api_url"), "https://old"));
    assert(stops == 1 && setup_changes == 1 && fresh == 1);
    assert(passcode_forgets == 1);   /* a completed setup erases the passcode */

    /* No Wi-Fi at all: no endpoints or username. */
    mode = LINK_WIFI_NONE;
    reset();
    run();
    assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed") && count() == 7);
    assert(!strcmp(get("node_id"), "@NODE@"));   /* with K, in the same commit */
    assert(!get("username") && !get("api_url_v2") && !get("noise_host") && !strcmp(get("mg_proof_k"), "@K@"));
    assert(!strcmp(get("pair_sdk_hint"), "mgst_hint"));   /* which SDK token setup handed over */

    /* Any write failing: error_storage, nothing left, no auth_ok, advertising stays. */
    const char *failing[] = {"setup_wifi", "refresh_token", "access_token", "auth_token", "mg_proof_k",
                             "node_id", "setup_complete", "api_url_v2", "username"};
    for (size_t i = 0; i < sizeof(failing) / sizeof(failing[0]); i++) {
        mode = LINK_WIFI_OPTIONAL;
        reset();
        fail_key = failing[i];
        run();
        if (!strcmp(failing[i], "username")) {
            /* The username is best effort, as in the Wi-Fi flow. */
            assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed"));
            continue;
        }
        if (nevents != 1 || strcmp(events[0], "fail:error_storage") != 0 || count() != 0 || stops) {
            fprintf(stderr, "failing %s: %d events (%s), %d keys, %d stops\n", failing[i], nevents,
                    nevents ? events[0] : "", count(), stops);
            abort();
        }
    }

    /* The session went away first: a pairing failure, nothing stored. */
    reset();
    session_valid = false;
    run();
    assert(nevents == 1 && !strcmp(events[0], "fail:auth_failed") && count() == 0 && !stops);

    /* A BLE-only gadget's boot: set up means the marker and the key. */
    reset();
    config_set_str("setup_complete", "1");
    config_set_str("mg_proof_k", "@K@");
    config_set_str("access_token", "a");
    assert(standalone_boot() && count() == 3);
    reset();
    config_set_str("setup_complete", "1");       /* no key: set up by Wi-Fi firmware */
    config_set_str("access_token", "a");
    assert(!standalone_boot() && count() == 0);
    reset();
    config_set_str("setup_wifi", "skipped");     /* cut off midway */
    config_set_str("access_token", "a");
    assert(!standalone_boot() && count() == 0);
    reset();
    config_set_str("mg_proof_k", "@K@");
    assert(!standalone_boot() && count() == 0);
    reset();
    config_set_str("unrelated", "kept");          /* never set up */
    assert(!standalone_boot() && count() == 1);

    /* Gadget setup v2: a second setup on a set-up gadget replaces the pair,
     * the key and the hint in the commit (both Wi-Fi modes). */
    uint8_t k2[MG_TOKEN_PROOF_KEY_LEN];
    assert(mg_token_proof_key("second-access", "@NODE@", k2));
    for (int i = 0; i < MG_TOKEN_PROOF_KEY_LEN; i++) snprintf(k2_hex + 2 * i, 3, "%02x", k2[i]);
    for (int m = 0; m < 2; m++) {
        set_up_first(m ? LINK_WIFI_NONE : LINK_WIFI_OPTIONAL);
        changed_while_held = false;
        run2();
        assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed") && !journal_open && !tokens_held);
        /* The device session stayed busy until the commit had ended the old
         * setup's sessions. */
        assert(changed_while_held);
        assert(!strcmp(get("access_token"), "second-access") && !strcmp(get("refresh_token"), "second-refresh"));
        assert(!strcmp(get("mg_proof_k"), k2_hex) && strcmp(k2_hex, "@K@") != 0);
        assert(config_setup_complete() && config_wifi_skipped() && !strcmp(get("pair_sdk_hint"), "mgst_hint"));
        assert(stops == 1 && setup_changes == 1);   /* setup_changes drops the old proof */
        /* A committed owner change forgets the last owner's apps, bonds and hand-set Muse server and token, and
         * its mark goes. */
        assert(owner_forgets == 1 && !get(OWNER_WIPE_KEY));
        assert(owner_forget_kept_phone);   /* the new owner's phone keeps its bond; earlier phones' go */
    }
    /* Their erase failed: still committed, but not clean: the mark stays for the next boot's replay. */
    set_up_first(LINK_WIFI_OPTIONAL);
    owner_forget_fails = true;
    run2();
    assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed") && owner_forgets == 1);
    assert(get(OWNER_WIPE_KEY) && !strcmp(get(OWNER_WIPE_KEY), "1"));
    owner_forget_fails = false;
    /* No username this time: the old one doesn't linger. */
    set_up_first(LINK_WIFI_OPTIONAL);
    run2();
    assert(!get("username") && !get("api_url_v2"));

    /* Any write failing, or the commit point itself: error_storage, and the
     * gadget keeps its first setup, still set up. */
    const char *failing2[] = {"setup_wifi", "refresh_token", "access_token", "auth_token", "mg_proof_k",
                              "node_id", "setup_complete", "api_url_v2", "pair_sdk_hint"};
    for (size_t i = 0; i < sizeof(failing2) / sizeof(failing2[0]) + 1; i++) {
        set_up_first(LINK_WIFI_OPTIONAL);
        if (i < sizeof(failing2) / sizeof(failing2[0])) fail_key = failing2[i];
        else journal_fails = "commit";
        if (fail_key && !strcmp(fail_key, "pair_sdk_hint")) {
            /* The hint is best effort, as in the Wi-Fi flow. */
            run2();
            assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed"));
            continue;
        }
        assert(provision_gate(10));
        assert(tokens_held);   /* the old setup's session stores no tokens meanwhile */
        provision_token_only_with_gate_held("second-access", "second-refresh", "", "https://other", "", "", 10);
        fail_key = NULL;
        if (nevents != 1 || strcmp(events[0], "fail:error_storage+committed") != 0 || stops) {
            fprintf(stderr, "re-provisioning, failing %s: %d events (%s), %d stops\n",
                    i < sizeof(failing2) / sizeof(failing2[0]) ? failing2[i] : "the commit", nevents,
                    nevents ? events[0] : "", stops);
            abort();
        }
        assert_first_setup("failed write");
        assert(!strcmp(get("api_url_v2"), "https://api"));   /* the old endpoint too */
        assert(owner_forgets == 0 && !get(OWNER_WIPE_KEY));   /* rolled back: the owner keeps them */
    }
    /* The session went away first: auth_failed, the first setup stays. */
    set_up_first(LINK_WIFI_NONE);
    session_valid = false;
    assert(provision_gate(10));
    provision_token_only_with_gate_held("second-access", "second-refresh", "", "", "", "", 10);
    assert(nevents == 1 && !strcmp(events[0], "fail:auth_failed+committed") && !stops);
    assert_first_setup("session gone");
    /* The old setup can't be kept aside: error_storage before anything is written. */
    set_up_first(LINK_WIFI_NONE);
    journal_fails = "begin";
    assert(!provision_gate(10));
    assert(nevents == 1 && !strcmp(events[0], "error_storage+committed") && disconnects == 1);
    assert_first_setup("begin failed");
    /* A gadget that isn't set up opens no journal. */
    reset();
    assert(provision_gate(9) && !journal_open);

    /* A session that set the gadget up is used up: another provision_v2 on it
     * is refused before anything is touched; a new setup takes a new session
     * (hello and press). The same after a second setup. */
    for (int m = 0; m < 2; m++) {
        set_up_first(m ? LINK_WIFI_NONE : LINK_WIFI_OPTIONAL);
        assert(!provision_gate(9));
        assert(nevents == 1 && !strcmp(events[0], "error_pairing_confirm_required+committed"));
        assert(!journal_open && !disconnects && !stops && !setup_changes);
        assert_first_setup("consumed session");
        nevents = 0;
        run2();   /* a new session sets it up again */
        assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed"));
        assert(!strcmp(get("access_token"), "second-access"));
        nevents = 0;
        assert(!provision_gate(10));
        assert(nevents == 1 && !strcmp(events[0], "error_pairing_confirm_required+committed"));
        assert(!strcmp(get("access_token"), "second-access") && !strcmp(get("mg_proof_k"), k2_hex) && !journal_open);
    }
    /* A failed attempt commits nothing: its session may try again. First a
     * failed re-provisioning (old setup kept), then a retry that succeeds. */
    set_up_first(LINK_WIFI_OPTIONAL);
    fail_key = "mg_proof_k";
    assert(provision_gate(10));
    provision_token_only_with_gate_held("second-access", "second-refresh", "", "", "", "", 10);
    fail_key = NULL;
    assert(nevents == 1 && !strcmp(events[0], "fail:error_storage+committed"));
    assert_first_setup("failed attempt");
    nevents = 0;
    run2();
    assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed") && !strcmp(get("access_token"), "second-access"));
    /* And on a gadget that isn't set up yet. */
    reset();
    mode = LINK_WIFI_NONE;
    fail_key = "access_token";
    assert(provision_gate(9));
    run();
    fail_key = NULL;
    assert(nevents == 1 && !strcmp(events[0], "fail:error_storage") && count() == 0);
    nevents = 0;
    assert(provision_gate(9));
    run();
    assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed"));
    assert(!provision_gate(9));

    /* Gadget ids: a gadget set up with a homelink- id keeps it
     * through a failed setup again, and the next one that commits stores the
     * new setup's id with its K, then takes it (refresh, the session's
     * registration). */
    set_up_first(LINK_WIFI_NONE);
    config_set_str("node_id", "homelink-legacy");
    fail_key = "setup_complete";
    assert(provision_gate(10));
    provision_token_only_with_gate_held("second-access", "second-refresh", "", "", "", "", 10);
    fail_key = NULL;
    assert(nevents == 1 && !strcmp(events[0], "fail:error_storage+committed"));
    assert(!strcmp(get("node_id"), "homelink-legacy") && !strcmp(get("mg_proof_k"), "@K@"));
    nevents = refreshes = noise_node_sets = 0;
    run2();
    assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed"));
    assert(!strcmp(get("node_id"), "@NODE@") && !strcmp(get("mg_proof_k"), k2_hex));
    assert(refreshes == 1 && noise_node_sets == 1);
    printf("PASS token-only commit\n");
    return 0;
}
"""


ADVERTISING = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ble_server.h"
#define TAG "ble"
#define CONFIG_MUSE_GADGET_BLE_AUDIO 1   /* the companion's UUID and its pace */
static void log_sink(const char *fmt, ...) { (void)fmt; }
#define ESP_LOGI(tag, ...) log_sink(__VA_ARGS__)
#define ESP_LOGW(tag, ...) log_sink(__VA_ARGS__)
#define ESP_LOGE(tag, ...) log_sink(__VA_ARGS__)
#define ESP_LOGD(tag, ...) log_sink(__VA_ARGS__)
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
#define BLE_UUID16_INIT(v) { { 16 }, (v) }
struct ble_hs_adv_fields {
    uint8_t flags, num_uuids128, uuids128_is_complete, mfg_data_len, name_len, name_is_complete;
    uint8_t num_uuids16, uuids16_is_complete, appearance_is_present;
    uint16_t appearance;
    ble_uuid128_t *uuids128;
    ble_uuid16_t *uuids16;
    const uint8_t *mfg_data, *name;
};
struct ble_gap_adv_params { uint8_t conn_mode, disc_mode; uint16_t itvl_min, itvl_max; uint8_t filter_policy; };
struct ble_npl_event { int unused; };
struct ble_npl_callout { void (*fn)(struct ble_npl_event *); uint32_t ticks; bool armed; };
typedef void ble_npl_event_fn(struct ble_npl_event *);
static int dflt_q;
static void *nimble_port_get_dflt_eventq(void) { return &dflt_q; }
static void ble_npl_callout_init(struct ble_npl_callout *co, void *q, ble_npl_event_fn *fn, void *arg) {
    (void)q; (void)arg; co->fn = fn; co->armed = false;
}
static int ble_npl_callout_reset(struct ble_npl_callout *co, uint32_t ticks) { co->ticks = ticks; co->armed = true; return 0; }
static uint32_t ble_npl_time_ms_to_ticks32(uint32_t ms) { return ms; }
#define BLE_HCI_ADV_FILT_NONE 0
#define BLE_HCI_ADV_FILT_CONN 2
struct ble_gap_event { int type; };
typedef int ble_gap_event_fn(struct ble_gap_event *, void *);
#define BLE_UUID_TYPE_128 128
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_GEN 2
#define BLE_HS_ADV_F_DISC_GEN 2
#define BLE_HS_ADV_F_BREDR_UNSUP 4
#define BLE_GAP_ADV_ITVL_MS(ms) ((uint16_t)((ms) * 8 / 5))
#define BLE_HS_FOREVER INT32_MAX
#define BLE_OWN_ADDR_PUBLIC 0
#define BLE_OWN_ADDR_RANDOM 1
#define BLE_ADDR_PUBLIC 0
static int ble_hs_id_copy_addr(uint8_t t, uint8_t *a, int *nrpa) { (void)t; (void)nrpa; memset(a, 0, 6); return 0; }
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define ADV_TURN_MS 1500
#define MAX_COMPANIONS 2
static const ble_uuid128_t SVC_UUID = { { BLE_UUID_TYPE_128 }, { 0x1c } };   /* Link setup */
static const uint8_t MG_UUID[16] = { 0x5d };                                 /* a companion */
static char s_device_name[] = "MuseGadget-A7F5B4";
static bool s_started = true, s_shutting_down, s_synced, s_advertising_enabled, s_advertising_active;
static bool s_adv_turn, s_companion_advertising, s_adv_link, setup_done, companion_wants = true;
static bool s_quiet;   /* ble_server_set_quiet: a Wi-Fi video stream wants the radio */
static bool s_gatt_failed;   /* A table that didn't register holds advertising */
uint8_t ble_hs_hci_get_hci_version(void) { return 0x09; }   /* on_sync logs the controller's LL version */
static void muse_boot_mark(const char *name) { (void)name; }   /* boot milestones (muse_restart.h) */
static bool s_ever_reachable;   /* the boot guard's health (ble_server.c) */
static bool s_bench_silent;   /* ble_server_bench_silence (the stay-reachable bench check) */
static volatile int64_t s_bench_pause_until_us;   /* ble_server_bench_pause (>ble.pause) */
static bool s_bench_pause_only;                    /* ...connectable only from the accept list */
#include "adv_pace.h"
static adv_pace_t s_adv_pace;                      /* The companion's interval */
static struct ble_npl_callout s_adv_pace_co;
static bool s_adv_pace_co_ready, s_adv_paced, s_adv_paced_fast, s_adv_pace_logged_fast;
static int64_t fake_now_us;   /* esp_timer_get_time(), for !SETUP ADV's window */
static int64_t esp_timer_get_time(void) { return fake_now_us; }
static bool s_safe_identity;   /* SAFE's other identity */
static bool s_safe_public;     /* SAFE on the public address */
static bool s_stall_quiet;     /* a stalled companion protocol */
static int ble_hs_id_set_rnd(const uint8_t *a) { (void)a; return 0; }
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
static int ble_hs_id_gen_rnd(int nrpa, ble_addr_t *out) { (void)nrpa; memset(out, 0, sizeof(*out)); return 0; }
static const ble_companion_t *s_adv_other;
static ble_companion_t s_companions[MAX_COMPANIONS];
static int s_ncompanions;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int starts, stops, last_duration, last_filter = -1, last_itvl_min = -1, last_itvl_max = -1;
static uint8_t staged, last_uuid;
static int staged_flags = -1, last_flags = -1;   /* Link's manufacturer data flags byte, -1 without it */
static bool gadget = true;                      /* link_pairing_set_gadget(): serves musegadgets */
@FLAGS@
static int gap_event_cb(struct ble_gap_event *e, void *a) { (void)e; (void)a; return 0; }
static bool config_setup_complete(void) { return setup_done; }
static bool link_pairing_mgcommands(void) { return gadget; }
static bool wants(void) { return companion_wants; }
static int ble_hs_util_ensure_addr(int random) { (void)random; return 0; }
static int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f) {
    staged = f->uuids128->value[0];
    staged_flags = -1;
    if (f->mfg_data) {
        assert(f->mfg_data_len == 3 && f->mfg_data[0] == 0xFF && f->mfg_data[1] == 0xFF);
        staged_flags = f->mfg_data[2];
    }
    return 0;
}
static int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f) { (void)f; return 0; }
static int ble_gap_adv_start(uint8_t own, const void *peer, int32_t duration, const struct ble_gap_adv_params *p,
                             ble_gap_event_fn *cb, void *arg) {
    (void)own; (void)peer; (void)cb; (void)arg;
    assert(!s_advertising_active);   /* NimBLE refuses a second start */
    starts++; last_duration = duration; last_uuid = staged; last_flags = staged_flags; last_filter = p->filter_policy;
    last_itvl_min = p->itvl_min; last_itvl_max = p->itvl_max;
    return 0;
}
static int ble_gap_adv_stop(void) { stops++; return 0; }
void ble_server_disconnect_client(void) {}
static void rx_reset(void) {}
static void link_pairing_reset(void) {}
static bool s_plaintext_status_blocked;
static void start_advertising(void);
static void turn_ends(void) { s_advertising_active = false; s_adv_turn = !s_adv_turn; start_advertising(); }
@REGION@
@BEGIN@
@STOP@
@COMPANION@
int main(void) {
    /* A gadget: the musegadgets companion wants advertising from boot. */
    s_companions[s_ncompanions++] = (ble_companion_t){ .adv_uuid128 = MG_UUID, .wants_advertising = wants };
    /* Host sync comes first: the companion alone, with no end. */
    on_sync();
    assert(starts == 1 && last_uuid == 0x5d && last_duration == BLE_HS_FOREVER);
    assert(last_flags == -1);   /* the companion's payload carries no Link flags */
    /* Then Link setup asks to be found: restarted, taking turns. */
    ble_server_begin_advertising();
    assert(stops == 1 && starts == 2 && last_duration == ADV_TURN_MS && s_adv_link && s_adv_other);
    /* Gadget setup v2: a gadget's Link payload says Muse Gadget over Bluetooth, not set up. */
    assert(last_uuid == 0x1c && last_flags == 0x02);
    turn_ends();
    assert(starts == 3 && last_uuid != (uint8_t)(s_adv_turn ? 0x1c : 0x5d) && last_duration == ADV_TURN_MS);
    turn_ends();
    assert(starts == 4 && last_uuid == (s_adv_turn ? 0x5d : 0x1c));
    /* Asking again changes nothing, and nothing restarts. */
    ble_server_begin_advertising();
    ble_server_set_companion_advertising(false);
    assert(stops == 1 && starts == 4);
    /* Setup completes: the companion alone again. */
    setup_done = true;
    ble_server_stop_advertising(false);
    assert(stops == 2 && starts == 5 && last_uuid == 0x5d && last_duration == BLE_HS_FOREVER);
    /* Kept in rotation after setup (Muse's phone setup wants Link found): 0x03. */
    s_companion_advertising = true;
    s_advertising_active = false;
    s_adv_turn = false;
    start_advertising();
    assert(last_uuid == 0x1c && last_flags == 0x03);
    s_companion_advertising = false;
    /* Bench (!SETUP ADV): set up, Link setup in rotation again, flagged as not set
     * up (0x02) so an app that lists only Link setup adverts finds it; it ends by
     * itself after its window, and a setup's commit ends it at once. */
    s_advertising_active = false; s_adv_turn = false; starts = stops = 0;
    start_advertising();
    assert(starts == 1 && last_uuid == 0x5d && last_duration == BLE_HS_FOREVER);
    ble_server_bench_setup_adv(600);
    assert(stops == 1 && starts == 2 && last_uuid == 0x1c && last_flags == 0x02 && last_duration == ADV_TURN_MS);
    turn_ends();
    assert(starts == 3 && last_uuid == 0x5d);
    turn_ends();
    assert(starts == 4 && last_uuid == 0x1c && last_flags == 0x02);
    fake_now_us += 601LL * 1000000;   /* the window ran out: the companion alone, for good */
    turn_ends();
    assert(starts == 5 && last_uuid == 0x5d && last_duration == BLE_HS_FOREVER);
    s_adv_turn = false;   /* Link setup's turn first (either order is fine) */
    ble_server_bench_setup_adv(600);
    assert(starts == 6 && last_uuid == 0x1c && last_flags == 0x02);
    ble_server_stop_advertising(true);    /* a dropped pairing session: kept for another try */
    assert(last_uuid == 0x1c && last_flags == 0x02);
    s_advertising_active = false;
    ble_server_stop_advertising(false);   /* the setup's end (stop_setup_ble): over */
    assert(last_uuid == 0x5d && last_duration == BLE_HS_FOREVER);
    ble_server_bench_setup_adv(600);
    ble_server_bench_setup_adv(0);        /* !SETUP ADV 0 */
    assert(last_uuid == 0x5d && last_duration == BLE_HS_FOREVER);
    /* Begin, the other order: advertising starts at sync with both. */
    setup_done = false; s_advertising_active = false; s_advertising_enabled = false; s_synced = false;
    starts = stops = 0;
    ble_server_begin_advertising();
    assert(starts == 0);
    on_sync();
    assert(starts == 1 && last_duration == ADV_TURN_MS);
    /* A build without a companion: Link setup only, never restarted by asking again. */
    s_ncompanions = 0; s_advertising_active = false; s_advertising_enabled = false; starts = stops = 0;
    gadget = false;   /* Wi-Fi firmware: the flags byte as it always was */
    on_sync();
    assert(starts == 0);
    ble_server_begin_advertising();
    assert(starts == 1 && last_uuid == 0x1c && last_duration == BLE_HS_FOREVER && last_flags == 0x00);
    ble_server_begin_advertising();
    ble_server_set_companion_advertising(true);    /* Muse's phone setup: the same payload */
    assert(starts == 1 && stops == 0);
    setup_done = true;
    s_advertising_active = false;
    start_advertising();
    assert(last_uuid == 0x1c && last_flags == 0x01);
    printf("PASS advertising\n");
    return 0;
}
"""


HELLO = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TAG "ble"
#define CONFIG_MUSE_GADGET_BLE_AUDIO 1   /* the re-setup branches */
static bool set_up, gadget, plaintext_blocked;
static int hellos;
static char status[64];
typedef struct cJSON cJSON;
static bool config_setup_complete(void) { return set_up; }
static bool link_pairing_mgcommands(void) { return gadget; }
static void ble_server_send_status(const char *s) { snprintf(status, sizeof(status), "%s", s); }
static const char *link_pairing_handle_client_hello(cJSON *root, char **reply) {
    (void)root; hellos++; *reply = NULL; return NULL;
}
static void ble_server_send_chunked(const char *s) { (void)s; }
static uint32_t link_pairing_session_generation(void) { return 0; }   /* no session: test_a_takeover_hello_... */
static bool link_pairing_session_is_current(uint32_t g) { (void)g; return false; }
static bool link_pairing_reset_session(uint32_t g) { (void)g; return false; }
static void ble_server_delayed_disconnect(uint32_t ms) { (void)ms; }
static bool s_plaintext_status_blocked;
static bool resetup_open = true;   /* app_resetup_open(): setup mode on a talk-key-only board, never SAFE */
static bool app_resetup_open(void) { return resetup_open; }
#define RESETUP_EXEMPT false   /* a release build */
#define ESP_LOGW(tag, ...) ((void)(tag))
@GAP@
static resetup_gap_t s_prompt_gap;
static uint32_t s_conn_seq = 1;
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }
static void send_hello_answer_plaintext(const char *s) { snprintf(status, sizeof(status), "%s", s); }
static void hello(void) {
    bool decrypted = false;
    const char *act = "pairing_client_hello";
    cJSON *root = NULL;
    status[0] = 0;
    hellos = 0;
@BRANCH@
    }
    (void)plaintext_blocked;
}
int main(void) {
    /* Not set up: every build answers hello. */
    set_up = false; gadget = false; hello(); assert(hellos == 1 && !status[0]);
    set_up = false; gadget = true;  hello(); assert(hellos == 1 && !status[0]);
    /* Set up: Wi-Fi firmware still refuses, a gadget starts a new setup. */
    set_up = true; gadget = false; hello(); assert(hellos == 0 && !strcmp(status, "error_pairing_unavailable"));
    set_up = true; gadget = true;  hello(); assert(hellos == 1 && !status[0]);
    /* A talk-key-only board outside setup mode (or SAFE): error_pairing_unavailable in plaintext, nothing shows;
     * a gadget that isn't set up is never asked. */
    resetup_open = false;
    hello(); assert(hellos == 0 && !strcmp(status, "error_pairing_unavailable"));
    set_up = false; hello(); assert(hellos == 1 && !status[0]);
    set_up = true; resetup_open = true;
    /* The prompt up on connection 1: a hello on it takes it over; connection 2's gets
     * error_operation_in_progress (plaintext), nothing shown. */
    resetup_gap_shown(&s_prompt_gap, 1);
    s_conn_seq = 1; hello(); assert(hellos == 1 && !status[0]);
    assert(resetup_hello(&s_prompt_gap, false, true, 1, 0) == RESETUP_TAKEOVER);
    s_conn_seq = 2; hello(); assert(hellos == 0 && !strcmp(status, "error_operation_in_progress"));
    set_up = false; hello(); assert(hellos == 1 && !status[0]);   /* not set up: no rate */
    set_up = true;
    /* Dismissed at 0 s: on its connection a hello 4 s later is refused, 10 s after the dismiss it shows; another
     * connection shows at once. A confirm (no dismiss) refuses nothing. Test and bench builds: exempt. */
    resetup_gap_ended(&s_prompt_gap, true, 0);
    now_us = 4000000; s_conn_seq = 1; hello(); assert(hellos == 0 && !strcmp(status, "error_operation_in_progress"));
    assert(resetup_hello(&s_prompt_gap, true, true, 1, 4000000) == RESETUP_SHOW);
    s_conn_seq = 2; hello(); assert(hellos == 1 && !status[0]);
    now_us = 10000000; s_conn_seq = 1; hello(); assert(hellos == 1 && !status[0]);
    s_prompt_gap = (resetup_gap_t){ 0 };
    resetup_gap_shown(&s_prompt_gap, 1);
    resetup_gap_ended(&s_prompt_gap, false, 10000000);
    hello(); assert(hellos == 1 && !status[0]);
    /* Setup mode: the key pressed at power-on and let go early, on a normal boot, for 5 min or until the
     * confirmed re-setup; never in SAFE; test and bench builds always. */
    assert(setup_mode_now(false, false, true, false, 1));
    assert(!setup_mode_now(false, false, true, false, SETUP_MODE_US));
    assert(!setup_mode_now(false, true, true, false, 1));
    assert(!setup_mode_now(false, false, false, false, 1));
    assert(!setup_mode_now(false, false, true, true, 1));
    assert(setup_mode_now(true, true, false, true, SETUP_MODE_US));
    printf("PASS hello\n");
    return 0;
}
"""

PRESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TAG "app"
static void log_sink(const char *fmt, ...) { (void)fmt; }
#define ESP_LOGI(tag, ...) log_sink(__VA_ARGS__)
static bool set_up, pending, armed, connected;
static int confirms, windows;
static bool config_setup_complete(void) { return set_up; }
static bool link_pairing_confirmation_required(void) { return pending; }
static bool ble_server_has_connection(void) { return connected; }
static uint32_t link_pairing_confirm_active_session(int64_t pressed_after_us) {
    /* link_pairing.c: only a session whose prompt went out (armed); the
     * fresh-press rule is test_pairing_fresh_press's. */
    (void)pressed_after_us;
    if (!pending || !armed) return 0;
    pending = armed = false;
    return 7;
}
static void notify_pairing_confirmed(uint32_t g) { assert(g == 7); confirms++; }
static bool setup_mode;
static int dismisses;
static bool app_setup_mode(void) { return setup_mode; }
static void app_dismiss_pairing_prompt(void) { if (pending) { dismisses++; pending = armed = false; } }
static bool shown;   /* the prompt on show (app.c's prompt_shown(): after it's armed) */
static bool prompt_shown(void) { return shown; }
@DISMISSES@
static void open_setup_window(const char *why) { (void)why; windows++; }
static void factory_test_on_button_press(void) {}
static int64_t button_pressed_after_us(void) { return 0; }
static void on_button_short_press(void) {
@BUTTON@
}
static bool app_confirm_pairing_press(int64_t pressed_after_us, bool talk_key) {
@TALK@
}
static void reset(bool s, bool p, bool a) {
    set_up = s; pending = p; armed = a; shown = a; connected = true; confirms = windows = dismisses = 0;
    setup_mode = false;
}
int main(void) {
    for (int talk = 0; talk < 2; talk++) {
        /* A set-up gadget being set up again: one press of a confirm key (the second key, the Confirm tap)
         * confirms it, as a first setup's. */
        reset(true, true, true);
        if (talk) assert(app_confirm_pairing_press(0, false)); else on_button_short_press();
        assert(confirms == 1 && windows == 0);
        /* Set up, nothing waiting: the press confirms nothing. */
        reset(true, false, false);
        if (talk) assert(!app_confirm_pairing_press(0, false)); else on_button_short_press();
        assert(confirms == 0 && windows == 0);
        /* A setup whose prompt hasn't gone out yet (not armed): nothing confirmed. */
        reset(true, true, false);
        if (talk) app_confirm_pairing_press(0, false); else on_button_short_press();
        assert(confirms == 0);
        /* No phone connected: nothing confirmed. */
        reset(false, true, true);
        connected = false;
        if (talk) assert(!app_confirm_pairing_press(0, true)); else on_button_short_press();
        assert(confirms == 0);
        /* Not set up, waiting: any press confirms, the talk key's included; nothing waiting: the button opens
         * setup. */
        reset(false, true, true);
        if (talk) app_confirm_pairing_press(0, true); else on_button_short_press();
        assert(confirms == 1);
        reset(false, false, false);
        if (talk) app_confirm_pairing_press(0, true); else on_button_short_press();
        assert(confirms == 0 && windows == (talk ? 0 : 1));
    }
    /* A stranger raised a re-setup prompt: the owner's talk press only dismisses it, never confirms (and the
     * press is used: no talk turn). In setup mode the talk key confirms. */
    reset(true, true, true);
    assert(app_confirm_pairing_press(0, true) && confirms == 0 && dismisses == 1 && !pending);
    reset(true, true, true);
    setup_mode = true;
    assert(app_confirm_pairing_press(0, true) && confirms == 1 && dismisses == 0);
    /* Armed but not on show yet (on_pairing_client_finished arms the prompt, then shows it): a press then is the
     * prompt's and confirms nothing. */
    for (int s = 0; s < 2; s++) {
        reset(s, true, true);
        shown = false;
        assert(app_confirm_pairing_press(0, false) && confirms == 0 && pending);
    }
    printf("PASS press\n");
    return 0;
}
"""

GHOST = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TAG "app"
#define CONFIG_MUSE_GADGET_BLE_AUDIO 1
static void log_sink(const char *fmt, ...) { (void)fmt; }
#define ESP_LOGI(tag, ...) log_sink(__VA_ARGS__)
#define ESP_LOGW(tag, ...) log_sink(__VA_ARGS__)
#define ESP_LOGE(tag, ...) log_sink(__VA_ARGS__)
typedef struct cJSON cJSON;
typedef int BaseType_t;
#define pdPASS 1
/* link_pairing.c as a model (test_link_pairing_handshake runs the real one): the session's phase, its token, the
 * prompt's arming; a hello refused on its own fields or keys changes nothing, every other one starts over. */
enum { IDLE, WAIT, CONFIRM, READY } state;
static uint32_t gen = 1;
static bool armed;
enum { HELLO_OK, HELLO_BAD_KEY, HELLO_FAILS_AFTER_WIPE } hello_kind;
static uint32_t link_pairing_session_generation(void) { return state != IDLE ? gen : 0; }
static bool link_pairing_session_is_current(uint32_t g) { return g && g == gen; }
static const char *link_pairing_handle_client_hello(cJSON *root, char **reply) {
    (void)root; *reply = NULL;
    if (hello_kind == HELLO_BAD_KEY) return "error_pairing_invalid_hello";
    gen++; armed = false; state = IDLE;
    if (hello_kind == HELLO_FAILS_AFTER_WIPE) return "error_pairing_unavailable";   /* a draw, the key agreement */
    state = WAIT;
    *reply = calloc(1, 1);   /* pairing_ready (ble_server.c frees it) */
    return NULL;
}
static bool link_pairing_confirmation_required(void) { return state == CONFIRM; }
static bool link_pairing_arm_confirmation(uint32_t g) {
    if (g != gen || state != CONFIRM) return false;
    armed = true;
    return true;
}
static void link_pairing_disarm_confirmation(uint32_t g) { if (g == gen) armed = false; }
static uint32_t link_pairing_confirm_active_session(int64_t p) {
    (void)p;
    if (state != CONFIRM || !armed) return 0;
    state = READY;
    return ++gen;
}
static bool link_pairing_reset_session(uint32_t g) {
    if (!g || g != gen) return false;
    gen++; state = IDLE; armed = false;
    return true;
}
static bool link_pairing_mgcommands(void) { return true; }
static uint32_t client_finished(void) { assert(state == WAIT); state = CONFIRM; return ++gen; }
/* ble_server.c */
static bool set_up = true, setup_mode, connected = true;
static char status[64];
static int disconnects;
static bool config_setup_complete(void) { return set_up; }
static void ble_server_send_status(const char *s) { snprintf(status, sizeof(status), "%s", s); }
static void ble_server_send_chunked(const char *s) { (void)s; }
static bool s_plaintext_status_blocked;
static bool app_resetup_open(void) { return true; }   /* a board with a second key */
#define RESETUP_EXEMPT false   /* a release build */
@GAP@
static resetup_gap_t s_prompt_gap;
static uint32_t s_conn_seq = 1;
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }
static void send_hello_answer_plaintext(const char *s) { snprintf(status, sizeof(status), "%s", s); }
static void ble_server_delayed_disconnect(uint32_t ms) { (void)ms; disconnects++; }
static void ble_server_resetup_prompt_shown(void) { resetup_gap_shown(&s_prompt_gap, s_conn_seq); }
static void ble_server_resetup_prompt_ended(bool dismissed) { resetup_gap_ended(&s_prompt_gap, dismissed, now_us); }
static bool ble_server_has_connection(void) { return connected; }
static bool ble_server_send_pairing_status(const char *s, uint32_t g) { (void)g; ble_server_send_status(s); return true; }
static void ble_server_disconnect_pairing_session(uint32_t g) { if (link_pairing_reset_session(g)) disconnects++; }
static void hello(int kind) {
    bool decrypted = false;
    const char *act = "pairing_client_hello";
    cJSON *root = NULL;
    hello_kind = kind;
    status[0] = 0;
@BRANCH@
    }
}
/* app.c */
static const char *s_setup_stage = "done";
static void setup_stage_set(const char *s) { s_setup_stage = s; }
static void setup_window_lock_take(void) {}
static void setup_window_lock_give(void) {}
static uint32_t s_confirm_generation, s_confirm_session_generation;
static uint32_t advance_confirm_generation_locked(void) { return ++s_confirm_generation; }
static char ui[40];
static void ui_set_status(const char *s) { snprintf(ui, sizeof(ui), "%s", s); }
typedef enum { LED_STATE_PAIRING_CONFIRM_REQUIRED = 1 } led_state_t;
static int led;
static void led_status_set_state(led_state_t s) { led = s; }
static bool app_setup_mode(void) { return setup_mode; }
static void factory_test_on_button_press(void) {}
static int timeout_tasks;
static void confirm_timeout_task(void *arg) { (void)arg; }
static BaseType_t helper_task_create(void (*fn)(void *), const char *name, int stack, void *arg, int prio, void *h) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)h;
    timeout_tasks++;   /* the dismiss's end: its status, then the session's disconnect */
    return pdPASS;
}
static int confirms;
static void notify_pairing_confirmed(uint32_t g) { (void)g; confirms++; }
@APP@
static bool app_confirm_pairing_press(int64_t pressed_after_us, bool talk_key) {
@TALK@
}
static bool prompt_up(void) { return !strcmp(s_setup_stage, "confirm") && led == LED_STATE_PAIRING_CONFIRM_REQUIRED; }
/* The link drops (ble_server.c's disconnect, then on_client_disconnected(): the set-up status again); the phone
 * connects again: a new connection. */
static void reconnect(void) {
    resetup_gap_ended(&s_prompt_gap, false, now_us);
    gen++; state = IDLE; armed = false;
    s_plaintext_status_blocked = false;
    s_setup_stage = "done"; led = 0;
    s_conn_seq++;
    disconnects = timeout_tasks = confirms = 0;
}
/* A valid hello and its client_finished: the prompt shows. */
static void prompt(void) {
    hello(HELLO_OK);
    assert(!status[0] && state == WAIT);
    on_pairing_client_finished(client_finished());
    assert(prompt_up() && armed && !confirms);
}
int main(void) {
    for (int mode = 0; mode < 2; mode++) {
        setup_mode = mode;   /* outside setup mode the talk key dismisses a re-setup; in it the talk key may confirm */
        for (int kind = HELLO_BAD_KEY; kind <= HELLO_FAILS_AFTER_WIPE; kind++) {
            /* A takeover hello that fails, refused on its own fields or keys (before any wipe) or failing after the
             * wipe (a draw, the key agreement): it's answered (under the session's keys, or in plaintext once
             * they're gone), the session ends at once and its prompt with the link. */
            reconnect();
            prompt();
            hello(kind);
            assert(!strcmp(status, kind == HELLO_BAD_KEY ? "error_pairing_invalid_hello" : "error_pairing_unavailable"));
            assert(state == IDLE && !armed && disconnects == 1);
            /* Before the link is gone: a talk press is the prompt's (no talk turn) and confirms nothing; the back
             * gesture ends what's on show; neither confirms anything. */
            assert(app_confirm_pairing_press(0, true) && !confirms);
            assert(app_confirm_pairing_press(0, false) && !confirms);
            app_dismiss_pairing_prompt();
            assert(!confirms && disconnects >= 2);
            /* The next valid hello asks again, and nothing from before confirms it: only a press on it. */
            reconnect();
            prompt();
            assert(!confirms);
            assert(app_confirm_pairing_press(0, false) && confirms == 1);   /* the second key or the Confirm disc */
        }
        /* Dismissed while a takeover's hello is still answered: that session ends (it never finishes into a new
         * prompt), and the prompt with the link. */
        reconnect();
        prompt();
        hello(HELLO_OK);
        assert(state == WAIT && prompt_up());
        assert(app_confirm_pairing_press(0, false) && !confirms);   /* not that hello's prompt yet: nothing */
        app_dismiss_pairing_prompt();
        assert(state == IDLE && disconnects == 1 && !confirms);
        /* Dismissed with its press armed: no press confirms it from then on. */
        reconnect();
        prompt();
        app_dismiss_pairing_prompt();
        assert(!armed && timeout_tasks == 1);
        assert(app_confirm_pairing_press(0, false) && !confirms);
    }
    printf("PASS ghost\n");
    return 0;
}
"""

TAKEOVER = r"""
/* ---- the update prompt (muse_local_press, real), which the gadget's own key passes through first ---- */
#include "muse_local_press.h"
static muse_local_press_t w = { .prompt_ms = -1, .shown_ms = -1, .bound_ms = -1, .covered_ms = -1, .armed_ms = -1 };
static int64_t now_ms;
/* muse_input.c's routing of the gadget's own key (real): the update's prompt takes a press, only that; else the
 * pairing prompt's (aux_key: the second key confirms; talk_input: the talk key's goes to muse_link_confirm_press). */
bool muse_local_press_prompting(void) { return muse_local_press_core_prompting(&w, now_ms); }
bool muse_local_press_note(const char *what) { (void)what; return muse_local_press_core_note(&w, now_ms, false); }
@BTNS@
@UPDATE_PRESS@
static void physical_press(unsigned down, unsigned up) {
    unsigned own = update_prompt_press(down);
    if (own & MUSE_BTN_AUX_PRESS) (void)app_confirm_pairing_press(0, false);
    if (own & MUSE_BTN_TALK_PRESS) (void)app_confirm_pairing_press(0, true);
    (void)update_prompt_press(up);
}
/* A valid hello and its client_finished on this link: the re-setup prompt. */
static void resetup_request(void) {
    hello(HELLO_OK);
    assert(!status[0] && state == WAIT);
    on_pairing_client_finished(client_finished());
}
/* The step's resetup state: a prompt up (ble_server.c's on this link, on show and armed), a hello confirmed. */
static void want_resetup(bool up, bool confirmed) {
    assert((confirms > 0) == confirmed);
    if (!confirmed) assert((s_prompt_gap.prompt_conn != 0 && prompt_up() && armed) == up);
}
int main(void) {
    /* a takeover hello that fails validation ends the prompt, answered encrypted, the link dropped (the refusal
     * under the session still current: ble_server_send_status; the drop: ble_server_delayed_disconnect) */
    reconnect();
@TAKEOVER_STEPS@
    assert(confirms == 1);
    printf("PASS a takeover hello that fails validation ends the prompt, answered encrypted, the link dropped\n");
    return 0;
}
"""

DISCONNECT = r"""
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_MUSE_GADGET_BLE_AUDIO 1
@STANDALONE@
typedef enum { LED_STATE_BLE_ADVERTISING, LED_STATE_BLE_CONNECTED, LED_STATE_AUTH_OK, LED_STATE_WIFI_CONNECTED,
               LED_STATE_VM_OK, LED_STATE_WS_CONNECTED, LED_STATE_PAIRING_CONFIRM_REQUIRED } led_state_t;
enum { SCAN_REFRESH_REPLY_PENDING = 2 };
static atomic_uint s_scan_refresh_flags;
static const char *s_setup_stage = "done";
static bool set_up = true, wifi_up, vm_up, tunnel_up;
static int led = -1, advertising, generations;
static char status[32], ble[32];
static bool config_setup_complete(void) { return set_up; }
static void setup_window_lock_take(void) {}
static void setup_window_lock_give(void) {}
static uint32_t advance_confirm_generation_locked(void) { return ++generations; }
static void setup_stage_set(const char *s) { s_setup_stage = s; }
static void ui_set_status(const char *s) { snprintf(status, sizeof(status), "%s", s); }
static void ui_set_ble(const char *s) { snprintf(ble, sizeof(ble), "%s", s); }
static void led_status_set_state(led_state_t s) { led = s; }
static void ble_coex_set_active(bool on) { (void)on; }
static void ble_server_begin_advertising(void) { advertising++; }
static bool noise_ctrl_is_connected(void) { return vm_up; }
static bool noise_tunnel_is_connected(void) { return tunnel_up; }
static bool wifi_mgr_is_connected(void) { return wifi_up; }
@FUNCS@
int main(void) {
    /* A phone subscribing to a set-up gadget's setup service: the status stays. */
    led = -1;
    on_client_connected();
    assert(led == -1 && !strcmp(ble, "connected"));
    /* Setting up again, abandoned at the press: back to the set-up status. */
    s_setup_stage = "confirm";
    led = LED_STATE_PAIRING_CONFIRM_REQUIRED;
    on_client_disconnected();
    assert(!strcmp(s_setup_stage, "done") && !strcmp(status, "auth_ok") && generations == 1 && !advertising);
#if CONFIG_MUSE_GADGET_BLE_STANDALONE
    assert(led == LED_STATE_AUTH_OK);
#else
    assert(led == LED_STATE_AUTH_OK);   /* off line */
    s_setup_stage = "failed"; wifi_up = true;
    on_client_disconnected();
    assert(led == LED_STATE_WIFI_CONNECTED);
    s_setup_stage = "confirmed"; vm_up = true;
    on_client_disconnected();
    assert(led == LED_STATE_VM_OK);
    s_setup_stage = "confirm"; tunnel_up = true;
    on_client_disconnected();
    assert(led == LED_STATE_WS_CONNECTED);
#endif
    /* A connection that never started a setup (stage done): nothing changes. */
    led = -1; generations = 0;
    on_client_disconnected();
    assert(led == -1 && generations == 0);
    /* Not set up: advertising again, as before. */
    set_up = false;
    on_client_disconnected();
    assert(advertising == 1 && led == LED_STATE_BLE_ADVERTISING && !strcmp(s_setup_stage, "advertising"));
    on_client_connected();
    assert(led == LED_STATE_BLE_CONNECTED);
    printf("PASS disconnect\n");
    return 0;
}
"""


class LinkGadgetSetupTest(unittest.TestCase):
    def setUp(self) -> None:
        self.cc = shlex.split(os.environ.get("CC", "cc"))
        if not self.cc or shutil.which(self.cc[0]) is None:
            self.skipTest("C compiler not available")

    def build_and_run(self, source: str, extra: list[Path], incs: tuple = ()) -> str:
        with tempfile.TemporaryDirectory(prefix="link-gadget-") as directory:
            path = Path(directory) / "harness.c"
            path.write_text(source)
            (Path(directory) / "sdkconfig.h").write_text("#pragma once\n")
            binary = Path(directory) / "harness"
            r = subprocess.run(
                [*self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-I", directory, "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/link_fakes"), "-I", str(MG / "include"),
                 "-I", str(ROOT / "components/muse"), *[a for i in incs for a in ("-I", str(i))],
                 "-I", str(PROTOCOLS), str(path), *[str(p) for p in extra], "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r.stdout

    def test_advertising_follows_what_is_wanted(self) -> None:
        # Host sync may start the companion's advertising before Link setup
        # asks for its own: the running advertisement must be restarted.
        ble = (ROOT / "main/ble_server.c").read_text()
        flags = "".join(l + "\n" for l in ble.splitlines() if l.startswith("#define LINK_ADV_"))
        self.assertEqual(flags.count("#define"), 2)
        source = (ADVERTISING
                  .replace("@FLAGS@", flags)
                  .replace("@REGION@", _between(ble, "// The companion whose UUID wants advertising now",
                                                "static void on_reset(int reason)"))
                  .replace("@BENCH@", "")
                  .replace("@BEGIN@", _between(ble, "void ble_server_begin_advertising(void) {",
                                               "void ble_server_disconnect_client(void)"))
                  .replace("@STOP@", _between(ble, "void ble_server_stop_advertising(bool disconnect_client) {",
                                              "void ble_server_set_companion(const ble_companion_t"))
                  .replace("@COMPANION@", _between(ble, "void ble_server_set_companion_advertising(bool enabled) {",
                                                   "#if CONFIG_HOMEHUB_BLE_PERSISTENT\nvoid ble_server_set_companion_interval_ms(")))
        self.assertIn("PASS advertising", self.build_and_run(source, []))


    def test_provision_v2_wifi_modes(self) -> None:
        ble = (ROOT / "main/ble_server.c").read_text()
        branch = _between(ble, '    } else if (decrypted && strcmp(act, "provision_v2") == 0) {',
                          '    } else if (strcmp(act, "get_device_info") == 0) {')
        types = _between(ble, "typedef struct {\n    char *ssid;", "static void provision_task(")
        types += "static const char *optional_ota_url(cJSON *root) {" + _function_body(
            ble, "static const char *optional_ota_url(cJSON *root)") + "}\n"
        types += "static bool optional_ota_force(cJSON *root) {" + _function_body(
            ble, "static bool optional_ota_force(cJSON *root)") + "}\n"
        # The hosts check (muse_server_hosts.h), in a build that takes custom
        # hosts: test_link_server_hosts covers the release build's refusal.
        types += "#define CONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS 1\n#include \"muse_server_hosts.h\"\n"
        types += "static const char *optional_str(cJSON *root, const char *key) {" + _function_body(
            ble, "static const char *optional_str(cJSON *root, const char *key)") + "}\n"
        types += "static bool setup_hosts_allowed(cJSON *root) {" + _function_body(
            ble, "static bool setup_hosts_allowed(cJSON *root)") + "}\n"
        source = PROVISION_V2.replace("@TYPES@", types).replace("@BRANCH@", branch)
        out = self.build_and_run(source, [ROOT / "tests/link_fakes/cJSON.c"])
        self.assertIn("PASS provision_v2", out)

    def test_token_only_commit_and_rollback(self) -> None:
        app = (ROOT / "main/app.c").read_text()
        commit = _between(app, "#if CONFIG_MUSE_GADGET_BLE_AUDIO\n// The gadget's part of a provisioning commit",
                          "static void stop_setup_ble(")
        token_only = _function_body(app, "static void provision_token_only_with_gate_held(")
        save_signature = ("static bool save_setup_endpoints(const char *api_url, const char *api_url_v2,\n"
                          "                                 const char *noise_host)")
        endpoints = ("static void vm_api_set_base_url(const char *url) { (void)url; }\n"
                     "static void noise_ctrl_set_host(const char *host) { (void)host; }\n"
                     + save_signature + " {" + _function_body(app, save_signature) + "}\n")
        standalone = _between(app, "    bool setup_complete = config_setup_complete();\n    bool have_key",
                              '    s_setup_stage = setup_complete ? "done" : "idle";\n    ui_set_ble("musegadgets");')
        gate = _between(app, "#if CONFIG_MUSE_GADGET_BLE_AUDIO\n    // A set-up gadget being set up again",
                        "#if CONFIG_MUSE_GADGET_BLE_STANDALONE\n    // wifi \"none\"")
        fail_restore = _between(app, "#if CONFIG_MUSE_GADGET_BLE_AUDIO\n    // Setting a set-up gadget up again (its",
                                "    // The operation gate still belongs")
        basic = VECTORS["vectors"][0]
        source = (TOKEN_ONLY.replace("static void provision_token_only_with_gate_held(",
                                     endpoints + "static void provision_token_only_with_gate_held(", 1)
                  .replace("@COMMIT@", commit).replace("@TOKEN_ONLY@", token_only)
                  .replace("@GATE@", gate).replace("@FAIL_RESTORE@", fail_restore)
                  .replace("@STANDALONE@", standalone).replace("@NODE@", basic["node_id"])
                  .replace("@ACCESS@", basic["access_token"]).replace("@K@", basic["K"]))
        out = self.build_and_run(source, [ROOT / "tests/mg_token_proof_ref.c"])
        self.assertIn("PASS token-only commit", out)

    def test_reprovision_journal(self) -> None:
        # The journal itself (main/config_store.c) on a fake NVS.
        with tempfile.TemporaryDirectory(prefix="link-journal-") as directory:
            binary = Path(directory) / "harness"
            r = subprocess.run(
                [*self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                 "-I", str(ROOT / "tests/link_fakes"), "-I", str(ROOT / "main"),
                 str(ROOT / "tests/link_reprovision_journal_harness.c"), str(ROOT / "main/config_store.c"),
                 "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("PASS reprovision journal", r.stdout)

    def test_hello_while_set_up(self) -> None:
        # Gadget setup v2: a set-up gadget answers pairing_client_hello; Wi-Fi
        # firmware still refuses it.
        ble = (ROOT / "main/ble_server.c").read_text()
        branch = _between(ble, '    if (!decrypted && strcmp(act, "pairing_client_hello") == 0 && config_setup_complete()',
                          '    } else if (!decrypted && strcmp(act, "pairing_encrypted") == 0) {')
        gap = (ROOT / "main/resetup_gap.h").read_text()
        gap = gap[gap.index("#define RESETUP_PROMPT_GAP_US"):]
        self.assertIn("PASS hello", self.build_and_run(HELLO.replace("@BRANCH@", branch).replace("@GAP@", gap), []))
        # The rate: the prompt's connection; a reconnect is a new one and ends the prompt; the dismiss rule on
        # release builds only.
        self.assertIn("#if CONFIG_MUSE_GADGET_TEST_BUILD || CONFIG_MUSE_GADGET_AUTOMATION\n#define RESETUP_EXEMPT true",
                      ble)
        self.assertIn("s_conn_seq++;", ble)
        self.assertIn("resetup_gap_ended(&s_prompt_gap, false, esp_timer_get_time());   /* its prompt ends with the "
                      "link */", ble)
        app = (ROOT / "main/app.c").read_text()
        self.assertIn("ble_server_resetup_prompt_shown();", app)
        self.assertIn("ble_server_resetup_prompt_ended(true);", _function_body(app, "void app_dismiss_pairing_prompt("))
        self.assertIn("ble_server_resetup_prompt_ended(false);",
                      _function_body(app, "static void notify_pairing_confirmed("))
        # Setup mode: the board's keys, the key pressed at power-on (muse_bootguard), never SAFE.
        self.assertIn("return !b || (!b->touch && !b->keyboard && !b->aux_button);",
                      (ROOT / "main/muse_glue.c").read_text())
        self.assertIn("talk_key_only() && muse_bootguard_setup_press()", app)

    def test_press_confirms_only_an_armed_setup(self) -> None:
        # The button (and Muse's talk button) confirms a setup waiting for it,
        # set up or not, and nothing else.
        app = (ROOT / "main/app.c").read_text()
        dismisses = app[app.index("static bool talk_press_dismisses(bool talk_key) {"):]
        dismisses = dismisses[:dismisses.index("\n}\n") + 3]
        source = (PRESS.replace("@BUTTON@", _function_body(app, "static void on_button_short_press("))
                  .replace("@TALK@", _function_body(app, "bool app_confirm_pairing_press("))
                  .replace("@DISMISSES@", dismisses))
        self.assertIn("PASS press", self.build_and_run(source, []))

    def test_a_takeover_hello_that_fails_leaves_no_ghost_prompt(self) -> None:
        # The portable core's #524 finding, on the ESP32 (proof_gates: a takeover hello that fails validation ends
        # the prompt): a takeover hello that fails, before or after the wipe, is answered, and ends the session and
        # its prompt with the link; a talk press then is never a talk turn and confirms nothing; a dismiss ends a
        # prompt on show in any phase; the next valid hello asks again and only a press on it confirms. The real
        # hello branch, prompt, press and dismiss.
        ble = (ROOT / "main/ble_server.c").read_text()
        branch = _between(ble, '    if (!decrypted && strcmp(act, "pairing_client_hello") == 0 && config_setup_complete()',
                          '    } else if (!decrypted && strcmp(act, "pairing_encrypted") == 0) {')
        gap = (ROOT / "main/resetup_gap.h").read_text()
        gap = gap[gap.index("#define RESETUP_PROMPT_GAP_US"):]
        app = (ROOT / "main/app.c").read_text()
        funcs = []
        for sig in ("static bool talk_press_dismisses(bool talk_key) {", "static bool prompt_shown(void) {",
                    "void app_dismiss_pairing_prompt(void) {", "static void on_pairing_client_finished("):
            if sig in app:
                body = app[app.index(sig):]
                funcs.append(body[:body.index("\n}\n") + 3])
        source = (GHOST.replace("@BRANCH@", branch).replace("@GAP@", gap).replace("@APP@", "\n".join(funcs))
                  .replace("@TALK@", _function_body(app, "bool app_confirm_pairing_press(")))
        self.assertIn("PASS ghost", self.build_and_run(source, []))

    def test_the_proof_gates_bad_takeover_exchange(self) -> None:
        # mg-session-v1.json proof_gates, replayed step for step on one gadget (a set-up release build with a second
        # key): muse_input.c's routing of the gadget's own key (update_prompt_press: an update's prompt would take
        # the press, only that) over the muse_local_press core, and the real hello branch, prompt and press for the
        # re-setup (link pairing modelled as test_a_takeover_hello_that_fails_leaves_no_ghost_prompt's). The
        # takeover's variant is the ESP32's: the refusal goes under the session still current (link_pairing.c checks
        # the hello's key and nonce before any wipe), and the link drops after it (ble_server_delayed_disconnect).
        session = json.loads((PROTOCOLS / "test-vectors/mg-session-v1.json").read_text(encoding="utf-8"))
        exchanges = {ex["name"]: ex for ex in session["proof_gates"]["exchanges"]}
        takeover = exchanges["a takeover hello that fails validation ends the prompt, answered encrypted, the link "
                             "dropped"]
        self.assertEqual(takeover["device"], {"talk_key_only": False, "bad_hello_encrypted": True,
                                              "bad_hello_drops_link": True})

        def resetup(st: dict) -> str:   # the step's resetup state: is a prompt up, is a hello confirmed
            r = st.get("resetup") or {}
            return f"want_resetup({int(r.get('prompt_conn') is not None)}, {int(r.get('confirmed_conn') is not None)});"
        over = []
        for i, st in enumerate(takeover["steps"]):
            ev = st["event"]
            if ev == "resetup_request" and st.get("invalid"):
                setup = st["setup"]
                over.append(f"    hello(HELLO_BAD_KEY);   /* step {i}: {st['note'][:70]} */")
                over.append(f'    assert(!strcmp(status, "{setup["status"]}") && state == IDLE && !armed);')
                over.append(f"    assert(plain == {int('plaintext' in setup)} && {int(bool(setup.get('encrypted')))});")
                over.append(f"    assert(disconnects == {int(takeover['device']['bad_hello_drops_link'])});")
                self.assertEqual(setup["prompt"], "ended")
            elif ev == "resetup_request":
                over.append(f"    resetup_request();   /* step {i}: {st['note'][:70]} */")
            elif ev == "reconnect":
                over.append(f"    reconnect();   /* step {i}: the link the bad hello dropped, again */")
                continue
            elif ev == "press":
                key = "TALK" if st.get("key") == "talk" else "AUX"
                over.append(f"    physical_press(MUSE_BTN_{key}_PRESS, MUSE_BTN_{key}_RELEASE);   /* step {i} */")
            else:
                self.fail(f"a proof_gates event this replay doesn't know: {ev!r}")
            over.append("    " + resetup(st))
        self.assertEqual([st["event"] for st in takeover["steps"]],
                         ["resetup_request", "resetup_request", "reconnect", "press", "resetup_request", "press"])
        ble = (ROOT / "main/ble_server.c").read_text()
        branch = _between(ble, '    if (!decrypted && strcmp(act, "pairing_client_hello") == 0 && config_setup_complete()',
                          '    } else if (!decrypted && strcmp(act, "pairing_encrypted") == 0) {')
        gap = (ROOT / "main/resetup_gap.h").read_text()
        gap = gap[gap.index("#define RESETUP_PROMPT_GAP_US"):]
        app = (ROOT / "main/app.c").read_text()
        funcs = []
        for sig in ("static bool talk_press_dismisses(bool talk_key) {", "static bool prompt_shown(void) {",
                    "void app_dismiss_pairing_prompt(void) {", "static void on_pairing_client_finished("):
            if sig in app:
                body = app[app.index(sig):]
                funcs.append(body[:body.index("\n}\n") + 3])
        inp = (ROOT / "components/muse/muse_input.c").read_text()
        press = inp[inp.index("static unsigned update_prompt_press(unsigned phys)"):]
        press = press[:press.index("\n}\n") + 3]
        btns = "".join(l + "\n" for l in (ROOT / "components/muse/muse_board.h").read_text().splitlines()
                       if l.startswith("#define MUSE_BTN_"))
        ghost = GHOST[:GHOST.index("int main(void) {")]
        ghost = ghost.replace('static void send_hello_answer_plaintext(const char *s) { snprintf(status, sizeof(status), "%s", s); }',
                              'static bool plain;   /* the answer went in plaintext (send_hello_answer_plaintext) */\n'
                              'static void send_hello_answer_plaintext(const char *s) { snprintf(status, sizeof(status), "%s", s); plain = true; }')
        ghost = ghost.replace("    hello_kind = kind;\n    status[0] = 0;\n", "    hello_kind = kind;\n    status[0] = 0;\n    plain = false;\n")
        self.assertIn("plain = true;", ghost)
        self.assertIn("plain = false;", ghost)
        source = ((ghost + TAKEOVER).replace("@BRANCH@", branch).replace("@GAP@", gap).replace("@APP@", "\n".join(funcs))
                  .replace("@TALK@", _function_body(app, "bool app_confirm_pairing_press("))
                  .replace("@BTNS@", btns).replace("@UPDATE_PRESS@", press)
                  .replace("@TAKEOVER_STEPS@", "\n".join(over)))
        out = self.build_and_run(source, [ROOT / "components/muse_restart/muse_local_press_core.c"],
                                 (ROOT / "components/muse_restart/include",))
        self.assertIn("PASS a takeover hello that fails validation ends the prompt, answered encrypted, the link "
                      "dropped", out)

    def test_abandoned_setup_leaves_a_set_up_gadget_as_it_was(self) -> None:
        app = (ROOT / "main/app.c").read_text()
        funcs = _between(app, "static void on_client_connected(void) {", "#if CONFIG_MUSE_GADGET_BLE_STANDALONE || (CONFIG_MUSE_BOOTGUARD && CONFIG_MUSE_GADGET_BLE_AUDIO)\n// A BLE-only gadget, or a Wi-Fi gadget in SAFE")
        for standalone in ("", "#define CONFIG_MUSE_GADGET_BLE_STANDALONE 1\n"):
            source = DISCONNECT.replace("@STANDALONE@", standalone).replace("@FUNCS@", funcs)
            self.assertIn("PASS disconnect", self.build_and_run(source, []))


    def test_a_committed_session_is_used_up(self) -> None:
        # Gadget setup v2: both gadget commits (token-only and the Wi-Fi path)
        # mark their session used up between the commit and auth_ok, and
        # on_provision refuses such a session before it opens the journal or
        # touches Wi-Fi (the harness above runs the token-only path).
        app = (ROOT / "main/app.c").read_text()
        commit = "link_pairing_commit_provisioning(session_generation, commit_setup)"
        auth_ok = 'ble_server_send_pairing_status("auth_ok", session_generation)'
        mark = "s_consumed_session = session_generation;"
        token_only = _function_body(app, "static void provision_token_only_with_gate_held(")
        provision = _function_body(app, "static void on_provision(")
        wifi_path = provision[provision.index("// A gadget commits before auth_ok"):]
        for name, body in (("token-only", token_only), ("Wi-Fi path", wifi_path)):
            with self.subTest(path=name):
                self.assertEqual(body.count(mark), 1)
                self.assertLess(body.index(commit), body.index(mark))
                self.assertLess(body.index(mark), body.index(auth_ok))
        refuse = provision.index("if (session_generation == s_consumed_session)")
        begin = provision.index("reprovision_begin_holding_session_tokens()")
        self.assertLess(refuse, begin)
        self.assertLess(refuse, provision.index("wifi_mgr_connect"))
        self.assertIn('"error_pairing_confirm_required"', provision[refuse:begin])


    def test_token_pair_writers_and_readers_share_one_lock(self) -> None:
        # On a gadget every pair write (the device session's update,
        # the gadget's own refresh, setup's store) and read takes one lock,
        # the refresh through the journalled one-commit replace; real threads
        # on the real config_store.c (tests/link_token_pair_lock_harness.c).
        app = (ROOT / "main/app.c").read_text()
        block = _between(app, "#if CONFIG_MUSE_GADGET_BLE_AUDIO\n// The device session's token updates and a re-setup share the",
                         "static void setup_fail_for_session(")
        stores = _between(app, "static bool store_token_pair_unlocked(", "static void store_pairing_username_unlocked(")
        with tempfile.TemporaryDirectory(prefix="link-pairlock-") as directory:
            d = Path(directory)
            (d / "token_pair_lock.inc").write_text(block + "\n" + stores)
            for control in (False, True):
                binary = d / ("harness_nolock" if control else "harness")
                r = subprocess.run(
                    [*self.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                     *(["-DTOKEN_PAIR_NO_LOCK"] if control else ["-fsanitize=thread"]), "-g", "-O1",
                     "-I", str(d), "-I", str(ROOT / "tests/link_fakes"),
                     "-I", str(ROOT / "main"), str(ROOT / "tests/link_token_pair_lock_harness.c"),
                     str(ROOT / "main/config_store.c"), "-lpthread", "-o", str(binary)],
                    capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                r = tsan_run.run(self, [str(binary)], timeout=300) if not control else \
                    subprocess.run([str(binary)], capture_output=True, text=True, timeout=300)
                if control:
                    # Without the lock the first forced interleaving fails.
                    self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
                    self.assertRegex(r.stderr, r"both stored|saw half of it|interleaved writes")
                else:
                    self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                    self.assertIn("PASS token pair lock", r.stdout)
        # The refresh goes through it; the session's credentials read through it.
        self.assertIn("ok = store_refreshed_pair_unlocked(tokens.access_token, tokens.refresh_token,", app)
        glue = (ROOT / "main/mg_glue.c").read_text()
        self.assertIn("app_gadget_read_session_setup(c->node_id", glue)
        self.assertNotIn("config_get_token_pair(", glue)


    def test_no_credentials_while_a_resetup_commits(self) -> None:
        # From a re-setup's begin until its commit has ended the
        # old setup's sessions, the device session hands out no credentials
        # (app_gadget_session_busy(): the held flag under its lock), wired
        # into every gadget build.
        app = (ROOT / "main/app.c").read_text()
        busy = _function_body(app, "bool app_gadget_session_busy(void)")
        self.assertIn("bool held = s_session_tokens_held;", busy)
        self.assertLess(busy.index("session_tokens_lock_take();"), busy.index("s_session_tokens_held"))
        glue = (ROOT / "main/mg_glue.c").read_text()
        self.assertIn(".session_busy = app_gadget_session_busy,", glue)
        ble = (ROOT / "components/muse_gadget_ble/mg_ble.c").read_text()
        self.assertEqual(ble.count("ops.session_busy = op_session_busy;"), 2)


if __name__ == "__main__":
    unittest.main()
