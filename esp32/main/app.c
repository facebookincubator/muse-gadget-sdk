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

#include "app.h"
#include "stack_monitor.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "esp_heap_caps.h"
#if CONFIG_MUSE_WATCHER_CAMERA
#include "freertos/idf_additions.h"
#endif
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_efuse.h"

#include "lwip/ip4_addr.h"

#include "identity.h"
#include "config_store.h"
#include "wifi_mgr.h"
#include "wifi_known.h"
#include "customer_product_info.h"
#include "ble_server.h"
#include "factory_test.h"
#include "vm_api.h"
#include "vm_connect.h"
#include "noise_control.h"
#include "image_fetch.h"
#include "noise_tunnel.h"
#include "led_status.h"
#if CONFIG_HOMEHUB_DISPLAY_TEXT
#include "x3_display.h"
#endif
#if CONFIG_HOMEHUB_TEXT_PAGES
#include "x3_pages.h"
#endif
#include "button.h"
#include "tunnel_netif.h"
#include "net_discovery.h"
#include "ota.h"
#include "link_pairing.h"
#include "bug_report.h"
#include "diagnostic_log.h"
#if CONFIG_HOMEHUB_VOICE
#include "voice.h"
#endif
#if CONFIG_HOMEHUB_SENSECAP_SENSORS
#include "sensecap_sensors.h"
#endif
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif
#if CONFIG_MUSE_ENABLED
#include "muse_glue.h"
// Muse joins Wi-Fi from its own settings, before or without pairing.
#define WIFI_WITHOUT_PAIRING 1
#else
#define WIFI_WITHOUT_PAIRING 0
#endif

static const char *TAG = "link.app";
static const char *HEARTBEAT_TAG = "link.heartbeat";

#define WIFI_CONNECT_TIMEOUT_MS 60000
#define WIFI_CHANNEL_KEY        "wifi_channel"
#define WIFI_CHANNEL_VALUE_BYTES 4
#define WIFI_JOIN_TRIES         3       // saved networks tried per join
#define WIFI_JOIN_TRY_MIN_MS    8000    // each one's share of the timeout, at least
#define WIFI_JOIN_SCAN_MAX      40      // as many networks as a scan reports
#define WIFI_QUICK_MIN_RSSI     (-75)   // near enough to join without a full scan
#define MAX_SCAN_RESULTS        25
#define TOKEN_BUF_BYTES         2048
#define VM_AUTH_REFRESH_RETRY_INTERVAL_US (60LL * 1000000LL)
#define VM_AUTH_REFRESH_MAX_INTERVAL_US   (30LL * 60LL * 1000000LL)

// Slow proactive refresh, on top of the reactive one. Guards against the
// access token's real lifetime being shorter than the stated 365 days.
// Uptime-based (see the Kconfig help): the device has no wall clock, so a
// device rebooting more often than this never trips it — those boots
// re-validate via fetch_vms instead. 0 disables.
#define TOKEN_PERIODIC_REFRESH_INTERVAL_US \
    ((int64_t)CONFIG_HOMEHUB_TOKEN_REFRESH_INTERVAL_DAYS \
     * 24LL * 60LL * 60LL * 1000000LL)
#define PAIRING_CONFIRM_WINDOW_MS   (60 * 1000)
#define PAIRING_CONFIRMED_IDLE_TIMEOUT_MS (120 * 1000)

// OTA rollback: how long a freshly-installed (PENDING_VERIFY) image has to
// reach the authenticated control WebSocket before the bootloader reverts it.
// The control WS is the recovery channel — if a new image can't reach it,
// no future device.ota could repair the device, so rolling back is correct.
#define OTA_VERIFY_TIMEOUT_US (300LL * 1000000LL)

// Static IPs for the raw L3 tunnel. ESP side hosts .1; VM side .2.
#define TUNNEL_IP_ESP   "10.42.0.1"
#define TUNNEL_NETMASK  "255.255.255.0"
#define TUNNEL_GATEWAY  "10.42.0.2"

// Ownership-neutral gate for operations that can replace or erase setup state.
// Remote control reserves it in the command task and releases it from a worker,
// so this must be a binary semaphore rather than a task-owned FreeRTOS mutex.
static SemaphoreHandle_t s_operation_gate = NULL;
static SemaphoreHandle_t s_auth_lock = NULL;
static SemaphoreHandle_t s_setup_window_lock = NULL;
static int64_t s_last_vm_auth_refresh_attempt_us = 0;
// Uptime of the last proactive-refresh attempt. 0 = none this boot, so the
// first one falls due once uptime alone exceeds the interval.
static int64_t s_last_periodic_token_refresh_us = 0;
// The SDK token reaches Muse only in refresh bodies until apps forward it at
// mint, so each boot of a token-bearing build attempts one refresh to report it.
static bool s_sdk_token_report_attempted = false;
static int64_t s_vm_auth_refresh_interval_us = VM_AUTH_REFRESH_RETRY_INTERVAL_US;
static uint8_t s_vm_auth_refusals = 0;
static bool s_vm_auth_refresh_pending = false;
static atomic_bool s_setup_reset_pending = ATOMIC_VAR_INIT(false);
static bool s_ble_started = false;
static bool s_ota_pending_verify = false;
static uint32_t s_confirm_generation = 0;
static uint32_t s_confirm_session_generation = 0;
static TaskHandle_t s_confirm_timeout_task = NULL;
#if CONFIG_MUSE_ENABLED
static volatile bool s_muse_napping = false;   // app_wifi_nap(), or no network nearby, until the next join
static volatile bool s_muse_resume_vm = false;  // napped with a VM session: resume it
static SemaphoreHandle_t s_muse_joined = NULL;
#endif

enum scan_refresh_flag {
    SCAN_REFRESH_RUNNING = 1u << 0,
    SCAN_REFRESH_REPLY_PENDING = 1u << 1,
};
static atomic_uint s_scan_refresh_flags = ATOMIC_VAR_INIT(0);

// Pairing progress (idle->wifi->auth->vm->done, or failed); logged + heartbeat.
static const char *s_setup_stage = "idle";

// ---- UI state (logged in heartbeat, LED driven by state enum) -------------
static char s_ui_ble[40]    = "idle";
static char s_ui_wifi[48]   = "down";
static char s_ui_vm[48]     = "--";
static char s_ui_ws[16]     = "down";
static char s_ui_raw[16]    = "down";
static char s_ui_status[48] = "boot";

static void ui_set_status(const char *s) {
    strncpy(s_ui_status, s, sizeof(s_ui_status) - 1);
    s_ui_status[sizeof(s_ui_status) - 1] = '\0';
}

static void ui_set_wifi(const char *ssid) {
    if (ssid && *ssid) {
        snprintf(s_ui_wifi, sizeof(s_ui_wifi), "%s", ssid);
    } else {
        strcpy(s_ui_wifi, "down");
    }
}

static void ui_set_vm(const char *name) {
    char prev[sizeof(s_ui_vm)];
    strcpy(prev, s_ui_vm);
    if (name && *name) {
        snprintf(s_ui_vm, sizeof(s_ui_vm), "%s", name);
    } else {
        strcpy(s_ui_vm, "--");
    }
    // A different VM means a different agent; drop its name until the new
    // session reports one.
    if (strcmp(prev, s_ui_vm) != 0) led_status_set_title(NULL);
}

static void ui_set_ble(const char *s) {
    strncpy(s_ui_ble, s, sizeof(s_ui_ble) - 1);
    s_ui_ble[sizeof(s_ui_ble) - 1] = '\0';
}

static void setup_stage_set(const char *stage) {
    if (s_setup_stage != stage) {
        ESP_LOGI(TAG, "setup stage: %s -> %s", s_setup_stage, stage);
    }
    s_setup_stage = stage;
}

static void setup_window_lock_take(void) {
    if (s_setup_window_lock) xSemaphoreTake(s_setup_window_lock, portMAX_DELAY);
}

static void setup_window_lock_give(void) {
    if (s_setup_window_lock) xSemaphoreGive(s_setup_window_lock);
}

static uint32_t advance_confirm_generation_locked(void) {
    if (s_confirm_timeout_task) {
        // The waiter owns no resources until it clears this handle under
        // the same lock after its delay.
        vTaskDelete(s_confirm_timeout_task);
        s_confirm_timeout_task = NULL;
    }
    return ++s_confirm_generation;
}

static BaseType_t start_confirm_timeout(TaskFunction_t task, const char *name,
                                        uint32_t generation, uint32_t session_generation) {
    setup_window_lock_take();
    BaseType_t rc = pdPASS;
    if (s_confirm_generation == generation
        && link_pairing_session_is_current(session_generation)) {
        s_confirm_session_generation = session_generation;
        rc = xTaskCreate(task, name, 4096, (void *)(uintptr_t)generation, 4,
                         &s_confirm_timeout_task);
    }
    setup_window_lock_give();
    return rc;
}

static void open_setup_window(const char *reason);
static void heap_snapshot(const char *label);
static void confirmed_idle_timeout_task(void *arg);

// ---- Helpers ---------------------------------------------------------------

// Lazy-creates the tunnel netif on first raw tunnel connect. Idempotent.
static void tunnel_ensure_started(void) {
#if CONFIG_HOMEHUB_TUNNEL
    static bool s_tunnel_started = false;
    if (s_tunnel_started) return;
    ip4_addr_t ip, mask, gw;
    ip4addr_aton(TUNNEL_IP_ESP, &ip);
    ip4addr_aton(TUNNEL_NETMASK, &mask);
    ip4addr_aton(TUNNEL_GATEWAY, &gw);
    if (tunnel_netif_start(&ip, &mask, &gw)) {
        s_tunnel_started = true;
    } else {
        ESP_LOGE(TAG, "tunnel netif failed to start");
    }
#endif
}

// Deliberately does NOT touch s_vm_auth_refusals. This runs whenever a pair is
// stored, including the 200-with-a-dead-token case, so resetting the refusal
// count here would cancel the backoff on every cycle of exactly the loop it
// exists to brake. Only note_vm_credentials_accepted() — a fetch that actually
// succeeded — clears it.
static void note_access_token_fresh(void) {
    s_last_vm_auth_refresh_attempt_us = 0;
    // A newly stored pair restarts the proactive clock whatever put it there
    // (pairing, reactive refresh, or a proactive one), so we don't refresh
    // again straight after having just replaced the credential.
    s_last_periodic_token_refresh_us = esp_timer_get_time();
}

static void auth_lock_take(void) {
    if (s_auth_lock) xSemaphoreTake(s_auth_lock, portMAX_DELAY);
}

static void auth_lock_give(void) {
    if (s_auth_lock) xSemaphoreGive(s_auth_lock);
}

// Count one refusal of freshly-obtained VM credentials and widen the floor
// between refresh attempts accordingly: 1m/2m/4m/8m/16m/30m.
// note_vm_credentials_accepted() drops it back to 1m.
//
// A separate counter is needed because note_access_token_fresh() zeroes
// s_last_vm_auth_refresh_attempt_us whenever a token pair is stored — right
// when the new pair works, wrong when it does not. The API can return 200
// from a refresh and hand back an access token that every endpoint then
// rejects, so the pair IS stored, the floor IS cleared, and the only thing
// left pacing us is noise_ctrl's reconnect backoff: three short sessions at a
// 15 s cap, i.e. a refresh roughly every 45 s, indefinitely.
//
// A 401 from fetch_vms no longer unpairs the device, so nothing else
// terminates that loop. This is what bounds it.
static void note_vm_credentials_refused(void) {
    auth_lock_take();
    if (s_vm_auth_refusals < UINT8_MAX) s_vm_auth_refusals++;
    unsigned doublings = s_vm_auth_refusals - 1u;
    if (doublings > 5u) doublings = 5u;
    int64_t interval = VM_AUTH_REFRESH_RETRY_INTERVAL_US << doublings;
    if (interval > VM_AUTH_REFRESH_MAX_INTERVAL_US) {
        interval = VM_AUTH_REFRESH_MAX_INTERVAL_US;
    }
    s_vm_auth_refresh_interval_us = interval;
    s_last_vm_auth_refresh_attempt_us = esp_timer_get_time();
    unsigned refusals = s_vm_auth_refusals;
    auth_lock_give();
    ESP_LOGW(TAG,
             "VM credentials refused %u time(s) in a row; next refresh no "
             "sooner than %llds",
             refusals, (long long)(interval / 1000000));
}

static void note_vm_credentials_accepted(void) {
    auth_lock_take();
    bool was_backing_off = s_vm_auth_refusals > 0;
    s_vm_auth_refusals = 0;
    s_vm_auth_refresh_interval_us = VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    s_last_vm_auth_refresh_attempt_us = 0;
    auth_lock_give();
    if (was_backing_off) {
        ESP_LOGI(TAG, "VM credentials accepted; refresh backoff reset");
    }
}

static bool operation_gate_take(TickType_t wait_ticks, const char *operation) {
    if (s_operation_gate
        && xSemaphoreTake(s_operation_gate, wait_ticks) == pdTRUE) {
        return true;
    }
    ESP_LOGW(TAG, "%s blocked; another setup operation is active", operation);
    return false;
}

static void operation_gate_give(void) {
    if (s_operation_gate) xSemaphoreGive(s_operation_gate);
}

static bool restore_wifi_channel_hint(const char *ssid) {
    char value[WIFI_CHANNEL_VALUE_BYTES] = {0};
    if (!ssid || !ssid[0]
        || !config_get_str(WIFI_CHANNEL_KEY, value, sizeof(value))) {
        return false;
    }

    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 || parsed > UINT8_MAX) {
        ESP_LOGW(TAG, "invalid saved wifi channel; ignoring");
        config_erase_key(WIFI_CHANNEL_KEY);
        return false;
    }
    if (!wifi_mgr_seed_channel_hint(ssid, (uint8_t)parsed)) {
        ESP_LOGW(TAG, "unsupported saved wifi channel; ignoring");
        config_erase_key(WIFI_CHANNEL_KEY);
        return false;
    }
    return true;
}

