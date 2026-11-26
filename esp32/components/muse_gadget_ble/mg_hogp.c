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
#include "mg_hogp.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "mgcommands.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "mg.hid";

/* The bond store kept in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST); NimBLE doesn't declare it. */
void ble_store_config_init(void);

#define UUID_HID_SERVICE 0x1812
#define UUID_HID_INFORMATION 0x2A4A
#define UUID_REPORT_MAP 0x2A4B
#define UUID_HID_CONTROL_POINT 0x2A4C
#define UUID_REPORT 0x2A4D
#define UUID_PROTOCOL_MODE 0x2A4E
#define UUID_REPORT_REFERENCE 0x2908
#define REPORT_TYPE_INPUT 1
#define REPORT_TYPE_FEATURE 3

/* Which report a characteristic is (its Report Reference): id and type. */
typedef struct {
    uint8_t id, type;
} report_ref_t;

static const report_ref_t REF_DIGITIZER = { MG_HID_REPORT_DIGITIZER, REPORT_TYPE_INPUT };
static const report_ref_t REF_MOUSE_ABS = { MG_HID_REPORT_MOUSE_ABS, REPORT_TYPE_INPUT };
static const report_ref_t REF_MOUSE_REL = { MG_HID_REPORT_MOUSE_REL, REPORT_TYPE_INPUT };
static const report_ref_t REF_CONSUMER = { MG_HID_REPORT_CONSUMER, REPORT_TYPE_INPUT };
static const report_ref_t REF_CONTACT_MAX = { MG_HID_REPORT_DIGITIZER, REPORT_TYPE_FEATURE };

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_in_handle[5];          /* by report ID */
static bool s_sub[5];                    /* the host enabled that input report's notifications */
static bool s_encrypted, s_bonded;
static uint8_t s_protocol_mode = 1;      /* report protocol (no boot protocol) */
static uint8_t s_last_report[5][MG_HID_REPORT_MAX];
static volatile bool s_visible;

/* The HID service shown or hidden as s_visible says: on the host task, at
 * each connection before any discovery, and when it changes. */
static bool apply_visibility(void)
{
    uint16_t svc;
    if (ble_gatts_find_svc(BLE_UUID16_DECLARE(UUID_HID_SERVICE), &svc) != 0) {
        return false;
    }
    return ble_gatts_svc_set_visibility(svc, s_visible ? 1 : 0) == 0;
}

/* A change on a connection, run on the host task: the ATT server's attribute
 * lists aren't locked, and the host walks them for every request. */
static struct ble_npl_event s_visibility_ev;
static bool s_visibility_ev_ready;

static void visibility_event(struct ble_npl_event *ev)
{
    (void)ev;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && apply_visibility()) {
        ble_svc_gatt_changed(0x0001, 0xFFFF);   /* the peer's view of the database is stale */
    }
}

void mg_hogp_set_visible(bool visible)
{
    if (visible == s_visible) {
        return;
    }
    s_visible = visible;
    /* With nobody connected (a map restored at boot, before the host has
     * even started) the next connection applies it, before any discovery. */
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_visibility_ev_ready) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_visibility_ev);
    }
    ESP_LOGI(TAG, "HID service %s", visible ? "shown" : "hidden");
}

bool mg_hogp_visible(void)
{
    return s_visible;
}

