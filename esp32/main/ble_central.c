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

#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_BT_NIMBLE_ROLE_CENTRAL \
    && CONFIG_BT_NIMBLE_ROLE_OBSERVER && CONFIG_BT_NIMBLE_GATT_CLIENT

#include "ble_central.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"

static const char *TAG = "link.ble_central";

// One central operation at a time; its state lives here.
static SemaphoreHandle_t s_op_lock;
static SemaphoreHandle_t s_done;

static struct {
    ble_central_dev_t *out;
    int max;
    int count;
    uint32_t reports;
    const char *want_name;   // ble_central_find
    bool found;
} s_scan;

static struct {
    int status;              // 0 ok, else NimBLE error
    uint16_t conn;
    uint16_t mtu;
    int services;
    char name[32];
} s_probe;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool ensure_init(void) {
    if (!s_op_lock) s_op_lock = xSemaphoreCreateMutex();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    return s_op_lock && s_done && ble_hs_synced();
}

static uint16_t ms_to_scan_units(uint16_t ms) {  // 0.625 ms units
    uint32_t u = (uint32_t)ms * 1000 / 625;
    if (u < 4) u = 4;
    if (u > 0x4000) u = 0x4000;
    return (uint16_t)u;
}

static void copy_name(char *dst, size_t cap, const uint8_t *src, size_t len) {
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int scan_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        s_scan.reports++;
        struct ble_hs_adv_fields f;
        char name[32] = {0};
        if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) == 0
            && f.name && f.name_len) {
            copy_name(name, sizeof(name), f.name, f.name_len);
        }
        int i;
        for (i = 0; i < s_scan.count; i++) {
            if (memcmp(s_scan.out[i].addr, event->disc.addr.val, 6) == 0) break;
        }
        if (i == s_scan.count && s_scan.count < s_scan.max) {
            ble_central_dev_t *d = &s_scan.out[s_scan.count++];
            memset(d, 0, sizeof(*d));
            memcpy(d->addr, event->disc.addr.val, 6);
            d->addr_type = event->disc.addr.type;
        }
        if (i < s_scan.count) {
            ble_central_dev_t *d = &s_scan.out[i];
            d->rssi = event->disc.rssi;
            if (event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND
                || event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {
                d->connectable = true;
            }
            if (name[0]) memcpy(d->name, name, sizeof(d->name));
            if (s_scan.want_name && name[0] && strcmp(name, s_scan.want_name) == 0
                && d->connectable && !s_scan.found) {
                s_scan.found = true;
                ble_gap_disc_cancel();
                xSemaphoreGive(s_done);
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        xSemaphoreGive(s_done);
        return 0;
    default:
        return 0;
    }
}

static int run_scan(uint32_t duration_ms, bool active, uint16_t itvl_ms,
                    uint16_t window_ms) {
    uint8_t own;
    int rc = ble_hs_id_infer_auto(0, &own);
    if (rc != 0) return rc;
    struct ble_gap_disc_params p = {0};
    p.itvl = ms_to_scan_units(itvl_ms);
    p.window = ms_to_scan_units(window_ms > itvl_ms ? itvl_ms : window_ms);
    p.passive = active ? 0 : 1;
    p.filter_duplicates = 0;
    xSemaphoreTake(s_done, 0);
    rc = ble_gap_disc(own, (int32_t)duration_ms, &p, scan_event, NULL);
    if (rc != 0) return rc;
    xSemaphoreTake(s_done, pdMS_TO_TICKS(duration_ms + 2000));
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    return 0;
}

int ble_central_scan(uint32_t duration_ms, bool active, uint16_t itvl_ms,
                     uint16_t window_ms, ble_central_dev_t *out, int max,
                     uint32_t *reports) {
    if (!ensure_init() || !out || max <= 0) return -1;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    memset(&s_scan, 0, sizeof(s_scan));
    s_scan.out = out;
    s_scan.max = max;
    int rc = run_scan(duration_ms, active, itvl_ms, window_ms);
    int count = rc == 0 ? s_scan.count : -1;
    if (reports) *reports = s_scan.reports;
    if (rc != 0) ESP_LOGW(TAG, "scan rc=%d", rc);
    xSemaphoreGive(s_op_lock);
    return count;
}

bool ble_central_find(const char *name, uint32_t timeout_ms, ble_central_dev_t *out) {
    static ble_central_dev_t seen[24];
    if (!ensure_init() || !name || !out) return false;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    memset(&s_scan, 0, sizeof(s_scan));
    s_scan.out = seen;
    s_scan.max = sizeof(seen) / sizeof(seen[0]);
    s_scan.want_name = name;
    int rc = run_scan(timeout_ms, true, 60, 30);
    bool ok = false;
    if (rc == 0 && s_scan.found) {
        for (int i = 0; i < s_scan.count; i++) {
            if (strcmp(seen[i].name, name) == 0) {
                *out = seen[i];
                ok = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_op_lock);
    return ok;
}

// ---- probe: connect, MTU, discover, read Device Name -------------------------

static int read_name_cb(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg) {
    (void)conn;
    (void)arg;
    if (err->status == 0 && attr && attr->om) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len >= sizeof(s_probe.name)) len = sizeof(s_probe.name) - 1;
        ble_hs_mbuf_to_flat(attr->om, s_probe.name, len, NULL);
        s_probe.name[len] = '\0';
        return 0;   // the read-by-UUID procedure ends with BLE_HS_EDONE
    }
    if (err->status != BLE_HS_EDONE) s_probe.status = err->status;
    xSemaphoreGive(s_done);
    return 0;
}

static int disc_svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg) {
    (void)conn;
    (void)svc;
    (void)arg;
    if (err->status == 0) {
        s_probe.services++;
        return 0;
    }
    if (err->status != BLE_HS_EDONE) s_probe.status = err->status;
    xSemaphoreGive(s_done);
    return 0;
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t mtu, void *arg) {
    (void)conn;
    (void)arg;
    if (err->status == 0) s_probe.mtu = mtu;
    else s_probe.status = err->status;
    xSemaphoreGive(s_done);
    return 0;
}