static void persist_connected_wifi_channel(void) {
    uint8_t channel = 0;
    if (!wifi_mgr_get_connected_channel(&channel)) return;

    char value[WIFI_CHANNEL_VALUE_BYTES] = {0};
    snprintf(value, sizeof(value), "%u", (unsigned)channel);

    char current[WIFI_CHANNEL_VALUE_BYTES] = {0};
    if (config_get_str(WIFI_CHANNEL_KEY, current, sizeof(current))
        && strcmp(current, value) == 0) {
        return;
    }
    if (!config_set_str(WIFI_CHANNEL_KEY, value)) {
        ESP_LOGW(TAG, "failed to save wifi channel");
    }
}

// The saved channel of the first network, seeded for wifi_mgr_connect(); 0
// if there's none.
static uint8_t saved_wifi_channel(const char *ssid) {
    char value[WIFI_CHANNEL_VALUE_BYTES] = {0};
    if (!restore_wifi_channel_hint(ssid)
        || !config_get_str(WIFI_CHANNEL_KEY, value, sizeof(value))) {
        return 0;
    }
    return (uint8_t)strtoul(value, NULL, 10);
}

// app_wifi_none_nearby_at(). Written by the join at boot, then only by Muse's
// keeper task, which reads it, so it needs no lock.
static int64_t s_none_nearby_us;

typedef enum {
    JOIN_ANY,           // scan, then join a saved network that showed up
    JOIN_ANY_CACHED,    // the same, starting from the boot scan
    JOIN_FIRST,         // just the first saved network, which was just chosen
} join_mode_t;

static int join_ms_left(int64_t deadline_us) {
    int64_t left = (deadline_us - esp_timer_get_time()) / 1000;
    return left > 0 ? (int)left : 0;
}

