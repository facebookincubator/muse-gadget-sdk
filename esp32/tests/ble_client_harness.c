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

// main/ble_client.c against a scripted NimBLE (tests/ble_client_fakes). The
// fake runs callbacks either at once (the peer answers in time) or holds them
// for the test to fire later (a peer answering after the worker gave up),
// which is how the timeout, late-callback and disconnect races are checked.
// Built with AddressSanitizer by test_ble_client.py, so a write into freed
// memory fails the run.

#include <assert.h>

#include "ble_client.h"
#include "nimble_fake.h"

// ---- FreeRTOS: one thread, so a take on an empty semaphore is a timeout ----
struct fake_sem {
    bool mutex;
    int count;
};
static int g_critical;
void fake_critical_enter(void) { g_critical++; }
void fake_critical_exit(void) { assert(g_critical > 0); g_critical--; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return calloc(1, sizeof(struct fake_sem)); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    struct fake_sem *s = calloc(1, sizeof(*s));
    s->mutex = true;
    s->count = 1;
    return s;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    (void)ticks;
    // A mutex already held here would deadlock on the device: a callback
    // that runs while the worker holds it, or a missing give.
    if (s->mutex) assert(s->count == 1);
    if (s->count == 0) return pdFALSE;
    s->count--;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (s->mutex) assert(s->count == 0);
    s->count = 1;
    return pdTRUE;
}
static void (*g_on_delay)(void);
void vTaskDelay(TickType_t ticks) {
    (void)ticks;
    if (g_on_delay) g_on_delay();
}
static int64_t g_now;
int64_t esp_timer_get_time(void) { return g_now += 1000; }

// ---- mbufs ----
struct os_mbuf *ble_hs_mbuf_from_flat(const void *buf, uint16_t len) {
    static struct os_mbuf pool[8];
    static int next;
    struct os_mbuf *om = &pool[next++ % 8];
    assert(len <= sizeof(om->data));
    memcpy(om->data, buf, len);
    om->len = len;
    return om;
}
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *flat, uint16_t max_len, uint16_t *out_len) {
    uint16_t n = om->len < max_len ? om->len : max_len;
    memcpy(flat, om->data, n);
    if (out_len) *out_len = n;
    return 0;
}

// ---- scripted NimBLE ----
static bool g_answer = true;      // false: hold the next callback for later
static int g_start_rc;            // what the next procedure start returns
static int g_terminated = -1;     // last ble_gap_terminate() handle

static ble_gap_event_fn *g_conn_cb;
static void *g_conn_arg;

static ble_gatt_disc_svc_fn *g_held_svc;
static ble_gatt_attr_fn *g_held_attr;
static void *g_held_arg;

int ble_hs_synced(void) { return 1; }
int ble_hs_id_infer_auto(int privacy, uint8_t *own) { (void)privacy; *own = 0; return 0; }
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f, const uint8_t *d, uint8_t len) {
    (void)d; (void)len;
    memset(f, 0, sizeof(*f));
    return 0;
}
int ble_gap_disc(uint8_t own, int32_t ms, const struct ble_gap_disc_params *p,
                 ble_gap_event_fn *cb, void *arg) {
    (void)own; (void)ms; (void)p; (void)cb; (void)arg;
    return g_start_rc;
}
int ble_gap_disc_cancel(void) { return 0; }
int ble_gap_connect(uint8_t own, const ble_addr_t *peer, int32_t ms, const void *params,
                    ble_gap_event_fn *cb, void *arg) {
    (void)own; (void)peer; (void)ms; (void)params;
    g_conn_cb = cb;
    g_conn_arg = arg;
    if (g_start_rc) return g_start_rc;
    if (g_answer) {
        struct ble_gap_event ev = {.type = BLE_GAP_EVENT_CONNECT};
        ev.connect.status = 0;
        ev.connect.conn_handle = 1;
        cb(&ev, arg);
    }
    return 0;
}
int ble_gap_conn_cancel(void) { return 0; }
int ble_gap_terminate(uint16_t conn, uint8_t reason) { (void)reason; g_terminated = conn; return 0; }
int ble_gap_security_initiate(uint16_t conn) { (void)conn; return 0; }
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *out) { (void)conn; (void)out; return 1; }
int ble_store_util_delete_peer(const ble_addr_t *addr) { (void)addr; return 0; }
int ble_gattc_exchange_mtu(uint16_t conn, ble_gatt_mtu_fn *cb, void *arg) {
    struct ble_gatt_error err = {0};
    cb(conn, &err, 247, arg);
    return 0;
}

