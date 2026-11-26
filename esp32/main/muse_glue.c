/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_restart.h"
#include "muse_bootguard.h"
#include "muse_stack.h"
#include "muse_glue.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_AVATAR_SYNC
#include "avatar_sync.h"
#include "muse_avatar.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "app.h"
#include "ble_server.h"
#include "config_store.h"
#include "identity.h"
#include "noise_control.h"
#if CONFIG_GADGET_SDK_TOKEN_CONSOLE
#include "sdk_token_console.h"
#endif
#include "session_image.h"
#include "mg_face.h"
#include "muse_clock.h"
#include "token_fp.h"
#if CONFIG_MUSE_WASM_APPS
#include "muse_apps.h"
#endif
#if CONFIG_CAPABILITIES_REGISTRY
#include "board_capabilities.h"
#endif
#include "token_fp.h"
#include "stack_monitor.h"
#include "wifi_known.h"
#include "network_proxy_tls.h"
#include "wifi_mgr.h"

#include "muse_ble.h"
#include "muse_board.h"
#include "muse_input.h"
#include "muse_ui.h"
#include "muse_local_press.h"
#include "muse_passcode.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_state.h"
#if CONFIG_MUSE_REALTIME_VOICE
#include "muse_voice_call.h"
#endif
#if CONFIG_MUSE_GATT_DBHASH
#include "gatt_dbhash.h"
#endif
#if CONFIG_MUSE_GADGET_BLE_AUDIO
#include "mg_ble.h"

// A talk press the gadget itself detected, as mg's source (start_mic's press rule).
static void glue_press(muse_press_t src) {
    mg_ble_press(src == MUSE_PRESS_PANEL ? MG_PRESS_SRC_PANEL : MG_PRESS_SRC_KEY);
}
#endif

static const char *TAG = "link.muse";

#define BIT_STORAGE     BIT0    // NVS + identity ready: Muse may start
#define BIT_LINK        BIT1    // boot connect done: Muse may use the radios
#define BIT_MUSE        BIT2    // Muse settings loaded

#define KEEP_WIFI       BIT0    // wifi on/off or network changed
#define KEEP_BLE        BIT1    // phone setup setting changed
#define KEEP_SAVE       BIT2    // remember s_ssid/s_pass first
#define KEEP_RELOAD     BIT3    // Link may have changed the saved networks
#define KEEP_PROXY      BIT5    // persist a per-network proxy on the keeper
#define KEEP_FORGET     BIT4    // forget the networks marked in s_saved

#define CONNECT_TIMEOUT_MS 20000
#define RETRY_MIN_US (5LL * 1000 * 1000)
#define RETRY_MAX_US (60LL * 1000 * 1000)
#define RETRY_AWAKE_MAX_US (15LL * 1000 * 1000)   // screen on: it says it's reconnecting
// No saved network nearby: look again after this, doubling up to a minute with
// the screen on and ten asleep.
#define AWAY_MIN_US (30LL * 1000 * 1000)
#define AWAY_AWAKE_MAX_US (60LL * 1000 * 1000)
#define AWAY_MAX_US (10LL * 60 * 1000 * 1000)
#define AWAY_FRESH_US (10LL * 1000 * 1000)   // a look this recent that found none stands
#define SCAN_MAX 16

_Static_assert(MUSE_WIFI_SAVED_MAX >= WIFI_KNOWN_MAX, "room for every saved network");

static EventGroupHandle_t s_ready;
static TaskHandle_t s_keeper;
/* muse_stack.h: the boot task (once; its worker then stays parked). */
static muse_task_slot_t s_boot_slot;

// Link config lives in NVS, which tasks with PSRAM stacks must not touch
// (unless flash writes leave the cache on: CONFIG_MUSE_STACKS_IN_PSRAM), so
// Muse reads these copies and the keeper task does the flash I/O.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_ssid[MUSE_SSID_MAX + 1];      // the first saved network
static char s_pass[MUSE_PASS_MAX + 1];
static uint32_t s_creds_gen;
static volatile bool s_linked;
static volatile bool s_joining;
static volatile bool s_nap;     // Muse's screen off a while: stay off Wi-Fi
static char s_fail[40];

// The saved networks, most recently joined first; Wi-Fi passwords stay in NVS.
typedef struct {
    char ssid[MUSE_SSID_MAX + 1];
    bool hidden;
    bool forgotten;             // KEEP_FORGET will forget it
    network_proxy_config_t proxy;
} saved_net_t;
// With their proxies these take about 2.4 KB: in PSRAM where the board allows.
EXT_RAM_BSS_ATTR static saved_net_t s_saved[MUSE_WIFI_SAVED_MAX];
static int s_saved_count;
static bool s_proxy_pending;
static int s_proxy_result;
static char s_proxy_ssid[MUSE_SSID_MAX + 1];
EXT_RAM_BSS_ATTR static network_proxy_config_t s_proxy_next;
// Muse changed the saved networks and the keeper hasn't written them yet, so
// NVS is behind the copies here.
static volatile bool s_editing;
static volatile bool s_chosen;  // s_ssid was just chosen: join it, not whichever is nearest
static volatile bool s_away;    // no saved network showed up in the last scan

static muse_wifi_ap_t s_aps[SCAN_MAX];
static int s_ap_count;
static uint32_t s_scan_gen;
static volatile bool s_scanning;
static int64_t s_saw_saved_us;  // when one of Muse's scans last showed a saved network

static void keeper_kick(uint32_t bits) {
    if (s_keeper) xTaskNotify(s_keeper, bits, eSetBits);
}

