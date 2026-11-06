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

// Runs main/ble_commands.c against a fake ble_client and fake firmware hooks.

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ble_client.h"
#include "ble_commands.h"
#include "cJSON.h"

// ---- fake firmware hooks ----
static bool g_on, g_adv, g_wifi_first = true;
static int g_interval = 1000;
static int g_power_calls;
static bool power(bool on) { g_power_calls++; g_on = on; return true; }
static bool is_on(void) { return g_on; }
static void advertise(bool on, int interval) { g_adv = on; if (interval) g_interval = interval; }
static bool is_adv(void) { return g_adv; }
static int adv_interval(void) { return g_interval; }
static bool own_addr(uint8_t a[6]) { for (int i = 0; i < 6; i++) a[i] = (uint8_t)(0x10 + i); return true; }
static const char *name(void) { return "MuseGadget-D65408"; }
static bool periph(void) { return false; }
static void set_wifi_first(bool on) { g_wifi_first = on; }
static bool wifi_first(void) { return g_wifi_first; }

// ---- fake ble_client ----
static int g_scan_rc;
static uint32_t g_scan_ms;
static bool g_scan_active;
static char g_scan_filter[32];
int ble_client_scan(uint32_t ms, bool active, const char *f, ble_client_dev_t *out, int max,
                    int *found, uint32_t *reports) {
    if (!g_on) return BLE_CLIENT_ENOTREADY;
    if (g_scan_rc) return g_scan_rc;
    g_scan_ms = ms;
    g_scan_active = active;
    snprintf(g_scan_filter, sizeof(g_scan_filter), "%s", f ? f : "");
    assert(max >= 3);
    memset(out, 0, sizeof(*out) * 3);
    // Weakest first, to check the sort. Addresses are little-endian.
    uint8_t a0[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(out[0].addr, a0, 6); out[0].rssi = -80; out[0].addr_type = 1;
    uint8_t a1[6] = {0xb4, 0xf5, 0xa7, 0x16, 0xa3, 0x98};
    memcpy(out[1].addr, a1, 6); out[1].rssi = -40; out[1].connectable = true;
    strcpy(out[1].name, "MuseGadget-A7F5B4");
    out[1].num_uuids = 2;
    out[1].uuids[0].type = 16; out[1].uuids[0].value[0] = 0x0f; out[1].uuids[0].value[1] = 0x18;
    out[1].uuids[1].type = 128;
    for (int i = 0; i < 16; i++) out[1].uuids[1].value[i] = (uint8_t)i;
    out[1].mfg_len = 3; out[1].mfg[0] = 0xff; out[1].mfg[1] = 0xff; out[1].mfg[2] = 0x01;
    out[1].has_tx_power = true; out[1].tx_power = 4;
    memcpy(out[2].addr, a0, 6); out[2].addr[0] = 0x09; out[2].rssi = -60;
    *reports = 42;
    *found = 3;
    return 0;
}

static uint8_t g_conn_addr[6], g_conn_type = 0xee;
static int g_connect_rc, g_secure_calls;
int ble_client_connect(const uint8_t addr[6], uint8_t type, uint32_t timeout, ble_client_conn_info_t *out) {
    (void)timeout;
    if (g_connect_rc) return g_connect_rc;
    memcpy(g_conn_addr, addr, 6);
    g_conn_type = type;
    memset(out, 0, sizeof(*out));
    out->conn = 1; memcpy(out->addr, addr, 6); out->addr_type = type; out->mtu = 247; out->central = true;
    return 0;
}
static int g_disconnects;
int ble_client_disconnect(uint16_t conn) { g_disconnects++; return conn == 1 ? 0 : BLE_CLIENT_ENOCONN; }
int ble_client_connections(ble_client_conn_info_t *out, int max) {
    if (max < 1) return 0;
    memset(out, 0, sizeof(*out));
    out->conn = 1; out->mtu = 247; out->central = true;
    return 1;
}
int ble_client_secure(uint16_t conn, uint32_t timeout) { (void)conn; (void)timeout; g_secure_calls++; return 0; }

static ble_client_db_t g_db;
static void make_db(void) {
    memset(&g_db, 0, sizeof(g_db));
    g_db.num_svcs = 2;
    g_db.svcs[0].uuid.type = 16; g_db.svcs[0].uuid.value[0] = 0x0f; g_db.svcs[0].uuid.value[1] = 0x18;
    g_db.svcs[1].uuid.type = 128;
    for (int i = 0; i < 16; i++) g_db.svcs[1].uuid.value[i] = (uint8_t)(0xa0 + i);
    g_db.num_chrs = 3;
    g_db.chrs[0].uuid.type = 16; g_db.chrs[0].uuid.value[0] = 0x19; g_db.chrs[0].uuid.value[1] = 0x2a;
    g_db.chrs[0].val_handle = 3; g_db.chrs[0].properties = 0x12; g_db.chrs[0].svc = 0;  // read, notify
    g_db.chrs[1].uuid.type = 16; g_db.chrs[1].uuid.value[0] = 0x01; g_db.chrs[1].uuid.value[1] = 0x00;
    g_db.chrs[1].val_handle = 10; g_db.chrs[1].properties = 0x04; g_db.chrs[1].svc = 1;  // write_no_response
    g_db.chrs[2].uuid.type = 16; g_db.chrs[2].uuid.value[0] = 0x01; g_db.chrs[2].uuid.value[1] = 0x00;
    g_db.chrs[2].val_handle = 12; g_db.chrs[2].properties = 0x0a; g_db.chrs[2].svc = 0;  // read, write
}
int ble_client_discover(uint16_t conn, const ble_client_db_t **db) {
    if (conn != 1) return BLE_CLIENT_ENOCONN;
    *db = &g_db;
    return 0;
}
static bool uuid16_is(const ble_client_uuid_t *u, uint16_t v) {
    return u->type == 16 && u->value[0] == (v & 0xff) && u->value[1] == (v >> 8);
}
int ble_client_find_chr(uint16_t conn, uint16_t handle, const ble_client_uuid_t *uuid,
                        const ble_client_uuid_t *svc, ble_client_chr_t *out) {
    if (conn != 1) return BLE_CLIENT_ENOCONN;
    for (int i = 0; i < g_db.num_chrs; i++) {
        const ble_client_chr_t *c = &g_db.chrs[i];
        if (handle ? c->val_handle != handle : !(uuid && c->uuid.type == uuid->type
                                                && !memcmp(c->uuid.value, uuid->value, 16))) continue;
        if (!handle && svc && !(svc->type == g_db.svcs[c->svc].uuid.type
                                && !memcmp(svc->value, g_db.svcs[c->svc].uuid.value, 16))) continue;
        *out = *c;
        return 0;
    }
    (void)uuid16_is;
    return BLE_CLIENT_ENOTFOUND;
}
int ble_client_read(uint16_t conn, uint16_t handle, uint8_t *buf, size_t cap, size_t *len) {
    (void)conn; (void)cap;
    if (handle == 3) { buf[0] = 0x64; *len = 1; return 0; }
    if (handle == 12) { memcpy(buf, "hello\0", 6); *len = 6; return 0; }
    return 0x100 + 0x02;   // read not permitted
}
static uint8_t g_written[600];
static size_t g_written_len;
static bool g_written_rsp;
static uint16_t g_written_handle;
int ble_client_write(uint16_t conn, uint16_t handle, const uint8_t *d, size_t len, bool rsp) {
    (void)conn;
    memcpy(g_written, d, len); g_written_len = len; g_written_rsp = rsp; g_written_handle = handle;
    return 0;
}
static int g_sub_rc;
int ble_client_subscribe(uint16_t conn, const ble_client_chr_t *chr, uint32_t ms, bool keep,
                         ble_client_notification_t *out, int max, int *count, uint32_t *dropped) {
    (void)conn; (void)ms; (void)keep; (void)max;
    assert(chr->val_handle == 3);
    *count = 0;
    if (g_sub_rc) return g_sub_rc;   // a NimBLE error is never a count
    out[0].ms = 10; out[0].len = 1; out[0].data[0] = 0x63;
    out[1].ms = 1010; out[1].len = 100;
    for (int i = 0; i < BLE_CLIENT_NOTIFY_BYTES; i++) out[1].data[i] = (uint8_t)i;
    *dropped = 2;
    *count = 2;
    return 0;
}
static int g_resets;
void ble_client_reset(void) { g_resets++; }

// ---- helpers ----
static cJSON *run(const char *cmd, const char *params_json) {
    cJSON *params = params_json ? cJSON_Parse(params_json) : NULL;
    assert(!params_json || params);
    cJSON *r = ble_commands_run(cmd, params);
    cJSON_Delete(params);
    assert(r);
    return r;
}
static bool ok(const cJSON *r) { return cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")); }
static const char *err_code(const cJSON *r) {
    const cJSON *e = cJSON_GetObjectItem(r, "error");
    return e ? cJSON_GetObjectItem(e, "code")->valuestring : "";
}
static const cJSON *payload(const cJSON *r) { return cJSON_GetObjectItem(r, "payload"); }
static const char *pstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static double pnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : -999;
}

int main(void) {
    ble_commands_ops_t ops = {power, is_on, advertise, is_adv, adv_interval, own_addr, name,
                              periph, set_wifi_first, wifi_first};
    ble_commands_init(&ops);
    make_db();

    // Registration: every command, small enough for link.register.
    cJSON *cmds = cJSON_CreateObject();
    ble_commands_register(cmds);
    const char *names[] = {"ble.state", "ble.power", "ble.advertise", "ble.scan", "ble.connect",
                           "ble.disconnect", "ble.discover", "ble.read", "ble.write", "ble.subscribe"};
    for (int i = 0; i < 10; i++) {
        assert(cJSON_GetObjectItem(cmds, names[i]));
        assert(ble_commands_handles(names[i]));
    }
    assert(cJSON_GetArraySize(cmds) == 10);
    assert(!ble_commands_handles("ble.unknown") && !ble_commands_handles("device.health"));
    assert(pnum(cJSON_GetObjectItem(cmds, "ble.subscribe"), "timeout_ms") == 75000);
    char *printed = cJSON_PrintUnformatted(cmds);
    printf("register_bytes=%zu\n", strlen(printed));
    assert(strlen(printed) < 4500);
    free(printed);
    cJSON_Delete(cmds);

    // Off: state says so; GATT commands say how to fix it.
    cJSON *r = run("ble.state", NULL);
    assert(ok(r) && cJSON_IsFalse(cJSON_GetObjectItem(payload(r), "enabled")));
    cJSON_Delete(r);
    r = run("ble.scan", NULL);
    assert(!ok(r) && !strcmp(err_code(r), "ble_off"));
    cJSON_Delete(r);

    // Power.
    r = run("ble.power", "{}");
    assert(!ok(r) && !strcmp(err_code(r), "missing_param"));
    cJSON_Delete(r);
    r = run("ble.power", "{\"enabled\": \"maybe\"}");
    assert(!ok(r) && !strcmp(err_code(r), "invalid_params"));
    cJSON_Delete(r);
    r = run("ble.power", "{\"enabled\": \"true\", \"wifi_first\": false}");
    assert(ok(r) && g_on && !g_wifi_first);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(payload(r), "enabled")));
    assert(!strcmp(pstr(payload(r), "address"), "15:14:13:12:11:10"));
    cJSON_Delete(r);

    // Advertise.
    r = run("ble.advertise", "{\"enabled\": true, \"interval_ms\": 10}");
    assert(!ok(r) && !strcmp(err_code(r), "invalid_params"));
    cJSON_Delete(r);
    r = run("ble.advertise", "{\"enabled\": true, \"interval_ms\": \"500\"}");
    assert(ok(r) && g_adv && g_interval == 500);
    cJSON_Delete(r);

    // Scan: strongest first, fields formatted, limit applied.
    r = run("ble.scan", "{\"duration_ms\": 50}");
    assert(!ok(r) && !strcmp(err_code(r), "invalid_params"));
    cJSON_Delete(r);
    r = run("ble.scan", "{\"duration_ms\": \"3000\", \"name\": \"Muse\", \"active\": false, \"limit\": 2}");
    assert(ok(r) && g_scan_ms == 3000 && !g_scan_active && !strcmp(g_scan_filter, "Muse"));
    const cJSON *devs = cJSON_GetObjectItem(payload(r), "devices");
    assert(cJSON_GetArraySize(devs) == 2 && pnum(payload(r), "found") == 3);
    const cJSON *d0 = cJSON_GetArrayItem(devs, 0);
    assert(!strcmp(pstr(d0, "address"), "98:A3:16:A7:F5:B4"));
    assert(!strcmp(pstr(d0, "name"), "MuseGadget-A7F5B4") && pnum(d0, "rssi") == -40);
    assert(!strcmp(pstr(d0, "manufacturer_data"), "ffff01") && pnum(d0, "tx_power") == 4);
    const cJSON *svcs = cJSON_GetObjectItem(d0, "services");
    assert(!strcmp(cJSON_GetArrayItem(svcs, 0)->valuestring, "180f"));
    assert(!strcmp(cJSON_GetArrayItem(svcs, 1)->valuestring, "0f0e0d0c-0b0a-0908-0706-050403020100"));
    assert(pnum(cJSON_GetArrayItem(devs, 1), "rssi") == -60);
    cJSON_Delete(r);

    // Connect: address parsed, type remembered from the scan.
    r = run("ble.connect", "{\"address\": \"nope\"}");
    assert(!ok(r) && !strcmp(err_code(r), "invalid_params"));
    cJSON_Delete(r);
    r = run("ble.connect", "{\"address\": \"06:05:04:03:02:01\"}");
    assert(ok(r) && g_conn_type == 1 && g_conn_addr[0] == 0x01 && g_conn_addr[5] == 0x06);
    assert(pnum(payload(r), "conn") == 1 && pnum(payload(r), "mtu") == 247);
    cJSON_Delete(r);
    r = run("ble.connect", "{\"address\": \"98a316a7f5b4\", \"address_type\": \"public\", \"secure\": true}");
    assert(ok(r) && g_conn_type == 0 && g_secure_calls == 1);
    cJSON_Delete(r);
    g_connect_rc = BLE_CLIENT_EFULL;
    r = run("ble.connect", "{\"address\": \"98:A3:16:A7:F5:B4\"}");
    assert(!ok(r) && !strcmp(err_code(r), "too_many_connections"));
    cJSON_Delete(r);
    g_connect_rc = 0x200 + 0x3e;
    r = run("ble.connect", "{\"address\": \"98:A3:16:A7:F5:B4\"}");
    assert(!ok(r) && !strcmp(err_code(r), "connect_failed"));
    cJSON_Delete(r);
    g_connect_rc = 0;

    // Discover.
    r = run("ble.discover", "{\"conn\": 1}");
    assert(ok(r));
    const cJSON *s0 = cJSON_GetArrayItem(cJSON_GetObjectItem(payload(r), "services"), 0);
    assert(!strcmp(pstr(s0, "uuid"), "180f"));
    const cJSON *c0 = cJSON_GetArrayItem(cJSON_GetObjectItem(s0, "characteristics"), 0);
    assert(!strcmp(pstr(c0, "uuid"), "2a19") && pnum(c0, "handle") == 3);
    const cJSON *props = cJSON_GetObjectItem(c0, "properties");
    assert(cJSON_GetArraySize(props) == 2 && !strcmp(cJSON_GetArrayItem(props, 0)->valuestring, "read")
           && !strcmp(cJSON_GetArrayItem(props, 1)->valuestring, "notify"));
    cJSON_Delete(r);
    r = run("ble.discover", "{\"conn\": 7}");
    assert(!ok(r) && !strcmp(err_code(r), "not_connected"));
    cJSON_Delete(r);

    // Read by uuid and by handle; text only when printable.
    r = run("ble.read", "{\"conn\": 1, \"uuid\": \"0x2A19\"}");
    assert(ok(r) && !strcmp(pstr(payload(r), "hex"), "64") && pnum(payload(r), "handle") == 3);
    cJSON_Delete(r);
    r = run("ble.read", "{\"conn\": \"1\", \"handle\": 12}");
    assert(ok(r) && !strcmp(pstr(payload(r), "text"), "hello") && !strcmp(pstr(payload(r), "hex"), "68656c6c6f00"));
    cJSON_Delete(r);
    r = run("ble.read", "{\"conn\": 1, \"handle\": 10}");
    assert(!ok(r) && !strcmp(err_code(r), "invalid_params"));   // write-only
    cJSON_Delete(r);
    r = run("ble.read", "{\"conn\": 1}");
    assert(!ok(r) && !strcmp(err_code(r), "missing_param"));
    cJSON_Delete(r);
    r = run("ble.read", "{\"conn\": 1, \"handle\": 40}");   // not in the database: read anyway
    assert(!ok(r) && !strcmp(err_code(r), "device_refused"));
    cJSON_Delete(r);

    // Write: hex or text; the kind of write follows the characteristic.
    r = run("ble.write", "{\"conn\": 1, \"handle\": 12}");
    assert(!ok(r) && !strcmp(err_code(r), "missing_param"));
    cJSON_Delete(r);
    r = run("ble.write", "{\"conn\": 1, \"handle\": 12, \"hex\": \"01 02 0a\"}");
    assert(ok(r) && g_written_len == 3 && g_written[2] == 0x0a && g_written_rsp && g_written_handle == 12);
    cJSON_Delete(r);
    r = run("ble.write", "{\"conn\": 1, \"uuid\": \"0001\", \"service\": \"afaeadac-abaa-a9a8-a7a6-a5a4a3a2a1a0\", \"text\": \"on\"}");
    assert(ok(r) && g_written_handle == 10 && !g_written_rsp && g_written_len == 2);
    cJSON_Delete(r);
    r = run("ble.write", "{\"conn\": 1, \"handle\": 12, \"hex\": \"0g\"}");
    assert(!ok(r) && !strcmp(err_code(r), "invalid_params"));
    cJSON_Delete(r);

    // Subscribe.
    r = run("ble.subscribe", "{\"conn\": 1, \"uuid\": \"2a19\", \"duration_ms\": 2000}");
    assert(ok(r) && pnum(payload(r), "count") == 2 && pnum(payload(r), "dropped") == 2);
    const cJSON *vals = cJSON_GetObjectItem(payload(r), "values");
    assert(!strcmp(pstr(cJSON_GetArrayItem(vals, 0), "hex"), "63"));
    assert(pnum(cJSON_GetArrayItem(vals, 1), "length") == 100);
    cJSON_Delete(r);
    r = run("ble.subscribe", "{\"conn\": 1, \"handle\": 12}");
    assert(!ok(r) && !strcmp(err_code(r), "invalid_params"));   // no notify
    cJSON_Delete(r);
    // A peer that rejects the CCCD write answers with a positive NimBLE
    // code (ATT 0x103 here, 259): an error, never 259 values.
    g_sub_rc = 0x103;
    r = run("ble.subscribe", "{\"conn\": 1, \"uuid\": \"2a19\"}");
    assert(!ok(r) && !cJSON_GetObjectItem(r, "payload"));
    cJSON_Delete(r);
    g_sub_rc = 0;
    g_scan_rc = 3;   // BLE_HS_EALREADY from ble_gap_disc
    r = run("ble.scan", NULL);
    assert(!ok(r) && !cJSON_GetObjectItem(r, "payload"));
    cJSON_Delete(r);
    g_scan_rc = 0;

    // The session ends: the connection Muse opened is closed and Wi-Fi gets
    // its radio priority back.
    g_disconnects = 0;
    r = run("ble.power", "{\"enabled\": true, \"wifi_first\": false}");
    assert(ok(r) && !g_wifi_first);
    cJSON_Delete(r);
    ble_commands_cleanup();
    assert(g_disconnects == 1 && g_wifi_first);

    // Disconnect, then power off drops the client's state.
    r = run("ble.disconnect", "{\"conn\": 1}");
    assert(ok(r));
    cJSON_Delete(r);
    r = run("ble.power", "{\"enabled\": false}");
    assert(ok(r) && !g_on && g_resets == 1);
    cJSON_Delete(r);

    printf("ok\n");
    return 0;
}