// Scans, waiting out another scan or a connect. Returns -1 if none ran before
// the deadline.
static int join_scan(int64_t deadline_us, wifi_scan_entry_t *seen,
                     uint8_t channel, const char *ssid) {
    for (;;) {
        int n = wifi_mgr_scan(seen, WIFI_JOIN_SCAN_MAX, channel, ssid);
        if (n >= 0 || join_ms_left(deadline_us) < 1000) return n;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static int find_saved_network(const wifi_known_list_t *list, const char *ssid) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->nets[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

// Joins saved network k. The one joined goes first in the list, so it's tried
// first next time and its channel is the one saved. hidden: whether the scan
// showed it doesn't broadcast its name (-1: it couldn't tell).
static bool join_saved_network(const wifi_known_list_t *list, int k,
                               int timeout_ms, bool led, int hidden,
                               char *joined) {
    const wifi_known_net_t *net = &list->nets[k];
    ESP_LOGI(TAG, "joining saved network %s", net->ssid);
    ui_set_status("wifi_connecting");
    if (led) led_status_set_state(LED_STATE_WIFI_CONNECTING);
    if (!wifi_mgr_connect(net->ssid, net->password, timeout_ms)) {
        ESP_LOGW(TAG, "couldn't join %s", net->ssid);
        return false;
    }
    bool now_hidden = hidden < 0 ? net->hidden : hidden != 0;
    if ((k > 0 || now_hidden != net->hidden)
        && !wifi_known_remember(net->ssid, net->password, now_hidden)) {
        ESP_LOGW(TAG, "failed to move %s first among the saved networks",
                 net->ssid);
    }
    snprintf(joined, WIFI_KNOWN_SSID_MAX + 1, "%s", net->ssid);
    return true;
}

// Joins whichever saved network is in range: the first on its saved channel
// if it's there, else the ones a scan shows, strongest first, then any hidden
// ones, which only answer a probe that names them. Tries at most
// WIFI_JOIN_TRIES within timeout_ms. led: show joining on the LED. joined gets
// the network's name (WIFI_KNOWN_SSID_MAX + 1 bytes). Needs an internal-RAM
// stack (NVS).
static app_wifi_join_t join_saved_networks(int timeout_ms, join_mode_t mode,
                                           bool led, char *joined) {
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    wifi_known_list_t *list = malloc(sizeof(*list));
    wifi_scan_entry_t *seen = malloc(WIFI_JOIN_SCAN_MAX * sizeof(*seen));
    bool tried[WIFI_KNOWN_MAX] = {0};
    int tries = 0;
    bool scanned = false;
    bool ok = false;
    int n = -1;
    uint8_t channel = 0;
    joined[0] = '\0';
    if (!list || !seen) {
        ESP_LOGW(TAG, "no memory to join wifi");
        goto done;
    }
    if (wifi_known_load(list) == 0) {
        tries = 1;   // nothing to join: a failure, not a network out of range
        goto done;
    }
    // Stops wifi_mgr's own reconnect attempts, which would take the radio
    // from the scans below and churn while no network is in range.
    if (!wifi_mgr_is_connected()) wifi_mgr_disconnect();

    if (mode == JOIN_FIRST) {
        restore_wifi_channel_hint(list->nets[0].ssid);
        tries = 1;
        ok = join_saved_network(list, 0, join_ms_left(deadline), led, -1, joined);
        goto done;
    }

    channel = saved_wifi_channel(list->nets[0].ssid);
    if (channel && mode == JOIN_ANY) {
        // A probe on one channel takes a moment; a full scan takes seconds.
        n = join_scan(deadline, seen, channel, list->nets[0].ssid);
        scanned = n >= 0;
        if (n > 0 && seen[0].rssi >= WIFI_QUICK_MIN_RSSI) {
            tried[0] = true;
            tries++;
            ok = join_saved_network(list, 0, join_ms_left(deadline) / 2, led,
                                    -1, joined);
            if (ok) goto done;
        }
    }

    n = mode == JOIN_ANY_CACHED ? wifi_mgr_get_cached_scan(seen, WIFI_JOIN_SCAN_MAX) : 0;
    if (n <= 0) n = join_scan(deadline, seen, 0, NULL);
    scanned = scanned || n >= 0;
    while (tries < WIFI_JOIN_TRIES) {
        // The strongest saved network in the scan not yet tried.
        int best = -1;
        int left = 0;
        for (int i = 0; i < n; i++) {
            int k = find_saved_network(list, seen[i].ssid);
            if (k < 0 || tried[k]) continue;
            left++;
            if (best < 0 || seen[i].rssi > seen[best].rssi) best = i;
        }
        int ms = join_ms_left(deadline);
        if (best < 0 || ms < 2000) break;
        if (left > WIFI_JOIN_TRIES - tries) left = WIFI_JOIN_TRIES - tries;
        int share = ms / left < WIFI_JOIN_TRY_MIN_MS ? WIFI_JOIN_TRY_MIN_MS : ms / left;
        int k = find_saved_network(list, seen[best].ssid);
        tried[k] = true;
        tries++;
        ok = join_saved_network(list, k, share < ms ? share : ms, led, 0, joined);
        if (ok) goto done;
    }

    for (int k = 0; k < list->count && tries < WIFI_JOIN_TRIES; k++) {
        if (!list->nets[k].hidden || tried[k]) continue;
        if (join_ms_left(deadline) < WIFI_JOIN_TRY_MIN_MS) break;
        n = join_scan(deadline, seen, 0, list->nets[k].ssid);
        scanned = scanned || n >= 0;
        if (n <= 0) continue;
        tried[k] = true;
        tries++;
        ok = join_saved_network(list, k, join_ms_left(deadline), led, 1, joined);
        if (ok) goto done;
    }

done:
    if (list) wifi_known_wipe(list);
    free(list);
    free(seen);
    if (!ok && tries == 0 && !scanned) return APP_WIFI_BUSY;
    s_none_nearby_us = ok || tries > 0 ? 0 : esp_timer_get_time();
    if (ok) return APP_WIFI_JOINED;
    if (tries > 0) return APP_WIFI_FAILED;
    ESP_LOGI(TAG, "no saved wifi network nearby");
    return APP_WIFI_NOT_NEARBY;
}


// A network the phone set up, just joined: first among the saved networks,
// alongside the others rather than in place of them. One the phone's scan
// didn't show must hide its name.
static void remember_joined_wifi(const char *ssid, const char *password) {
    if (!wifi_known_remember(ssid, password, !wifi_mgr_cached_scan_has(ssid))) {
        ESP_LOGW(TAG, "failed to save wifi network");
    }
}

static void disconnect_vm_transports(void) {
    // The tunnel is multiplexed on the control session; disconnecting the
    // control channel tears down both.
    noise_ctrl_disconnect();
    tunnel_netif_set_link(false);
    strcpy(s_ui_ws, "down");
    strcpy(s_ui_raw, "down");
}

static bool clear_setup_credentials(void) {
    auth_lock_take();
    s_last_vm_auth_refresh_attempt_us = 0;
    s_vm_auth_refresh_interval_us = VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    s_vm_auth_refusals = 0;
    bool cleared = config_clear_setup();
    auth_lock_give();
    if (!cleared) {
        ESP_LOGE(TAG, "setup credential deletion could not be verified");
    }
    return cleared;
}

// Reset to a clean, unprovisioned state: drop transports, disconnect Wi-Fi, and
// clear all local setup config. Every pairing-recovery path funnels through
// here; BLE is left as-is for the caller to keep advertising or reboot. Success
// means NVS committed every erase and readback verified each credential absent.
static void setup_disconnect_to_clean(void) {
    disconnect_vm_transports();
    vm_api_set_base_url(NULL);
    noise_ctrl_set_host(NULL);
#if CONFIG_MUSE_ENABLED
    s_muse_resume_vm = false;
#endif
    wifi_mgr_disconnect();
    ui_set_vm(NULL);
    ui_set_wifi(NULL);
    diagnostic_log_clear();
}

static bool setup_wipe_to_clean(void) {
    setup_disconnect_to_clean();
    return clear_setup_credentials();
}

static void setup_fail_for_session(const char *stage, const char *status,
                                   uint32_t session_generation) {
    ESP_LOGW(TAG, "pairing failed at stage '%s' (%s); wiping partial state for clean re-pair",
             stage, status);
    // The operation gate still belongs to this worker, so a replacement
    // request cannot have stored credentials. Roll back this worker's partial
    // state even when its BLE session has already gone away.
    bool cleared = setup_wipe_to_clean();
    const char *reported_status = cleared ? status : "error_storage";
    setup_window_lock_take();
    if (session_generation != 0
        && !link_pairing_session_is_current(session_generation)) {
        setup_window_lock_give();
        return;
    }
    setup_stage_set("failed");
    ui_set_status(reported_status);
    led_status_set_state(LED_STATE_ERROR);
    setup_window_lock_give();
    if (session_generation != 0) {
        ble_server_send_pairing_status(reported_status, session_generation);
    } else {
        ble_server_send_status(reported_status);
    }
    bool wifi_only = strcmp(stage, "wifi") == 0;
    if (wifi_only) {
        setup_window_lock_take();
        if (!link_pairing_extend_provisioning_deadline(session_generation)) {
            setup_window_lock_give();
            return;
        }
        uint32_t generation = advance_confirm_generation_locked();
        setup_window_lock_give();
        start_confirm_timeout(confirmed_idle_timeout_task, "pair_idle_to", generation,
                              session_generation != 0 ? session_generation
                                  : link_pairing_session_generation());
    } else if (session_generation != 0) {
        ble_server_disconnect_pairing_session(session_generation);
    } else {
        link_pairing_reset();
        ble_server_delayed_disconnect(500);
    }
}

static bool require_provisioning_pairing_session(uint32_t session_generation) {
    if (link_pairing_provisioning_session_valid(session_generation)) return true;
    setup_fail_for_session("pairing", "auth_failed", session_generation);
    return false;
}

static void shutdown_ble_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    ui_set_ble("off");
#if CONFIG_MUSE_ENABLED
    // Muse's phone-setup service shares the server: stop setup advertising only.
    ble_server_stop_advertising(false);
#else
    ble_server_full_shutdown();
#endif
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static bool complete_setup_and_stop_ble(const char *reason, uint32_t session_generation) {
    if (session_generation != 0) {
        if (!link_pairing_commit_provisioning(session_generation, config_mark_setup_complete)) {
            return false;
        }
    } else if (!config_mark_setup_complete()) {
        ESP_LOGW(TAG, "failed to mark setup complete");
    }
    setup_window_lock_take();
    advance_confirm_generation_locked();
    setup_window_lock_give();
    if (!s_ble_started) return true;
    ESP_LOGI(TAG, "setup complete via %s; stopping BLE", reason);
    if (xTaskCreate(shutdown_ble_task, "ble_stop", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to start BLE stop task");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ui_set_ble("off");
#if CONFIG_MUSE_ENABLED
        ble_server_stop_advertising(false);
#else
        ble_server_full_shutdown();
#endif
    }
    return true;
}

static void restart_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    stack_monitor_record(NULL);
    esp_restart();
}

// On boards with the full UI, display and audio leave too little internal RAM
// to hold BLE, Wi-Fi and a TLS handshake at once (the S3's esp-aes DMA buffers fail even
// with PSRAM). Pairing ends once the credentials are saved, and the device
// restarts to reach its VM with the BLE stack never started.
#define PAIR_THEN_RESTART CONFIG_MUSE_ENABLED

static char *load_token_unlocked(const char *key) {
    char *buf = calloc(1, TOKEN_BUF_BYTES);
    if (!buf) return NULL;
    if (!config_get_str(key, buf, TOKEN_BUF_BYTES)) {
        free(buf);
        return NULL;
    }
    return buf;
}

static char *load_token(const char *key) {
    auth_lock_take();
    char *token = load_token_unlocked(key);
    auth_lock_give();
    return token;
}

static bool store_token_pair_unlocked(const char *access_token,
                                      const char *refresh_token) {
    if (!access_token || !*access_token || !refresh_token || !*refresh_token) {
        return false;
    }
    bool ok = config_set_str("refresh_token", refresh_token);
    ok = config_set_str("access_token", access_token) && ok;
    config_erase_key("auth_token");  // legacy key from early Link firmware
    if (ok) {
        note_access_token_fresh();
    } else {
        config_erase_key("access_token");
        config_erase_key("refresh_token");
    }
    return ok;
}

static void store_pairing_username_unlocked(const char *username) {
    if (username && *username) {
        config_set_str("username", username);
    } else {
        config_erase_key("username");
    }
}

static void handle_token_revoked_unlocked(const char *context) {
    ESP_LOGW(TAG, "%s: device credentials rejected; clearing pairing", context);
    s_last_vm_auth_refresh_attempt_us = 0;
    s_vm_auth_refresh_interval_us = VM_AUTH_REFRESH_RETRY_INTERVAL_US;
    s_vm_auth_refusals = 0;
    if (!config_clear_pairing()) {
        ESP_LOGE(TAG, "%s: pairing credential deletion could not be verified",
                 context);
    }
}

static void defer_token_revocation_reset(void) {
    ui_set_status("auth_failed");
    led_status_set_state(LED_STATE_ERROR);
    if (strcmp(s_setup_stage, "done") != 0) {
        ESP_LOGW(TAG, "token rejected during setup; setup failure will clean state");
        return;
    }
    ESP_LOGW(TAG, "token revoked - deferring setup reset to app task");
    atomic_store_explicit(&s_setup_reset_pending, true, memory_order_release);
}

static bool mint_and_store_device_tokens_unlocked(const char *source_token,
                                                  const char *username) {
    vm_device_tokens_t tokens = {0};
    int rc = vm_api_mint_device_token(source_token, identity_node_id(), &tokens);
    if (rc == 0) {
        bool ok = store_token_pair_unlocked(tokens.access_token, tokens.refresh_token);
        if (ok && username) store_pairing_username_unlocked(username);
        vm_device_tokens_free(&tokens);
        return ok;
    }
    vm_device_tokens_free(&tokens);
    if (rc == VM_API_ERR_AUTH) {
        handle_token_revoked_unlocked("device token mint");
    } else {
        ESP_LOGW(TAG, "device token mint failed");
    }
    return false;
}

static bool accept_pairing_credentials(const char *access_token,
                                       const char *refresh_token,
                                       const char *username) {
    if (!access_token || !*access_token) return false;
    auth_lock_take();
    bool ok = false;
    if (refresh_token && *refresh_token) {
        ok = store_token_pair_unlocked(access_token, refresh_token);
        if (ok) store_pairing_username_unlocked(username);
    } else if (!wifi_mgr_is_connected()) {
        ESP_LOGW(TAG, "cannot mint device token before WiFi is connected");
        ok = false;
    } else {
        ESP_LOGI(TAG, "no refresh_token supplied; minting device token pair");
        ok = mint_and_store_device_tokens_unlocked(access_token, username);
    }
    auth_lock_give();
    return ok;
}

static bool migrate_legacy_auth_token_locked(void) {
    char *access = load_token_unlocked("access_token");
    if (access) {
        free(access);
        return true;
    }
    char *legacy = load_token_unlocked("auth_token");
    if (!legacy) {
        return false;
    }
    if (!wifi_mgr_is_connected()) {
        ESP_LOGW(TAG, "legacy auth_token present but WiFi is down; cannot mint");
        free(legacy);
        return false;
    }
    ESP_LOGI(TAG, "migrating legacy auth_token to device token pair");
    bool ok = mint_and_store_device_tokens_unlocked(legacy, NULL);
    free(legacy);
    return ok;
}

// Leave a usable access token in NVS, refreshing or minting one if needed.
// Caller must hold the operation gate. Returns false when it could not: no
// token to migrate, a failed mint, or a refresh that failed with
// allow_stale_on_refresh_failure = false.
//
// In order: migrate a legacy auth token if that is all we have; mint a pair if
// there is an access token but no refresh token; otherwise keep the stored
// token unless it has been rejected or the proactive interval has elapsed, in
// which case refresh it on the refresh-token leg.
//
// access_token_rejected           the caller just had the stored token refused.
//                                 The primary refresh trigger.
// allow_stale_on_refresh_failure  when a refresh is itself refused, keep the
//                                 existing token (true) instead of treating
//                                 the device as revoked and unpairing (false).
//
// Refreshing is otherwise avoided. The 45-minute timer this replaced came from
// expires_in=14400 in the refresh response, which does not describe this
// credential — the API puts the access token's lifetime at 365 days — so it
// replaced a year-long credential ~2000x more often than needed, through the
// one endpoint that mints unusable tokens after an idle period.
// CONFIG_HOMEHUB_TOKEN_REFRESH_INTERVAL_DAYS (default 30, uptime-based) keeps a
// safety net against that 365-day figure being wrong, at a thousandth the
// exposure. It cannot unpair: every caller reaching it passes
// allow_stale_on_refresh_failure = true.
static bool ensure_access_token_ready_with_gate_held(
    bool access_token_rejected, bool allow_stale_on_refresh_failure) {
    auth_lock_take();

    bool ok = false;
    bool revoked = false;
    char *access = NULL;
    char *refresh = NULL;

    access = load_token_unlocked("access_token");
    if (!access) {
        ok = migrate_legacy_auth_token_locked();
        goto done;
    }

    refresh = load_token_unlocked("refresh_token");
    if (!refresh) {
        ESP_LOGI(TAG, "access_token has no refresh_token; minting device pair");
        ok = mint_and_store_device_tokens_unlocked(access, NULL);
        goto done;
    }

    bool periodic_due = false;
#if CONFIG_HOMEHUB_TOKEN_REFRESH_INTERVAL_DAYS > 0
    periodic_due = (esp_timer_get_time() - s_last_periodic_token_refresh_us)
                   >= TOKEN_PERIODIC_REFRESH_INTERVAL_US;
#endif

    bool sdk_token_report_due = identity_sdk_token() && !s_sdk_token_report_attempted;

    if (!access_token_rejected && !periodic_due && !sdk_token_report_due) {
        // Nothing has complained about this token, so keep using it.
        ok = true;
        goto done;
    }

    if (periodic_due && !access_token_rejected) {
        // Stamp the attempt, not the success. A failure here is not urgent —
        // the token still works, nothing has rejected it — and retrying on
        // every fetch_vms would hammer the endpoint we are trying to avoid.
        s_last_periodic_token_refresh_us = esp_timer_get_time();
        ESP_LOGI(TAG, "proactive device token refresh (every %d days of uptime)",
                 CONFIG_HOMEHUB_TOKEN_REFRESH_INTERVAL_DAYS);
    } else if (sdk_token_report_due && !access_token_rejected) {
        ESP_LOGI(TAG, "refreshing device token to report the SDK token");
    }
    // Stamped on the attempt, like the periodic refresh, so failures don't
    // retry on every fetch_vms.
    if (sdk_token_report_due) s_sdk_token_report_attempted = true;

    vm_device_tokens_t tokens = {0};
    vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
    // NULL access token: we only get here because the stored one was refused,
    // so re-presenting it is pointless. It is also actively harmful — on the
    // first call after an idle period the server accepts the access token,
    // returns 200, and mints a replacement that every endpoint then rejects.
    // That would consume our single retry and make the caller conclude the
    // device has been revoked. (The accepted token is not expired; it has a
    // 365-day life. The defect is in what the call mints, not what it takes.)
    int rc = vm_api_refresh_device_token(NULL, refresh, identity_node_id(),
                                         &tokens, &method);
    if (rc == 0) {
        ok = store_token_pair_unlocked(tokens.access_token, tokens.refresh_token);
        vm_device_tokens_free(&tokens);
        ESP_LOGI(TAG, "device token refresh stored (method=%d)", (int)method);
        goto done;
    }
    vm_device_tokens_free(&tokens);
    if (rc == VM_API_ERR_AUTH) {
        if (allow_stale_on_refresh_failure) {
            ESP_LOGW(TAG, "device token refresh rejected; keeping stale token");
            ok = true;
            goto done;
        }
        handle_token_revoked_unlocked("device token refresh");
        revoked = true;
        ok = false;
        goto done;
    }
    ESP_LOGW(TAG, "device token refresh failed");
    ok = allow_stale_on_refresh_failure;

done:
    free(access);
    free(refresh);
    auth_lock_give();
    if (revoked) {
        defer_token_revocation_reset();
    }
    return ok;
}

static int fetch_vms_with_refresh_with_gate_held(vm_info_t *vms, int max) {
    if (!ensure_access_token_ready_with_gate_held(false, true)) {
        return VM_API_ERR_FAILED;
    }

    char *access = load_token("access_token");
    if (!access) {
        return VM_API_ERR_FAILED;
    }

    int count = vm_api_fetch_vms(access, vms, max);
    free(access);
    if (count >= 0) note_vm_credentials_accepted();
    if (count != VM_API_ERR_AUTH) return count;

    ESP_LOGW(TAG, "VM fetch rejected token; forcing refresh");
    if (!ensure_access_token_ready_with_gate_held(true, false)) {
        return VM_API_ERR_AUTH;
    }

    access = load_token("access_token");
    if (!access) {
        return VM_API_ERR_FAILED;
    }
    count = vm_api_fetch_vms(access, vms, max);
    free(access);
    if (count == VM_API_ERR_AUTH) {
        // Deliberately NOT a revocation. A 401 from USING a token means "get a
        // new one", and we already did — so this says the credential we were
        // just handed does not work, not that this device has lost its
        // pairing. Only the refresh token being refused proves that, and
        // ensure_access_token_ready_with_gate_held() already unpairs on it
        // (allow_stale_on_refresh_failure is false on the call above).
        //
        // The distinction is not academic: the API can return 200 from a
        // refresh and hand back an access token that every endpoint then
        // rejects, so treating this 401 as revocation wipes a working pairing
        // over a server-side fault. Report the failure and let the caller
        // retry.
        ESP_LOGW(TAG,
                 "VM fetch still rejected after refresh — the new credential "
                 "was refused; leaving pairing intact and retrying later");
        // Re-arm the refresh floor that storing the (dead) pair just cleared,
        // and widen it. Without this the retry cadence is set by noise_ctrl's
        // 15 s reconnect cap, not by us.
        note_vm_credentials_refused();
    } else if (count >= 0) {
        note_vm_credentials_accepted();
    }
    return count;
}

static bool connect_vm_target(const vm_info_t *target) {
    // vm_connect_params() owns the whole admission decision: it refuses an
    // entry that cannot supply both an id and its own token, and there is no
    // other credential to fall back to. See vm_connect.h.
    char vm_id[128];
    const char *auth_token = NULL;
    if (!vm_connect_params(target, vm_id, sizeof(vm_id), &auth_token)) {
        ESP_LOGE(TAG, "no usable vm_id/token for VM entry");
        return false;
    }

    const char *network_ssid = strcmp(s_ui_wifi, "down") == 0 ? NULL : s_ui_wifi;
    bool ctrl_started = noise_ctrl_connect(vm_id, auth_token, network_ssid);
    if (!ctrl_started) {
        ESP_LOGE(TAG, "noise control start failed for %s", target->vm_url);
        return false;
    }
    ESP_LOGI(TAG, "connecting to VM %s", vm_id);
    // The data-plane tunnel is multiplexed on the control session (stream 2);
    // no separate connection to start here.
    return true;
}

static bool connect_preferred_vm_with_gate_held(uint32_t session_generation) {
    vm_info_t vms[VM_API_MAX_VMS];
    int count = fetch_vms_with_refresh_with_gate_held(vms, VM_API_MAX_VMS);
    if (count <= 0) return false;
    if (session_generation != 0
        && !link_pairing_provisioning_session_valid(session_generation)) {
        vm_list_free(vms, count);
        return false;
    }

    char preferred[256] = {0};
    config_get_str("vm_url", preferred, sizeof(preferred));

    const vm_info_t *target = NULL;
    if (preferred[0]) {
        target = vm_find_by_url(vms, count, preferred);
    }
    if (!target) target = vm_find_default(vms, count);
    if (!target) {
        vm_list_free(vms, count);
        return false;
    }
    const char *name = (target->vm_name && *target->vm_name)
                       ? target->vm_name : target->vm_url;
    ESP_LOGI(TAG, "connecting to VM %s", name);
    ui_set_vm(name);
    bool ok = connect_vm_target(target);
    vm_list_free(vms, count);
    return ok;
}

static void finish_vm_auth_refresh_task(void) {
    auth_lock_take();
    s_vm_auth_refresh_pending = false;
    auth_lock_give();
}

static void vm_auth_refresh_task(void *arg) {
    (void)arg;

    bool have_operation_gate = false;
    if (s_operation_gate) {
        have_operation_gate = operation_gate_take(0, "VM credential refresh");
        if (!have_operation_gate) {
            ESP_LOGI(TAG, "skipping VM credential refresh; another operation is active");
            finish_vm_auth_refresh_task();
            stack_monitor_record(NULL);
            vTaskDelete(NULL);
            return;
        }
    }

    ESP_LOGI(TAG, "refreshing VM credentials after tunnel auth rejection");
    if (wifi_mgr_is_connected() && config_is_provisioned()) {
        if (!connect_preferred_vm_with_gate_held(0)) {
            ESP_LOGW(TAG, "VM credential refresh failed after tunnel auth rejection");
        }
    } else {
        ESP_LOGW(TAG, "cannot refresh VM credentials without WiFi and pairing");
    }

    if (have_operation_gate) {
        operation_gate_give();
    }
    finish_vm_auth_refresh_task();
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static void schedule_vm_auth_refresh(void) {
    int64_t now = esp_timer_get_time();
    auth_lock_take();
    if (s_vm_auth_refresh_pending) {
        auth_lock_give();
        return;
    }
    if (s_last_vm_auth_refresh_attempt_us
        && (now - s_last_vm_auth_refresh_attempt_us) < s_vm_auth_refresh_interval_us) {
        auth_lock_give();
        return;
    }

    s_vm_auth_refresh_pending = true;
    s_last_vm_auth_refresh_attempt_us = now;
    auth_lock_give();

    if (xTaskCreate(vm_auth_refresh_task, "vm_auth_rfsh", 8192, NULL, 4, NULL) != pdPASS) {
        auth_lock_take();
        s_vm_auth_refresh_pending = false;
        auth_lock_give();
        ESP_LOGE(TAG, "failed to start VM credential refresh task");
    }
}

// ---- BLE callbacks ---------------------------------------------------------

static void ble_ota_status(const ota_event_t *ev, void *user);

static bool start_wifi_join_ota_if_requested(const char *url, bool force,
                                             uint32_t session_generation) {
    // A client-supplied update must not interrupt setup when OTA is disabled.
    if (!ota_is_enabled()) return false;
    if (!url || !*url) return false;
    if (!wifi_mgr_is_connected()) {
        if (session_generation != 0) {
            ble_server_send_pairing_status("ota_wifi_required", session_generation);
        } else {
            ble_server_send_status("ota_wifi_required");
        }
        return true;
    }

    ESP_LOGI(TAG, "BLE WiFi-join OTA requested (force=%d)", force);
    if (session_generation != 0) {
        ble_server_send_pairing_status("ota_starting", session_generation);
        if (!require_provisioning_pairing_session(session_generation)) return true;
    } else {
        ble_server_send_status("ota_starting");
    }
    ui_set_status("ota_starting");
    ota_start(url, force, ble_ota_status, (void *)(uintptr_t)session_generation);
    return true;
}

static void on_provision(const char *ssid, const char *password,
                         const char *access_token,
                         const char *refresh_token,
                         const char *username,
                         const char *ota_url,
                         bool ota_force,
                         const char *api_url,
                         const char *api_url_v2,
                         const char *noise_host,
                         uint32_t session_generation) {
    if (session_generation == 0) return;
    if (!operation_gate_take(0, "BLE provision")) {
        ble_server_send_pairing_status("error_operation_in_progress", session_generation);
        return;
    }
    // A queued callback may be stale before it ever owned the operation gate.
    // It has no partial state to roll back and must not erase a later setup.
    if (!link_pairing_provisioning_session_valid(session_generation)) goto done;

    setup_stage_set("wifi");
    ble_server_send_pairing_status("wifi_connecting", session_generation);
    if (!require_provisioning_pairing_session(session_generation)) goto done;
    ui_set_status("wifi_connecting");
    led_status_set_state(LED_STATE_WIFI_CONNECTING);
    if (!wifi_mgr_connect(ssid, password, WIFI_CONNECT_TIMEOUT_MS)) {
        setup_fail_for_session("wifi", "wifi_failed", session_generation);
        goto done;
    }
    if (!require_provisioning_pairing_session(session_generation)) goto done;

    ble_server_send_pairing_status("wifi_connected", session_generation);
    if (!require_provisioning_pairing_session(session_generation)) goto done;
    ui_set_wifi(ssid);
    ui_set_status("wifi_connected");
    led_status_set_state(LED_STATE_WIFI_CONNECTED);
    remember_joined_wifi(ssid, password);
    persist_connected_wifi_channel();
    config_erase_key("vm_url");

    // Only older firmware reads api_url. Keep it current so a device flashed
    // back to that firmware still reaches the API it was paired with.
    bool api_saved = api_url && *api_url
        ? config_set_str("api_url", api_url) : config_erase_key("api_url");
    bool api_v2_saved = api_url_v2 && *api_url_v2
        ? config_set_str("api_url_v2", api_url_v2) : config_erase_key("api_url_v2");
    bool noise_saved = noise_host && *noise_host
        ? config_set_str("noise_host", noise_host) : config_erase_key("noise_host");
    if (!api_saved || !api_v2_saved || !noise_saved) {
        setup_fail_for_session("storage", "error_storage", session_generation);
        goto done;
    }
    vm_api_set_base_url(api_url_v2);
    noise_ctrl_set_host(noise_host);

    if (!require_provisioning_pairing_session(session_generation)) goto done;
    setup_stage_set("auth");
    if (!accept_pairing_credentials(access_token, refresh_token, username)) {
        setup_fail_for_session("auth", "auth_failed", session_generation);
        goto done;
    }

    if (!require_provisioning_pairing_session(session_generation)) goto done;
    if (start_wifi_join_ota_if_requested(ota_url, ota_force, session_generation)) {
        goto done;
    }

    if (!require_provisioning_pairing_session(session_generation)) goto done;
#if !PAIR_THEN_RESTART
    setup_stage_set("vm");
    if (!connect_preferred_vm_with_gate_held(session_generation)) {
        setup_fail_for_session("vm", "auth_failed", session_generation);
        goto done;
    }
#endif

    if (!require_provisioning_pairing_session(session_generation)) goto done;
    setup_stage_set("done");
    ble_server_send_pairing_status("auth_ok", session_generation);
    if (!require_provisioning_pairing_session(session_generation)) goto done;
    ui_set_status("auth_ok");
    led_status_set_state(LED_STATE_AUTH_OK);
    if (!complete_setup_and_stop_ble("provision", session_generation)) {
        setup_fail_for_session("storage", "error_storage", session_generation);
        goto done;
    }
#if PAIR_THEN_RESTART
    ESP_LOGI(TAG, "paired; restarting to connect without BLE");
    vTaskDelay(pdMS_TO_TICKS(2000));  // let auth_ok reach the phone
    if (xTaskCreate(restart_task, "restart", 2048, NULL, 3, NULL) != pdPASS) {
        esp_restart();
    }
#endif

done:
    operation_gate_give();
}

static bool send_cached_scan_to_client(void) {
    uint32_t session_generation = link_pairing_session_generation();
    if (!link_pairing_session_confirmed()) return false;

    wifi_scan_entry_t scan[MAX_SCAN_RESULTS];
    int n = wifi_mgr_get_cached_scan(scan, MAX_SCAN_RESULTS);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "wifi_scan_result");
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", scan[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", scan[i].rssi);
        cJSON_AddBoolToObject(e, "secure", scan[i].auth_mode != 0);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(root, "networks", arr);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;

    bool sent = ble_server_send_encrypted_json(json, session_generation);
    free(json);
    return sent;
}

static bool send_cached_scan_or_error(void) {
    if (!send_cached_scan_to_client()) {
        ble_server_send_status("error_encryption_required");
        return false;
    }
    return true;
}

static void scan_refresh_task(void *arg) {
    int total = wifi_mgr_scan_and_merge_cache();
    unsigned completed_flags = atomic_exchange_explicit(
        &s_scan_refresh_flags, 0, memory_order_acq_rel);
    if (total > 0) {
        ESP_LOGI(TAG, "scan refresh complete: %d cached", total);
        if (completed_flags & SCAN_REFRESH_REPLY_PENDING) {
            (void)send_cached_scan_to_client();
        }
    } else {
        ESP_LOGI(TAG, "scan refresh cancelled or unavailable");
    }
    heap_snapshot("after wifi_scan");
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static bool schedule_scan_refresh(bool reply_requested, bool *joined) {
    unsigned requested_flags = SCAN_REFRESH_RUNNING;
    if (reply_requested) requested_flags |= SCAN_REFRESH_REPLY_PENDING;

    unsigned previous_flags = atomic_fetch_or_explicit(
        &s_scan_refresh_flags, requested_flags, memory_order_acq_rel);
    bool already_running = (previous_flags & SCAN_REFRESH_RUNNING) != 0;
    if (joined) *joined = already_running;
    if (already_running) {
        if (reply_requested) {
            ESP_LOGI(TAG, "scan command joined active scan");
        }
        return true;
    }

    if (xTaskCreate(scan_refresh_task, "scan_rfsh", 4096, NULL, 4, NULL)
        != pdPASS) {
        atomic_store_explicit(&s_scan_refresh_flags, 0, memory_order_release);
        ESP_LOGE(TAG, "failed to start wifi scan task");
        return false;
    }
    return true;
}

static void on_wifi_scan(void) {
    if (!link_pairing_session_confirmed()) {
        (void)send_cached_scan_or_error();
        return;
    }

    // Join an active boot scan; otherwise return the current cache and start a
    // refresh. A joined request receives the completed scan instead of an empty list.
    ESP_LOGI(TAG, "scan command received");
    bool joined = false;
    bool scheduled = schedule_scan_refresh(true, &joined);
    if (!joined) {
        (void)send_cached_scan_or_error();
    }
    if (!scheduled) {
        ESP_LOGW(TAG, "wifi scan refresh unavailable");
    }
}

static void set_vm_with_gate_held(const char *url) {
    ble_server_send_status("vm_switching");
    ui_set_status("vm_switching");
    led_status_set_state(LED_STATE_VM_SWITCHING);
    vm_info_t vms[VM_API_MAX_VMS];
    int count = fetch_vms_with_refresh_with_gate_held(vms, VM_API_MAX_VMS);
    if (count <= 0) {
        ble_server_send_status("vm_failed");
        ui_set_status("vm_failed");
        led_status_set_state(LED_STATE_ERROR);
        goto done;
    }
    const vm_info_t *target = vm_find_by_url(vms, count, url);
    if (!target) {
        ESP_LOGE(TAG, "VM not found: %s", url);
        vm_list_free(vms, count);
        ble_server_send_status("vm_failed");
        ui_set_status("vm_failed");
        led_status_set_state(LED_STATE_ERROR);
        goto done;
    }
    config_set_str("vm_url", url);
    const char *name = (target->vm_name && *target->vm_name) ? target->vm_name : target->vm_url;
    ui_set_vm(name);
    if (!connect_vm_target(target)) {
        vm_list_free(vms, count);
        ble_server_send_status("vm_failed");
        ui_set_status("vm_failed");
        led_status_set_state(LED_STATE_ERROR);
        goto done;
    }
    vm_list_free(vms, count);
    ble_server_send_status("vm_ok");
    ui_set_status("vm_ok");
    led_status_set_state(LED_STATE_VM_OK);

done:
    return;
}

static void finish_setup_reset_with_gate_held(void) {
    setup_window_lock_take();
    advance_confirm_generation_locked();
    setup_window_lock_give();
    ui_set_status("unpaired");
    led_status_set_state(LED_STATE_UNPAIRED);
    ble_server_send_status("unpaired");
    link_pairing_reset();
}

static bool reset_setup_with_gate_held(const char *source) {
    ESP_LOGI(TAG, "%s - reset setup", source);
    setup_stage_set("reset");
    if (!setup_wipe_to_clean()) {
        ESP_LOGE(TAG, "%s - setup deletion failed; reset aborted", source);
        ui_set_status("error_storage");
        led_status_set_state(LED_STATE_ERROR);
        ble_server_send_status("error_storage");
        return false;
    }
    finish_setup_reset_with_gate_held();
    return true;
}

static void on_unpair(void) {
    if (!operation_gate_take(0, "BLE unpair")) {
        ble_server_send_status("error_operation_in_progress");
        return;
    }
    if (!reset_setup_with_gate_held("BLE unpair")) {
        operation_gate_give();
        return;
    }

    // The encrypted session was erased by the reset, so do not leave mobile on
    // a connected-but-unusable GATT link. Disconnect cleanup resumes advertising.
    if (ble_server_has_connection()) {
        ble_server_disconnect_client();
    } else {
        open_setup_window("BLE unpair");
    }
    operation_gate_give();
}

static void on_get_device_info(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "device_info");
    cJSON_AddStringToObject(root, "node_id", identity_node_id());
    cJSON_AddStringToObject(root, "version", desc ? desc->version : "unknown");
    link_pairing_add_device_info(root);
    char build_sha[17] = {0};
    if (desc) {
        for (int i = 0; i < 8; i++) {
            snprintf(build_sha + i * 2, 3, "%02x", desc->app_elf_sha256[i]);
        }
    }
    cJSON_AddStringToObject(root, "build_sha", build_sha);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        ble_server_send_chunked(json);
        free(json);
    }
}

static void ble_ota_status(const ota_event_t *ev, void *user) {
    uint32_t session_generation = (uint32_t)(uintptr_t)user;
    const char *status;
    switch (ev->result) {
        case OTA_RESULT_APPLIED:
            status = "ota_applied";
            break;
        case OTA_RESULT_SKIPPED:
            status = "ota_skipped";
            break;
        case OTA_RESULT_FAILED:
        default:
            status = "ota_failed";
            break;
    }
    if (session_generation != 0) {
        ble_server_send_pairing_status(status, session_generation);
    } else {
        ble_server_send_status(status);
    }
    setup_window_lock_take();
    if (session_generation == 0 || link_pairing_session_is_current(session_generation)) {
        ui_set_status(status);
        if (ev->result != OTA_RESULT_APPLIED && ev->result != OTA_RESULT_SKIPPED) {
            led_status_set_state(LED_STATE_ERROR);
        }
    }
    setup_window_lock_give();
}

static void on_ble_ota(const char *url, bool force) {
    if (!ota_is_enabled()) {
        ble_server_send_status("ota_skipped");
        ui_set_status("ota_skipped");
        return;
    }
    if (!url || !*url) {
        ble_server_send_status("error_missing_url");
        return;
    }
    if (!wifi_mgr_is_connected()) {
        ble_server_send_status("ota_wifi_required");
        return;
    }
    ESP_LOGI(TAG, "BLE OTA requested (force=%d)", force);
    ble_server_send_status("ota_starting");
    ui_set_status("ota_starting");
    ota_start(url, force, ble_ota_status, NULL);
}

#if CONFIG_HOMEHUB_TUNNEL
// ---- Network discovery task ------------------------------------------------

typedef struct {
    noise_ctrl_session_generation_t session_generation;
    char request_id[64];
    cJSON *params;
} discover_task_args_t;

static void discover_task(void *arg) {
    discover_task_args_t *args = (discover_task_args_t *)arg;
    ESP_LOGI(TAG, "starting network discovery scan");
    cJSON *result = net_discovery_run(args->params);
    cJSON *wrapper = result ? cJSON_CreateObject() : NULL;
    if (!wrapper || !cJSON_AddBoolToObject(wrapper, "ok", true)
        || !cJSON_AddItemToObject(wrapper, "payload", result)) {
        // Out of memory. Answer anyway, so the Muse isn't left waiting out
        // the command's timeout.
        ESP_LOGW(TAG, "network discovery ran out of memory");
        cJSON_Delete(wrapper);
        cJSON_Delete(result);
        wrapper = cJSON_CreateObject();
        cJSON *failure = cJSON_AddObjectToObject(wrapper, "error");
        if (!cJSON_AddBoolToObject(wrapper, "ok", false)
            || !cJSON_AddStringToObject(failure, "code", "out_of_memory")
            || !cJSON_AddStringToObject(failure, "message",
                                        "not enough memory for the discovery results")) {
            cJSON_Delete(wrapper);
            wrapper = NULL;
        }
    }
    noise_ctrl_send_command_result(
        args->session_generation, args->request_id, wrapper);
    if (args->params) cJSON_Delete(args->params);
    free(args);
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}
#endif

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
// ---- Image download ----------------------------------------------------------

typedef struct {
    noise_ctrl_session_generation_t session_generation;
    char request_id[64];
} draw_url_ctx_t;

static void draw_url_done(const image_fetch_result_t *r, void *user) {
    draw_url_ctx_t *ctx = user;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", r->ok);
    if (r->ok) {
        cJSON *payload = cJSON_AddObjectToObject(result, "payload");
        cJSON_AddStringToObject(payload, "format", r->format);
        cJSON_AddNumberToObject(payload, "width", r->width);
        cJSON_AddNumberToObject(payload, "height", r->height);
        cJSON_AddNumberToObject(payload, "scale", r->scale);
        cJSON_AddNumberToObject(payload, "bytes", (double)r->bytes);
        cJSON_AddNumberToObject(payload, "ms", r->ms);
    } else {
        cJSON *error = cJSON_AddObjectToObject(result, "error");
        cJSON_AddStringToObject(error, "code", r->code);
        cJSON_AddStringToObject(error, "message", r->message);
    }
    noise_ctrl_send_command_result(ctx->session_generation, ctx->request_id, result);
    free(ctx);
}
#endif

#if CONFIG_MUSE_WATCHER_CAMERA
typedef struct {
    noise_ctrl_session_generation_t session_generation;
    char request_id[64];
} watcher_camera_task_args_t;

static void watcher_camera_capture_task(void *arg) {
    watcher_camera_task_args_t *args = arg;
    char *image = NULL;
    const char *error = NULL;
    cJSON *result = cJSON_CreateObject();
    bool ok = watcher_camera_capture(&image, &error);
    cJSON_AddBoolToObject(result, "ok", ok);
    if (ok) {
        cJSON *payload = cJSON_AddObjectToObject(result, "payload");
        cJSON_AddStringToObject(payload, "format", "jpeg-base64");
        cJSON_AddStringToObject(payload, "data_base64", image);
    } else {
        cJSON *failure = cJSON_AddObjectToObject(result, "error");
        cJSON_AddStringToObject(failure, "code", "camera_capture_failed");
        cJSON_AddStringToObject(failure, "message", error ? error : "camera capture failed");
    }
    free(image);
    noise_ctrl_send_command_result(args->session_generation, args->request_id, result);
    free(args);
    stack_monitor_record(NULL);
    vTaskDeleteWithCaps(NULL);
}
#endif

// ---- WebSocket command callbacks -------------------------------------------

typedef enum {
    WS_CONTROL_SET_VM,
    WS_CONTROL_RESET_VM,
    WS_CONTROL_UNPAIR,
} ws_control_action_t;

typedef struct {
    ws_control_action_t action;
    char *url;
    bool setup_credentials_cleared;
} ws_control_args_t;

static const char *ws_control_action_name(ws_control_action_t action) {
    switch (action) {
        case WS_CONTROL_SET_VM: return "device.set_vm";
        case WS_CONTROL_RESET_VM: return "device.reset_vm";
        case WS_CONTROL_UNPAIR: return "device.unpair";
        default: return "unknown";
    }
}

static cJSON *command_ok_accepted(void) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddBoolToObject(result, "accepted", true);
    return result;
}

