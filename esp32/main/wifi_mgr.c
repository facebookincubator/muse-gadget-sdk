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

#include "wifi_mgr.h"

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

static const char *TAG = "link.wifi";

#define BIT_CONNECTED   BIT0
#define BIT_FAIL        BIT1
#define BIT_GOT_IP      BIT2

#define MAX_CACHED_SCANS 25
// Raw per-BSSID records to pull before collapsing to one entry per SSID.
// A dual-band/mesh network reports one BSSID per radio, so this is larger
// than the unique-SSID cap to avoid dropping a network whose BSSIDs are dense.
#define SCAN_MAX_RECORDS 40

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_scan_mutex = NULL;
static SemaphoreHandle_t s_cache_mutex = NULL;
static esp_netif_t *s_sta_netif;
static bool s_inited = false;
static bool s_connecting = false;        // true during initial wifi_mgr_connect() call
// The network wifi_mgr_connect() is joining, for status screens.
static portMUX_TYPE s_join_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_join_ssid[33];
static bool s_keep_connected = false;    // true once we've ever had a successful association
static atomic_bool s_connect_pending = ATOMIC_VAR_INIT(false);
// Not joined or joining: nothing for wifi_mgr_connect() to tear down first.
static atomic_bool s_sta_idle = ATOMIC_VAR_INIT(true);
static int s_retry = 0;
static int s_reconnect_backoff_ms = 1000;  // grows on consecutive failures
#if CONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM
static SemaphoreHandle_t s_transfer_mutex;
static unsigned s_transfers;
#endif
#define MAX_INITIAL_RETRY        3
#define RECONNECT_BACKOFF_MIN_MS 1000
#define RECONNECT_BACKOFF_MAX_MS 60000

static wifi_scan_entry_t s_cache[MAX_CACHED_SCANS];
static int s_cache_count = 0;

// Best-BSSID cache: stores the strongest BSSID seen per SSID from the most
// recent scan. wifi_mgr_connect uses this to target a specific AP with
// WIFI_FAST_SCAN, skipping the ~10s all-channel re-scan the driver does
// when selecting by signal across all BSSIDs.
typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
} bssid_cache_entry_t;

static bssid_cache_entry_t s_bssid_cache[MAX_CACHED_SCANS];
static int s_bssid_cache_count = 0;
static struct {
    char ssid[33];
    uint8_t channel;
} s_channel_hint;

static bool channel_hint_valid(uint8_t channel) {
    return (channel >= 1 && channel <= 13)
           || (channel >= 36 && channel <= 64 && channel % 4 == 0)
           || (channel >= 100 && channel <= 144 && channel % 4 == 0)
           || (channel >= 149 && channel <= 177 && (channel - 149) % 4 == 0);
}

static void reconnect_timer_cb(TimerHandle_t t) {
    ESP_LOGI(TAG, "reconnect attempt (post-association)");
    atomic_store(&s_sta_idle, false);
    esp_wifi_connect();
}

static TimerHandle_t s_reconnect_timer = NULL;

static void schedule_reconnect(void) {
    if (!s_reconnect_timer) {
        s_reconnect_timer = xTimerCreate("wifi_rcn",
                                         pdMS_TO_TICKS(s_reconnect_backoff_ms),
                                         pdFALSE, NULL, reconnect_timer_cb);
    } else {
        xTimerChangePeriod(s_reconnect_timer,
                           pdMS_TO_TICKS(s_reconnect_backoff_ms), 0);
    }
    xTimerStart(s_reconnect_timer, 0);
    ESP_LOGI(TAG, "reconnect scheduled in %dms", s_reconnect_backoff_ms);

    // Exponential backoff, capped.
    s_reconnect_backoff_ms *= 2;
    if (s_reconnect_backoff_ms > RECONNECT_BACKOFF_MAX_MS) {
        s_reconnect_backoff_ms = RECONNECT_BACKOFF_MAX_MS;
    }
}

#if CONFIG_MUSE_ENABLED
// A saved channel only says where the last AP was: on a mesh, the node found
// there can be a room too far, and a join to it limps along at -80 dBm. The
// fast scan skips APs weaker than this and goes on to the other channels.
#define FAST_SCAN_MIN_RSSI (-75)