static int ref_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    const report_ref_t *r = arg;
    uint8_t v[2] = { r->id, r->type };
    return os_mbuf_append(ctxt->om, v, sizeof(v)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int report_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    const report_ref_t *r = arg;
    if (r->type == REPORT_TYPE_FEATURE) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            uint8_t max = 1;   /* contact count maximum: one finger */
            return os_mbuf_append(ctxt->om, &max, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;   /* a host setting it: nothing to set */
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    size_t len = r->id == MG_HID_REPORT_DIGITIZER   ? MG_HID_DIGITIZER_LEN
                 : r->id == MG_HID_REPORT_MOUSE_ABS ? MG_HID_MOUSE_ABS_LEN
                 : r->id == MG_HID_REPORT_CONSUMER  ? MG_HID_CONSUMER_LEN
                                                    : MG_HID_MOUSE_REL_LEN;
    return os_mbuf_append(ctxt->om, s_last_report[r->id], len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);
    switch (uuid) {
    case UUID_HID_INFORMATION: {
        /* bcdHID 1.11, no country, normally connectable (no remote wake) */
        static const uint8_t info[4] = { 0x11, 0x01, 0x00, 0x02 };
        return os_mbuf_append(ctxt->om, info, sizeof(info)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case UUID_REPORT_MAP:
        return os_mbuf_append(ctxt->om, mg_hid_report_map, mg_hid_report_map_len) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    case UUID_HID_CONTROL_POINT:
        return 0;   /* suspend / exit suspend: nothing to do */
    case UUID_PROTOCOL_MODE:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            return os_mbuf_append(ctxt->om, &s_protocol_mode, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;   /* only report protocol is offered (no boot protocol) */
    default:
        (void)arg;
        return BLE_ATT_ERR_UNLIKELY;
    }
}

#define REPORT_REF(r)                                                                  \
    (struct ble_gatt_dsc_def[])                                                        \
    {                                                                                  \
        { .uuid = BLE_UUID16_DECLARE(UUID_REPORT_REFERENCE), .att_flags = BLE_ATT_F_READ, \
          .access_cb = ref_access, .arg = (void *)&(r) },                              \
        { 0 },                                                                         \
    }

/* Input reports and the report map need encryption: a bonded host only. */
#define F_IN (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY)

static const struct ble_gatt_svc_def s_hid_svc[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_HID_SERVICE),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(UUID_HID_INFORMATION), .access_cb = hid_access,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(UUID_REPORT_MAP), .access_cb = hid_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
            { .uuid = BLE_UUID16_DECLARE(UUID_HID_CONTROL_POINT), .access_cb = hid_access,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(UUID_PROTOCOL_MODE), .access_cb = hid_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(UUID_REPORT), .access_cb = report_access, .arg = (void *)&REF_DIGITIZER,
              .flags = F_IN, .val_handle = &s_in_handle[MG_HID_REPORT_DIGITIZER],
              .descriptors = REPORT_REF(REF_DIGITIZER) },
            { .uuid = BLE_UUID16_DECLARE(UUID_REPORT), .access_cb = report_access, .arg = (void *)&REF_MOUSE_ABS,
              .flags = F_IN, .val_handle = &s_in_handle[MG_HID_REPORT_MOUSE_ABS],
              .descriptors = REPORT_REF(REF_MOUSE_ABS) },
            { .uuid = BLE_UUID16_DECLARE(UUID_REPORT), .access_cb = report_access, .arg = (void *)&REF_MOUSE_REL,
              .flags = F_IN, .val_handle = &s_in_handle[MG_HID_REPORT_MOUSE_REL],
              .descriptors = REPORT_REF(REF_MOUSE_REL) },
            { .uuid = BLE_UUID16_DECLARE(UUID_REPORT), .access_cb = report_access, .arg = (void *)&REF_CONSUMER,
              .flags = F_IN, .val_handle = &s_in_handle[MG_HID_REPORT_CONSUMER],
              .descriptors = REPORT_REF(REF_CONSUMER) },
            { .uuid = BLE_UUID16_DECLARE(UUID_REPORT), .access_cb = report_access, .arg = (void *)&REF_CONTACT_MAX,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE |
                       BLE_GATT_CHR_F_WRITE_ENC,
              .descriptors = REPORT_REF(REF_CONTACT_MAX) },
            { 0 },
        },
    },
    { 0 },
};

const struct ble_gatt_svc_def *mg_hogp_service(void)
{
    return s_hid_svc;
}

/*
 * The bond store's overflow policy. NimBLE keeps, beside each bond, a record
 * of the phone's random address and its identity (BLE_STORE_OBJ_TYPE_PEER_ADDR),
 * at most MAX_BONDS of them. In IDF v6.0.1 neither ble_store_clear() nor
 * ble_store_iterate() ever matches those records (their lookup has no "any
 * address" key), so they outlive the bonds they went with: forgetting every
 * phone, or bonding and unbonding a few, fills them up for good. The next
 * bond's record then overflows, and ble_store_util_status_rr() makes room by
 * unpairing the oldest bond, which with one bond is the phone that has just
 * paired: its link drops ~200 ms after "bonded", the gadget keeps no keys and
 * the phone keeps its own, so each reconnection fails encryption (the iPhone
 * 17 Pro Max did this on the StopWatch). The record only serves
 * ble_gap_unpair() called with the random address; bonds and reconnections
 * work without it (the controller resolves the phone with its IRK). So with
 * the address records full the new one isn't written and the bond stays;
 * every other overflow is status_rr's (the oldest bond goes, never the
 * current peer's for its subscriptions).
 */
static int store_status(struct ble_store_status_event *ev, void *arg)
{
    if (ev->event_code == BLE_STORE_EVENT_OVERFLOW && ev->overflow.obj_type == BLE_STORE_OBJ_TYPE_PEER_ADDR) {
        ESP_LOGW(TAG, "bond store: no room for the phone's address record; keeping the bond without it");
        return BLE_HS_ESTORE_CAP;   /* the record isn't stored; nothing is unpaired */
    }
    return ble_store_util_status_rr(ev, arg);
}

/* The NimBLE host task's stack margin (we run on it), at the deepest moments
 * we see: pairing (its key exchange runs on this task) and our logging in GAP
 * events through the Nordic UART mirror. */
static void log_host_stack(const char *when)
{
    static UBaseType_t least = UINT32_MAX;
    UBaseType_t free_now = uxTaskGetStackHighWaterMark(NULL);
    if (free_now < least) {
        least = free_now;
        ESP_LOGI(TAG, "host stack: %u bytes never used (%s)", (unsigned)free_now, when);
    }
}

/* The key store's writes that fail, and its deletes, logged (a bond that
 * isn't stored can't be told apart from one that is, otherwise). */
static ble_store_write_fn *s_store_write;
static ble_store_delete_fn *s_store_delete;
static int store_write_logged(int obj_type, const union ble_store_value *val)
{
    int rc = s_store_write ? s_store_write(obj_type, val) : BLE_HS_ENOTSUP;
    if (rc != 0) {
        ESP_LOGW(TAG, "bond store: write of type %d failed (rc %d)", obj_type, rc);
    }
    return rc;
}

static int store_delete_logged(int obj_type, const union ble_store_key *key)
{
    int rc = s_store_delete ? s_store_delete(obj_type, key) : BLE_HS_ENOTSUP;
    ESP_LOGI(TAG, "bond store: delete type %d rc %d", obj_type, rc);
    return rc;
}

int mg_hogp_forget_addr_records(void)
{
    /* NimBLE's store can't list them (no "any address" match), but its NVS
     * copy can be read: collect the addresses, then delete each through the
     * store, which drops it from RAM and NVS alike. */
    ble_addr_t addrs[MYNEWT_VAL(BLE_STORE_MAX_BONDS) + 1];
    int n = 0;
    nvs_handle_t h;
    if (nvs_open("nimble_bond", NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i <= MYNEWT_VAL(BLE_STORE_MAX_BONDS) && n < (int)(sizeof(addrs) / sizeof(addrs[0])); i++) {
            char key[16];
            struct ble_store_value_rpa_rec v;
            size_t len = sizeof(v);
            snprintf(key, sizeof(key), "rpa_rec_%d", i);
            if (nvs_get_blob(h, key, &v, &len) == ESP_OK && len == sizeof(v)) {
                addrs[n++] = v.peer_rpa_addr;
            }
        }
        nvs_close(h);
    }
    int gone = 0;
    for (int i = 0; i < n; i++) {
        union ble_store_key k;
        memset(&k, 0, sizeof(k));
        k.rpa_rec.peer_rpa_addr = addrs[i];
        gone += ble_store_delete(BLE_STORE_OBJ_TYPE_PEER_ADDR, &k) == 0;
    }
    ESP_LOGI(TAG, "bond store: %d of %d phone-address record(s) deleted", gone, n);
    return gone;
}

void mg_hogp_configure_host(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    ble_npl_event_init(&s_visibility_ev, visibility_event, NULL);
    s_visibility_ev_ready = true;
    /* Bonding, and both sides' identity keys: phones connect from resolvable
     * private addresses, and only the peer's IRK finds its bond again after
     * a restart (without it every reconnection fails encryption and the
     * phone drops the bond). NimBLE's own defaults bond with no keys at all.
     * Muse's phone setup (when it shares the host) sets passkey pairing
     * itself; otherwise Just Works: the gadget has no keypad, and a HID host
     * only needs a bonded, encrypted link. */
    if (!ble_hs_cfg.sm_mitm) {
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    }
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = store_status;   /* full: forget the oldest bond, keep the current one */
    if (!ble_hs_cfg.store_read_cb) {
        ble_store_config_init();
    }
    if (ble_hs_cfg.store_write_cb != store_write_logged) {
        s_store_write = ble_hs_cfg.store_write_cb;
        s_store_delete = ble_hs_cfg.store_delete_cb;
        ble_hs_cfg.store_write_cb = store_write_logged;
        ble_hs_cfg.store_delete_cb = store_delete_logged;
    }
    ble_svc_gap_device_appearance_set(0x03C0);   /* generic HID */
    ESP_LOGI(TAG, "HID over GATT: report map %u bytes", (unsigned)mg_hid_report_map_len);
}

uint8_t mg_hogp_status(void)
{
    bool host = s_sub[MG_HID_REPORT_DIGITIZER] || s_sub[MG_HID_REPORT_MOUSE_ABS] || s_sub[MG_HID_REPORT_MOUSE_REL];
    return (s_encrypted && s_bonded ? mg_hid_status_bonded : 0) | (host ? mg_hid_status_host : 0);
}

static void read_security(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) == 0) {
        s_encrypted = d.sec_state.encrypted;
        s_bonded = d.sec_state.bonded;
    }
}