static cJSON *command_error(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(result, "error", error);
    return result;
}

static char *json_strdup_string(cJSON *params, const char *key) {
    cJSON *item = params ? cJSON_GetObjectItem(params, key) : NULL;
    if (!cJSON_IsString(item) || !item->valuestring || !*item->valuestring) {
        return NULL;
    }
    return strdup(item->valuestring);
}

static void free_ws_control_args(ws_control_args_t *args) {
    if (!args) return;
    free(args->url);
    free(args);
}

static void finish_precleared_setup_reset_with_gate_held(const char *source) {
    ESP_LOGI(TAG, "%s - finish verified setup reset", source);
    setup_stage_set("reset");
    setup_disconnect_to_clean();
    finish_setup_reset_with_gate_held();
}

static void restart_after_setup_reset(void) {
    if (xTaskCreate(restart_task, "restart", 2048, NULL, 3, NULL) != pdPASS) {
        stack_monitor_record(NULL);
        esp_restart();
    }
}

static bool reset_setup_from_control(const char *source) {
    if (!operation_gate_take(portMAX_DELAY, source)) {
        ESP_LOGE(TAG, "%s could not acquire the setup operation gate", source);
        return false;
    }
    bool reset = reset_setup_with_gate_held(source);
    if (!reset) {
        operation_gate_give();
        return false;
    }
    // Keep the gate reserved until reboot so no setup writer can recreate
    // credentials after the verified erase.
    restart_after_setup_reset();
    return true;
}