static int conn_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_probe.status = event->connect.status;
        s_probe.conn = event->connect.status == 0
            ? event->connect.conn_handle : BLE_HS_CONN_HANDLE_NONE;
        xSemaphoreGive(s_done);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "peer disconnected reason=%d", event->disconnect.reason);
        if (s_probe.conn == event->disconnect.conn.conn_handle) {
            s_probe.conn = BLE_HS_CONN_HANDLE_NONE;
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        s_probe.mtu = event->mtu.value;
        return 0;
    default:
        return 0;
    }
}

static bool wait_step(uint32_t timeout_ms) {
    return xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE
        && s_probe.status == 0;
}

bool ble_central_probe(const ble_central_dev_t *dev, uint32_t timeout_ms,
                       bool keep_open, ble_central_session_t *out) {
    if (!ensure_init() || !dev || !out) return false;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    memset(out, 0, sizeof(*out));
    memset(&s_probe, 0, sizeof(s_probe));
    s_probe.conn = BLE_HS_CONN_HANDLE_NONE;
    xSemaphoreTake(s_done, 0);

    bool ok = false;
    uint8_t own;
    ble_addr_t peer = {.type = dev->addr_type};
    memcpy(peer.val, dev->addr, 6);
    uint32_t t0 = now_ms();
    int rc = ble_hs_id_infer_auto(0, &own);
    if (rc == 0) rc = ble_gap_connect(own, &peer, (int32_t)timeout_ms, NULL, conn_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "connect rc=%d", rc);
        goto out;
    }
    if (!wait_step(timeout_ms + 1000)) {
        ESP_LOGW(TAG, "connect failed status=%d", s_probe.status);
        if (s_probe.conn == BLE_HS_CONN_HANDLE_NONE) ble_gap_conn_cancel();
        goto out;
    }
    out->conn_handle = s_probe.conn;
    out->connect_ms = now_ms() - t0;

    t0 = now_ms();
    rc = ble_gattc_exchange_mtu(s_probe.conn, mtu_cb, NULL);
    if (rc != 0 || !wait_step(5000)) {
        ESP_LOGW(TAG, "mtu rc=%d status=%d", rc, s_probe.status);
        goto close;
    }
    out->mtu = s_probe.mtu;
    out->mtu_ms = now_ms() - t0;

    t0 = now_ms();
    rc = ble_gattc_disc_all_svcs(s_probe.conn, disc_svc_cb, NULL);
    if (rc != 0 || !wait_step(10000)) {
        ESP_LOGW(TAG, "discover rc=%d status=%d", rc, s_probe.status);
        goto close;
    }
    out->services = s_probe.services;
    out->discover_ms = now_ms() - t0;

    rc = ble_gattc_read_by_uuid(s_probe.conn, 1, 0xffff,
                                BLE_UUID16_DECLARE(0x2A00),
                                read_name_cb, NULL);
    if (rc != 0 || !wait_step(5000)) {
        ESP_LOGW(TAG, "read name rc=%d status=%d", rc, s_probe.status);
        goto close;
    }
    memcpy(out->gap_name, s_probe.name, sizeof(out->gap_name));
    ok = true;

close:
    if (!keep_open || !ok) {
        if (s_probe.conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_probe.conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        out->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
out:
    xSemaphoreGive(s_op_lock);
    return ok;
}

void ble_central_disconnect(uint16_t conn_handle) {
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

#endif
