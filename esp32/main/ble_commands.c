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

// See ble_commands.h. Parameters arrive as Muse wrote them, so numbers and
// booleans are also taken as strings ("5000", "true").

#include "sdkconfig.h"

#if CONFIG_HOMEHUB_BLE_COMMANDS

#include "ble_commands.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_client.h"

#ifndef BLE_COMMANDS_HOST_TEST
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "noise_control.h"
#define BLE_CMD_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define BLE_CMD_ALLOC(n) calloc(1, (n))
#endif

#define SCAN_DEFAULT_MS   5000
#define SCAN_MAX_MS       30000
#define SCAN_MAX_DEVICES  40
#define SUB_DEFAULT_MS    5000
#define SUB_MAX_MS        60000

static ble_commands_ops_t s_ops;

// The last scan's addresses and types, so ble.connect can take an address
// without its type.
static struct {
    uint8_t addr[6];
    uint8_t type;
} s_seen[SCAN_MAX_DEVICES];
static int s_num_seen;

void ble_commands_init(const ble_commands_ops_t *ops) {
    if (ops) s_ops = *ops;
}

// ---- results ------------------------------------------------------------------

static cJSON *error_result(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

static cJSON *ok_result(cJSON *payload) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload ? payload : cJSON_CreateObject());
    return result;
}

static cJSON *client_error(int rc, const char *what) {
    char msg[96];
    const char *code = "ble_error";
    switch (rc) {
    case BLE_CLIENT_ENOTREADY:
        return error_result("ble_off", "Bluetooth is off; turn it on with ble.power");
    case BLE_CLIENT_ENOCONN:
        return error_result("not_connected", "no such connection; connect with ble.connect");
    case BLE_CLIENT_ETIMEOUT:
        snprintf(msg, sizeof(msg), "%s timed out", what);
        return error_result("timeout", msg);
    case BLE_CLIENT_EFULL:
        return error_result("too_many_connections",
                            "all " "3" " connections are in use; disconnect one first");
    case BLE_CLIENT_ENOTFOUND:
        return error_result("not_found", "no such characteristic; list them with ble.discover");
    case BLE_CLIENT_EINVAL:
        snprintf(msg, sizeof(msg), "%s: not supported by that characteristic", what);
        return error_result("invalid_params", msg);
    case BLE_CLIENT_ENOMEM:
        return error_result("out_of_memory", "out of memory");
    default:
        break;
    }
    if (rc >= 0x100 && rc < 0x200) {
        // ATT errors from the peer.
        int att = rc - 0x100;
        const char *name = att == 0x01 ? "invalid handle"
                         : att == 0x02 ? "read not permitted"
                         : att == 0x03 ? "write not permitted"
                         : att == 0x05 ? "insufficient authentication"
                         : att == 0x06 ? "request not supported"
                         : att == 0x0d ? "invalid attribute value length"
                         : att == 0x0f ? "insufficient encryption"
                         : "error";
        snprintf(msg, sizeof(msg), "%s failed: the device answered %s (ATT 0x%02x)", what, name, att);
        code = "device_refused";
    } else if (rc >= 0x200 && rc < 0x300) {
        snprintf(msg, sizeof(msg), "%s failed: link error 0x%02x", what, rc - 0x200);
        code = rc == 0x23e ? "connect_failed" : "link_error";
    } else if (rc == 7 /* BLE_HS_ENOTCONN */) {
        snprintf(msg, sizeof(msg), "%s failed: the device disconnected", what);
        code = "not_connected";
    } else {
        snprintf(msg, sizeof(msg), "%s failed (NimBLE %d)", what, rc);
    }
    return error_result(code, msg);
}

// ---- parameters ---------------------------------------------------------------

static const cJSON *param(const cJSON *params, const char *key) {
    return params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
}

// 1 true, 0 false, -1 absent, -2 malformed.
static int get_bool(const cJSON *params, const char *key) {
    const cJSON *v = param(params, key);
    if (!v || cJSON_IsNull(v)) return -1;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v) && v->valuestring) {
        const char *s = v->valuestring;
        if (!strcasecmp(s, "true") || !strcasecmp(s, "on") || !strcmp(s, "1") || !strcasecmp(s, "yes")) return 1;
        if (!strcasecmp(s, "false") || !strcasecmp(s, "off") || !strcmp(s, "0") || !strcasecmp(s, "no")) return 0;
    }
    return -2;
}