static void reset_default_vm_with_gate_held(void) {
    config_erase_key("vm_url");
    ble_server_send_status("vm_switching");
    ui_set_status("vm_switching");
    led_status_set_state(LED_STATE_VM_SWITCHING);
    if (connect_preferred_vm_with_gate_held(0)) {
        ble_server_send_status("vm_ok");
        ui_set_status("vm_ok");
        led_status_set_state(LED_STATE_VM_OK);
    } else {
        ble_server_send_status("vm_failed");
        ui_set_status("vm_failed");
        led_status_set_state(LED_STATE_ERROR);
    }
}

static void ws_control_task(void *arg) {
    ws_control_args_t *args = (ws_control_args_t *)arg;
    bool release_operation_gate = true;
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "running server command: %s",
             ws_control_action_name(args->action));

    switch (args->action) {
        case WS_CONTROL_SET_VM:
            set_vm_with_gate_held(args->url);
            break;
        case WS_CONTROL_RESET_VM:
            reset_default_vm_with_gate_held();
            break;
        case WS_CONTROL_UNPAIR:
            if (args->setup_credentials_cleared) {
                finish_precleared_setup_reset_with_gate_held("server unpair");
                restart_after_setup_reset();
                release_operation_gate = false;
            } else {
                ESP_LOGE(TAG, "server unpair worker missing verified deletion state");
            }
            break;
    }

    if (release_operation_gate) operation_gate_give();
    free_ws_control_args(args);
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static cJSON *queue_ws_control(ws_control_action_t action, char *url) {
    if (!operation_gate_take(0, ws_control_action_name(action))) {
        free(url);
        return command_error("busy", "another operation is active");
    }

    ws_control_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        operation_gate_give();
        free(url);
        return command_error("out_of_memory", "failed to allocate command");
    }
    args->action = action;
    args->url = url;

    // The control result is the caller-visible success boundary. Delete and
    // read back setup credentials before returning ok/accepted. The operation
    // gate stays reserved across the worker handoff, so no setup writer can
    // recreate credentials before the delayed disconnect and reboot.
    if (action == WS_CONTROL_UNPAIR) {
        if (!clear_setup_credentials()) {
            operation_gate_give();
            free_ws_control_args(args);
            return command_error("storage_error",
                                 "failed to clear setup credentials");
        }
        args->setup_credentials_cleared = true;
    }

    if (xTaskCreate(ws_control_task, "ws_control", 8192, args, 4, NULL)
        != pdPASS) {
        if (args->setup_credentials_cleared) {
            // Keep the gate reserved until reboot after verified deletion.
            restart_after_setup_reset();
        } else {
            operation_gate_give();
        }
        free_ws_control_args(args);
        return command_error("out_of_memory", "failed to start command task");
    }

    ESP_LOGI(TAG, "accepted server command: %s",
             ws_control_action_name(action));
    return command_ok_accepted();
}

static cJSON *list_vms_command(void) {
    bool locked = false;
    if (s_operation_gate) {
        if (!operation_gate_take(0, "device.list_vms")) {
            return command_error("busy", "another operation is active");
        }
        locked = true;
    }

    vm_info_t vms[VM_API_MAX_VMS];
    int count = fetch_vms_with_refresh_with_gate_held(vms, VM_API_MAX_VMS);
    if (count <= 0) {
        if (locked) operation_gate_give();
        return command_error("fetch_failed", "failed to fetch VMs");
    }

    char current_url[256] = {0};
    config_get_str("vm_url", current_url, sizeof(current_url));

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "vm_url", vms[i].vm_url ? vms[i].vm_url : "");
        cJSON_AddStringToObject(item, "vm_name", vms[i].vm_name ? vms[i].vm_name : "");
        cJSON_AddBoolToObject(item, "is_default", vms[i].is_default);
        bool is_current = current_url[0]
                          ? (vms[i].vm_url
                             && strcmp(current_url, vms[i].vm_url) == 0)
                          : vms[i].is_default;
        cJSON_AddBoolToObject(item, "is_current", is_current);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(result, "vms", arr);
    vm_list_free(vms, count);
    if (locked) operation_gate_give();
    return result;
}

#if CONFIG_HOMEHUB_SUPPORT_BUG_REPORT
static cJSON *bug_report_command(
    const char *request_id,
    noise_ctrl_session_generation_t session_generation) {
    cJSON *async = cJSON_CreateObject();
    if (!async || !cJSON_AddBoolToObject(async, "_async", true)) {
        cJSON_Delete(async);
        return command_error("out_of_memory", "failed to start bug report");
    }

    bug_report_start_status_t status = bug_report_start(
        request_id, session_generation);
    if (status == BUG_REPORT_START_ACCEPTED) return async;
    cJSON_Delete(async);

    switch (status) {
        case BUG_REPORT_START_INVALID_REQUEST_ID:
            return command_error("invalid_params", "request_id is invalid");
        case BUG_REPORT_START_INVALID_SESSION:
            return command_error("unavailable", "control session is unavailable");
        case BUG_REPORT_START_BUSY:
            return command_error("busy", "another bug report is active");
        case BUG_REPORT_START_TASK_UNAVAILABLE:
            return command_error(
                "unavailable", "bug report worker is unavailable");
        case BUG_REPORT_START_OUT_OF_MEMORY:
            return command_error("out_of_memory", "failed to start bug report");
        case BUG_REPORT_START_ACCEPTED:
        default:
            return command_error("internal", "failed to start bug report");
    }
}
#endif