// Refreshes the copies from NVS, unless Muse's changes aren't written yet.
// Returns true if the first network changed.
static bool load_creds(void) {
    char ssid[MUSE_SSID_MAX + 1] = {0};
    char pass[MUSE_PASS_MAX + 1] = {0};
    if (!config_get_str("ssid", ssid, sizeof(ssid))) ssid[0] = '\0';
    if (!config_get_str("password", pass, sizeof(pass))) pass[0] = '\0';
    saved_net_t *saved = malloc(sizeof(s_saved));
    int n = -1;
    wifi_known_list_t *list = malloc(sizeof(*list));
    if (list && saved) {
        n = wifi_known_load(list);
        for (int i = 0; i < n; i++) {
            strlcpy(saved[i].ssid, list->nets[i].ssid, sizeof(saved[i].ssid));
            saved[i].hidden = list->nets[i].hidden;
            saved[i].forgotten = false;
            saved[i].proxy = list->nets[i].proxy;
        }
    }
    if (list) { wifi_known_wipe(list); free(list); }
    bool changed = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_editing) {
        if (strcmp(ssid, s_ssid) != 0 || strcmp(pass, s_pass) != 0) {
            strlcpy(s_ssid, ssid, sizeof(s_ssid));
            strlcpy(s_pass, pass, sizeof(s_pass));
            s_creds_gen++;
            changed = true;
        }
        if (n >= 0) {
            network_proxy_wipe(s_saved, sizeof(s_saved));
            memcpy(s_saved, saved, n * sizeof(saved[0]));
            s_saved_count = n;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (saved) { network_proxy_wipe(saved, sizeof(s_saved)); free(saved); }
    memset(pass, 0, sizeof(pass));
    s_linked = config_is_provisioned();
    return changed;
}

// The first saved network not about to be forgotten.
static void first_ssid_locked(char *out, size_t cap) {
    for (int i = 0; i < s_saved_count; i++) {
        if (!s_saved[i].forgotten) {
            strlcpy(out, s_saved[i].ssid, cap);
            return;
        }
    }
    strlcpy(out, s_saved_count ? "" : s_ssid, cap);
}

static void set_fail(const char *why) {
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_fail, why, sizeof(s_fail));
    portEXIT_CRITICAL(&s_lock);
}

// ---- Wi-Fi ------------------------------------------------------------------

static void op_wifi_status(muse_wifi_status_t *out) {
#if CONFIG_MUSE_GADGET_BLE_STANDALONE
    // A BLE-only gadget never brings Wi-Fi up.
    memset(out, 0, sizeof(*out));
    out->state = MUSE_WIFI_OFF;
    return;
#endif
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_lock);
    first_ssid_locked(out->ssid, sizeof(out->ssid));
    strlcpy(out->detail, s_fail, sizeof(out->detail));
    portEXIT_CRITICAL(&s_lock);
    char joining[MUSE_SSID_MAX + 1];

    if (wifi_mgr_is_connected()) {
        out->state = MUSE_WIFI_CONNECTED;
        out->detail[0] = '\0';
        esp_netif_ip_info_t ip;
        esp_netif_t *netif = wifi_mgr_get_netif();
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
            // Not always the first saved network until the keeper moves it up.
            if (ap.ssid[0]) strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
        }
    } else if (!muse_settings_wifi_on()) {
        out->state = MUSE_WIFI_OFF;
        out->detail[0] = '\0';
    } else if (wifi_mgr_joining(joining, sizeof(joining))) {
        // Found one: which, when it's not the first saved network.
        out->state = MUSE_WIFI_CONNECTING;
        strlcpy(out->ssid, joining, sizeof(out->ssid));
        strlcpy(out->detail, "Joining...", sizeof(out->detail));
    } else if (!out->ssid[0]) {
        out->state = MUSE_WIFI_NO_NETWORK;
    } else if (s_away) {
        // Even while looking again, so the screen doesn't flicker.
        out->state = MUSE_WIFI_NOT_NEARBY;
    } else if (out->detail[0] && !s_joining) {
        out->state = MUSE_WIFI_FAILED;
    } else {
        out->state = MUSE_WIFI_CONNECTING;
        strlcpy(out->detail, "Joining...", sizeof(out->detail));
    }
}

static void op_wifi_apply(void) {
    keeper_kick(KEEP_WIFI);
}

static void op_wifi_nap(bool nap) {
    s_nap = nap;
    keeper_kick(KEEP_WIFI);   // waking rejoins without the backoff
}

static void op_wifi_get(char *ssid, char *pass) {
    portENTER_CRITICAL(&s_lock);
    strlcpy(ssid, s_ssid, MUSE_SSID_MAX + 1);
    if (pass) strlcpy(pass, s_pass, MUSE_PASS_MAX + 1);
    portEXIT_CRITICAL(&s_lock);
}

// Whether a scan has shown this network broadcasting its name. Call with
// s_lock held.
static bool seen_in_scan_locked(const char *ssid) {
    for (int i = 0; i < s_ap_count; i++) {
        if (strcmp(s_aps[i].ssid, ssid) == 0) return true;
    }
    return false;
}

static void op_wifi_set(const char *ssid, const char *pass) {
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, ssid[0] ? pass : "", sizeof(s_pass));
    s_creds_gen++;
    s_fail[0] = '\0';
    s_editing = true;
    s_chosen = ssid[0] != '\0';
    s_away = false;
    if (!ssid[0]) {
        s_saved_count = 0;
    } else {
        // First, moving it up or pushing out the oldest.
        int at = 0;
        while (at < s_saved_count && strcmp(s_saved[at].ssid, ssid) != 0) at++;
        network_proxy_config_t proxy = {0};
        if (at < s_saved_count) proxy = s_saved[at].proxy;
        // Picked from a scan, it broadcasts its name; typed, it may not.
        bool hidden = !seen_in_scan_locked(ssid) && (at == s_saved_count || s_saved[at].hidden);
        if (at == s_saved_count && s_saved_count < MUSE_WIFI_SAVED_MAX) s_saved_count++;
        if (at == MUSE_WIFI_SAVED_MAX) at--;
        memmove(&s_saved[1], &s_saved[0], at * sizeof(s_saved[0]));
        strlcpy(s_saved[0].ssid, ssid, sizeof(s_saved[0].ssid));
        s_saved[0].hidden = hidden;
        s_saved[0].forgotten = false;
        s_saved[0].proxy = proxy;
        network_proxy_wipe(&proxy, sizeof(proxy));
    }
    portEXIT_CRITICAL(&s_lock);
    keeper_kick(KEEP_SAVE);
}