// true with *out set (or left at its default when absent); false if
// malformed or out of range.
static bool get_int(const cJSON *params, const char *key, long min, long max, long *out) {
    const cJSON *v = param(params, key);
    if (!v || cJSON_IsNull(v)) return true;
    long n;
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble != (double)(long)v->valuedouble) return false;
        n = (long)v->valuedouble;
    } else if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        char *end;
        n = strtol(v->valuestring, &end, 0);
        if (*end) return false;
    } else {
        return false;
    }
    if (n < min || n > max) return false;
    *out = n;
    return true;
}

static const char *get_str(const cJSON *params, const char *key) {
    const cJSON *v = param(params, key);
    return (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) ? v->valuestring : NULL;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// "AA:BB:CC:DD:EE:FF", "aa-bb-...", or 12 hex digits, most significant first.
static bool parse_addr(const char *s, uint8_t out[6]) {
    uint8_t be[6];
    int n = 0;
    for (const char *p = s; *p && n < 12; p++) {
        if (*p == ':' || *p == '-') continue;
        int h = hexval(*p);
        if (h < 0) return false;
        if (n % 2 == 0) be[n / 2] = (uint8_t)(h << 4);
        else be[n / 2] |= (uint8_t)h;
        n++;
    }
    if (n != 12) return false;
    for (int i = 0; i < 6; i++) out[i] = be[5 - i];
    return true;
}

static void format_addr(const uint8_t a[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static const char *addr_type_name(uint8_t t) {
    return t == 0 ? "public" : t == 1 ? "random" : t == 2 ? "public_id" : "random_id";
}

static int parse_addr_type(const char *s) {
    if (!strcasecmp(s, "public")) return 0;
    if (!strcasecmp(s, "random")) return 1;
    if (!strcasecmp(s, "public_id")) return 2;
    if (!strcasecmp(s, "random_id")) return 3;
    return -1;
}

// "180f", "0x180F", 8 hex digits, or a 128-bit UUID with or without dashes.
static bool parse_uuid(const char *s, ble_client_uuid_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint8_t be[16];
    int n = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '-') continue;
        int h = hexval(*p);
        if (h < 0 || n >= 32) return false;
        if (n % 2 == 0) be[n / 2] = (uint8_t)(h << 4);
        else be[n / 2] |= (uint8_t)h;
        n++;
    }
    if (n != 4 && n != 8 && n != 32) return false;
    memset(out, 0, sizeof(*out));
    out->type = (uint8_t)(n * 4);
    for (int i = 0; i < n / 2; i++) out->value[i] = be[n / 2 - 1 - i];
    return true;
}

static void format_uuid(const ble_client_uuid_t *u, char out[37]) {
    const uint8_t *v = u->value;
    if (u->type == 16) {
        snprintf(out, 37, "%02x%02x", v[1], v[0]);
    } else if (u->type == 32) {
        snprintf(out, 37, "%02x%02x%02x%02x", v[3], v[2], v[1], v[0]);
    } else {
        snprintf(out, 37,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 v[15], v[14], v[13], v[12], v[11], v[10], v[9], v[8], v[7], v[6],
                 v[5], v[4], v[3], v[2], v[1], v[0]);
    }
}

static void add_hex(cJSON *obj, const char *key, const uint8_t *data, size_t len) {
    char *hex = malloc(len * 2 + 1);
    if (!hex) return;
    for (size_t i = 0; i < len; i++) snprintf(hex + 2 * i, 3, "%02x", data[i]);
    hex[len * 2] = '\0';
    cJSON_AddStringToObject(obj, key, hex);
    free(hex);
}

// Printable UTF-8 (no control characters but tab/newline) as text.
static void add_text_if_printable(cJSON *obj, const uint8_t *data, size_t len) {
    // One byte is a number far more often than a character (a battery
    // level of 0x64 isn't "d").
    if (len < 2) return;
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i];
        if (c == 0 && i + 1 == len) {          // a trailing NUL is fine,
            if (i == 0) return;                  // but not as the whole value
            break;
        }
        if (c < 0x80) {
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return;
            i++;
            continue;
        }
        int extra = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (extra < 0 || i + (size_t)extra >= len) return;
        for (int k = 1; k <= extra; k++) {
            if ((data[i + k] & 0xc0) != 0x80) return;
        }
        i += (size_t)extra + 1;
    }
    char *text = malloc(len + 1);
    if (!text) return;
    memcpy(text, data, len);
    text[len] = '\0';
    cJSON_AddStringToObject(obj, "text", text);
    free(text);
}

static size_t parse_hex_bytes(const char *s, uint8_t *out, size_t cap, bool *ok) {
    size_t n = 0;
    int half = -1;
    *ok = false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (const char *p = s; *p; p++) {
        if (*p == ' ' || *p == ':' || *p == '-' || *p == ',') continue;
        int h = hexval(*p);
        if (h < 0) return 0;
        if (half < 0) {
            half = h;
        } else {
            if (n >= cap) return 0;
            out[n++] = (uint8_t)(half << 4 | h);
            half = -1;
        }
    }
    if (half >= 0) return 0;
    *ok = true;
    return n;
}

