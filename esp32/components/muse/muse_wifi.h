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
#include <stdint.h>

#include "esp_err.h"

#include "muse_settings.h"

/*
 * Wi-Fi station driven by muse_settings: joins whichever saved network is in
 * range, reconnects with backoff, and runs scans for the settings screen. All
 * calls are safe from any task; status is copied out for the UI to poll.
 */

typedef enum {
    MUSE_WIFI_OFF,
    MUSE_WIFI_NO_NETWORK,     /* on, but nothing saved */
    MUSE_WIFI_CONNECTING,
    MUSE_WIFI_CONNECTED,
    MUSE_WIFI_FAILED,         /* gave up (e.g. wrong password) */
    MUSE_WIFI_NOT_NEARBY,     /* none of the saved networks showed up in a scan; looking again less often */
} muse_wifi_state_t;

typedef struct {
    muse_wifi_state_t state;
    char ssid[MUSE_SSID_MAX + 1];
    char ip[16];
    int rssi;
    char detail[40];          /* human-readable reason while connecting/failed */
} muse_wifi_status_t;

typedef struct {
    char ssid[MUSE_SSID_MAX + 1];
    int8_t rssi;
    bool secure;
} muse_wifi_ap_t;

#define MUSE_WIFI_SAVED_MAX 8
typedef struct {
    char ssid[MUSE_SSID_MAX + 1];
    bool hidden;              /* joined by name; doesn't show in scans */
} muse_wifi_saved_t;

esp_err_t muse_wifi_start(void);
/* Re-reads on/off and credentials from settings and (re)connects. */
void muse_wifi_apply(void);
void muse_wifi_status(muse_wifi_status_t *out);
bool muse_wifi_connected(void);
typedef enum {
    MUSE_WIFI_FULL,   /* the mode from before: no modem sleep, but for BLE coexistence */
    MUSE_WIFI_DOZE,   /* modem sleep between DTIM beacons: a few hundred ms extra latency */
    MUSE_WIFI_REST,   /* over several beacons, and Link's session polls less: up to a second */
} muse_wifi_power_t;
void muse_wifi_power(muse_wifi_power_t level);
/* Leaves Wi-Fi, dropping Link's session and Hatch's, until called with false,
 * which rejoins at once. */
void muse_wifi_nap(bool nap);

esp_err_t muse_wifi_scan(void);

/* 开"用手机配网"门户（设备自开热点 + 网页），10 分钟无操作自动关 */
esp_err_t muse_wifi_open_portal(void);
bool muse_wifi_scanning(void);
/* Copies the latest results (strongest first); *gen changes when they do. */
int muse_wifi_scan_results(muse_wifi_ap_t *out, int max, uint32_t *gen);

/* Saved networks, most recently joined first. Returns the count. */
int muse_wifi_saved(muse_wifi_saved_t *out, int max);
/* Forgets one saved network, disconnecting first if it's the one in use. */
void muse_wifi_forget(const char *ssid);