// With no AP above the floor, take the strongest there is.
static bool drop_rssi_floor(uint8_t reason) {
    if (reason != WIFI_REASON_NO_AP_FOUND
        && reason != WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD) {
        return false;
    }
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) != ESP_OK
        || wc.sta.threshold.rssi != FAST_SCAN_MIN_RSSI) {
        return false;
    }
    wc.sta.threshold.rssi = 0;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
    ESP_LOGI(TAG, "no AP above %d dBm; joining the strongest",
             FAST_SCAN_MIN_RSSI);
    return true;
}
#else
static bool drop_rssi_floor(uint8_t reason) {
    (void)reason;
    return false;
}
#endif

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Don't auto-connect — wait for explicit wifi_mgr_connect() call.
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev =
            (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "station disconnected reason=%u rssi=%d",
                 ev ? ev->reason : 0, ev ? ev->rssi : 0);

        xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_GOT_IP);
        atomic_store(&s_sta_idle, true);
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected reason=%u", event ? event->reason : 0u);

        bool floor_dropped = event && drop_rssi_floor(event->reason);
        if (s_connecting) {
            // Initial connect: retry up to MAX_INITIAL_RETRY, then fail.
            // Dropping the RSSI floor starts a fresh scan, not a retry.
            if (floor_dropped) {
                atomic_store(&s_sta_idle, false);
                esp_wifi_connect();
            } else if (s_retry < MAX_INITIAL_RETRY) {
                s_retry++;
                ESP_LOGI(TAG, "retry connect (%d/%d)", s_retry, MAX_INITIAL_RETRY);
                atomic_store(&s_sta_idle, false);
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_events, BIT_FAIL);
            }
        } else if (s_keep_connected) {
            // Post-association drop. Reconnect forever with exponential backoff.
            ESP_LOGW(TAG, "wifi disconnected post-association, scheduling reconnect");
            schedule_reconnect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_keep_connected = true;
        s_reconnect_backoff_ms = RECONNECT_BACKOFF_MIN_MS;  // reset on success
        xEventGroupSetBits(s_events, BIT_CONNECTED | BIT_GOT_IP);
    }
}