static void add_properties(cJSON *obj, uint8_t p) {
    static const char *names[8] = {"broadcast", "read", "write_no_response", "write",
                                   "notify", "indicate", "signed_write", "extended"};
    cJSON *arr = cJSON_AddArrayToObject(obj, "properties");
    for (int i = 0; i < 8; i++) {
        if (p & (1u << i)) cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    }
}

static bool get_conn(const cJSON *params, uint16_t *conn) {
    long c = -1;
    if (!get_int(params, "conn", 0, 0xffff, &c) || c < 0) return false;
    *conn = (uint16_t)c;
    return true;
}

// The characteristic a read, write or subscribe names: handle, or uuid
// (optionally within service).
static cJSON *resolve_chr(const cJSON *params, uint16_t conn, ble_client_chr_t *chr) {
    long handle = 0;
    if (!get_int(params, "handle", 1, 0xffff, &handle)) {
        return error_result("invalid_params", "handle must be 1-65535");
    }
    ble_client_uuid_t uuid, svc;
    bool have_uuid = false, have_svc = false;
    const char *u = get_str(params, "uuid");
    if (u) {
        if (!parse_uuid(u, &uuid)) return error_result("invalid_params", "uuid is not a BLE UUID");
        have_uuid = true;
    }
    const char *sv = get_str(params, "service");
    if (sv) {
        if (!parse_uuid(sv, &svc)) return error_result("invalid_params", "service is not a BLE UUID");
        have_svc = true;
    }
    if (!handle && !have_uuid) return error_result("missing_param", "handle or uuid is required");
    int rc = ble_client_find_chr(conn, (uint16_t)handle, have_uuid ? &uuid : NULL,
                                 have_svc ? &svc : NULL, chr);
    if (rc == BLE_CLIENT_ENOTFOUND && handle) {
        // Not in the discovered database (or discovery failed): use the
        // handle as given.
        memset(chr, 0, sizeof(*chr));
        chr->val_handle = (uint16_t)handle;
        chr->properties = 0xff;
        return NULL;
    }
    return rc == 0 ? NULL : client_error(rc, "discovery");
}

// ---- commands -----------------------------------------------------------------

static cJSON *cmd_state(void) {
    cJSON *p = cJSON_CreateObject();
    bool on = s_ops.is_on && s_ops.is_on();
    cJSON_AddBoolToObject(p, "enabled", on);
    cJSON_AddBoolToObject(p, "advertising", on && s_ops.is_advertising && s_ops.is_advertising());
    if (s_ops.name) cJSON_AddStringToObject(p, "name", s_ops.name());
    uint8_t a[6];
    char as[18];
    if (on && s_ops.own_address && s_ops.own_address(a)) {
        format_addr(a, as);
        cJSON_AddStringToObject(p, "address", as);
    }
    if (s_ops.adv_interval_ms) cJSON_AddNumberToObject(p, "advertise_interval_ms", s_ops.adv_interval_ms());
    cJSON_AddBoolToObject(p, "wifi_first", s_ops.wifi_first && s_ops.wifi_first());
    cJSON *conns = cJSON_AddArrayToObject(p, "connections");
    ble_client_conn_info_t info[BLE_CLIENT_MAX_CONNS];
    int n = on ? ble_client_connections(info, BLE_CLIENT_MAX_CONNS) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "conn", info[i].conn);
        format_addr(info[i].addr, as);
        cJSON_AddStringToObject(c, "address", as);
        cJSON_AddStringToObject(c, "address_type", addr_type_name(info[i].addr_type));
        cJSON_AddNumberToObject(c, "mtu", info[i].mtu);
        cJSON_AddBoolToObject(c, "encrypted", info[i].encrypted);
        cJSON_AddItemToArray(conns, c);
    }
    cJSON_AddBoolToObject(p, "phone_connected", on && s_ops.peripheral_connected && s_ops.peripheral_connected());
    return ok_result(p);
}

static cJSON *cmd_power(const cJSON *params) {
    int on = get_bool(params, "enabled");
    if (on < 0) return error_result(on == -1 ? "missing_param" : "invalid_params",
                                    "enabled (true or false) is required");
    int wifi_first = get_bool(params, "wifi_first");
    if (wifi_first == -2) return error_result("invalid_params", "wifi_first must be true or false");
    if (!s_ops.power) return error_result("unsupported", "BLE power control is unavailable");
    if (!on) ble_client_reset();
    if (!s_ops.power(on != 0)) {
        return error_result(on ? "ble_start_failed" : "ble_stop_failed",
                            on ? "Bluetooth didn't start (not enough free memory?)"
                               : "Bluetooth didn't stop");
    }
    if (wifi_first >= 0 && s_ops.set_wifi_first) s_ops.set_wifi_first(wifi_first != 0);
    return cmd_state();
}