// Two services; the second has a notify characteristic with a CCCD.
int ble_gattc_disc_all_svcs(uint16_t conn, ble_gatt_disc_svc_fn *cb, void *arg) {
    if (g_start_rc) return g_start_rc;
    if (!g_answer) {
        g_held_svc = cb;
        g_held_arg = arg;
        return 0;
    }
    struct ble_gatt_error ok = {0}, done = {.status = BLE_HS_EDONE};
    struct ble_gatt_svc s1 = {.start_handle = 1, .end_handle = 5, .uuid.u16 = {{16}, 0x1800}};
    struct ble_gatt_svc s2 = {.start_handle = 6, .end_handle = 10, .uuid.u16 = {{16}, 0x180f}};
    cb(conn, &ok, &s1, arg);
    cb(conn, &ok, &s2, arg);
    cb(conn, &done, NULL, arg);
    return 0;
}
int ble_gattc_disc_all_chrs(uint16_t conn, uint16_t start, uint16_t end, ble_gatt_chr_fn *cb, void *arg) {
    (void)end;
    struct ble_gatt_error ok = {0}, done = {.status = BLE_HS_EDONE};
    struct ble_gatt_chr c = {.def_handle = (uint16_t)(start + 1), .val_handle = (uint16_t)(start + 2)};
    c.uuid.u16.u.type = 16;
    c.uuid.u16.value = start == 1 ? 0x2a00 : 0x2a19;
    c.properties = start == 1 ? 0x02 : (0x02 | BLE_GATT_CHR_PROP_NOTIFY);
    cb(conn, &ok, &c, arg);
    cb(conn, &done, NULL, arg);
    return 0;
}
int ble_gattc_disc_all_dscs(uint16_t conn, uint16_t start, uint16_t end, ble_gatt_dsc_fn *cb, void *arg) {
    (void)end;
    struct ble_gatt_error ok = {0}, done = {.status = BLE_HS_EDONE};
    struct ble_gatt_dsc d = {.handle = (uint16_t)(start + 1)};
    d.uuid.u16.u.type = 16;
    d.uuid.u16.value = BLE_GATT_DSC_CLT_CFG_UUID16;
    cb(conn, &ok, start, &d, arg);
    cb(conn, &done, start, NULL, arg);
    return 0;
}
static void fire_read(ble_gatt_attr_fn *cb, void *arg, const char *value) {
    struct ble_gatt_error ok = {0}, done = {.status = BLE_HS_EDONE};
    struct ble_gatt_attr a = {.handle = 3, .om = ble_hs_mbuf_from_flat(value, (uint16_t)strlen(value))};
    if (cb(1, &ok, &a, arg) == 0) cb(1, &done, NULL, arg);
}
int ble_gattc_read_long(uint16_t conn, uint16_t handle, uint16_t offset, ble_gatt_attr_fn *cb, void *arg) {
    (void)conn; (void)handle; (void)offset;
    if (g_start_rc) return g_start_rc;
    if (!g_answer) {
        g_held_attr = cb;
        g_held_arg = arg;
        return 0;
    }
    fire_read(cb, arg, "fresh");
    return 0;
}
static int g_write_status;   // the peer's answer to a write with response
int ble_gattc_write_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t len,
                         ble_gatt_attr_fn *cb, void *arg) {
    (void)handle; (void)data; (void)len;
    if (g_start_rc) return g_start_rc;
    if (!g_answer) {
        g_held_attr = cb;
        g_held_arg = arg;
        return 0;
    }
    struct ble_gatt_error err = {.status = (uint16_t)g_write_status};
    cb(conn, &err, NULL, arg);
    return 0;
}
int ble_gattc_write_long(uint16_t conn, uint16_t handle, uint16_t offset, struct os_mbuf *om,
                         ble_gatt_attr_fn *cb, void *arg) {
    (void)offset; (void)om;
    return ble_gattc_write_flat(conn, handle, NULL, 0, cb, arg);
}
int ble_gattc_write_no_rsp_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t len) {
    (void)conn; (void)handle; (void)data; (void)len;
    return 0;
}

static void host_event(uint8_t type) {
    struct ble_gap_event ev = {.type = type};
    if (type == BLE_GAP_EVENT_DISCONNECT) ev.disconnect.conn.conn_handle = 1;
    g_conn_cb(&ev, g_conn_arg);
}

static void notify(const char *value) {
    struct ble_gap_event ev = {.type = BLE_GAP_EVENT_NOTIFY_RX};
    ev.notify_rx.conn_handle = 1;
    ev.notify_rx.attr_handle = 8;
    ev.notify_rx.om = ble_hs_mbuf_from_flat(value, (uint16_t)strlen(value));
    g_conn_cb(&ev, g_conn_arg);
}
static void notify_three(void) {
    notify("a");
    notify("b");
    notify("c");
}

static uint16_t connect_one(void) {
    static const uint8_t addr[6] = {1, 2, 3, 4, 5, 6};
    ble_client_conn_info_t info;
    assert(ble_client_connect(addr, 0, 1000, &info) == 0);
    assert(info.conn == 1 && info.mtu == 247);
    return info.conn;
}

