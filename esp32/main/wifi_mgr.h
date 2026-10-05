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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_netif.h"

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth_mode;
} wifi_scan_entry_t;

void wifi_mgr_init(void);

// Returns the STA esp_netif handle (NULL before wifi_mgr_init()). The pointer
// is stable for the lifetime of the process.
esp_netif_t *wifi_mgr_get_netif(void);

// Blocking connect. Returns true if associated + got IP within timeout_ms.
bool wifi_mgr_connect(const char *ssid, const char *password, int timeout_ms);

// Seed the last channel used by a provisioned SSID. Connect will scan this
// channel first without pinning a BSSID, so mesh/router changes still fall
// back through the driver's normal fast scan.
bool wifi_mgr_seed_channel_hint(const char *ssid, uint8_t channel);

// Read the channel of the currently associated AP.
bool wifi_mgr_get_connected_channel(uint8_t *channel);

void wifi_mgr_disconnect(void);

bool wifi_mgr_is_connected(void);

/*
 * 最近一次 STA 断连的原因（esp_wifi_reason_t），以及一句人话。
 * 屏幕和配网门户都用它解释"为什么连不上" —— 以前只有日志里有。
 */
uint8_t wifi_mgr_last_disconnect_reason(void);
void wifi_mgr_failure_text(char *out, size_t out_size);

/*
 * 配网门户：临时起一个开放热点（Muse-XXXXXX），让手机连上来用浏览器填 Wi-Fi。
 * 之所以不走 BLE：那条路要完成配对握手并把命令加密，设备配好之后还会拒绝重新配对，
 * 而 HTTP 表单在手机上任何浏览器都能用，也能把失败原因直接回显。
 */
esp_err_t wifi_mgr_portal_start(void);
void wifi_mgr_portal_stop(void);
bool wifi_mgr_portal_active(void);
const char *wifi_mgr_portal_ip(void);
const char *wifi_mgr_portal_ssid(void);
const char *wifi_mgr_portal_password(void);


// While wifi_mgr_connect() is joining a network, copies its name out and
// returns true.
bool wifi_mgr_joining(char *ssid, size_t cap);

// Synchronous scan. Caller passes a buffer; returns the number of unique
// SSIDs written (≤ max_entries) — per-BSSID records are collapsed to one entry
// per SSID (best RSSI), hidden/blank SSIDs dropped, ordered by RSSI desc.
// channel 0 scans every channel. A target_ssid probes for that network alone,
// which also finds it if it hides its name. Returns -1 if no scan ran (another
// scan or a connect had the radio).
int wifi_mgr_scan(wifi_scan_entry_t *out, int max_entries, uint8_t channel,
                  const char *target_ssid);

// Run a scan and store the results in the internal cache. Returns count.
int wifi_mgr_scan_and_cache(void);

// Run a scan and merge into the existing cache. Existing entries with the
// same SSID get updated RSSI (best-of). New SSIDs are appended. Cached
// entries not seen in the fresh scan are kept (BLE coex causes false
// negatives on this antenna). Returns total cache count, or 0 when no fresh
// scan completed.
int wifi_mgr_scan_and_merge_cache(void);

// Copy cached scan results out. Returns count copied.
int wifi_mgr_get_cached_scan(wifi_scan_entry_t *out, int max_entries);

// Whether the cached scan has this network, broadcasting its name.
bool wifi_mgr_cached_scan_has(const char *ssid);