static cJSON *cmd_advertise(const cJSON *params) {
    int on = get_bool(params, "enabled");
    if (on < 0) return error_result(on == -1 ? "missing_param" : "invalid_params",
                                    "enabled (true or false) is required");
    long interval = 0;
    if (!get_int(params, "interval_ms", 20, 10240, &interval)) {
        return error_result("invalid_params", "interval_ms must be 20-10240");
    }
    if (!s_ops.is_on || !s_ops.is_on()) return client_error(BLE_CLIENT_ENOTREADY, "advertise");
    if (s_ops.advertise) s_ops.advertise(on != 0, (int)interval);
    return cmd_state();
}

static int by_rssi(const void *a, const void *b) {
    return ((const ble_client_dev_t *)b)->rssi - ((const ble_client_dev_t *)a)->rssi;
}

static cJSON *cmd_scan(const cJSON *params) {
    long ms = SCAN_DEFAULT_MS, limit = 20;
    if (!get_int(params, "duration_ms", 100, SCAN_MAX_MS, &ms)) {
        return error_result("invalid_params", "duration_ms must be 100-30000");
    }
    if (!get_int(params, "limit", 1, SCAN_MAX_DEVICES, &limit)) {
        return error_result("invalid_params", "limit must be 1-40");
    }
    int active = get_bool(params, "active");
    if (active == -2) return error_result("invalid_params", "active must be true or false");
    const char *name = get_str(params, "name");
    ble_client_dev_t *devs = BLE_CMD_ALLOC(sizeof(ble_client_dev_t) * SCAN_MAX_DEVICES);
    if (!devs) return error_result("out_of_memory", "out of memory");
    uint32_t reports = 0;
    int n = 0;
    int rc = ble_client_scan((uint32_t)ms, active != 0, name, devs, SCAN_MAX_DEVICES, &n, &reports);
    if (rc != 0) {
        free(devs);
        return client_error(rc, "scan");
    }
    if (n < 0) n = 0;
    if (n > SCAN_MAX_DEVICES) n = SCAN_MAX_DEVICES;
    qsort(devs, (size_t)n, sizeof(*devs), by_rssi);
    s_num_seen = 0;
    for (int i = 0; i < n && s_num_seen < SCAN_MAX_DEVICES; i++) {
        memcpy(s_seen[s_num_seen].addr, devs[i].addr, 6);
        s_seen[s_num_seen++].type = devs[i].addr_type;
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "found", n);
    cJSON_AddNumberToObject(p, "reports", reports);
    cJSON *arr = cJSON_AddArrayToObject(p, "devices");
    for (int i = 0; i < n && i < limit; i++) {
        const ble_client_dev_t *d = &devs[i];
        cJSON *o = cJSON_CreateObject();
        char s[37];
        format_addr(d->addr, s);
        cJSON_AddStringToObject(o, "address", s);
        cJSON_AddStringToObject(o, "address_type", addr_type_name(d->addr_type));
        cJSON_AddNumberToObject(o, "rssi", d->rssi);
        if (d->name[0]) cJSON_AddStringToObject(o, "name", d->name);
        cJSON_AddBoolToObject(o, "connectable", d->connectable);
        if (d->num_uuids) {
            cJSON *u = cJSON_AddArrayToObject(o, "services");
            for (int k = 0; k < d->num_uuids; k++) {
                format_uuid(&d->uuids[k], s);
                cJSON_AddItemToArray(u, cJSON_CreateString(s));
            }
        }
        if (d->mfg_len) add_hex(o, "manufacturer_data", d->mfg, d->mfg_len);
        if (d->has_tx_power) cJSON_AddNumberToObject(o, "tx_power", d->tx_power);
        cJSON_AddItemToArray(arr, o);
    }
    free(devs);
    return ok_result(p);
}

