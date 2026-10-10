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

#if CONFIG_HOMEHUB_BLE_PERSISTENT

#include "ble_basic.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"

#include "noise_control.h"
#include "noise_tunnel.h"
#include "wifi_mgr.h"

static const char *TAG = "link.ble_basic";

/* 4d475342-000X-4000-8000-6c696e6b6267, little-endian. */
#define BASIC_UUID(x) BLE_UUID128_INIT(0x67, 0x62, 0x6b, 0x6e, 0x69, 0x6c, 0x00, 0x80, \
                                       0x00, 0x40, x, 0x00, 0x42, 0x53, 0x47, 0x4d)
static const ble_uuid128_t SVC_UUID = BASIC_UUID(0x01);
static const ble_uuid128_t STATUS_UUID = BASIC_UUID(0x02);
#if CONFIG_HOMEHUB_BLE_THROUGHPUT_TEST
static const ble_uuid128_t TPUT_UUID = BASIC_UUID(0x03);
#endif

static uint16_t s_status_handle;
static _Atomic uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static _Atomic uint16_t s_mtu = 23;
static atomic_bool s_status_subscribed;
static _Atomic uint32_t s_rx_bytes;
static _Atomic uint32_t s_tx_bytes;
static _Atomic uint32_t s_rx_first_ms, s_rx_last_ms;   // throughput-test writes

static int build_status(char *out, size_t cap) {
    return snprintf(out, cap,
                    "{\"wifi\":%d,\"ws\":%d,\"tun\":%d,"
                    "\"int\":%u,\"dma\":%u,\"dma_big\":%u,\"psram\":%u,"
                    "\"rx\":%lu,\"rx_ms\":%lu,\"tx\":%lu,\"mtu\":%u}",
                    wifi_mgr_is_connected() ? 1 : 0,
                    noise_ctrl_is_connected() ? 1 : 0,
                    noise_tunnel_is_connected() ? 1 : 0,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    (unsigned long)atomic_load(&s_rx_bytes),
                    (unsigned long)(atomic_load(&s_rx_last_ms) - atomic_load(&s_rx_first_ms)),
                    (unsigned long)atomic_load(&s_tx_bytes),
                    (unsigned)atomic_load(&s_mtu));
}

static int status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    char buf[224];
    int n = build_status(buf, sizeof(buf));
    if (n < 0 || (size_t)n >= sizeof(buf)) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, buf, (uint16_t)n) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

#if CONFIG_HOMEHUB_BLE_THROUGHPUT_TEST
static uint16_t s_tput_handle;
static TaskHandle_t s_tput_task;
static _Atomic uint32_t s_tput_pending;

// Notify the requested byte count in MTU-sized chunks. An mbuf comes from
// msys, so a full pool (BLE_HS_ENOMEM, or a NULL mbuf) means the controller
// is behind: back off a tick and retry rather than drop.
static void tput_task(void *arg) {
    (void)arg;
    static uint8_t chunk[512];
    for (size_t i = 0; i < sizeof(chunk); i++) chunk[i] = (uint8_t)i;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t left = atomic_exchange(&s_tput_pending, 0);
        int64_t t0 = esp_log_timestamp();
        uint32_t sent = 0;
        while (left > 0) {
            uint16_t conn = atomic_load(&s_conn);
            if (conn == BLE_HS_CONN_HANDLE_NONE) break;
            uint16_t payload = atomic_load(&s_mtu) - 3;
            if (payload > sizeof(chunk)) payload = sizeof(chunk);
            uint16_t len = left < payload ? (uint16_t)left : payload;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(chunk, len);
            if (!om) {
                vTaskDelay(1);
                continue;
            }
            int rc = ble_gatts_notify_custom(conn, s_tput_handle, om);
            if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
                vTaskDelay(1);
                continue;
            }
            if (rc != 0) {
                ESP_LOGW(TAG, "throughput notify rc=%d", rc);
                break;
            }
            left -= len;
            sent += len;
            atomic_fetch_add(&s_tx_bytes, len);
        }
        uint32_t ms = (uint32_t)(esp_log_timestamp() - t0);
        ESP_LOGI(TAG, "throughput: notified %lu B in %lu ms (%lu kbit/s)",
                 (unsigned long)sent, (unsigned long)ms,
                 (unsigned long)(ms ? (uint64_t)sent * 8 / ms : 0));
    }
}

static int tput_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint32_t now = (uint32_t)esp_log_timestamp();
    if (len == 1) {
        // One byte resets the write counters before an uplink run.
        atomic_store(&s_rx_bytes, 0);
        atomic_store(&s_rx_first_ms, now);
        atomic_store(&s_rx_last_ms, now);
        return 0;
    }
    atomic_fetch_add(&s_rx_bytes, len);
    atomic_store(&s_rx_last_ms, now);
    if (len == 4) {
        uint8_t b[4];
        if (ble_hs_mbuf_to_flat(ctxt->om, b, sizeof(b), NULL) == 0) {
            uint32_t want = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
                            | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
            atomic_store(&s_tput_pending, want);
            if (s_tput_task) xTaskNotifyGive(s_tput_task);
        }
    }
    return 0;
}
#endif  // CONFIG_HOMEHUB_BLE_THROUGHPUT_TEST

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &STATUS_UUID.u,
        .access_cb = status_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_status_handle,
    },
#if CONFIG_HOMEHUB_BLE_THROUGHPUT_TEST
    {
        .uuid = &TPUT_UUID.u,
        .access_cb = tput_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_tput_handle,
    },
#endif
    {0},
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = s_chrs,
    },
    {0},
};

static int basic_gap_event(struct ble_gap_event *event) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                atomic_store(&s_conn, event->connect.conn_handle);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            atomic_store(&s_conn, BLE_HS_CONN_HANDLE_NONE);
            atomic_store(&s_mtu, 23);
            atomic_store(&s_status_subscribed, false);
            break;
        case BLE_GAP_EVENT_MTU:
            atomic_store(&s_mtu, event->mtu.value);
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_status_handle) {
                atomic_store(&s_status_subscribed, event->subscribe.cur_notify);
            }
            break;
        default:
            break;
    }
    return 0;
}

void ble_basic_register(void) {
#if CONFIG_HOMEHUB_BLE_THROUGHPUT_TEST
    if (!s_tput_task
        && xTaskCreate(tput_task, "ble_tput", 3072, NULL, 4, &s_tput_task) != pdPASS) {
        ESP_LOGW(TAG, "throughput task not started");
        s_tput_task = NULL;
    }
#endif
    ble_companion_t companion = {
        .svcs = s_svcs,
        .configure_host = NULL,
        .on_gap_event = basic_gap_event,
    };
    ble_server_set_companion(&companion);
}

bool ble_basic_client_connected(void) {
    return atomic_load(&s_conn) != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_basic_host_synced(void) {
    return ble_hs_synced();
}

void ble_basic_notify_status(void) {
    uint16_t conn = atomic_load(&s_conn);
    if (conn == BLE_HS_CONN_HANDLE_NONE || !atomic_load(&s_status_subscribed)) return;
    char buf[224];
    int n = build_status(buf, sizeof(buf));
    if (n < 0 || (size_t)n >= sizeof(buf)) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)n);
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn, s_status_handle, om);
    if (rc != 0) ESP_LOGD(TAG, "status notify rc=%d", rc);
}

#endif  // CONFIG_HOMEHUB_BLE_PERSISTENT