static int op_wifi_saved(muse_wifi_saved_t *out, int max) {
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_saved_count && n < max; i++) {
        if (s_saved[i].forgotten) continue;
        strlcpy(out[n].ssid, s_saved[i].ssid, sizeof(out[n].ssid));
        out[n].hidden = s_saved[i].hidden;
        n++;
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

static int op_wifi_proxy_get(const char *ssid, network_proxy_config_t *out) {
    int state = -1;
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_saved_count; i++) {
        if (!s_saved[i].forgotten && !strcmp(ssid, s_saved[i].ssid)) {
            *out = s_saved[i].proxy;
            state = !strcmp(ssid, s_proxy_ssid) ? s_proxy_pending ? 1 : s_proxy_result : 0;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return state;
}

static bool op_wifi_proxy_set(const char *ssid, const network_proxy_config_t *proxy) {
    if (!network_proxy_valid(proxy)) return false;
    bool queued = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; !s_proxy_pending && i < s_saved_count; i++) {
        if (!s_saved[i].forgotten && !strcmp(ssid, s_saved[i].ssid)) {
            strlcpy(s_proxy_ssid, ssid, sizeof(s_proxy_ssid));
            s_proxy_next = *proxy;
            if (proxy->type == NETWORK_PROXY_OFF) memset(&s_proxy_next, 0, sizeof(s_proxy_next));
            s_proxy_pending = true;
            queued = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (queued) keeper_kick(KEEP_PROXY);
    return queued;
}

static void op_wifi_forget(const char *ssid) {
    if (!ssid || !ssid[0]) {
        op_wifi_set("", "");
        return;
    }
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_saved_count; i++) {
        if (strcmp(s_saved[i].ssid, ssid) == 0) {
            s_saved[i].forgotten = true;
            s_editing = true;
            found = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (found) keeper_kick(KEEP_FORGET);
}

#if !CONFIG_MUSE_GADGET_BLE_STANDALONE   // the Wi-Fi scan (BLE-only builds refuse it: op_wifi_scan)
// Whether ssid is a saved network. Call with s_lock held.
static bool saved_locked(const char *ssid) {
    for (int i = 0; i < s_saved_count; i++) {
        if (!s_saved[i].forgotten && strcmp(s_saved[i].ssid, ssid) == 0) return true;
    }
    return false;
}

/* muse_stack.h: the Wi-Fi scan's task, started again for each scan. */
static muse_task_slot_t s_scan_slot;

static void scan_task(void *arg) {
    (void)arg;
    wifi_scan_entry_t found[SCAN_MAX];
    int n = wifi_mgr_scan(found, SCAN_MAX, 0, NULL);
    if (n >= 0) {
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        s_ap_count = 0;
        for (int i = 0; i < n; i++) {
            muse_wifi_ap_t *ap = &s_aps[s_ap_count++];
            strlcpy(ap->ssid, found[i].ssid, sizeof(ap->ssid));
            ap->rssi = found[i].rssi;
            ap->secure = found[i].auth_mode != WIFI_AUTH_OPEN;
            if (saved_locked(ap->ssid)) s_saw_saved_us = now;
        }
        s_scan_gen++;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "scan found %d networks", n);
    } else {
        ESP_LOGI(TAG, "no scan; the radio was busy");   // the last results stand
    }
    s_scanning = false;
    stack_monitor_record(NULL);
    MUSE_TASK_END();
}
#endif

static esp_err_t op_wifi_scan(void) {
#if CONFIG_MUSE_GADGET_BLE_STANDALONE
    // BLE only: Link's session never starts, so BIT_LINK never comes and the
    // scan is refused, as below; this keeps the Wi-Fi scan out of the image.
    return ESP_ERR_INVALID_STATE;
#else
    if (!(xEventGroupGetBits(s_ready) & BIT_LINK) || s_joining) return ESP_ERR_INVALID_STATE;
    if (s_scanning) return ESP_OK;
    s_scanning = true;
    if (muse_task_slot_run(&s_scan_slot, scan_task, "muse_scan", 4096, NULL, 4, NULL, tskNO_AFFINITY) != pdPASS) {
        s_scanning = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

static bool op_wifi_scanning(void) {
    return s_scanning;
}

static int op_wifi_scan_results(muse_wifi_ap_t *out, int max, uint32_t *gen) {
    portENTER_CRITICAL(&s_lock);
    int n = s_ap_count < max ? s_ap_count : max;
    memcpy(out, s_aps, n * sizeof(*out));
    *gen = s_scan_gen;
    portEXIT_CRITICAL(&s_lock);
    return n;
}

// ---- BLE, Muse, setup --------------------------------------------------------

static void op_ble_apply(void) {
    keeper_kick(KEEP_BLE);
}

static bool op_ble_started(void) {
    return ble_server_is_started();
}

static bool op_hatch_linked(void) {
    return s_linked;
}

static bool op_hatch_vm(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name, size_t name_cap,
                        char **vm_token) {
#if CONFIG_MUSE_GADGET_BLE_STANDALONE
    // BLE only: its one caller, the Muse session's VM lookup, never runs (no
    // Wi-Fi), and the account API it would reach needs the network.
    (void)want_vm; (void)vm_id; (void)id_cap; (void)vm_name; (void)name_cap; (void)vm_token;
    return false;
#else
    return s_linked && app_hatch_vm_credentials(want_vm, vm_id, id_cap, vm_name, name_cap, vm_token);
#endif
}

// Pairing and the API client take the SDK token at boot, so a new one
// needs a restart, as when it's set over serial (sdk_token_console.h).
static bool op_sdk_token_set(const char *token) {
    if (!identity_set_sdk_token(token)) return false;
    char fp[TOKEN_FP_TEXT_LEN];
    ESP_LOGI(TAG, "SDK token (%s) set in settings, restarting to use it", token_fp_text(token, fp));
    muse_restart(MUSE_RESTART_SETUP);
    return true;
}

static bool op_sdk_token_changed(void) {
    return identity_sdk_token_changed_since_pairing(NULL, 0);
}

// What Settings shows of it: its length and SHA-256 prefix, never any of it.
static bool op_sdk_token_fp(unsigned *len, char hex[TOKEN_FP_HEX_LEN]) {
    return token_fp_parts(identity_sdk_token(), len, hex);
}

static bool op_talk_press(int64_t pressed_after_us, bool talk_key) {
    return app_confirm_pairing_press(pressed_after_us, talk_key);
}

bool muse_glue_talk_key_only(void) {
    // No touch screen (its Confirm), no menu keys (Enter) and no second key (aux): only the talk key, which never
    // confirms a re-setup, so the board takes one only in setup mode (app_resetup_open).
    const muse_board_t *b = muse_board_get();
    return !b || (!b->touch && !b->keyboard && !b->aux_button);
}

static void op_reset_setup(void) {
    ESP_LOGW(TAG, "setup reset from Muse");
    app_reset_setup_async();
}

static const muse_link_ops_t s_ops = {
    .wifi_status = op_wifi_status,
    .wifi_apply = op_wifi_apply,
    .wifi_get = op_wifi_get,
    .wifi_set = op_wifi_set,
    .wifi_scan = op_wifi_scan,
    .wifi_scanning = op_wifi_scanning,
    .wifi_scan_results = op_wifi_scan_results,
    .ble_apply = op_ble_apply,
    .ble_started = op_ble_started,
    .hatch_linked = op_hatch_linked,
    .token_stored = config_is_provisioned,   // the device token Link setup stored (else "Not set up")
    .sdk_token = identity_sdk_token,
    .sdk_token_set = op_sdk_token_set,
    .sdk_token_changed = op_sdk_token_changed,
    .sdk_token_fp = op_sdk_token_fp,
    .hatch_vm = op_hatch_vm,
    .talk_press = op_talk_press,
    .confirm_dismiss = app_dismiss_pairing_prompt,
    .reset_setup = op_reset_setup,
    .req_ready = noise_ctrl_is_connected,
    .req_open = noise_ctrl_req_open,
    .req_send = noise_ctrl_req_send,
    .req_cancel = noise_ctrl_req_cancel,
#if CONFIG_MUSE_WASM_APPS
    .req_max_body = noise_ctrl_req_max_body,
#endif
    .power_save = noise_ctrl_set_power_save,
    .wifi_nap = op_wifi_nap,
    .ble_quiet = ble_server_set_quiet,
    .wifi_saved = op_wifi_saved,
    .wifi_forget = op_wifi_forget,
    .wifi_proxy_get = op_wifi_proxy_get,
    .wifi_proxy_set = op_wifi_proxy_set,
#if CONFIG_MUSE_SESSION_IMAGE
    .image_ask = session_image_ask,
    .image_status = session_image_status,
#endif
#if CONFIG_MUSE_AVATAR_FACE
    .face_label = mg_face_label,
    .face_avatar_picked = mg_face_avatar_picked,
    .face_pick = mg_face_pick,
    .face_size_small = mg_face_size_small,
    .face_size_set = mg_face_size_set,
#endif
};

// ---- Keeper: joins a saved network and applies BLE, on an internal stack ----
// (in PSRAM with CONFIG_MUSE_STACKS_IN_PSRAM, where flash writes keep the cache on)

// Refreshes the copies after a change that needs no rejoin (Link saving the
// network it's on, or a network moving up or being forgotten): if joined_gen
// was current it stays current. A change Muse made meanwhile still rejoins.
static void reload_creds(uint32_t *joined_gen) {
    uint32_t gen = s_creds_gen;
    if (load_creds() && *joined_gen == gen && wifi_mgr_is_connected()) *joined_gen = gen + 1;
}

// Writes Muse's changes to the saved networks: s_ssid/s_pass go first, then
// the ones marked forgotten go. Leaves a forgotten network that's in use.
static void save_saved(uint32_t pending, int64_t *next_try) {
    // Before reading the copies: a change after this marks them again.
    s_editing = false;
    char gone[MUSE_WIFI_SAVED_MAX][MUSE_SSID_MAX + 1];
    int n_gone = 0;
    char ssid[MUSE_SSID_MAX + 1], pass[MUSE_PASS_MAX + 1];
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_saved_count; i++) {
        if (s_saved[i].forgotten) strlcpy(gone[n_gone++], s_saved[i].ssid, sizeof(gone[0]));
    }
    strlcpy(ssid, s_ssid, sizeof(ssid));
    strlcpy(pass, s_pass, sizeof(pass));
    // As op_wifi_set() judged it.
    bool hidden = ssid[0] && s_saved_count && strcmp(s_saved[0].ssid, ssid) == 0 && s_saved[0].hidden;
    portEXIT_CRITICAL(&s_lock);

    if (pending & KEEP_SAVE) {
        // Link's boot scan may have seen it where Muse's list hadn't.
        app_wifi_set_credentials(ssid, pass, hidden && !wifi_mgr_cached_scan_has(ssid));
    }
    memset(pass, 0, sizeof(pass));
    if (n_gone) {
        wifi_ap_record_t ap;
        bool on = wifi_mgr_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        for (int i = 0; i < n_gone; i++) {
            if (on && strcmp((const char *)ap.ssid, gone[i]) == 0) {
                ESP_LOGI(TAG, "forgetting the wifi network in use; leaving it");
                wifi_mgr_disconnect();
                *next_try = 0;
                on = false;
            }
            if (!app_wifi_forget(gone[i])) ESP_LOGW(TAG, "couldn't forget a wifi network");
        }
    }
}

// Whether a look has just found no saved network nearby and no scan of Muse's
// has shown one since, so looking again would only repeat it.
static bool none_nearby_lately(void) {
    int64_t at = app_wifi_none_nearby_at();
    if (!at || esp_timer_get_time() - at >= AWAY_FRESH_US) return false;
    portENTER_CRITICAL(&s_lock);
    bool seen = s_saw_saved_us > at;
    portEXIT_CRITICAL(&s_lock);
    return !seen;
}

// CONFIG_HOMEHUB_WIFI_SSID (a development override, never saved to NVS):
// Link joins that network itself and wifi_mgr rejoins it, so Muse's keeper
// leaves Wi-Fi alone instead of dropping a network it has no copy of.
#define WIFI_OVERRIDE (CONFIG_HOMEHUB_WIFI_SSID[0] != '\0')

static void save_proxy(int64_t *next_try) {
    // s_proxy_pending owns these fields until the result is published.
    bool ok = wifi_known_set_proxy(s_proxy_ssid, &s_proxy_next);
    network_proxy_config_t check;
    if (ok) ok = wifi_known_get_proxy(s_proxy_ssid, &check)
                 && network_proxy_equal(&check, &s_proxy_next);
    wifi_ap_record_t ap;
    bool active = wifi_mgr_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK
                  && !strcmp((const char *)ap.ssid, s_proxy_ssid);
    if (ok && active) {
        network_proxy_set_active(&s_proxy_next);
        // Both Noise sessions re-open using the new configuration.
        wifi_mgr_disconnect();
        *next_try = 0;
    }
    load_creds();
    portENTER_CRITICAL(&s_lock);
    if (ok) {
        for (int i = 0; i < s_saved_count; i++) {
            if (!strcmp(s_saved[i].ssid, s_proxy_ssid)) s_saved[i].proxy = s_proxy_next;
        }
    }
    s_proxy_result = ok ? 0 : -2;
    s_proxy_pending = false;
    network_proxy_wipe(&s_proxy_next, sizeof(s_proxy_next));
    portEXIT_CRITICAL(&s_lock);
    network_proxy_wipe(&check, sizeof(check));
    printf("@wifi_proxy %s\n", ok ? "saved" : "error storage");
}

static void keeper_task(void *arg) {
    (void)arg;
    xEventGroupWaitBits(s_ready, BIT_LINK | BIT_MUSE, pdFALSE, pdTRUE, portMAX_DELAY);
    load_creds();
    uint32_t joined_gen = s_creds_gen;
    int64_t next_try = 0;
    int64_t backoff = RETRY_MIN_US;
    int64_t away_backoff = AWAY_MIN_US;
    uint32_t pending = KEEP_BLE;
    bool napped = false;
    bool wifi_stopped = false;
    bool setup_done = config_setup_complete();
    if (setup_done && muse_settings_ble_on()) {
        // Registered devices boot with the phone companion off; the settings
        // switch turns it on until the next restart.
        ESP_LOGI(TAG, "registered; phone setup BLE off at boot");
        muse_settings_set_ble_on(false);
    }
    stack_monitor_t stack = STACK_MONITOR_INIT;

    for (;;) {
        if (pending & (KEEP_SAVE | KEEP_FORGET)) {
            save_saved(pending, &next_try);
            reload_creds(&joined_gen);
            next_try = 0;
            backoff = RETRY_MIN_US;
            away_backoff = AWAY_MIN_US;
        }
        if (pending & KEEP_PROXY) save_proxy(&next_try);
        if (pending & KEEP_RELOAD) {
            // Link saves a network it has already joined (provisioning), so
            // don't treat that as a change to rejoin: dropping Wi-Fi mid-setup
            // fails pairing.
            reload_creds(&joined_gen);
            // Once registered the phone is done with BLE, so stop the
            // companion; the settings switch can turn it back on.
            bool done = config_setup_complete();
            if (done && !setup_done && muse_settings_ble_on()) {
                ESP_LOGI(TAG, "setup complete; turning phone setup BLE off");
                muse_settings_set_ble_on(false);
                pending |= KEEP_BLE;
            }
            setup_done = done;
        }
        if (pending & KEEP_BLE) {
            app_ble_companion_set(muse_settings_ble_on());
        }
        if (pending & KEEP_WIFI) {
            backoff = RETRY_MIN_US;
            // Muse starting up, or the screen waking, just after a look found
            // nothing would only look again.
            if (!s_away || !none_nearby_lately()) {
                next_try = 0;
                away_backoff = AWAY_MIN_US;
            }
        }

        bool have = s_ssid[0] != '\0' || WIFI_OVERRIDE;
        napped = napped && s_nap;
        bool stop_wifi = !muse_settings_wifi_on() || !have;
        wifi_stopped = wifi_stopped && stop_wifi;
        if (stop_wifi) {
            // A dropped connection may still have a reconnect timer pending.
            // Cancel it once on entry, even without an IP address. Retry if a
            // setup operation owns the gate, as for the nap below.
            if ((!wifi_stopped || wifi_mgr_is_connected()) && app_wifi_nap()) {
                ESP_LOGI(TAG, "wifi %s; disconnecting", have ? "off" : "forgotten");
                wifi_stopped = true;
            }
            joined_gen = s_creds_gen;
            s_away = false;
        } else if (s_nap) {
            // Once per nap, even if Wi-Fi had already dropped: it stops
            // wifi_mgr's and Link's retries too. Retried each pass while
            // another setup operation holds the gate.
            if (!napped && app_wifi_nap()) {
                ESP_LOGI(TAG, "screen off a while; left wifi");
                napped = true;
            }
        } else if (!WIFI_OVERRIDE && wifi_mgr_is_connected() && joined_gen != s_creds_gen) {
            ESP_LOGI(TAG, "wifi network changed; rejoining");
            wifi_mgr_disconnect();
            next_try = 0;
        } else if (wifi_mgr_is_connected()) {
            joined_gen = s_creds_gen;
            backoff = RETRY_MIN_US;
            away_backoff = AWAY_MIN_US;
            s_away = false;
        } else if (!WIFI_OVERRIDE && !s_editing && esp_timer_get_time() >= next_try) {
            // NVS has Muse's changes (s_editing is clear), so Link joins from
            // the same list the settings show.
            uint32_t gen = s_creds_gen;
            bool first = s_chosen;
            s_chosen = false;
            app_wifi_join_t r;
            if (!first && none_nearby_lately()) {
                r = APP_WIFI_NOT_NEARBY;   // Link's join at boot just looked
            } else {
                s_joining = true;
                if (first) {
                    ESP_LOGI(TAG, "joining %s", s_ssid);
                } else {
                    ESP_LOGI(TAG, "looking for a saved wifi network");
                }
                r = app_wifi_join_saved(CONNECT_TIMEOUT_MS, first);
                s_joining = false;
            }
            int64_t now = esp_timer_get_time();
            if (r == APP_WIFI_JOINED) {
                set_fail("");
                s_away = false;
                joined_gen = gen;
                // Joining a network moves it first.
                reload_creds(&joined_gen);
                backoff = RETRY_MIN_US;
                away_backoff = AWAY_MIN_US;
            } else if (r == APP_WIFI_NOT_NEARBY) {
                // Quietly: nothing here touches the screen. Waking it or a
                // talk press looks again at once (muse_wifi_apply), unless
                // this look was just now.
                set_fail("No saved network nearby");
                s_away = true;
                int64_t wait = muse_state_asleep() || away_backoff < AWAY_AWAKE_MAX_US ? away_backoff
                                                                                      : AWAY_AWAKE_MAX_US;
                next_try = now + wait;
                away_backoff = away_backoff * 2 > AWAY_MAX_US ? AWAY_MAX_US : away_backoff * 2;
                ESP_LOGI(TAG, "no saved wifi network nearby; looking again in %d s", (int)(wait / 1000000));
            } else if (r == APP_WIFI_FAILED) {
                set_fail("Can't join; retrying");
                s_away = false;
                // Waking the screen or a talk press retries at once
                // (muse_wifi_apply).
                int64_t wait = muse_state_asleep() || backoff < RETRY_AWAKE_MAX_US ? backoff : RETRY_AWAKE_MAX_US;
                next_try = now + wait;
                backoff = backoff * 2 > RETRY_MAX_US ? RETRY_MAX_US : backoff * 2;
            } else {
                // Another setup operation had the radio; soon, as it was.
                s_chosen = s_chosen || first;
                next_try = now + 1000 * 1000;
            }
            s_linked = config_is_provisioned();
        }

        pending = 0;
        // Napped, there's nothing to check until the nap ends (KEEP_WIFI).
        xTaskNotifyWait(0, UINT32_MAX, &pending, napped ? portMAX_DELAY : pdMS_TO_TICKS(1000));
        stack_monitor_poll(&stack);
    }
}

static void boot_task(void *arg) {
    (void)arg;
    xEventGroupWaitBits(s_ready, BIT_STORAGE, pdFALSE, pdTRUE, portMAX_DELAY);
    if (muse_bootguard_mode() == MUSE_BOOT_SAFE) {
        // SAFE mode (muse_bootguard.h): no display, audio or input, only
        // the state the link and BLE code touch.
        muse_state_init();
        const muse_board_t *b = muse_board_get();
        if (b && b->safe_init && b->safe_init() != ESP_OK) {
            ESP_LOGE(TAG, "SAFE mode: the board's minimum setup failed");
        }
        // The panel dark (the C6: an AMOLED keeps the last frame
        // across the reset, so SAFE looked like a working home), without its
        // display init or LVGL: a reset held or a backlight off.
        if (b && b->safe_display_off) {
            b->safe_display_off();
            ESP_LOGW(TAG, "SAFE mode: the panel is off");
        } else {
            ESP_LOGW(TAG, "SAFE mode: this board's panel can't be turned off without its display");
        }
        ESP_LOGW(TAG, "SAFE mode: the Muse UI, audio and input stay off");
        // The way out without a phone: the USB
        // console's recovery words, and a 5 s hold of the talk key where the
        // board reads it without Muse (muse_board_t.safe_key). No UI in SAFE.
        if (muse_input_safe_console_start() != ESP_OK) {
            ESP_LOGE(TAG, "SAFE mode: no USB console");
        }
        if (b && b->safe_key) {
            muse_bootguard_safe_watch(b->safe_key, NULL);
        } else {
            ESP_LOGW(TAG, "SAFE mode: no key hold on this board (safe.off or >safe=0 leaves SAFE)");
        }
        MUSE_TASK_END();
    }
    load_creds();
    muse_app_run(muse_board_get());
#if CONFIG_CAPABILITIES_REGISTRY
    // The battery as a device for apps, now the board is up. Here rather than
    // in muse_app_run(), so builds without apps keep muse_app.c as it was.
    board_capabilities_init();
#endif
    xEventGroupSetBits(s_ready, BIT_MUSE);
    stack_monitor_record(NULL);
    MUSE_TASK_END();
}

static bool glue_console_command(const char *line);
static bool glue_link_console(const char *line);

#if !CONFIG_MUSE_GADGET_BLE_STANDALONE && !CONFIG_MUSE_GADGET_BLE_AUDIO
// SAFE mode on a Wi-Fi Muse build without musegadgets: Link's boot
// connect joined Wi-Fi once; with no keeper (it needs Muse), this rejoins the
// saved networks whenever Wi-Fi drops, so the Link session stays
// reachable. muse_keep_vm_session (app.c) brings the session back after.
static void safe_wifi_task(void *arg) {
    (void)arg;
    xEventGroupWaitBits(s_ready, BIT_LINK, pdFALSE, pdTRUE, portMAX_DELAY);   // Link's boot connect is done
    if (WIFI_OVERRIDE) {
        ESP_LOGI(TAG, "SAFE mode: Wi-Fi override, no rejoin loop");
        vTaskDelete(NULL);
    }
    int64_t backoff = RETRY_MIN_US;
    int64_t next_try = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (wifi_mgr_is_connected()) {
            backoff = RETRY_MIN_US;
            continue;
        }
        if (esp_timer_get_time() < next_try) {
            continue;
        }
        app_wifi_join_t r = app_wifi_join_saved(CONNECT_TIMEOUT_MS, false);
        if (r == APP_WIFI_JOINED) {
            ESP_LOGI(TAG, "SAFE mode: Wi-Fi back");
            backoff = RETRY_MIN_US;
            continue;
        }
        int64_t wait = r == APP_WIFI_BUSY ? 1000 * 1000 : backoff;
        next_try = esp_timer_get_time() + wait;
        if (r != APP_WIFI_BUSY) {
            backoff = backoff * 2 > RETRY_MAX_US ? RETRY_MAX_US : backoff * 2;
            ESP_LOGI(TAG, "SAFE mode: no Wi-Fi; looking again in %d s", (int)(wait / 1000000));
        }
    }
}
#endif

#if CONFIG_MUSE_WASM_APPS || CONFIG_MUSE_AVATAR_SYNC
// The console's lines: the gadget's own (glue_console_command, which ends
// with the SDK token's), then avatar sync's and the app loader (muse_apps.h).
static bool console_line(const char *line) {
    if (glue_console_command(line)) return true;
#if CONFIG_MUSE_AVATAR_SYNC
    if (avatar_sync_console(line)) return true;
#endif
#if CONFIG_MUSE_WASM_APPS
    if (muse_apps_console(line)) return true;
#endif
    return false;
}
#endif

void muse_glue_start(void) {
    // The user's way into SAFE (muse_bootguard.h): the board's SAFE key
    // pressed in the first 3 s and held 10 s. Watched from here, the boot's
    // start, so the window is still open; the key is read raw as in SAFE.
    const muse_board_t *board = muse_board_get();
    if (board && board->safe_key) {
        muse_bootguard_entry_watch(board->safe_key);
    }
    // An update's ask wakes the screen; a passcode lock asks for the passcode before the confirm press
    // (muse_local_press.h).
    muse_local_press_set_ui(muse_passcode_locked, muse_input_update_wake);
#if !CONFIG_MUSE_UI_FREEINK   // the e-paper UI draws no update card: every prompted press counts
    muse_local_press_set_shown(muse_ui_update_prompt_shown);   // a press counts only while the card is on show
#endif
    // First, before Link's Wi-Fi, BLE and tunnel and Muse's display take and
    // fragment internal RAM: the CPU's light-sleep retention memory.
    muse_input_power_init();
    s_ready = xEventGroupCreate();
    muse_link_register(&s_ops);
#if CONFIG_MUSE_WASM_APPS || CONFIG_MUSE_AVATAR_SYNC
    muse_input_set_console_hook(console_line);
#if CONFIG_MUSE_WASM_APPS
    muse_apps_start(noise_ctrl_send_command_result);
#endif
#else
    muse_input_set_console_hook(glue_console_command);
#endif
    muse_input_set_safe_console_hook(glue_link_console);
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    muse_input_set_press_hook(glue_press);   // a press the gadget itself detected opens start_mic
#endif
    muse_ble_set_name(identity_ble_name());
#if CONFIG_MUSE_GADGET_BLE_STANDALONE
    // A BLE-only gadget: no phone setup service (it configures Wi-Fi and the
    // Muse session) and no keeper (it joins Wi-Fi).
    (void)keeper_task;
    // Pinned to the UI core as below: the display's SPI interrupt is set up
    // in it, and must land beside the panel's send task, not on the core
    // that decodes video (muse_lcd_bands.h).
    if (muse_task_slot_run(&s_boot_slot, boot_task, "muse_boot", 8192, NULL, 5, NULL, MUSE_UI_CORE) != pdPASS) {
        ESP_LOGE(TAG, "failed to start Muse");
    }
#else
    if (muse_bootguard_mode() == MUSE_BOOT_SAFE) {
        // SAFE mode: no phone setup service (it configures Wi-Fi and the
        // Muse session) and no keeper (it serves Muse's Wi-Fi settings);
        // boot_task leaves the Muse UI off. With musegadgets it's Bluetooth
        // only (app.c run_safe_bluetooth_only); without, Link stays up for
        // recovery (device.health, the VM management commands), so a small loop rejoins Wi-Fi in the keeper's
        // place.
        if (muse_task_slot_run(&s_boot_slot, boot_task, "muse_boot", 8192, NULL, 5, NULL, MUSE_UI_CORE) != pdPASS) {
            ESP_LOGE(TAG, "failed to start Muse");
        }
#if !CONFIG_MUSE_GADGET_BLE_AUDIO
        if (muse_task_create(safe_wifi_task, "safe_wifi", 6144, NULL, 4, NULL, tskNO_AFFINITY) != pdPASS) {
            ESP_LOGE(TAG, "SAFE mode: no Wi-Fi rejoin");
        }
#endif
        return;
    }
    ble_companion_t companion = {
        .svcs = muse_ble_services(),
        .configure_host = muse_ble_configure_host,
        .on_gap_event = muse_ble_gap_event,
    };
    ble_server_set_companion(&companion);
    // Pinned to the UI core, so the display's SPI interrupt, set up in it,
    // lands beside the task that sends to the display (muse_lcd_bands.h).
    if (muse_task_slot_run(&s_boot_slot, boot_task, "muse_boot", 8192, NULL, 5, NULL, MUSE_UI_CORE) != pdPASS
        || muse_task_create(keeper_task, "muse_keep", 6144, NULL, 4, &s_keeper, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "failed to start Muse tasks");
    }
#endif
}

void muse_glue_storage_ready(void) {
    muse_ble_set_name(identity_ble_name());
    xEventGroupSetBits(s_ready, BIT_STORAGE);
}

/* Link's own console lines on the Muse console: the device session's ping,
 * session images, the avatar face and the clock (these reach Muse), then
 * glue_link_console's. */
static bool glue_console_command(const char *line) {
#if CONFIG_MUSE_GADGET_BLE_SESSION
    // One link.heartbeat on the device session; the answer is on mg.session.
    if (!strcmp(line, "session.ping")) {
        noise_ctrl_ble_ping();
        return true;
    }
#endif
#if CONFIG_MUSE_SESSION_IMAGE
    // img.ask, img.show=PATH[,MS], img? (session_image.h).
    if (session_image_console(line)) {
        return true;
    }
#endif
#if CONFIG_MUSE_AVATAR_FACE
    // face?, face.check, face.pick=avatar|pixel, face.size=, face.erase (mg_face.h).
    if (mg_face_console(line)) {
        return true;
    }
#endif
    // clock=UNIX_SECONDS[,OFFSET_MINUTES]: the time as a phone sets it
    // (device action set_unix_time, muse_clock.h), for a bench gadget with no
    // phone to set it (a Wi-Fi build's home shows no clock until then).
    if (!strncmp(line, "clock=", 6)) {
        char *end = NULL;
        long long s = strtoll(line + 6, &end, 10);
        bool off = end && *end == ',';
        long m = off ? strtol(end + 1, &end, 10) : 0;
        bool ok = s > 1600000000LL && end && *end == '\0' && m >= -840 && m <= 840;
        if (ok) {
            muse_clock_set((int64_t)s * 1000, off, (int16_t)m, false, 0, true);
        }
        printf("@clock %s\n", ok ? "set" : "usage: clock=UNIX_SECONDS[,OFFSET_MINUTES]");
        fflush(stdout);
        return true;
    }
    return glue_link_console(line);
}

/* The console words that reach no Muse feature: the GATT table walk behind the
 * Database Hash, musegadgets BLE's bench words, and Link's (sdk_token_console:
 * version, the SDK token, hang=). The normal console takes them through
 * glue_console_command, and SAFE mode's USB console too (muse_input.c,
 * safe_console_line), since SAFE runs musegadgets BLE and Link. */
static bool glue_link_console(const char *line) {
#if CONFIG_MUSE_GATT_DBHASH
    if (!strcmp(line, "gatt.dump")) {
        gatt_dbhash_dump();
        return true;
    }
#endif
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    // ble.pause=SECS[,AA:BB:CC:DD:EE:FF] (1-600 s): off the phone for a while,
    // for A/B tests: the link dropped and no advertising, or with an address
    // only that central may connect (a phone's app that can't let a gadget
    // go), then as before (mg_ble_bench_pause; logged on mg.ble). No unpair,
    // nothing stored, a restart ends it. The ESP-Mosaico's console (ESP-Iris
    // over USB) reaches it here too.
    if (!strncmp(line, "ble.pause=", 10)) {
        mg_ble_bench_pause_text(line + 10);
        return true;
    }
    // bench.out=0|1 / bench.out?: the bench switch (mg_ble_bench_out_word):
    // mic output to a Mac bench client without this connection's token proof.
    // Off at every boot, RAM only, kept across connections (the Nordic UART's
    // own bench.out is for its connection only: mg_ble.c's nus_bench_out).
    if (!strncmp(line, "bench.out", 9)) {
        char out[96];
        if (mg_ble_bench_out_word(line, out, sizeof(out))) {
            printf("@%s\n", out);
            fflush(stdout);
            return true;
        }
    }
    // link.wake=0|1 / link.wake?: listening at every connection event during
    // a link-mode switch (mg_ble_link_wake_word, on by default, RAM only).
    if (!strncmp(line, "link.wake", 9)) {
        char out[160];
        if (mg_ble_link_wake_word(line, out, sizeof(out))) {
            printf("@%s\n", out);
            fflush(stdout);
            return true;
        }
    }
    // link.sleep=ITVL_MIN,ITVL_MAX,LAT / link.sleep=0 / link.sleep?: the sleep
    // mode's request for the link policy's sleep-parameter A/B (mg_ble_link_sleep_word,
    // RAM only, kept across connections).
    if (!strncmp(line, "link.sleep", 10)) {
        char out[288];
        if (mg_ble_link_sleep_word(line, out, sizeof(out))) {
            printf("@%s\n", out);
            fflush(stdout);
            return true;
        }
    }
    // pair.auto / pair.auto=0: the bench pairing auto-confirm, one pairing
    // within 10 min, as the Nordic UART's !PAIR AUTO (for a bench on the USB
    // console, the ESP-Mosaico's ESP-Iris included).
    if (!strcmp(line, "pair.auto") || !strcmp(line, "pair.auto=0")) {
        bool on = line[9] == 0;
        bool ok = mg_ble_pair_auto_arm(on);
        printf("@pair.auto %s\n", !ok ? "unavailable" : on ? "on" : "off");
        fflush(stdout);
        return true;
    }
#endif
#if CONFIG_GADGET_SDK_TOKEN_CONSOLE
    return sdk_token_console_command(line);
#else
    return false;
#endif
}

void muse_glue_link_ready(void) {
    xEventGroupSetBits(s_ready, BIT_LINK);
}

void muse_glue_led_state(led_state_t state) {
    muse_link_state_t st = MUSE_LINK_BOOT;
    switch (state) {
        case LED_STATE_BOOT:
            st = MUSE_LINK_BOOT;
            break;
        case LED_STATE_SETUP_IDLE:
        case LED_STATE_BLE_ADVERTISING:
        case LED_STATE_UNPAIRED:
            st = MUSE_LINK_UNPAIRED;
            break;
        case LED_STATE_BLE_CONNECTED:
            st = MUSE_LINK_PAIRING;
            break;
        case LED_STATE_PAIRING_CONFIRM_REQUIRED:
            st = MUSE_LINK_CONFIRM;
            break;
        case LED_STATE_WIFI_CONNECTING:
        case LED_STATE_WIFI_CONNECTED:
        case LED_STATE_AUTH_OK:
        case LED_STATE_VM_SWITCHING:
        case LED_STATE_VM_OK:
            st = MUSE_LINK_CONNECTING;
            break;
        case LED_STATE_WS_CONNECTED:
            st = MUSE_LINK_ONLINE;
            break;
        case LED_STATE_WS_DISCONNECTED:
            st = MUSE_LINK_OFFLINE;
            break;
        case LED_STATE_ERROR:
            st = MUSE_LINK_ERROR;
            break;
    }
    muse_link_set_state(st);
#if CONFIG_MUSE_AVATAR_SYNC
    if (state == LED_STATE_UNPAIRED) muse_avatar_clear();
    if (state == LED_STATE_UNPAIRED || state == LED_STATE_VM_SWITCHING || state == LED_STATE_PAIRING_CONFIRM_REQUIRED)
        muse_avatar_pairing_changed();
#endif
    // Pairing, provisioning and unpair all move the LED; pick up their config.
    keeper_kick(KEEP_RELOAD);
}

#if CONFIG_MUSE_REALTIME_VOICE
// The call names Link's node as its device (a RAM string set at boot).
const char *muse_voice_call_device_id(void) {
    return identity_node_id();
}
#endif

// Every chat turn names Link's node (muse_chat_priv.h; a RAM string set at boot).
const char *muse_chat_node_id(void) {
    return identity_node_id();
}