static cJSON *cmd_connect(const cJSON *params) {
    const char *as = get_str(params, "address");
    uint8_t addr[6];
    if (!as) return error_result("missing_param", "address is required");
    if (!parse_addr(as, addr)) return error_result("invalid_params", "address must look like AA:BB:CC:DD:EE:FF");
    int type = -1;
    const char *ts = get_str(params, "address_type");
    if (ts) {
        type = parse_addr_type(ts);
        if (type < 0) return error_result("invalid_params", "address_type must be public or random");
    } else {
        for (int i = 0; i < s_num_seen; i++) {
            if (memcmp(s_seen[i].addr, addr, 6) == 0) type = s_seen[i].type;
        }
        if (type < 0) type = 0;
    }
    long timeout = 10000;
    if (!get_int(params, "timeout_ms", 1000, 30000, &timeout)) {
        return error_result("invalid_params", "timeout_ms must be 1000-30000");
    }
    int secure = get_bool(params, "secure");
    if (secure == -2) return error_result("invalid_params", "secure must be true or false");
    ble_client_conn_info_t info;
    int rc = ble_client_connect(addr, (uint8_t)type, (uint32_t)timeout, &info);
    if (rc != 0) return client_error(rc, "connect");
    if (secure == 1) {
        rc = ble_client_secure(info.conn, 30000);
        if (rc != 0) return client_error(rc, "pairing");
        info.encrypted = true;
    }
    cJSON *p = cJSON_CreateObject();
    char s[18];
    cJSON_AddNumberToObject(p, "conn", info.conn);
    format_addr(info.addr, s);
    cJSON_AddStringToObject(p, "address", s);
    cJSON_AddNumberToObject(p, "mtu", info.mtu);
    cJSON_AddBoolToObject(p, "encrypted", info.encrypted);
    return ok_result(p);
}

static cJSON *cmd_disconnect(const cJSON *params) {
    uint16_t conn;
    if (!get_conn(params, &conn)) return error_result("missing_param", "conn (from ble.connect) is required");
    int rc = ble_client_disconnect(conn);
    if (rc != 0) return client_error(rc, "disconnect");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "conn", conn);
    cJSON_AddBoolToObject(p, "disconnected", true);
    return ok_result(p);
}

static cJSON *cmd_discover(const cJSON *params) {
    uint16_t conn;
    if (!get_conn(params, &conn)) return error_result("missing_param", "conn (from ble.connect) is required");
    const ble_client_db_t *db;
    int rc = ble_client_discover(conn, &db);
    if (rc != 0) return client_error(rc, "discovery");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "conn", conn);
    cJSON *svcs = cJSON_AddArrayToObject(p, "services");
    char s[37];
    for (int i = 0; i < db->num_svcs; i++) {
        cJSON *so = cJSON_CreateObject();
        format_uuid(&db->svcs[i].uuid, s);
        cJSON_AddStringToObject(so, "uuid", s);
        cJSON *chrs = cJSON_AddArrayToObject(so, "characteristics");
        for (int k = 0; k < db->num_chrs; k++) {
            const ble_client_chr_t *c = &db->chrs[k];
            if (c->svc != i) continue;
            cJSON *co = cJSON_CreateObject();
            format_uuid(&c->uuid, s);
            cJSON_AddStringToObject(co, "uuid", s);
            cJSON_AddNumberToObject(co, "handle", c->val_handle);
            add_properties(co, c->properties);
            cJSON_AddItemToArray(chrs, co);
        }
        cJSON_AddItemToArray(svcs, so);
    }
    if (db->truncated) cJSON_AddBoolToObject(p, "truncated", true);
    return ok_result(p);
}

static cJSON *cmd_read(const cJSON *params) {
    uint16_t conn;
    if (!get_conn(params, &conn)) return error_result("missing_param", "conn (from ble.connect) is required");
    ble_client_chr_t chr;
    cJSON *err = resolve_chr(params, conn, &chr);
    if (err) return err;
    if (chr.properties != 0xff && !(chr.properties & 0x02)) {
        return error_result("invalid_params", "that characteristic can't be read");
    }
    uint8_t *buf = malloc(BLE_CLIENT_MAX_VALUE);
    if (!buf) return error_result("out_of_memory", "out of memory");
    size_t len = 0;
    int rc = ble_client_read(conn, chr.val_handle, buf, BLE_CLIENT_MAX_VALUE, &len);
    if (rc != 0) {
        free(buf);
        return client_error(rc, "read");
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "conn", conn);
    cJSON_AddNumberToObject(p, "handle", chr.val_handle);
    cJSON_AddNumberToObject(p, "length", (double)len);
    add_hex(p, "hex", buf, len);
    add_text_if_printable(p, buf, len);
    free(buf);
    return ok_result(p);
}

