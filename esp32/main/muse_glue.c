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

#include "muse_glue.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
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
#include "stack_monitor.h"
#include "wifi_known.h"
#include "wifi_mgr.h"

#include "muse_ble.h"
#include "muse_board.h"
#include "muse_input.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_state.h"

static const char *TAG = "link.muse";

#define BIT_STORAGE     BIT0    // NVS + identity ready: Muse may start
#define BIT_LINK        BIT1    // boot connect done: Muse may use the radios
#define BIT_MUSE        BIT2    // Muse settings loaded

#define KEEP_WIFI       BIT0    // wifi on/off or network changed
#define KEEP_BLE        BIT1    // phone setup setting changed
#define KEEP_SAVE       BIT2    // remember s_ssid/s_pass first
#define KEEP_RELOAD     BIT3    // Link may have changed the saved networks
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

// Link config lives in NVS, which tasks with PSRAM stacks must not touch, so
// Muse reads these copies and the keeper task does the flash I/O.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_ssid[MUSE_SSID_MAX + 1];      // the first saved network
static char s_pass[MUSE_PASS_MAX + 1];
static uint32_t s_creds_gen;
static volatile bool s_linked;
static volatile bool s_joining;
static volatile bool s_nap;     // Muse's screen off a while: stay off Wi-Fi
static char s_fail[40];

// The saved networks, most recently joined first, without their passwords.
typedef struct {
    char ssid[MUSE_SSID_MAX + 1];
    bool hidden;
    bool forgotten;             // KEEP_FORGET will forget it
} saved_net_t;
static saved_net_t s_saved[MUSE_WIFI_SAVED_MAX];
static int s_saved_count;
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
    saved_net_t saved[MUSE_WIFI_SAVED_MAX];
    int n = -1;
    wifi_known_list_t *list = malloc(sizeof(*list));
    if (list) {
        n = wifi_known_load(list);
        for (int i = 0; i < n; i++) {
            strlcpy(saved[i].ssid, list->nets[i].ssid, sizeof(saved[i].ssid));
            saved[i].hidden = list->nets[i].hidden;
            saved[i].forgotten = false;
        }
        wifi_known_wipe(list);
        free(list);
    }
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
            memcpy(s_saved, saved, n * sizeof(saved[0]));
            s_saved_count = n;
        }
    }
    portEXIT_CRITICAL(&s_lock);
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
        // Picked from a scan, it broadcasts its name; typed, it may not.
        bool hidden = !seen_in_scan_locked(ssid) && (at == s_saved_count || s_saved[at].hidden);
        if (at == s_saved_count && s_saved_count < MUSE_WIFI_SAVED_MAX) s_saved_count++;
        if (at == MUSE_WIFI_SAVED_MAX) at--;
        memmove(&s_saved[1], &s_saved[0], at * sizeof(s_saved[0]));
        strlcpy(s_saved[0].ssid, ssid, sizeof(s_saved[0].ssid));
        s_saved[0].hidden = hidden;
        s_saved[0].forgotten = false;
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

// Whether ssid is a saved network. Call with s_lock held.
static bool saved_locked(const char *ssid) {
    for (int i = 0; i < s_saved_count; i++) {
        if (!s_saved[i].forgotten && strcmp(s_saved[i].ssid, ssid) == 0) return true;
    }
    return false;
}

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
    vTaskDelete(NULL);
}

static esp_err_t op_wifi_scan(void) {
    if (!(xEventGroupGetBits(s_ready) & BIT_LINK) || s_joining) return ESP_ERR_INVALID_STATE;
    if (s_scanning) return ESP_OK;
    s_scanning = true;
    if (xTaskCreate(scan_task, "muse_scan", 4096, NULL, 4, NULL) != pdPASS) {
        s_scanning = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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

// ---- BLE, Hatch, setup -------------------------------------------------------

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
    return s_linked && app_hatch_vm_credentials(want_vm, vm_id, id_cap, vm_name, name_cap, vm_token);
}

static bool op_talk_press(void) {
    return app_confirm_pairing_press();
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
    .hatch_vm = op_hatch_vm,
    .talk_press = op_talk_press,
    .reset_setup = op_reset_setup,
    .req_ready = noise_ctrl_is_connected,
    .req_open = noise_ctrl_req_open,
    .req_send = noise_ctrl_req_send,
    .req_cancel = noise_ctrl_req_cancel,
    .power_save = noise_ctrl_set_power_save,
    .wifi_nap = op_wifi_nap,
    .wifi_saved = op_wifi_saved,
    .wifi_forget = op_wifi_forget,
};

// ---- Keeper: joins a saved network and applies BLE, on an internal stack ----

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

        bool have = s_ssid[0] != '\0';
        napped = napped && s_nap;
        if (!muse_settings_wifi_on() || !have) {
            if (wifi_mgr_is_connected()) {
                ESP_LOGI(TAG, "wifi %s; disconnecting", have ? "off" : "forgotten");
                wifi_mgr_disconnect();
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
        } else if (wifi_mgr_is_connected() && joined_gen != s_creds_gen) {
            ESP_LOGI(TAG, "wifi network changed; rejoining");
            wifi_mgr_disconnect();
            next_try = 0;
        } else if (wifi_mgr_is_connected()) {
            joined_gen = s_creds_gen;
            backoff = RETRY_MIN_US;
            away_backoff = AWAY_MIN_US;
            s_away = false;
        } else if (!s_editing && esp_timer_get_time() >= next_try) {
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
    load_creds();
    muse_app_run(muse_board_get());
    xEventGroupSetBits(s_ready, BIT_MUSE);
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

void muse_glue_start(void) {
    // First, before Link's Wi-Fi, BLE and tunnel and Muse's display take and
    // fragment internal RAM: the CPU's light-sleep retention memory.
    muse_input_power_init();
    s_ready = xEventGroupCreate();
    muse_link_register(&s_ops);
    muse_ble_set_name(identity_ble_name());
    ble_companion_t companion = {
        .svcs = muse_ble_services(),
        .configure_host = muse_ble_configure_host,
        .on_gap_event = muse_ble_gap_event,
    };
    ble_server_set_companion(&companion);
    // Pinned to the UI core, so the display's SPI interrupt, set up in it,
    // lands beside the task that sends to the display (muse_lcd_bands.h).
    if (xTaskCreatePinnedToCore(boot_task, "muse_boot", 8192, NULL, 5, NULL, MUSE_UI_CORE) != pdPASS
        || xTaskCreate(keeper_task, "muse_keep", 6144, NULL, 4, &s_keeper) != pdPASS) {
        ESP_LOGE(TAG, "failed to start Muse tasks");
    }
}

void muse_glue_storage_ready(void) {
    muse_ble_set_name(identity_ble_name());
    xEventGroupSetBits(s_ready, BIT_STORAGE);
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
    // Pairing, provisioning and unpair all move the LED; pick up their config.
    keeper_kick(KEEP_RELOAD);
}