static cJSON *on_ws_command(
    const char *command, cJSON *params, const char *request_id,
    noise_ctrl_session_generation_t session_generation) {
    if (strcmp(command, "device.list_vms") == 0) {
        return list_vms_command();
    }
    if (strcmp(command, "device.set_vm") == 0) {
        char *url = json_strdup_string(params, "url");
        if (!url) return command_error("missing_param", "url is required");
        return queue_ws_control(WS_CONTROL_SET_VM, url);
    }
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    if (strcmp(command, "display.draw_url") == 0) {
        cJSON *url = cJSON_GetObjectItem(params, "url");
        cJSON *row = cJSON_GetObjectItem(params, "row");
        if (!cJSON_IsString(url) || !url->valuestring || !url->valuestring[0]) {
            return command_error("missing_param", "url is required");
        }
        draw_url_ctx_t *ctx = calloc(1, sizeof(*ctx));
        if (!ctx) return command_error("out_of_memory", "failed to allocate");
        ctx->session_generation = session_generation;
        strncpy(ctx->request_id, request_id, sizeof(ctx->request_id) - 1);
        const char *code, *message;
        if (!image_fetch_start(url->valuestring, cJSON_IsNumber(row) ? row->valueint : IMAGE_FETCH_DEFAULT_ROW,
                               draw_url_done, ctx, &code, &message)) {
            free(ctx);
            return command_error(code, message);
        }
        cJSON *async = cJSON_CreateObject();
        cJSON_AddBoolToObject(async, "_async", true);
        return async;
    }
#if CONFIG_HOMEHUB_TEXT_PAGES
    if (strcmp(command, "pages.set") == 0 || strcmp(command, "pages.clear") == 0) {
        cJSON *page = cJSON_GetObjectItem(params, "page");
        if (!cJSON_IsString(page) || !page->valuestring || !page->valuestring[0]) {
            return command_error("missing_param", "page is required");
        }
        if (strlen(page->valuestring) > X3_PAGE_NAME_MAX) {
            return command_error("too_long", "page is at most 15 characters");
        }
        if (strcmp(command, "pages.clear") == 0) {
            if (!x3_pages_clear(page->valuestring)) {
                return command_error("not_found", "no page with that name");
            }
        } else {
            cJSON *title = cJSON_GetObjectItem(params, "title");
            cJSON *text = cJSON_GetObjectItem(params, "text");
            if (!cJSON_IsString(text) || !text->valuestring) {
                return command_error("missing_param", "text is required");
            }
            if (strlen(text->valuestring) > X3_PAGE_TEXT_MAX) {
                return command_error("too_long", "text is at most 460 characters");
            }
            if (title && (!cJSON_IsString(title) || !title->valuestring
                          || strlen(title->valuestring) > X3_PAGE_TITLE_MAX)) {
                return command_error("too_long", "title is a string of at most 20 characters");
            }
            if (!x3_pages_set(page->valuestring, title ? title->valuestring : NULL,
                              text->valuestring)) {
                return command_error("full", "all 6 pages are in use; clear one first");
            }
        }
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        return result;
    }
#endif
#if CONFIG_HOMEHUB_DISPLAY_TEXT
    if (strcmp(command, "display.show_text") == 0) {
        cJSON *text = cJSON_GetObjectItem(params, "text");
        if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
            return command_error("missing_param", "text is required");
        }
        if (strlen(text->valuestring) > 600) {
            return command_error("too_long", "text is at most 600 characters");
        }
        if (!x3_display_show_text(text->valuestring)) {
            return command_error("no_display", "the screen is not ready");
        }
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        return result;
    }
#endif
    if (strcmp(command, "display.show_animation") == 0) {
        led_status_show_animation();
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        return result;
    }
#endif
#if CONFIG_MUSE_WATCHER_CAMERA
    if (strcmp(command, "camera.capture") == 0) {
        watcher_camera_task_args_t *args = calloc(1, sizeof(*args));
        if (!args) return command_error("out_of_memory", "failed to allocate camera request");
        args->session_generation = session_generation;
        strncpy(args->request_id, request_id, sizeof(args->request_id) - 1);
        if (xTaskCreateWithCaps(watcher_camera_capture_task, "camera_capture", 8192,
                                args, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            free(args);
            return command_error("out_of_memory", "failed to start camera capture");
        }
        cJSON *async = cJSON_CreateObject();
        cJSON_AddBoolToObject(async, "_async", true);
        return async;
    }
#endif
#if CONFIG_HOMEHUB_VOICE
    if (strcmp(command, "voice.configure") == 0) {
        return voice_configure_command(params);
    }
#endif
#if CONFIG_HOMEHUB_SENSECAP_SENSORS
    if (strcmp(command, "sensors.read") == 0) {
        return sensecap_sensors_command();
    }
#endif
    if (strcmp(command, "device.reset_vm") == 0) {
        return queue_ws_control(WS_CONTROL_RESET_VM, NULL);
    }
    if (strcmp(command, "device.unpair") == 0) {
        return queue_ws_control(WS_CONTROL_UNPAIR, NULL);
    }
#if CONFIG_HOMEHUB_SUPPORT_BUG_REPORT
    if (strcmp(command, "bug.report") == 0) {
        return bug_report_command(request_id, session_generation);
    }
#endif
#if CONFIG_HOMEHUB_TUNNEL
    if (strcmp(command, "device.discover") == 0) {
        discover_task_args_t *args = calloc(1, sizeof(*args));
        if (!args) return command_error("out_of_memory", "failed to allocate");
        args->session_generation = session_generation;
        strncpy(args->request_id, request_id, sizeof(args->request_id) - 1);
        args->params = params ? cJSON_Duplicate(params, 1) : NULL;
        if (xTaskCreate(discover_task, "discover", 8192, args, 4, NULL)
            != pdPASS) {
            if (args->params) cJSON_Delete(args->params);
            free(args);
            return command_error("out_of_memory", "failed to start task");
        }
        cJSON *async = cJSON_CreateObject();
        cJSON_AddBoolToObject(async, "_async", true);
        return async;
    }
#endif

    char msg[128];
    snprintf(msg, sizeof(msg), "unsupported command: %s", command);
    return command_error("unsupported", msg);
}

// Re-enter the always-on advertising state used while the device is unpaired.
// Devices remain discoverable during setup; a session-bound button press
// confirms pairing before provisioning commands are accepted.
static void enter_advertising_state(const char *reason) {
    if (config_setup_complete()) return;
    ESP_LOGI(TAG, "%s - advertising (always on while unpaired)", reason);
    setup_stage_set("advertising");
    ui_set_ble("advertising");
    ui_set_status("pairing_advertising");
    led_status_set_state(LED_STATE_BLE_ADVERTISING);
    ble_server_begin_advertising();
}

static void confirm_timeout_task(void *arg) {
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(PAIRING_CONFIRM_WINDOW_MS));

    setup_window_lock_take();
    bool current = s_confirm_generation == generation;
    uint32_t session_generation = current ? s_confirm_session_generation : 0;
    if (current) s_confirm_timeout_task = NULL;
    current = current && link_pairing_session_is_current(session_generation);
    if (current) ui_set_status("pairing_confirm_timeout");
    setup_window_lock_give();

    if (current) {
        ESP_LOGW(TAG, "pairing confirmation timed out");
        ble_server_send_pairing_status("pairing_confirm_timeout", session_generation);
        vTaskDelay(pdMS_TO_TICKS(100));
        ble_server_disconnect_pairing_session(session_generation);
    }
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static void confirmed_idle_timeout_task(void *arg) {
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(PAIRING_CONFIRMED_IDLE_TIMEOUT_MS));

    setup_window_lock_take();
    bool current = s_confirm_generation == generation;
    uint32_t session_generation = current ? s_confirm_session_generation : 0;
    if (current) s_confirm_timeout_task = NULL;
    current = current && link_pairing_session_is_current(session_generation)
              && !link_pairing_provisioning_session_valid(0);
    if (current) ui_set_status("pairing_confirm_timeout");
    setup_window_lock_give();

    if (current) {
        ESP_LOGW(TAG, "confirmed pairing session timed out before provisioning");
        ble_server_send_pairing_status("pairing_confirm_timeout", session_generation);
        vTaskDelay(pdMS_TO_TICKS(100));
        ble_server_disconnect_pairing_session(session_generation);
    }
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static void notify_pairing_confirmed(uint32_t session_generation) {
    // Serialize the short UI update with disconnect cleanup, but release this
    // lock before notifications: sending their BLE chunks can take seconds.
    setup_window_lock_take();
    if (!link_pairing_session_is_current(session_generation)
        || !ble_server_has_connection() || !link_pairing_session_confirmed()
        || link_pairing_provisioning_session_valid(0)) {
        setup_window_lock_give();
        return;
    }
    uint32_t generation = advance_confirm_generation_locked();
    setup_stage_set("confirmed");
    ui_set_status("pairing_confirmed");
    led_status_set_state(LED_STATE_WIFI_CONNECTING);
    setup_window_lock_give();
    if (start_confirm_timeout(confirmed_idle_timeout_task, "pair_idle_to", generation,
                              session_generation) != pdPASS) {
        ESP_LOGE(TAG, "failed to start confirmed-session idle timer");
        ble_server_send_pairing_status("error_pairing_unavailable", session_generation);
        ble_server_disconnect_pairing_session(session_generation);
        return;
    }
    if (!ble_server_send_pairing_status("pairing_confirmed", session_generation)) {
        ble_server_disconnect_pairing_session(session_generation);
    }
}

static void on_pairing_client_finished(uint32_t session_generation) {
    if (!link_pairing_session_is_current(session_generation)) return;
    if (!ble_server_has_connection() || !link_pairing_confirmation_required()) return;

    // Emit the required prompt before arming the button. An early press must
    // not make pairing_confirmed overtake the record mobile expects first.
    if (!ble_server_send_pairing_status("confirm_required", session_generation)) {
        ble_server_disconnect_pairing_session(session_generation);
        return;
    }
    setup_window_lock_take();
    if (!link_pairing_arm_confirmation(session_generation)) {
        setup_window_lock_give();
        return;
    }
    uint32_t generation = advance_confirm_generation_locked();
    setup_stage_set("confirm");
    ui_set_status("confirm_required");
    led_status_set_state(LED_STATE_PAIRING_CONFIRM_REQUIRED);
    setup_window_lock_give();

    if (start_confirm_timeout(confirm_timeout_task, "pair_confirm_to", generation,
                              session_generation) != pdPASS) {
        ESP_LOGE(TAG, "failed to start pairing confirmation timer");
        ble_server_send_pairing_status("error_pairing_unavailable", session_generation);
        ble_server_disconnect_pairing_session(session_generation);
    }
}

static void on_client_connected(void) {
    ui_set_ble("connected");
    led_status_set_state(LED_STATE_BLE_CONNECTED);
}

static void on_client_disconnected(void) {
    atomic_fetch_and_explicit(
        &s_scan_refresh_flags,
        (unsigned)~SCAN_REFRESH_REPLY_PENDING,
        memory_order_acq_rel);
    if (config_setup_complete()) return;

    setup_window_lock_take();
    advance_confirm_generation_locked();
    // Always fall back to advertising while unpaired. Keep this UI update
    // serialized with confirmation workers; advertising itself runs unlocked.
    setup_stage_set("advertising");
    ui_set_ble("advertising");
    ui_set_status("pairing_advertising");
    led_status_set_state(LED_STATE_BLE_ADVERTISING);
    setup_window_lock_give();
    ble_server_begin_advertising();
}

static void start_ble_setup_server_if_needed(void) {
    if (s_ble_started) return;

    ble_callbacks_t cb = {
        .on_provision = on_provision,
        .on_wifi_scan = on_wifi_scan,
        .on_ota = on_ble_ota,
        .on_unpair = on_unpair,
        .on_get_device_info = on_get_device_info,
        .on_client_connected = on_client_connected,
        .on_client_disconnected = on_client_disconnected,
        .on_pairing_client_finished = on_pairing_client_finished,
    };
    ble_server_start(identity_ble_name(), &cb);
    s_ble_started = true;
    heap_snapshot("after ble_start");
}

static void open_setup_window(const char *reason) {
    if (config_setup_complete()) {
        ESP_LOGI(TAG, "%s ignored; setup already complete", reason);
        return;
    }

    start_ble_setup_server_if_needed();
    enter_advertising_state(reason);
}

static void on_button_short_press(void) {
    factory_test_on_button_press();

    if (link_pairing_confirmation_required()) {
        if (!ble_server_has_connection()) {
            ESP_LOGI(TAG, "button short-press ignored; no active BLE session");
            return;
        }
        uint32_t session_generation = link_pairing_confirm_active_session();
        if (session_generation) {
            notify_pairing_confirmed(session_generation);
            ESP_LOGI(TAG, "button short-press confirmed active pairing session");
        }
        return;
    }

    if (!config_setup_complete()) {
        open_setup_window("button short-press");
        return;
    }

    ESP_LOGI(TAG, "button short-press ignored; setup already complete");
}

static void on_button_double_press(void) {
    ESP_LOGI(TAG, "button double-press ignored");
}

static void on_button_long_press(void) {
    reset_setup_from_control("button long-press");
}


#if CONFIG_MUSE_ENABLED
// ---- Muse hooks (app.h) -----------------------------------------------------

void app_wifi_set_credentials(const char *ssid, const char *password, bool hidden) {
    if (!ssid || !ssid[0]) {
        wifi_known_forget_all();
        ui_set_wifi(NULL);
        return;
    }
    if (!wifi_known_remember(ssid, password ? password : "", hidden ? 1 : 0)) {
        ESP_LOGW(TAG, "failed to save wifi network");
    }
}

bool app_wifi_forget(const char *ssid) {
    return wifi_known_forget(ssid);
}

#define MUSE_VM_RETRY_MIN_US (5LL * 1000 * 1000)
#define MUSE_VM_RETRY_MAX_US (60LL * 1000 * 1000)

// Muse's keeper joins Wi-Fi but can't reach the VM: that fetches the VM list
// over HTTPS, which needs more stack than the keeper has, and by then there's
// seldom the internal RAM for a task of its own. So the heartbeat does it (as
// at boot), retrying until the session runs; from then on the session
// reconnects by itself.
static void muse_keep_vm_session(void) {
    static int64_t next_try;
    static int64_t backoff = MUSE_VM_RETRY_MIN_US;
    if (!wifi_mgr_is_connected() || !config_is_provisioned() || noise_ctrl_is_running()) {
        next_try = 0;
        backoff = MUSE_VM_RETRY_MIN_US;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now < next_try || !operation_gate_take(0, "Muse VM connect")) return;
    led_status_set_state(LED_STATE_WIFI_CONNECTED);
    // Back from a nap: the same VM with the same token, without fetching the
    // VM list again, which took most of the 15 s to reconnect. A refused
    // token is refreshed as usual (ws_auth_failed).
    bool connected = false;
    if (s_muse_resume_vm) {
        s_muse_resume_vm = false;
        connected = noise_ctrl_reconnect(strcmp(s_ui_wifi, "down") == 0 ? NULL : s_ui_wifi);
        if (connected) ESP_LOGI(TAG, "resuming the VM session from before the nap");
    }
    connected = connected
                || (ensure_access_token_ready_with_gate_held(false, true)
                    && connect_preferred_vm_with_gate_held(0));
    ui_set_status(connected ? "auth_ok" : "auth_failed");
    led_status_set_state(connected ? LED_STATE_AUTH_OK : LED_STATE_ERROR);
    operation_gate_give();
    if (!connected) {
        ESP_LOGW(TAG, "no VM session; retrying in %llds", (long long)(backoff / 1000000));
        next_try = now + backoff;
        backoff = backoff * 2 > MUSE_VM_RETRY_MAX_US ? MUSE_VM_RETRY_MAX_US : backoff * 2;
    }
}