void wifi_mgr_init(void) {
    if (s_inited) return;
    s_events = xEventGroupCreate();
    s_scan_mutex = xSemaphoreCreateMutex();
    s_cache_mutex = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_country_t country = {
        .cc = "US", .schan = 1, .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    ESP_ERROR_CHECK(esp_wifi_start());
#if SOC_WIFI_SUPPORT_5G
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
#endif
#if CONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM
    s_transfer_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_transfer_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
#else
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif
    s_inited = true;

    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(TAG, "WIFI STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "esp_wifi_get_mac failed");
    }
}

bool wifi_mgr_connect(const char *ssid, const char *password, int timeout_ms) {
    if (!s_inited) wifi_mgr_init();

    TickType_t started = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    bool connected = false;

    // Connect owns the radio. If an optional refresh scan is active, abort it
    // and wait for the scan task to release the shared operation mutex.
    s_connecting = false;
    s_keep_connected = false;
    if (s_reconnect_timer) xTimerStop(s_reconnect_timer, 0);
    atomic_store_explicit(&s_connect_pending, true, memory_order_release);
    bool scan_cancelled = false;
    while (xSemaphoreTake(s_scan_mutex, 0) != pdTRUE) {
        esp_err_t stop_err = esp_wifi_scan_stop();
        if (stop_err == ESP_OK && !scan_cancelled) {
            ESP_LOGI(TAG, "cancelled background scan for wifi connect");
            scan_cancelled = true;
        }
        if (xTaskGetTickCount() - started >= timeout_ticks) {
            ESP_LOGW(TAG, "wifi connect timed out waiting for radio");
            atomic_store_explicit(&s_connect_pending, false,
                                  memory_order_release);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    wifi_config_t wc = {0};
    // The driver reads these arrays without a terminator, so a 32-byte SSID
    // (the 802.11 maximum, which wifi_known and the scan picker accept) or a
    // 64-char PSK fills the whole field. wc is zeroed, so shorter values stay
    // NUL-padded.
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
    if (password) {
        memcpy(wc.sta.password, password,
               strnlen(password, sizeof(wc.sta.password)));
    }
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
#if CONFIG_MUSE_ENABLED || CONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM
    // Beacons between wakes in max modem sleep (Muse asleep on battery):
    // about 1 s at the usual 102.4 ms interval, a third of the wakes of the
    // default 3. Min modem sleep (awake between turns) wakes every DTIM.
    wc.sta.listen_interval = 10;
#endif

    // If we have a cached BSSID for this SSID from a recent scan, target it
    // directly with WIFI_FAST_SCAN. This skips the ~10s all-channel re-scan
    // the driver does with WIFI_ALL_CHANNEL_SCAN + WIFI_CONNECT_AP_BY_SIGNAL.
    // Fall back to the full scan when no cache is available (e.g. reconnect
    // after the AP changed channels).
    bool have_bssid = false;
    for (int i = 0; i < s_bssid_cache_count; i++) {
        if (strcmp(s_bssid_cache[i].ssid, ssid) == 0) {
            memcpy(wc.sta.bssid, s_bssid_cache[i].bssid, 6);
            wc.sta.bssid_set = true;
            wc.sta.channel = s_bssid_cache[i].channel;
            have_bssid = true;
            ESP_LOGI(TAG, "using cached BSSID %02x:%02x:%02x:%02x:%02x:%02x ch%d (rssi %d)",
                     wc.sta.bssid[0], wc.sta.bssid[1], wc.sta.bssid[2],
                     wc.sta.bssid[3], wc.sta.bssid[4], wc.sta.bssid[5],
                     wc.sta.channel, s_bssid_cache[i].rssi);
            break;
        }
    }

    bool have_channel_hint = !have_bssid && s_channel_hint.channel != 0
                             && strcmp(s_channel_hint.ssid, ssid) == 0;
    if (have_channel_hint) {
        wc.sta.channel = s_channel_hint.channel;
#if CONFIG_MUSE_ENABLED
        wc.sta.threshold.rssi = FAST_SCAN_MIN_RSSI;
#endif
        ESP_LOGI(TAG, "starting fast scan on saved channel %u",
                 wc.sta.channel);
    }

    if (have_bssid || have_channel_hint) {
        wc.sta.scan_method = WIFI_FAST_SCAN;
    } else {
        // No cached BSSID — full scan to find the best AP.
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    }

    // Let a join in progress (or a connection) wind down first; with none,
    // that would only cost half a second of every rejoin.
    if (!atomic_load(&s_sta_idle)) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_retry = 0;
    s_connecting = true;
    portENTER_CRITICAL(&s_join_lock);
    strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
    portEXIT_CRITICAL(&s_join_lock);
    s_reconnect_backoff_ms = RECONNECT_BACKOFF_MIN_MS;
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAIL | BIT_GOT_IP);

    // esp_wifi_set_config returns ESP_ERR_WIFI_STATE while the STA is still
    // tearing down a prior association; poll briefly instead of failing the
    // connect (which would surface as a misleading wifi_failed).
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 30; i++) {  // ~1.5s budget @ 50ms
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err != ESP_ERR_WIFI_STATE) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi set_config failed: %s", esp_err_to_name(err));
        goto done;
    }
    atomic_store(&s_sta_idle, false);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi connect start failed: %s", esp_err_to_name(err));
        goto done;
    }

    TickType_t elapsed = xTaskGetTickCount() - started;
    TickType_t remaining = elapsed < timeout_ticks
                           ? timeout_ticks - elapsed : 0;
    EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_GOT_IP | BIT_FAIL,
        pdFALSE, pdFALSE, remaining);
    connected = (bits & BIT_GOT_IP) != 0;
    if (!connected && have_bssid) {
        // Cached BSSID didn't work — invalidate so next attempt does a full
        // scan (AP may have changed channel or gone away).
        s_bssid_cache_count = 0;
    }

done:
    s_connecting = false;          // initial-attempt window is over
    portENTER_CRITICAL(&s_join_lock);
    s_join_ssid[0] = '\0';
    portEXIT_CRITICAL(&s_join_lock);
    atomic_store_explicit(&s_connect_pending, false,
                          memory_order_release);
    xSemaphoreGive(s_scan_mutex);
    return connected;
}

bool wifi_mgr_seed_channel_hint(const char *ssid, uint8_t channel) {
    if (!ssid || !channel_hint_valid(channel)) return false;
    size_t ssid_len = strnlen(ssid, sizeof(s_channel_hint.ssid));
    if (ssid_len == 0 || ssid_len >= sizeof(s_channel_hint.ssid)) return false;

    memset(&s_channel_hint, 0, sizeof(s_channel_hint));
    memcpy(s_channel_hint.ssid, ssid, ssid_len);
    s_channel_hint.channel = channel;
    return true;
}