static cJSON *cmd_write(const cJSON *params) {
    uint16_t conn;
    if (!get_conn(params, &conn)) return error_result("missing_param", "conn (from ble.connect) is required");
    const char *hex = get_str(params, "hex");
    const char *text = get_str(params, "text");
    if (!!hex == !!text) return error_result("missing_param", "give either hex or text");
    int response = get_bool(params, "response");
    if (response == -2) return error_result("invalid_params", "response must be true or false");
    uint8_t *data = malloc(BLE_CLIENT_MAX_VALUE);
    if (!data) return error_result("out_of_memory", "out of memory");
    size_t len;
    if (hex) {
        bool ok;
        len = parse_hex_bytes(hex, data, BLE_CLIENT_MAX_VALUE, &ok);
        if (!ok || len == 0) {
            free(data);
            return error_result("invalid_params", "hex must be 1-512 bytes of hex digits");
        }
    } else {
        len = strlen(text);
        if (len > BLE_CLIENT_MAX_VALUE) {
            free(data);
            return error_result("invalid_params", "text is longer than 512 bytes");
        }
        memcpy(data, text, len);
    }
    ble_client_chr_t chr;
    cJSON *err = resolve_chr(params, conn, &chr);
    if (err) {
        free(data);
        return err;
    }
    // Default to the kind of write the characteristic supports.
    if (response < 0) {
        response = (chr.properties != 0xff && !(chr.properties & 0x08) && (chr.properties & 0x04)) ? 0 : 1;
    }
    int rc = ble_client_write(conn, chr.val_handle, data, len, response != 0);
    free(data);
    if (rc != 0) return client_error(rc, "write");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "conn", conn);
    cJSON_AddNumberToObject(p, "handle", chr.val_handle);
    cJSON_AddNumberToObject(p, "written", (double)len);
    cJSON_AddBoolToObject(p, "with_response", response != 0);
    return ok_result(p);
}

static cJSON *cmd_subscribe(const cJSON *params) {
    uint16_t conn;
    if (!get_conn(params, &conn)) return error_result("missing_param", "conn (from ble.connect) is required");
    long ms = SUB_DEFAULT_MS;
    if (!get_int(params, "duration_ms", 100, SUB_MAX_MS, &ms)) {
        return error_result("invalid_params", "duration_ms must be 100-60000");
    }
    int keep = get_bool(params, "keep");
    if (keep == -2) return error_result("invalid_params", "keep must be true or false");
    ble_client_chr_t chr;
    cJSON *err = resolve_chr(params, conn, &chr);
    if (err) return err;
    if (chr.properties == 0xff) chr.properties = 0x10;   // unknown: try notify
    if (!(chr.properties & 0x30)) {
        return error_result("invalid_params", "that characteristic doesn't notify or indicate");
    }
    ble_client_notification_t *n = BLE_CMD_ALLOC(sizeof(*n) * BLE_CLIENT_MAX_NOTIFY);
    if (!n) return error_result("out_of_memory", "out of memory");
    uint32_t dropped = 0;
    int count = 0;
    int rc = ble_client_subscribe(conn, &chr, (uint32_t)ms, keep == 1, n, BLE_CLIENT_MAX_NOTIFY,
                                  &count, &dropped);
    if (rc != 0) {
        free(n);
        return client_error(rc, "subscribe");
    }
    if (count < 0) count = 0;
    if (count > BLE_CLIENT_MAX_NOTIFY) count = BLE_CLIENT_MAX_NOTIFY;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "conn", conn);
    cJSON_AddNumberToObject(p, "handle", chr.val_handle);
    cJSON_AddNumberToObject(p, "count", count);
    if (dropped) cJSON_AddNumberToObject(p, "dropped", dropped);
    cJSON *arr = cJSON_AddArrayToObject(p, "values");
    for (int i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ms", n[i].ms);
        size_t len = n[i].len < BLE_CLIENT_NOTIFY_BYTES ? n[i].len : BLE_CLIENT_NOTIFY_BYTES;
        add_hex(o, "hex", n[i].data, len);
        if (n[i].len > len) cJSON_AddNumberToObject(o, "length", n[i].len);
        cJSON_AddItemToArray(arr, o);
    }
    free(n);
    return ok_result(p);
}

// ---- table, registration, dispatch --------------------------------------------

typedef struct {
    const char *name;
    int timeout_ms;   // 0: Muse's default (30 s)
} cmd_info_t;

static const cmd_info_t k_cmds[] = {
    {"ble.state", 0},     {"ble.power", 0},      {"ble.advertise", 0},
    {"ble.scan", 40000},  {"ble.connect", 45000}, {"ble.disconnect", 0},
    {"ble.discover", 45000}, {"ble.read", 45000}, {"ble.write", 45000},
    {"ble.subscribe", 75000},
};

bool ble_commands_handles(const char *command) {
    if (!command || strncmp(command, "ble.", 4) != 0) return false;
    for (size_t i = 0; i < sizeof(k_cmds) / sizeof(k_cmds[0]); i++) {
        if (!strcmp(command, k_cmds[i].name)) return true;
    }
    return false;
}

