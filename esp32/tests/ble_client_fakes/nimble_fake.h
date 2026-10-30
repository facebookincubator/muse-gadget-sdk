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

// Just enough of ESP-IDF, FreeRTOS and the NimBLE host API to compile
// main/ble_client.c on a host (tests/ble_client_harness.c). Every host/*.h,
// freertos/*.h and esp_*.h in this directory includes this file.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- ESP-IDF ----
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_calloc(n, size, caps) calloc((n), (size))
#define heap_caps_free(p) free(p)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
int64_t esp_timer_get_time(void);

// ---- FreeRTOS ----
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void fake_critical_enter(void);
void fake_critical_exit(void);
#define portENTER_CRITICAL(m) ((void)(m), fake_critical_enter())
#define portEXIT_CRITICAL(m) ((void)(m), fake_critical_exit())
typedef struct fake_sem *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void vTaskDelay(TickType_t ticks);

// ---- NimBLE ----
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_EDONE 14
#define BLE_HS_ENOTCONN 7
#define BLE_HS_ERR_ATT_BASE 0x100
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 0x05
#define BLE_ATT_ERR_INSUFFICIENT_ENC 0x0f
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_GAP_REPEAT_PAIRING_RETRY 1
#define BLE_GAP_SCAN_ITVL_MS(ms) ((ms) * 1000 / 625)
#define BLE_GAP_SCAN_WIN_MS(ms) ((ms) * 1000 / 625)
#define BLE_HCI_ADV_RPT_EVTYPE_ADV_IND 0
#define BLE_HCI_ADV_RPT_EVTYPE_DIR_IND 1
#define BLE_GATT_CHR_PROP_NOTIFY 0x10
#define BLE_GATT_CHR_PROP_INDICATE 0x20
#define BLE_GATT_DSC_CLT_CFG_UUID16 0x2902
#define BLE_UUID_TYPE_16 16
#define BLE_UUID_TYPE_32 32
#define BLE_UUID_TYPE_128 128

enum {
    BLE_GAP_EVENT_CONNECT,
    BLE_GAP_EVENT_DISCONNECT,
    BLE_GAP_EVENT_DISC,
    BLE_GAP_EVENT_DISC_COMPLETE,
    BLE_GAP_EVENT_ENC_CHANGE,
    BLE_GAP_EVENT_MTU,
    BLE_GAP_EVENT_NOTIFY_RX,
    BLE_GAP_EVENT_REPEAT_PAIRING,
};

struct os_mbuf {
    uint16_t len;
    uint8_t data[600];
};
#define OS_MBUF_PKTLEN(om) ((om)->len)
struct os_mbuf *ble_hs_mbuf_from_flat(const void *buf, uint16_t len);
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *flat, uint16_t max_len, uint16_t *out_len);

typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef struct { ble_uuid_t u; uint32_t value; } ble_uuid32_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
typedef union {
    ble_uuid_t u;
    ble_uuid16_t u16;
    ble_uuid32_t u32;
    ble_uuid128_t u128;
} ble_uuid_any_t;

typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;

struct ble_gap_conn_desc { ble_addr_t peer_id_addr; };

struct ble_gap_event {
    uint8_t type;
    union {
        struct { int status; uint16_t conn_handle; } connect;
        struct { int reason; struct { uint16_t conn_handle; } conn; } disconnect;
        struct {
            uint8_t event_type;
            uint8_t length_data;
            ble_addr_t addr;
            int8_t rssi;
            const uint8_t *data;
        } disc;
        struct { uint16_t conn_handle; int status; } enc_change;
        struct { uint16_t conn_handle; uint16_t value; } mtu;
        struct { uint16_t conn_handle; uint16_t attr_handle; struct os_mbuf *om; } notify_rx;
        struct { uint16_t conn_handle; } repeat_pairing;
    };
};
typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);

struct ble_gap_disc_params {
    uint16_t itvl, window;
    uint8_t filter_duplicates, passive;
};

struct ble_hs_adv_fields {
    const uint8_t *name;
    uint8_t name_len;
    const ble_uuid16_t *uuids16;
    uint8_t num_uuids16;
    const ble_uuid32_t *uuids32;
    uint8_t num_uuids32;
    const ble_uuid128_t *uuids128;
    uint8_t num_uuids128;
    const uint8_t *mfg_data;
    uint8_t mfg_data_len;
    int8_t tx_pwr_lvl;
    unsigned tx_pwr_lvl_is_present : 1;
};

struct ble_gatt_error { uint16_t status; uint16_t att_handle; };
struct ble_gatt_svc { uint16_t start_handle, end_handle; ble_uuid_any_t uuid; };
struct ble_gatt_chr { uint16_t def_handle, val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct ble_gatt_attr { uint16_t handle, offset; struct os_mbuf *om; };

typedef int ble_gatt_mtu_fn(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg);
typedef int ble_gatt_disc_svc_fn(uint16_t conn, const struct ble_gatt_error *err,
                                 const struct ble_gatt_svc *svc, void *arg);
typedef int ble_gatt_chr_fn(uint16_t conn, const struct ble_gatt_error *err,
                            const struct ble_gatt_chr *chr, void *arg);
typedef int ble_gatt_dsc_fn(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                            const struct ble_gatt_dsc *dsc, void *arg);
typedef int ble_gatt_attr_fn(uint16_t conn, const struct ble_gatt_error *err,
                             struct ble_gatt_attr *attr, void *arg);

int ble_hs_synced(void);
int ble_hs_id_infer_auto(int privacy, uint8_t *own);
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f, const uint8_t *data, uint8_t len);
int ble_gap_disc(uint8_t own, int32_t ms, const struct ble_gap_disc_params *p, ble_gap_event_fn *cb, void *arg);
int ble_gap_disc_cancel(void);
int ble_gap_connect(uint8_t own, const ble_addr_t *peer, int32_t ms, const void *params,
                    ble_gap_event_fn *cb, void *arg);
int ble_gap_conn_cancel(void);
int ble_gap_terminate(uint16_t conn, uint8_t reason);
int ble_gap_security_initiate(uint16_t conn);
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *out);
int ble_store_util_delete_peer(const ble_addr_t *addr);
int ble_gattc_exchange_mtu(uint16_t conn, ble_gatt_mtu_fn *cb, void *arg);
int ble_gattc_disc_all_svcs(uint16_t conn, ble_gatt_disc_svc_fn *cb, void *arg);
int ble_gattc_disc_all_chrs(uint16_t conn, uint16_t start, uint16_t end, ble_gatt_chr_fn *cb, void *arg);
int ble_gattc_disc_all_dscs(uint16_t conn, uint16_t start, uint16_t end, ble_gatt_dsc_fn *cb, void *arg);
int ble_gattc_read_long(uint16_t conn, uint16_t handle, uint16_t offset, ble_gatt_attr_fn *cb, void *arg);
int ble_gattc_write_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t len,
                         ble_gatt_attr_fn *cb, void *arg);
int ble_gattc_write_long(uint16_t conn, uint16_t handle, uint16_t offset, struct os_mbuf *om,
                         ble_gatt_attr_fn *cb, void *arg);
int ble_gattc_write_no_rsp_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t len);