int main(void) {
    uint16_t conn = connect_one();

    // A read answered in time.
    uint8_t buf[16];
    size_t len = 0;
    assert(ble_client_read(conn, 3, buf, sizeof(buf), &len) == 0);
    assert(len == 5 && !memcmp(buf, "fresh", 5));

    // A read that times out, answered after the worker has moved on: the
    // late answer must not land in the buffer (which the caller has freed
    // or reused) and must not complete the next operation.
    uint8_t *heap_buf = malloc(16);
    g_answer = false;
    assert(ble_client_read(conn, 3, heap_buf, 16, &len) == BLE_CLIENT_ETIMEOUT);
    assert(len == 0);
    free(heap_buf);   // ASan reports a write here if the late callback writes
    g_answer = true;
    struct ble_gatt_error ok = {0};
    struct ble_gatt_attr late = {.handle = 3, .om = ble_hs_mbuf_from_flat("late!", 5)};
    assert(g_held_attr(conn, &ok, &late, g_held_arg) != 0);   // non-zero ends the procedure
    struct ble_gatt_error done = {.status = BLE_HS_EDONE};
    (void)g_held_attr(conn, &done, NULL, g_held_arg);   // and completes nothing (checked next)
    assert(ble_client_read(conn, 3, buf, sizeof(buf), &len) == 0);
    assert(len == 5 && !memcmp(buf, "fresh", 5));

    // A discovery that times out: its database is freed, and services the
    // peer reports afterwards go nowhere.
    const ble_client_db_t *db = NULL;
    g_answer = false;
    assert(ble_client_discover(conn, &db) == BLE_CLIENT_ETIMEOUT);
    g_answer = true;
    struct ble_gatt_svc svc = {.start_handle = 1, .end_handle = 5};
    assert(g_held_svc(conn, &ok, &svc, g_held_arg) != 0);

    // Discovery in time, and its cache survives a disconnect until the
    // worker's next call: the host task only marks the link gone.
    assert(ble_client_discover(conn, &db) == 0);
    assert(db->num_svcs == 2 && db->num_chrs == 2 && db->chrs[1].cccd_handle == 9);
    host_event(BLE_GAP_EVENT_DISCONNECT);
    assert(db->num_chrs == 2 && db->chrs[1].val_handle == 8);   // still readable
    ble_client_conn_info_t infos[BLE_CLIENT_MAX_CONNS];
    assert(ble_client_connections(infos, BLE_CLIENT_MAX_CONNS) == 0);   // reaped now
    assert(ble_client_read(conn, 3, buf, sizeof(buf), &len) == BLE_CLIENT_ENOCONN);

    // A subscription whose CCCD write the peer rejects (ATT 0x103) is an
    // error, not 259 notifications.
    conn = connect_one();
    ble_client_chr_t chr = {.val_handle = 8, .cccd_handle = 9, .properties = BLE_GATT_CHR_PROP_NOTIFY};
    ble_client_notification_t vals[4];
    int count = -1;
    uint32_t dropped = 99;
    g_write_status = BLE_HS_ERR_ATT_BASE + 3;
    assert(ble_client_subscribe(conn, &chr, 100, false, vals, 4, &count, &dropped)
           == BLE_HS_ERR_ATT_BASE + 3);
    assert(count == 0 && dropped == 0);
    g_write_status = 0;

    // Notifications are capped at max, and the rest are counted.
    g_on_delay = notify_three;
    assert(ble_client_subscribe(conn, &chr, 100, false, vals, 2, &count, &dropped) == 0);
    g_on_delay = NULL;
    assert(count == 2 && dropped == 1);
    assert(vals[0].len == 1 && vals[0].data[0] == 'a' && vals[1].data[0] == 'b');

    // keep: values between calls wait for the next one, and the buffer
    // goes with the link.
    assert(ble_client_subscribe(conn, &chr, 100, true, vals, 4, &count, &dropped) == 0);
    notify("x");
    assert(ble_client_subscribe(conn, &chr, 100, true, vals, 4, &count, &dropped) == 0);
    assert(count == 1 && vals[0].data[0] == 'x' && vals[0].ms == 0);
    notify("y");
    host_event(BLE_GAP_EVENT_DISCONNECT);
    notify("z");   // after the link dropped: taken by nothing
    assert(ble_client_connections(infos, BLE_CLIENT_MAX_CONNS) == 0);

    // Scanning that NimBLE refuses to start reports the error, no devices.
    ble_client_dev_t devs[4];
    int found = -1;
    uint32_t reports = 7;
    g_start_rc = 3;
    assert(ble_client_scan(1000, true, NULL, devs, 4, &found, &reports) == 3);
    assert(found == 0 && reports == 0);
    g_start_rc = 0;

    // A connect that completes after the worker gave up is closed again
    // instead of being left without a slot.
    static const uint8_t addr2[6] = {9, 9, 9, 9, 9, 9};
    g_answer = false;
    assert(ble_client_connect(addr2, 0, 1000, NULL) == BLE_CLIENT_ETIMEOUT);
    g_answer = true;
    struct ble_gap_event ev = {.type = BLE_GAP_EVENT_CONNECT};
    ev.connect.status = 0;
    ev.connect.conn_handle = 2;
    g_terminated = -1;
    g_conn_cb(&ev, g_conn_arg);
    assert(g_terminated == 2);
    assert(ble_client_connections(infos, BLE_CLIENT_MAX_CONNS) == 0);

    ble_client_reset();
    assert(g_critical == 0);
    puts("ble_client ok");
    return 0;
}