cJSON *ble_commands_run(const char *command, const cJSON *params) {
    if (!strcmp(command, "ble.state")) return cmd_state();
    if (!strcmp(command, "ble.power")) return cmd_power(params);
    if (!strcmp(command, "ble.advertise")) return cmd_advertise(params);
    if (!strcmp(command, "ble.scan")) return cmd_scan(params);
    if (!strcmp(command, "ble.connect")) return cmd_connect(params);
    if (!strcmp(command, "ble.disconnect")) return cmd_disconnect(params);
    if (!strcmp(command, "ble.discover")) return cmd_discover(params);
    if (!strcmp(command, "ble.read")) return cmd_read(params);
    if (!strcmp(command, "ble.write")) return cmd_write(params);
    if (!strcmp(command, "ble.subscribe")) return cmd_subscribe(params);
    return error_result("unsupported", "unknown BLE command");
}

static cJSON *typed(const char *type, const char *description) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
    return p;
}

static void add(cJSON *commands, const char *name, const char *description,
                cJSON *required, cJSON *optional) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "description", description);
    cJSON_AddItemToObject(c, "required", required ? required : cJSON_CreateObject());
    cJSON_AddItemToObject(c, "optional", optional ? optional : cJSON_CreateObject());
    for (size_t i = 0; i < sizeof(k_cmds) / sizeof(k_cmds[0]); i++) {
        if (!strcmp(k_cmds[i].name, name) && k_cmds[i].timeout_ms) {
            cJSON_AddNumberToObject(c, "timeout_ms", k_cmds[i].timeout_ms);
        }
    }
    cJSON_AddItemToObject(commands, name, c);
}

static cJSON *chr_params(cJSON *optional) {
    cJSON_AddItemToObject(optional, "handle", typed("integer", "Value handle from ble.discover."));
    cJSON_AddItemToObject(optional, "uuid", typed("string", "Or the characteristic UUID, e.g. 2a19."));
    cJSON_AddItemToObject(optional, "service", typed("string", "Service UUID, if several share uuid."));
    return optional;
}

static cJSON *conn_required(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "conn", typed("integer", "From ble.connect."));
    return r;
}

void ble_commands_register(cJSON *commands) {
    add(commands, "ble.state",
        "The Link's Bluetooth LE state: on/off, advertising (name, address, interval), connections, "
        "and whether Wi-Fi has priority on the shared radio.",
        NULL, NULL);

    cJSON *r = cJSON_CreateObject(), *o = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "enabled", typed("boolean", "On (true) or off."));
    cJSON_AddItemToObject(o, "wifi_first", typed("boolean",
        "Default true: BLE takes radio time only as needed, keeping Wi-Fi fast. false: even "
        "split, for a sustained BLE stream."));
    add(commands, "ble.power",
        "Turn the Link's Bluetooth LE on or off (off frees its memory and drops connections; Wi-Fi "
        "is unaffected). Returns the state.", r, o);

    r = cJSON_CreateObject();
    o = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "enabled", typed("boolean", "Start (true) or stop."));
    cJSON_AddItemToObject(o, "interval_ms", typed("integer",
        "20-10240 (default 1000); shorter is found sooner but takes more airtime."));
    add(commands, "ble.advertise",
        "Start or stop the Link's own BLE advertising, by which phones find and connect to it.",
        r, o);

    o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "duration_ms", typed("integer", "100-30000 (default 5000)."));
    cJSON_AddItemToObject(o, "name", typed("string", "Only names containing this."));
    cJSON_AddItemToObject(o, "active", typed("boolean", "Request scan responses (default true)."));
    cJSON_AddItemToObject(o, "limit", typed("integer", "1-40 (default 20)."));
    add(commands, "ble.scan",
        "Scan for BLE devices. Returns them strongest first: address, address_type, rssi, name, "
        "connectable, service UUIDs, manufacturer_data (hex, little-endian company ID first), tx_power.", NULL, o);

    r = cJSON_CreateObject();
    o = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "address", typed("string", "AA:BB:CC:DD:EE:FF, from ble.scan."));
    cJSON_AddItemToObject(o, "address_type", typed("string", "public or random, from ble.scan."));
    cJSON_AddItemToObject(o, "timeout_ms", typed("integer", "1000-30000 (default 10000)."));
    cJSON_AddItemToObject(o, "secure", typed("boolean", "Pair now; otherwise pairing happens when needed."));
    add(commands, "ble.connect",
        "Connect to a BLE device and keep it connected (up to 3). Returns conn for the other "
        "ble.* commands, and the MTU.", r, o);

    add(commands, "ble.disconnect", "Close a BLE connection opened by ble.connect.", conn_required(), NULL);

    add(commands, "ble.discover",
        "List a connected device's services and characteristics: UUID, value handle, properties. "
        "Standard UUIDs are 4 hex digits (180f battery service, 2a19 battery level).", conn_required(), NULL);

    add(commands, "ble.read",
        "Read a characteristic by handle or uuid. Returns hex, and text when printable.", conn_required(), chr_params(cJSON_CreateObject()));

    o = chr_params(cJSON_CreateObject());
    cJSON_AddItemToObject(o, "hex", typed("string", "Bytes as hex."));
    cJSON_AddItemToObject(o, "text", typed("string", "Or UTF-8 text."));
    cJSON_AddItemToObject(o, "response", typed("boolean", "false: without response. Default: as supported."));
    add(commands, "ble.write",
        "Write a characteristic by handle or uuid. Write only what the user asked for.", conn_required(), o);

    o = chr_params(cJSON_CreateObject());
    cJSON_AddItemToObject(o, "duration_ms", typed("integer", "100-60000 (default 5000)."));
    cJSON_AddItemToObject(o, "keep", typed("boolean", "Stay subscribed; later values wait for the next call (default false)."));
    add(commands, "ble.subscribe",
        "Collect a characteristic's notifications or indications for duration_ms. Returns each as "
        "hex with ms since the start.",
        conn_required(), o);
}