// The heartbeat's pause, cut short once Muse joins Wi-Fi so the VM session
// starts at once. Napping off Wi-Fi, there's nothing for the heartbeat to do,
// and each tick wakes the CPU.
static void muse_heartbeat_pause(void) {
    xSemaphoreTake(s_muse_joined, pdMS_TO_TICKS(s_muse_napping ? 60000 : 5000));
}

app_wifi_join_t app_wifi_join_saved(int timeout_ms, bool first_only) {
    if (!operation_gate_take(0, "Muse Wi-Fi connect")) return APP_WIFI_BUSY;
    s_muse_napping = false;

    // Unpaired, the LED keeps showing the pairing state; Wi-Fi is Muse's alone.
    // With no network in range the LED and status stay as they were.
    bool paired = config_is_provisioned();
    char joined[WIFI_KNOWN_SSID_MAX + 1];
    app_wifi_join_t result = join_saved_networks(
        timeout_ms, first_only ? JOIN_FIRST : JOIN_ANY, paired, joined);
    if (result == APP_WIFI_JOINED) {
        ui_set_wifi(joined);
        ui_set_status("wifi_connected");
        persist_connected_wifi_channel();
        if (paired) xSemaphoreGive(s_muse_joined);   // muse_keep_vm_session()
    } else if (result == APP_WIFI_FAILED) {
        ui_set_status("wifi_failed");
        if (paired) led_status_set_state(LED_STATE_WS_DISCONNECTED);
    } else if (result == APP_WIFI_NOT_NEARBY) {
        s_muse_napping = true;   // off Wi-Fi until the next look
    }
    operation_gate_give();
    return result;
}

int64_t app_wifi_none_nearby_at(void) {
    return s_none_nearby_us;
}

bool app_wifi_nap(void) {
    if (!operation_gate_take(0, "Muse Wi-Fi nap")) return false;
    s_muse_resume_vm = s_muse_resume_vm || noise_ctrl_is_running();
    disconnect_vm_transports();
    wifi_mgr_disconnect();
    s_muse_napping = true;
    operation_gate_give();
    return true;
}
#endif  // CONFIG_MUSE_ENABLED

#if CONFIG_MUSE_ENABLED || CONFIG_HOMEHUB_VOICE
// Muse's and the voice board's own Hatch session reach the VM with these.
typedef struct {
    const char *want_vm;
    char *vm_id;
    size_t id_cap;
    char *vm_name;
    size_t name_cap;
    char *vm_token;
    bool ok;
    TaskHandle_t waiter;
} hatch_vm_request_t;

static const vm_info_t *find_vm_by_id(const vm_info_t *vms, int count, const char *want) {
    for (int i = 0; i < count; i++) {
        char id[128];
        if (vms[i].vm_id && strcmp(vms[i].vm_id, want) == 0) return &vms[i];
        if (vm_derive_id(vms[i].vm_url, id, sizeof(id)) && strcmp(id, want) == 0) {
            return &vms[i];
        }
    }
    return NULL;
}