bool wifi_mgr_get_connected_channel(uint8_t *channel) {
    if (!s_inited || !channel || !wifi_mgr_is_connected()) return false;

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK || ap.primary == 0) {
        return false;
    }
    *channel = ap.primary;
    return true;
}

void wifi_mgr_disconnect(void) {
    if (!s_inited) return;
    s_keep_connected = false;      // suppress auto-reconnect
    if (s_reconnect_timer) xTimerStop(s_reconnect_timer, 0);
    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_GOT_IP);
}

bool wifi_mgr_joining(char *ssid, size_t cap) {
    portENTER_CRITICAL(&s_join_lock);
    bool joining = s_join_ssid[0] != '\0';
    if (joining && ssid && cap) strlcpy(ssid, s_join_ssid, cap);
    portEXIT_CRITICAL(&s_join_lock);
    return joining;
}

bool wifi_mgr_is_connected(void) {
    if (!s_inited) return false;
    return (xEventGroupGetBits(s_events) & BIT_GOT_IP) != 0;
}

esp_netif_t *wifi_mgr_get_netif(void) {
    return s_sta_netif;
}

int wifi_mgr_scan(wifi_scan_entry_t *out, int max_entries, uint8_t channel,
                  const char *target_ssid) {
    if (!s_inited || !out || max_entries <= 0) return 0;

    // The driver refuses to scan while a join is winding down
    // (wifi_mgr_disconnect() during a reconnect attempt); give it a moment.
    for (int i = 0; i < 20 && !atomic_load(&s_sta_idle)
                    && !wifi_mgr_is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (xSemaphoreTake(s_scan_mutex, 0) != pdTRUE) {
        ESP_LOGI(TAG, "scan already in progress, skipping");
        return -1;
    }
    if (atomic_load_explicit(&s_connect_pending, memory_order_acquire)) {
        ESP_LOGI(TAG, "wifi connect pending, skipping scan");
        xSemaphoreGive(s_scan_mutex);
        return -1;
    }

    // A probe that names the network is answered by one that hides it too.
    wifi_scan_config_t sc = {
        .ssid = (uint8_t *)target_ssid,
        .channel = channel,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 0, .max = 0 },
    };
    const char *which = target_ssid ? ", one network" : "";
    if (channel) {
        ESP_LOGI(TAG, "starting wifi scan (ch=%u%s)...", channel, which);
    } else {
        wifi_country_t cc;
        esp_wifi_get_country(&cc);
        ESP_LOGI(TAG, "starting wifi scan (country=%s ch=%d-%d%s)...",
                 cc.cc, cc.schan, cc.schan + cc.nchan - 1, which);
    }
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (atomic_load_explicit(&s_connect_pending, memory_order_acquire)) {
        ESP_LOGI(TAG, "scan cancelled for pending wifi connect");
        esp_wifi_clear_ap_list();
        xSemaphoreGive(s_scan_mutex);
        return -1;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_scan_mutex);
        return -1;
    }

    uint16_t total = 0;
    esp_wifi_scan_get_ap_num(&total);
    ESP_LOGI(TAG, "scan complete, %d APs found", total);
    if (total == 0) {
        xSemaphoreGive(s_scan_mutex);
        return 0;
    }
    uint16_t want = total > SCAN_MAX_RECORDS ? SCAN_MAX_RECORDS : total;

    wifi_ap_record_t *recs = calloc(want, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        xSemaphoreGive(s_scan_mutex);
        return -1;
    }
    esp_wifi_scan_get_ap_records(&want, recs);

    // Collapse the per-BSSID records to one entry per SSID: a dual-band or
    // mesh network shows up once per radio. recs is sorted by RSSI desc
    // (esp_wifi_scan_get_ap_records), so the first BSSID seen for an SSID is
    // its strongest and the deduped list stays RSSI-ordered. Mark secure if
    // any BSSID is secured, and drop hidden (blank) SSIDs.
    //
    // Also populate the BSSID cache with the strongest AP per SSID so
    // wifi_mgr_connect can target it directly without re-scanning.
    int n = 0;
    for (int i = 0; i < want; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') continue;

        int found = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) { found = j; break; }
        }
        if (found >= 0) {
            if (recs[i].rssi > out[found].rssi) out[found].rssi = recs[i].rssi;
            if (out[found].auth_mode == 0) out[found].auth_mode = (uint8_t)recs[i].authmode;
            continue;
        }
        if (n >= max_entries) continue;
        strncpy(out[n].ssid, ssid, sizeof(out[n].ssid) - 1);
        out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
        out[n].rssi = recs[i].rssi;
        out[n].auth_mode = (uint8_t)recs[i].authmode;
        // Cache the strongest BSSID for this SSID (first seen = strongest,
        // since recs is RSSI-sorted). Retain entries missed by a BLE
        // coexistence refresh so Connect can still use the boot-scan target.
        int cached = -1;
        for (int j = 0; j < s_bssid_cache_count; j++) {
            if (strcmp(s_bssid_cache[j].ssid, ssid) == 0) {
                cached = j;
                break;
            }
        }
        if (cached < 0 && s_bssid_cache_count < MAX_CACHED_SCANS) {
            cached = s_bssid_cache_count++;
        }
        if (cached >= 0) {
            bssid_cache_entry_t *entry = &s_bssid_cache[cached];
            strncpy(entry->ssid, ssid, sizeof(entry->ssid) - 1);
            entry->ssid[sizeof(entry->ssid) - 1] = '\0';
            memcpy(entry->bssid, recs[i].bssid, sizeof(entry->bssid));
            entry->rssi = recs[i].rssi;
            entry->channel = recs[i].primary;
        }
        n++;
    }
    free(recs);
    xSemaphoreGive(s_scan_mutex);

    ESP_LOGI(TAG, "scan: %d records -> %d unique SSIDs", (int)want, n);
    return n;
}