/* The peer on conn holds a bond with this gadget (its identity, as the controller
 * resolved it with the bond's IRK, is a bonded peer's). */
bool mg_hogp_peer_bonded(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int bonds = 0;
    if (ble_gap_conn_find(conn, &d) != 0 ||
        ble_store_util_bonded_peers(peers, &bonds, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) != 0) {
        return false;
    }
    for (int i = 0; i < bonds; i++) {
        if (ble_addr_cmp(&peers[i], &d.peer_id_addr) == 0) {
            return true;
        }
    }
    return false;
}

bool mg_hogp_gap_event(struct ble_gap_event *ev)
{
    uint8_t before = mg_hogp_status();
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_encrypted = s_bonded = false;
            memset(s_sub, 0, sizeof(s_sub));
            apply_visibility();
            int bonds = 0;
            ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
            ble_store_util_bonded_peers(peers, &bonds, MYNEWT_VAL(BLE_STORE_MAX_BONDS));
            struct ble_gap_conn_desc d;
            bool known = false;
            if (ble_gap_conn_find(s_conn, &d) == 0) {
                for (int i = 0; i < bonds; i++) {
                    known |= ble_addr_cmp(&peers[i], &d.peer_id_addr) == 0;
                }
                /* Address types and the last byte only: enough to tell a resolved identity from an RPA. */
                ESP_LOGI(TAG, "peer: over the air type %u ..%02x, identity type %u ..%02x (%s)", d.peer_ota_addr.type,
                         d.peer_ota_addr.val[0], d.peer_id_addr.type, d.peer_id_addr.val[0],
                         known ? "bonded" : "no bond for it");
            }
            ESP_LOGI(TAG, "connected: %d bond(s) stored, HID %s", bonds, s_visible ? "shown" : "hidden");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        log_host_stack("disconnect");
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_encrypted = s_bonded = false;
        memset(s_sub, 0, sizeof(s_sub));
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ev->enc_change.status == 0) {
            read_security(ev->enc_change.conn_handle);
        }
        ESP_LOGI(TAG, "encryption %s%s (status 0x%x)", ev->enc_change.status == 0 ? "on" : "failed",
                 s_bonded ? ", bonded" : "", ev->enc_change.status);
        if (ev->enc_change.status == BLE_HS_ENOENT) {
            /* The phone has keys for a bond this gadget no longer has (it
             * was forgotten here, or reset): it fails like this at every
             * reconnection until the phone forgets the gadget too. */
            ESP_LOGW(TAG, "the phone holds a bond the gadget doesn't: forget the gadget in the phone's Bluetooth settings");
        }
        log_host_stack("encryption");
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        for (int id = 1; id < 5; id++) {
            if (s_in_handle[id] && ev->subscribe.attr_handle == s_in_handle[id]) {
                s_sub[id] = ev->subscribe.cur_notify;
                ESP_LOGI(TAG, "host %s report %d", s_sub[id] ? "subscribed to" : "left", id);
            }
        }
        break;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        ESP_LOGI(TAG, "peer identity resolved");
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The phone forgot the bond and pairs again: drop ours and let it. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
            ble_store_util_delete_peer(&d.peer_id_addr);
        }
        ESP_LOGI(TAG, "pairing again: the old bond is deleted");
        break;
    }
    default:
        break;
    }
    return mg_hogp_status() != before;
}

bool mg_hogp_ready(uint8_t report_id)
{
    return s_visible && s_conn != BLE_HS_CONN_HANDLE_NONE && s_encrypted && report_id > 0 && report_id < 5 && s_sub[report_id];
}

bool mg_hogp_send(const mg_hid_report_t *r)
{
    if (!mg_hogp_ready(r->id)) {
        return false;
    }
    memcpy(s_last_report[r->id], r->d, r->len);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(r->d, r->len);
    if (!om) {
        return false;
    }
    return ble_gatts_notify_custom(s_conn, s_in_handle[r->id], om) == 0;
}
