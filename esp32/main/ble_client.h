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

// BLE central for the Muse BLE commands (main/ble_commands.c): scan with the
// advertised fields decoded, hold up to a few connections, discover their GATT
// databases and read, write and subscribe to characteristics. Shares the one
// NimBLE host that ble_server starts.
//
// Every call blocks and must run on one worker task (never the NimBLE host
// task or a GAP/GATT callback); calls are serialized by the caller. Every
// call returns 0 on success, or an error: a negative BLE_CLIENT_E* value or
// one of NimBLE's own codes (positive, ATT errors from 0x100). Counts come
// back through out-parameters, never in the return value.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_CLIENT_MAX_CONNS      3
#define BLE_CLIENT_MAX_SVCS       16
#define BLE_CLIENT_MAX_CHRS       48
#define BLE_CLIENT_MAX_VALUE      512
#define BLE_CLIENT_MAX_NOTIFY     48
#define BLE_CLIENT_NOTIFY_BYTES   64

#define BLE_CLIENT_ENOTREADY  (-1)   // stack off or not synced
#define BLE_CLIENT_ENOCONN    (-2)   // no such connection
#define BLE_CLIENT_ETIMEOUT   (-3)
#define BLE_CLIENT_EFULL      (-4)   // no free connection slot
#define BLE_CLIENT_ENOTFOUND  (-5)   // handle/UUID not in the GATT database
#define BLE_CLIENT_EINVAL     (-6)
#define BLE_CLIENT_ENOMEM     (-7)

typedef struct {
    uint8_t type;      // 16, 32 or 128
    uint8_t value[16]; // little-endian, as on air
} ble_client_uuid_t;

typedef struct {
    uint8_t addr[6];         // little-endian, as NimBLE keeps it
    uint8_t addr_type;       // BLE_ADDR_PUBLIC (0), RANDOM (1), ...
    int8_t rssi;
    bool connectable;
    char name[32];
    ble_client_uuid_t uuids[4];
    uint8_t num_uuids;
    uint8_t mfg[24];
    uint8_t mfg_len;
    int8_t tx_power;
    bool has_tx_power;
} ble_client_dev_t;

// Scan for duration_ms (100..30000). With a non-empty name filter, only
// devices whose name contains it (case-insensitive) are kept. Fills up to max
// devices (strongest kept when full); *found gets how many (at most max).
// *reports gets the number of advertising reports, duplicates included.
int ble_client_scan(uint32_t duration_ms, bool active, const char *name_filter,
                    ble_client_dev_t *out, int max, int *found, uint32_t *reports);

typedef struct {
    uint16_t conn;
    uint8_t addr[6];
    uint8_t addr_type;
    uint16_t mtu;
    bool central;   // we connected out (false: a phone connected to us)
    bool encrypted;
} ble_client_conn_info_t;

// Connect to a peer (or return the existing connection to it), exchange MTU.
int ble_client_connect(const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms,
                       ble_client_conn_info_t *out);
int ble_client_disconnect(uint16_t conn);
// Connections this module made. Returns how many.
int ble_client_connections(ble_client_conn_info_t *out, int max);
// Start pairing (encryption, bonding as configured) and wait for it.
int ble_client_secure(uint16_t conn, uint32_t timeout_ms);

typedef struct {
    ble_client_uuid_t uuid;
    uint16_t def_handle;
    uint16_t val_handle;
    uint16_t cccd_handle;    // 0 when the characteristic has none
    uint8_t properties;      // BLE_GATT_CHR_PROP_*
    uint8_t svc;             // index into services
} ble_client_chr_t;

typedef struct {
    ble_client_uuid_t uuid;
    uint16_t start, end;
} ble_client_svc_t;

typedef struct {
    ble_client_svc_t svcs[BLE_CLIENT_MAX_SVCS];
    int num_svcs;
    ble_client_chr_t chrs[BLE_CLIENT_MAX_CHRS];
    int num_chrs;
    bool truncated;
} ble_client_db_t;

// Discover (once per connection; later calls return the cache).
int ble_client_discover(uint16_t conn, const ble_client_db_t **db);
// Find a characteristic by value handle, or by UUID (optionally within a
// service UUID). Discovers first if needed.
int ble_client_find_chr(uint16_t conn, uint16_t handle, const ble_client_uuid_t *uuid,
                        const ble_client_uuid_t *svc_uuid, ble_client_chr_t *out);

int ble_client_read(uint16_t conn, uint16_t handle, uint8_t *buf, size_t cap,
                    size_t *len);
int ble_client_write(uint16_t conn, uint16_t handle, const uint8_t *data, size_t len,
                     bool response);

typedef struct {
    uint32_t ms;     // since the subscription started
    uint16_t handle;
    uint16_t len;    // full length (data holds at most BLE_CLIENT_NOTIFY_BYTES)
    uint8_t data[BLE_CLIENT_NOTIFY_BYTES];
} ble_client_notification_t;

// Subscribe (notify, or indicate if that's all it has), collect what arrives
// for duration_ms, then unsubscribe unless keep. With keep, values that
// arrive afterwards are buffered and come first (ms 0) from the next call on
// the same characteristic, so a write's reply isn't lost between calls.
// *count gets how many were stored (at most max); *dropped counts the rest.
int ble_client_subscribe(uint16_t conn, const ble_client_chr_t *chr, uint32_t duration_ms,
                         bool keep, ble_client_notification_t *out, int max,
                         int *count, uint32_t *dropped);

// The stack is going away: forget every connection and cached database.
void ble_client_reset(void);

#ifdef __cplusplus
}
#endif