// Runs on an internal-RAM stack: the caller's may be in PSRAM, and this reads
// and (on token refresh) writes NVS.
static void hatch_vm_task(void *arg) {
    hatch_vm_request_t *req = arg;
    if (config_is_provisioned() && operation_gate_take(pdMS_TO_TICKS(15000), "Muse VM lookup")) {
        vm_info_t vms[VM_API_MAX_VMS];
        int count = fetch_vms_with_refresh_with_gate_held(vms, VM_API_MAX_VMS);
        if (count > 0) {
            const vm_info_t *target = NULL;
            if (req->want_vm && req->want_vm[0]) {
                target = find_vm_by_id(vms, count, req->want_vm);
            }
            char preferred[256] = {0};
            if (!target && config_get_str("vm_url", preferred, sizeof(preferred))
                && preferred[0]) {
                target = vm_find_by_url(vms, count, preferred);
            }
            if (!target) target = vm_find_default(vms, count);
            const char *token = NULL;
            if (target && vm_connect_params(target, req->vm_id, req->id_cap, &token)) {
                req->vm_token = strdup(token);
                strlcpy(req->vm_name, target->vm_name ? target->vm_name : "", req->name_cap);
                req->ok = req->vm_token != NULL;
            }
            vm_list_free(vms, count);
        }
        operation_gate_give();
    }
    xTaskNotifyGive(req->waiter);
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

bool app_hatch_vm_credentials(const char *want_vm, char *vm_id, size_t id_cap,
                              char *vm_name, size_t name_cap, char **vm_token) {
    hatch_vm_request_t req = {
        .want_vm = want_vm, .vm_id = vm_id, .id_cap = id_cap,
        .vm_name = vm_name, .name_cap = name_cap,
        .waiter = xTaskGetCurrentTaskHandle(),
    };
    if (xTaskCreate(hatch_vm_task, "muse_vm", 8192, &req, 4, NULL) != pdPASS) {
        return false;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    *vm_token = req.vm_token;
    return req.ok;
}
#endif  // CONFIG_MUSE_ENABLED || CONFIG_HOMEHUB_VOICE

#if CONFIG_MUSE_ENABLED
void app_ble_companion_set(bool advertise) {
    if (advertise) start_ble_setup_server_if_needed();
    ble_server_set_companion_advertising(advertise);
    if (config_setup_complete()) ui_set_ble(advertise ? "companion" : "off");
}

bool app_confirm_pairing_press(void) {
    factory_test_on_button_press();
    if (!link_pairing_confirmation_required() || !ble_server_has_connection()) {
        return false;
    }
    uint32_t session_generation = link_pairing_confirm_active_session();
    if (session_generation) {
        notify_pairing_confirmed(session_generation);
        ESP_LOGI(TAG, "talk button confirmed active pairing session");
    }
    return true;
}

static void reset_setup_task(void *arg) {
    (void)arg;
    reset_setup_from_control("Muse menu reset");
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

void app_reset_setup_async(void) {
    if (xTaskCreate(reset_setup_task, "muse_reset", 4096, NULL, 4, NULL) != pdPASS) {
        atomic_store_explicit(&s_setup_reset_pending, true, memory_order_release);
    }
}
#endif  // CONFIG_MUSE_ENABLED

// ---- VM transport status forwarding ----------------------------------------

static void ws_unpaired_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    reset_setup_from_control("server unpair (node.unpaired)");
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static void on_ws_control_status(const char *status) {
    ble_server_send_status(status);
    ui_set_status(status);
    if (strcmp(status, "ws_connected") == 0) {
        strcpy(s_ui_ws, "up");
        ESP_LOGI(TAG, "control WebSocket connected");
#if CONFIG_HOMEHUB_TUNNEL
        // Control is up, but the data-plane tunnel (multiplexed stream 2) opens
        // separately and can be reset/rejected independently — so DON'T claim
        // it's up here. Show "tunnel connecting" (yellow); the heartbeat drives
        // the LED green only once noise_tunnel_is_connected() is actually true.
        led_status_set_state(LED_STATE_VM_OK);
#else
        // No tunnel on this build: the control session is the whole connection.
        led_status_set_state(LED_STATE_WS_CONNECTED);
#endif
        // Bring up the L3 netif (idempotent; enables NAPT). Link state is
        // reconciled with the real tunnel status in the heartbeat.
        tunnel_ensure_started();
    } else if (strcmp(status, "ws_disconnected") == 0) {
        strcpy(s_ui_ws, "down");
        strcpy(s_ui_raw, "down");
        ESP_LOGI(TAG, "control WebSocket disconnected");
        led_status_set_state(LED_STATE_WS_DISCONNECTED);
        tunnel_netif_set_link(false);
    } else if (strcmp(status, "ws_unpaired") == 0) {
        strcpy(s_ui_ws, "down");
        ESP_LOGW(TAG, "server sent node.unpaired — clearing credentials and restarting");
        if (xTaskCreate(ws_unpaired_task, "ws_unpair", 4096, NULL, 4, NULL)
            != pdPASS) {
            // This callback runs on the Noise control task. Resetting inline
            // would make noise_ctrl_disconnect() wait for its own task to exit.
            ESP_LOGE(TAG, "server unpair worker unavailable; deferring reset");
            atomic_store_explicit(&s_setup_reset_pending, true,
                                  memory_order_release);
        }
    } else if (strcmp(status, "ws_auth_failed") == 0
               || strcmp(status, "ws_refresh_needed") == 0) {
        strcpy(s_ui_ws, "down");
        strcpy(s_ui_raw, "down");
        led_status_set_state(LED_STATE_WS_DISCONNECTED);
        tunnel_netif_set_link(false);
        ESP_LOGW(TAG, "control WebSocket needs fresh VM credentials: %s", status);
        schedule_vm_auth_refresh();
    }
}

// Validates a freshly-installed OTA image, or rolls it back. Spawned once at
// startup. Marking the running image valid (a) commits a PENDING_VERIFY image
// so the bootloader stops trying to revert it, and (b) blesses a normal boot
// so the running slot becomes a valid rollback target for the *next* OTA.
// We gate validation on the control WS coming up: that's the channel a future
// device.ota would arrive on, so if the new image can't reach it, reverting is
// the only way to keep the device recoverable.
static void ota_verify_task(void *arg) {
    (void)arg;
    int64_t deadline = esp_timer_get_time() + OTA_VERIFY_TIMEOUT_US;
    while (esp_timer_get_time() < deadline) {
        if (noise_ctrl_is_connected()) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                ESP_LOGI(TAG, "OTA image validated (control WS up)");
            } else {
                ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed");
            }
            stack_monitor_record(NULL);
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (s_ota_pending_verify) {
        ESP_LOGE(TAG, "OTA verify timed out; rolling back to previous firmware");
        stack_monitor_record(NULL);
        esp_ota_mark_app_invalid_rollback_and_reboot();
        // Only returns when there is no valid partition to revert to.
        ESP_LOGE(TAG, "rollback failed: no valid partition to revert to");
    } else {
        ESP_LOGW(TAG, "running image not validated (control WS never came up)");
    }
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

// ---- Heap snapshot helper ---------------------------------------------------

static void heap_snapshot(const char *label) {
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "HEAP %-20s  int=%5uK/%5uK  dma=%5uK/%5uK  psram=%5uK",
             label,
             (unsigned)(free_int / 1024), (unsigned)(largest_int / 1024),
             (unsigned)(free_dma / 1024), (unsigned)(largest_dma / 1024),
             (unsigned)(free_psram / 1024));
}

// ---- Entry point -----------------------------------------------------------

static void *psram_malloc(size_t sz) {
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(sz);
    return p;
}

void app_run(void) {
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);

    s_operation_gate = xSemaphoreCreateBinary();
#if CONFIG_MUSE_ENABLED
    s_muse_joined = xSemaphoreCreateBinary();
#endif
    s_auth_lock = xSemaphoreCreateMutex();
    s_setup_window_lock = xSemaphoreCreateMutex();
    if (!s_operation_gate || xSemaphoreGive(s_operation_gate) != pdTRUE) {
        ESP_LOGE(TAG, "failed to initialize setup operation gate");
        abort();
    }

    config_store_init();
    wifi_known_init();

    identity_init();
#if CONFIG_MUSE_ENABLED
    muse_glue_storage_ready();
#endif

    const esp_app_desc_t *app_desc = esp_app_get_description();
    char sn[CUSTOMER_PRODUCT_INFO_SN_LEN + 1] = {0};
    char colour[CUSTOMER_PRODUCT_INFO_TEXT_LEN + 1] = {0};
    char region[CUSTOMER_PRODUCT_INFO_TEXT_LEN + 1] = {0};
    bool have_sn = customer_product_info_get_sn(sn, sizeof(sn)) == ESP_OK;
    bool have_colour = customer_product_info_get_colour_text(colour, sizeof(colour)) == ESP_OK;
    bool have_region = customer_product_info_get_region_text(region, sizeof(region)) == ESP_OK;
    ESP_LOGI(TAG, "====== " CONFIG_GADGET_PRODUCT_NAME " ======");
    ESP_LOGI(TAG, "  Node:     %s", identity_node_id());
    ESP_LOGI(TAG, "  Version:  %s", app_desc ? app_desc->version : "?");
    ESP_LOGI(TAG, "  Serial:   %s", have_sn ? sn : "(none)");
    ESP_LOGI(TAG, "  Colour:   %s", have_colour ? colour : "(none)");
    ESP_LOGI(TAG, "  Region:   %s", have_region ? region : "(none)");
    ESP_LOGI(TAG, "  Verify:   %s", link_pairing_sign_factory_test());
    // Only the hint gadgets.muse.ai displays; the full token is never logged.
    ESP_LOGI(TAG, "  SDK token:  %.12s", identity_sdk_token() ? identity_sdk_token() : "(none)");
    ESP_LOGI(TAG, "========================");

    link_pairing_init(identity_node_id(), identity_device_id(), identity_mac(),
                      app_desc ? app_desc->version : "unknown", identity_sdk_token());
    vm_api_set_sdk_token(identity_sdk_token());

    if (!led_status_init()) {
        ESP_LOGW(TAG, "LED init failed — continuing without status LED");
    }
    led_status_set_state(LED_STATE_BOOT);

    heap_snapshot("after config+id");

    wifi_mgr_init();
    heap_snapshot("after wifi_mgr_init");

    noise_ctrl_init(identity_node_id(), identity_ble_name(), on_ws_control_status);
    noise_ctrl_set_command_cb(on_ws_command);
    noise_ctrl_set_agent_name_cb(led_status_set_title);
    heap_snapshot("after noise_ctrl_init");

    // Arm OTA rollback verification. If the running image was just installed
    // by device.ota it boots as PENDING_VERIFY and must reach the control WS
    // within OTA_VERIFY_TIMEOUT_US, else the bootloader reverts it on reboot.
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK
        && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_ota_pending_verify = true;
        ESP_LOGW(TAG, "running a PENDING_VERIFY OTA image; awaiting health check");
    }
    xTaskCreate(ota_verify_task, "ota_verify", 4096, NULL, 4, NULL);

    bool setup_complete = config_setup_complete();
    bool provisioned = config_is_provisioned();

    if (CONFIG_HOMEHUB_WIFI_SSID[0]) {
        // Dev override supplies Wi-Fi from menuconfig and may run token-only in
        // NVS, so don't treat that as a partial pair to wipe.
        if (!setup_complete && provisioned) {
            setup_complete = config_mark_setup_complete();
        }
    } else {
        char probe_ssid[33] = {0};
        bool have_wifi = config_get_str("ssid", probe_ssid, sizeof(probe_ssid))
                         && probe_ssid[0];
        if (!setup_complete && provisioned && have_wifi) {
            // Legacy pair from firmware predating the setup_complete marker.
            setup_complete = config_mark_setup_complete();
        } else if (!setup_complete && (provisioned || (have_wifi && !WIFI_WITHOUT_PAIRING))) {
            // Muse joins Wi-Fi from its own settings before (or without)
            // pairing, so Wi-Fi alone isn't a half-written pair there.
            // Half-written pair (only one of Wi-Fi creds / tokens, no marker):
            // clear it so we boot clean instead of auto-reconnecting half-paired.
            ESP_LOGW(TAG, "partial setup at boot (wifi=%d token=%d); clearing for clean pair",
                     have_wifi, provisioned);
            if (!config_clear_setup()) {
                ESP_LOGE(TAG, "partial setup deletion could not be verified");
            }
        } else if (setup_complete && !have_wifi && WIFI_WITHOUT_PAIRING) {
            // Muse can forget the network after pairing. With no Wi-Fi and
            // pairing refused once setup is done, the device would be stuck,
            // so start over and let the app pair it again.
            ESP_LOGW(TAG, "setup done but no saved wifi; clearing for a fresh pair");
            if (!config_clear_setup()) {
                ESP_LOGE(TAG, "setup deletion could not be verified");
            }
            setup_complete = false;
        }
    }

    // Restore endpoints only after saved setup has been validated or repaired.
    if (setup_complete && provisioned) {
        char url_buf[256] = {0};
        if (config_get_str("api_url_v2", url_buf, sizeof(url_buf)) && url_buf[0]) {
            vm_api_set_base_url(url_buf);
        }
        if (config_get_str("noise_host", url_buf, sizeof(url_buf)) && url_buf[0]) {
            noise_ctrl_set_host(url_buf);
        }
    } else {
        config_erase_key("api_url");
        config_erase_key("api_url_v2");
        config_erase_key("noise_host");
    }

    bool skip_boot_scan = false;
    if (setup_complete && !CONFIG_HOMEHUB_WIFI_SSID[0]) {
        char hint_ssid[33] = {0};
        if (config_get_str("ssid", hint_ssid, sizeof(hint_ssid))
            && config_key_lookup("password") == CONFIG_KEY_FOUND
            && restore_wifi_channel_hint(hint_ssid)) {
            skip_boot_scan = true;
            ESP_LOGI(TAG, "saved wifi channel available; skipping boot scan");
        }
    }

    if (setup_complete) {
        if (!skip_boot_scan) {
            vTaskDelay(pdMS_TO_TICKS(500));
            int n = wifi_mgr_scan_and_cache();
            ESP_LOGI(TAG, "boot scan cached %d APs", n);
            heap_snapshot("after wifi_scan");
        } else {
            heap_snapshot("boot scan skipped");
        }
    }

    s_setup_stage = setup_complete ? "done" : "idle";
#if CONFIG_MUSE_ENABLED
    // Muse's talk button confirms pairing and its menu resets setup; the Link
    // button GPIO may be a display or codec pin on these boards.
    (void)on_button_short_press;
    (void)on_button_double_press;
    (void)on_button_long_press;
#else
    if (!button_init(on_button_short_press, on_button_double_press,
                     on_button_long_press)) {
        ESP_LOGW(TAG, "button init failed — physical setup reset unavailable");
    }
#endif
#if CONFIG_HOMEHUB_VOICE
    voice_init();
#endif
#if CONFIG_HOMEHUB_SENSECAP_SENSORS
    sensecap_sensors_init();
#endif

    if (!setup_complete) {
        // Unpaired devices advertise automatically — no button press is needed
        // to become discoverable. Pairing enforces the configured confirmation policy.
        open_setup_window("boot: unpaired");
        ESP_LOGI(TAG, "BLE advertising; press the button to confirm pairing");
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!schedule_scan_refresh(false, NULL)) {
            ESP_LOGW(TAG, "background boot scan unavailable");
        }
    } else {
        ui_set_ble("off");
        ESP_LOGI(TAG, "BLE setup disabled; long-press reset to pair again");
#if CONFIG_MUSE_ENABLED && CONFIG_SPIRAM
        // Muse may turn on its BLE companion later. The controller needs a
        // 30 KB internal block that TLS and the VM session leave fragmented,
        // so bring the stack up now; it stays silent until advertising is on.
        start_ble_setup_server_if_needed();
#endif
    }

    // Dev override: CONFIG_HOMEHUB_WIFI_SSID lets us skip BLE provisioning
    // entirely so we can iterate on tunnel throughput without re-pairing
    // each flash. When set, we connect to the override SSID and try to
    // bring up the tunnel using whatever device token pair is in NVS. If no
    // override is set, check NVS for previously-provisioned creds (the
    // provision action stores ssid/password/access_token there). If those
    // are present, reconnect automatically — the user shouldn't have to
    // re-pair via BLE after every flash.
    const char *ovr_ssid = CONFIG_HOMEHUB_WIFI_SSID;
    const char *ovr_pwd  = CONFIG_HOMEHUB_WIFI_PASSWORD;

    char nvs_ssid[33] = {0};
    char nvs_pwd[65] = {0};

    const char *wifi_ssid = NULL;
    const char *wifi_pwd = NULL;

    if (ovr_ssid[0]) {
        // Menuconfig override takes priority.
        wifi_ssid = ovr_ssid;
        wifi_pwd = ovr_pwd;
        ESP_LOGI(TAG, "wifi override active, connecting to %s", wifi_ssid);
    } else if (config_get_str("ssid", nvs_ssid, sizeof(nvs_ssid))
               && config_get_str("password", nvs_pwd, sizeof(nvs_pwd))) {
        // Previously provisioned via BLE — auto-reconnect, to whichever saved
        // network is in range.
        memset(nvs_pwd, 0, sizeof(nvs_pwd));
        wifi_ssid = nvs_ssid;
        ESP_LOGI(TAG, "found provisioned creds in NVS, reconnecting (first: %s)", wifi_ssid);
    }

    if (wifi_ssid) {
        ui_set_status("wifi_connecting");
        led_status_set_state(LED_STATE_WIFI_CONNECTING);
        char joined[WIFI_KNOWN_SSID_MAX + 1] = {0};
        bool wifi_joined;
        if (ovr_ssid[0]) {
            wifi_joined = wifi_mgr_connect(wifi_ssid, wifi_pwd, WIFI_CONNECT_TIMEOUT_MS);
            snprintf(joined, sizeof(joined), "%s", wifi_ssid);
        } else {
            // The boot scan, when it ran, already shows which are in range.
            join_mode_t mode = setup_complete && !skip_boot_scan ? JOIN_ANY_CACHED : JOIN_ANY;
            wifi_joined = join_saved_networks(WIFI_CONNECT_TIMEOUT_MS, mode, false,
                                              joined) == APP_WIFI_JOINED;
        }
        if (wifi_joined) {
            ui_set_wifi(joined);
            ui_set_status("wifi_connected");
            led_status_set_state(LED_STATE_WIFI_CONNECTED);
            if (!ovr_ssid[0]) {
                persist_connected_wifi_channel();
            }
            if (config_is_provisioned()) {
                ESP_LOGI(TAG, "found device credentials in NVS, fetching leased VM");
                bool connected = false;
                if (operation_gate_take(portMAX_DELAY, "boot VM connect")) {
                    // Was `true` (force a refresh on every boot). Now lazy: use
                    // the stored token and let the 401 path below refresh it if
                    // the server disagrees.
                    if (ensure_access_token_ready_with_gate_held(false, true)) {
                        connected = connect_preferred_vm_with_gate_held(0);
                    }
                    operation_gate_give();
                }
                if (connected) {
                    ui_set_status("auth_ok");
                    led_status_set_state(LED_STATE_AUTH_OK);
                } else {
                    ui_set_status("auth_failed");
                    led_status_set_state(LED_STATE_ERROR);
                }
            } else {
                if (s_ble_started) {
                    ESP_LOGI(TAG, "no device credentials in NVS, waiting for BLE provision");
                } else {
                    ESP_LOGI(TAG, "no device credentials in NVS; long-press reset to pair again");
                }
            }
        } else {
            ui_set_status("wifi_failed");
            led_status_set_state(LED_STATE_ERROR);
        }
    } else {
        ESP_LOGI(TAG, "no wifi creds, waiting for BLE provision");
    }

#if CONFIG_MUSE_ENABLED
    muse_glue_link_ready();
#endif

    // Heartbeat: log full state every 5 s so we can tell where we're stuck.
    stack_monitor_t stack = STACK_MONITOR_INIT;
    tunnel_stats_t prev_tun = {0};
    while (1) {
        if (atomic_exchange_explicit(&s_setup_reset_pending, false,
                                     memory_order_acq_rel)) {
            reset_setup_from_control("deferred setup reset");
        }

#if CONFIG_MUSE_ENABLED
        muse_keep_vm_session();
#endif

        // Runs on every heartbeat tick, and does two things.
        //
        // Retries the setup paths that need WiFi and may have been skipped at
        // boot with the radio down: legacy auth_token migration, and minting a
        // pair for an access token stored without a refresh token. Both are
        // one-shot; once a complete pair is in NVS they are no-ops.
        //
        // And it is what drives the proactive refresh. Passing false says
        // nothing has rejected the token, so the refresh endpoint is contacted
        // only once CONFIG_HOMEHUB_TOKEN_REFRESH_INTERVAL_DAYS of uptime have
        // elapsed — but this poll is what makes that interval fire at all.
        if (wifi_mgr_is_connected() && config_is_provisioned()
            && operation_gate_take(0, "device token maintenance")) {
            ensure_access_token_ready_with_gate_held(false, true);
            operation_gate_give();
        }

        // Reconcile the data-plane view with the tunnel's real state. The tunnel
        // stream opens/resets independently of the control session, so the LED
        // and s_ui_raw must track noise_tunnel_is_connected() rather than the
        // ws_connected event. Only touch the LED while control is up, so we
        // don't stomp WiFi/BLE/error states during setup or reconnect.
        if (noise_ctrl_is_connected()) {
            bool tun_up = noise_tunnel_is_connected();
            bool was_up = strcmp(s_ui_raw, "up") == 0;
            if (tun_up != was_up) {
                strcpy(s_ui_raw, tun_up ? "up" : "down");
                tunnel_netif_set_link(tun_up);
                // Green when the tunnel is truly live; yellow (VM_OK =
                // "tunnel connecting") while control is up but tunnel is down.
                led_status_set_state(tun_up ? LED_STATE_WS_CONNECTED
                                            : LED_STATE_VM_OK);
            }
        }

        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
        size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        tunnel_stats_t tun;
        tunnel_netif_get_stats(&tun);
        ESP_LOGI(HEARTBEAT_TAG, "hb t=%llds setup=%s wifi=%s ws=%s raw=%s ble=%s "
                       "int=%uK/%uK dma=%uK/%uK psram=%uK "
                       "tun rx+%lu/%luB tx+%lu/%luB drop+%lu",
                 esp_timer_get_time() / 1000000,
                 s_setup_stage,
                 wifi_mgr_is_connected() ? "up" : "down",
                 noise_ctrl_is_connected() ? "up" : "down",
                 noise_tunnel_is_connected() ? "up" : "down",
                 s_ui_ble,
                 (unsigned)(free_int / 1024),
                 (unsigned)(largest_int / 1024),
                 (unsigned)(free_dma / 1024),
                 (unsigned)(largest_dma / 1024),
                 (unsigned)(free_psram / 1024),
                 (unsigned long)(tun.rx_pkts - prev_tun.rx_pkts),
                 (unsigned long)(tun.rx_bytes - prev_tun.rx_bytes),
                 (unsigned long)(tun.tx_pkts - prev_tun.tx_pkts),
                 (unsigned long)(tun.tx_bytes - prev_tun.tx_bytes),
                 (unsigned long)(tun.tx_dropped - prev_tun.tx_dropped));
        prev_tun = tun;
        stack_monitor_record(&stack);
#if CONFIG_MUSE_ENABLED
        muse_heartbeat_pause();
#else
        vTaskDelay(pdMS_TO_TICKS(5000));
#endif
    }
}