int wifi_mgr_scan_and_cache(void) {
    wifi_scan_entry_t fresh[MAX_CACHED_SCANS];
    int n = wifi_mgr_scan(fresh, MAX_CACHED_SCANS, 0, NULL);
    if (n <= 0) return 0;

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    memcpy(s_cache, fresh, sizeof(*fresh) * n);
    s_cache_count = n;
    xSemaphoreGive(s_cache_mutex);
    ESP_LOGI(TAG, "cached %d networks", n);
    return n;
}

bool wifi_mgr_cached_scan_has(const char *ssid) {
    if (!ssid || !ssid[0] || !s_cache_mutex) return false;
    bool found = false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (int i = 0; i < s_cache_count && !found; i++) {
        found = strcmp(s_cache[i].ssid, ssid) == 0;
    }
    xSemaphoreGive(s_cache_mutex);
    return found;
}

int wifi_mgr_get_cached_scan(wifi_scan_entry_t *out, int max_entries) {
    if (!out || max_entries <= 0) return 0;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    int n = s_cache_count > max_entries ? max_entries : s_cache_count;
    memcpy(out, s_cache, sizeof(*out) * n);
    xSemaphoreGive(s_cache_mutex);
    return n;
}

// Scan and merge into the cache. Existing entries with the same SSID are
// updated to the better RSSI (less negative); new SSIDs are appended.
// We don't drop cached entries that aren't in the fresh scan — under BLE
// coexistence the receiver can miss APs that were detected during the
// clean boot scan, and we don't want to forget them.
// Returns the new total cache count, or 0 when no fresh scan completed.
int wifi_mgr_scan_and_merge_cache(void) {
    wifi_scan_entry_t fresh[MAX_CACHED_SCANS];
    int n = wifi_mgr_scan(fresh, MAX_CACHED_SCANS, 0, NULL);
    if (n <= 0) return 0;

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
        bool merged = false;
        for (int j = 0; j < s_cache_count; j++) {
            if (strcmp(fresh[i].ssid, s_cache[j].ssid) == 0) {
                if (fresh[i].rssi > s_cache[j].rssi) {
                    s_cache[j].rssi = fresh[i].rssi;
                    s_cache[j].auth_mode = fresh[i].auth_mode;
                }
                merged = true;
                break;
            }
        }
        if (!merged && s_cache_count < MAX_CACHED_SCANS) {
            s_cache[s_cache_count++] = fresh[i];
        }
    }
    int total = s_cache_count;
    xSemaphoreGive(s_cache_mutex);
    ESP_LOGI(TAG, "merged scan: %d fresh, %d cached total", n, total);
    return total;
}

#if CONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM
void wifi_mgr_transfer(bool active) {
    if (!s_transfer_mutex) return;
    xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);
    unsigned before = s_transfers;
    s_transfers = active ? before + 1 : before ? before - 1 : 0;
    if (!before != !s_transfers) {
        esp_err_t err = esp_wifi_set_ps(s_transfers ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM);
        if (err != ESP_OK) ESP_LOGW(TAG, "modem sleep change failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_transfer_mutex);
}
#endif