// ---- worker -------------------------------------------------------------------

void ble_commands_cleanup(void) {
    ble_client_conn_info_t info[BLE_CLIENT_MAX_CONNS];
    int n = ble_client_connections(info, BLE_CLIENT_MAX_CONNS);
    for (int i = 0; i < n; i++) {
        if (info[i].central) (void)ble_client_disconnect(info[i].conn);
    }
    if (s_ops.set_wifi_first && s_ops.wifi_first && !s_ops.wifi_first()) {
        s_ops.set_wifi_first(true);
    }
}

#ifndef BLE_COMMANDS_HOST_TEST

typedef struct {
    char command[24];
    char request_id[64];
    uint64_t session_generation;
    cJSON *params;
} job_t;

static QueueHandle_t s_jobs;

// A NULL job is ble_commands_session_ended()'s cleanup, in queue order.
static void worker(void *arg) {
    (void)arg;
    job_t *job;
    for (;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        if (!job) {
            ble_commands_cleanup();
            continue;
        }
        cJSON *result = ble_commands_run(job->command, job->params);
        noise_ctrl_send_command_result(job->session_generation, job->request_id, result);
        cJSON_Delete(job->params);
        free(job);
    }
}

void ble_commands_session_ended(void) {
    if (!s_jobs) return;   // no command has run: nothing to clean up
    job_t *cleanup = NULL;
    // The queue holds at most 4 commands; wait briefly rather than skip.
    if (xQueueSend(s_jobs, &cleanup, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW("link.ble_cmd", "session ended: BLE cleanup not queued");
    }
}

cJSON *ble_commands_handle(const char *command, const cJSON *params,
                           const char *request_id, uint64_t session_generation) {
    if (!ble_commands_handles(command)) return error_result("unsupported", "unknown BLE command");
    if (!request_id || strlen(request_id) >= sizeof(((job_t *)0)->request_id)) {
        return error_result("invalid_params", "request_id is invalid");
    }
    if (!s_jobs) {
        s_jobs = xQueueCreate(4, sizeof(job_t *));
        // NimBLE calls and cJSON results need some stack; 6 KB in internal RAM.
        if (!s_jobs || xTaskCreate(worker, "ble_cmd", 6144, NULL, 4, NULL) != pdPASS) {
            if (s_jobs) vQueueDelete(s_jobs);
            s_jobs = NULL;
            return error_result("out_of_memory", "failed to start the BLE command task");
        }
    }
    job_t *job = calloc(1, sizeof(*job));
    if (!job) return error_result("out_of_memory", "out of memory");
    strncpy(job->command, command, sizeof(job->command) - 1);
    strncpy(job->request_id, request_id, sizeof(job->request_id) - 1);
    job->session_generation = session_generation;
    job->params = params ? cJSON_Duplicate(params, 1) : NULL;
    if ((params && !job->params) || xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        cJSON_Delete(job->params);
        free(job);
        return error_result("busy", "too many BLE commands queued; try again");
    }
    cJSON *async = cJSON_CreateObject();
    cJSON_AddBoolToObject(async, "_async", true);
    return async;
}

#endif  // !BLE_COMMANDS_HOST_TEST

#endif  // CONFIG_HOMEHUB_BLE_COMMANDS
