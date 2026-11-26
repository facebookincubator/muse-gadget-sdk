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
#include "gatt_dbhash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gatt_dbhash_core.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

static const char *TAG = "link.gatt";

#define MAX_ATTRS 192
#define VALUE_MARK 0xFFFF   /* a characteristic value: accounted for, not hashed */

static gatt_dbhash_attr_t *s_attrs;   /* the registration as it happened (PSRAM when there is any) */
static size_t s_nattrs;
static bool s_unsupported;            /* something the walk doesn't model (an include, extended properties) */
static const char *s_why = "not computed";
static uint8_t s_hash[16];
static bool s_served;

#define MAX_CONNS 4
static struct {
    uint16_t conn;
    uint8_t csf;
} s_csf[MAX_CONNS];

static void note(uint16_t handle, uint16_t type, const uint8_t *value, size_t len)
{
    if (!s_attrs || s_nattrs >= MAX_ATTRS || len > GATT_DBHASH_VALUE_MAX) {
        s_unsupported = true;
        return;
    }
    gatt_dbhash_attr_t *a = &s_attrs[s_nattrs++];
    a->handle = handle;
    a->type = type;
    a->len = (uint8_t)len;
    if (len) memcpy(a->value, value, len);
}

/* A UUID as an attribute value carries it: 2 octets for a 16-bit one, else
 * the 16 of a 128-bit one (a 32-bit UUID goes out as 128 bits), LSO first. */
static size_t uuid_flat(const ble_uuid_t *u, uint8_t out[16])
{
    static const uint8_t base[16] = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                      0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    switch (u->type) {
    case BLE_UUID_TYPE_16:
        out[0] = (uint8_t)BLE_UUID16(u)->value;
        out[1] = (uint8_t)(BLE_UUID16(u)->value >> 8);
        return 2;
    case BLE_UUID_TYPE_32: {
        uint32_t v = BLE_UUID32(u)->value;
        memcpy(out, base, 16);
        out[12] = (uint8_t)v;
        out[13] = (uint8_t)(v >> 8);
        out[14] = (uint8_t)(v >> 16);
        out[15] = (uint8_t)(v >> 24);
        return 16;
    }
    default:
        memcpy(out, BLE_UUID128(u)->value, 16);
        return 16;
    }
}

static void on_register(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    uint8_t flat[16];
    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC: {
        const struct ble_gatt_svc_def *svc = ctxt->svc.svc_def;
        if (svc->includes && svc->includes[0]) s_unsupported = true;
        size_t n = uuid_flat(svc->uuid, flat);
        note(ctxt->svc.handle, svc->type == BLE_GATT_SVC_TYPE_SECONDARY ? 0x2801 : 0x2800, flat, n);
        break;
    }
    case BLE_GATT_REGISTER_OP_CHR: {
        const struct ble_gatt_chr_def *chr = ctxt->chr.chr_def;
        if (chr->flags & (BLE_GATT_CHR_F_RELIABLE_WRITE | BLE_GATT_CHR_F_AUX_WRITE)) s_unsupported = true;
        note(ctxt->chr.def_handle, 0x2803, NULL, 0);   /* its value read back at sync */
        note(ctxt->chr.val_handle, VALUE_MARK, NULL, 0);
        /* NimBLE adds the CCC right after the value for notify and indicate. */
        if (chr->flags & (BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE)) {
            note(ctxt->chr.val_handle + 1, 0x2902, NULL, 0);
        }
        break;
    }
    case BLE_GATT_REGISTER_OP_DSC: {
        const ble_uuid_t *u = ctxt->dsc.dsc_def->uuid;
        note(ctxt->dsc.handle, u->type == BLE_UUID_TYPE_16 ? BLE_UUID16(u)->value : VALUE_MARK, NULL, 0);
        break;
    }
    default:
        break;
    }
}

static int cmp_handle(const void *a, const void *b)
{
    return (int)((const gatt_dbhash_attr_t *)a)->handle - (int)((const gatt_dbhash_attr_t *)b)->handle;
}

/* The declaration's value as the server serves it. */
static bool read_back(uint16_t handle, gatt_dbhash_attr_t *a)
{
    struct os_mbuf *om = NULL;
    if (ble_att_svr_read_local(handle, &om) != 0 || !om) return false;
    uint16_t len = OS_MBUF_PKTLEN(om);
    bool ok = len <= GATT_DBHASH_VALUE_MAX && os_mbuf_copydata(om, 0, len, a->value) == 0;
    a->len = (uint8_t)len;
    os_mbuf_free_chain(om);
    return ok;
}

void gatt_dbhash_sync(void)
{
    s_served = false;
    if (!s_attrs) {
        s_why = "no memory for the walk";
    } else if (s_unsupported) {
        s_why = "the table has something the walk doesn't model";
    } else {
        qsort(s_attrs, s_nattrs, sizeof(s_attrs[0]), cmp_handle);
        s_why = NULL;
        for (size_t i = 0; i < s_nattrs && !s_why; i++) {
            gatt_dbhash_attr_t *a = &s_attrs[i];
            if (a->handle != i + 1) {
                s_why = "a handle the registration didn't account for";   /* a gap or a duplicate */
            } else if (a->type >= 0x2800 && a->type <= 0x2803) {
                uint8_t expect[GATT_DBHASH_VALUE_MAX];
                uint8_t elen = a->len;
                memcpy(expect, a->value, elen);
                if (!read_back(a->handle, a)) {
                    s_why = "a declaration that didn't read back";
                } else if (a->type != 0x2803 && (a->len != elen || memcmp(expect, a->value, elen))) {
                    s_why = "a service declaration that reads back differently";
                }
            }
        }
        /* Nothing past the last: the walk covers the whole table. */
        struct os_mbuf *om = NULL;
        if (!s_why && s_nattrs && ble_att_svr_read_local((uint16_t)(s_nattrs + 1), &om) == 0) {
            s_why = "attributes past the registration";
        }
        if (om) os_mbuf_free_chain(om);
        static uint8_t buf[MAX_ATTRS * (4 + GATT_DBHASH_VALUE_MAX)];
        size_t len = s_why ? 0 : gatt_dbhash_serialize(s_attrs, s_nattrs, buf, sizeof(buf));
        static const uint8_t zero[16];
        if (!s_why && (!len || !gatt_dbhash_cmac(zero, buf, len, s_hash))) {
            s_why = "the hash failed";
        }
    }
    s_served = s_why == NULL;
    if (s_served) {
        ESP_LOGI(TAG, "database hash %02x%02x%02x%02x... over %u attributes: served", s_hash[15], s_hash[14],
                 s_hash[13], s_hash[12], (unsigned)s_nattrs);
    } else {
        ESP_LOGE(TAG, "database hash NOT served: %s", s_why);
    }
}

void gatt_dbhash_conn(uint16_t conn_handle, bool connected)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_csf[i].conn == conn_handle + 1) {
            s_csf[i].conn = 0;
            s_csf[i].csf = 0;
        }
    }
    if (connected) {
        for (int i = 0; i < MAX_CONNS; i++) {
            if (!s_csf[i].conn) {
                s_csf[i].conn = conn_handle + 1;
                s_csf[i].csf = 0;
                break;
            }
        }
    }
}

static int csf_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    int slot = -1;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_csf[i].conn == conn_handle + 1) slot = i;
    }
    uint8_t cur = slot >= 0 ? s_csf[slot].csf : 0;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &cur, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t v = 0;
    os_mbuf_copydata(ctxt->om, 0, 1, &v);
    /* A client can't turn a feature it enabled back off (Core Vol 3 Part G 7.2). */
    if (cur & ~v) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    if (slot >= 0) s_csf[slot].csf = v & 0x07;   /* robust caching, EATT, multiple notifications */
    return 0;
}

static int hash_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (!s_served) return BLE_ATT_ERR_UNLIKELY;   /* a phone then rediscovers, as without a hash */
    return os_mbuf_append(ctxt->om, s_hash, sizeof(s_hash)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int sc_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;   /* indicate only; never indicated: the table doesn't change */
}

static uint16_t s_sc_handle;

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1801),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A05), .access_cb = sc_access, .flags = BLE_GATT_CHR_F_INDICATE,
              .val_handle = &s_sc_handle },
            { .uuid = BLE_UUID16_DECLARE(0x2B29), .access_cb = csf_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = BLE_UUID16_DECLARE(0x2B2A), .access_cb = hash_access, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

void gatt_dbhash_init(void)
{
    if (!s_attrs) {
        s_attrs = heap_caps_calloc(MAX_ATTRS, sizeof(s_attrs[0]), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_attrs) s_attrs = calloc(MAX_ATTRS, sizeof(s_attrs[0]));
    }
    s_nattrs = 0;
    s_unsupported = false;
    s_served = false;
    memset(s_csf, 0, sizeof(s_csf));
    ble_hs_cfg.gatts_register_cb = on_register;
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    rc = rc ? rc : ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Generic Attribute service rc=%d", rc);
        s_unsupported = true;
    }
}

bool gatt_dbhash_served(void)
{
    return s_served;
}

void gatt_dbhash_dump(void)
{
    /* On the log, so ESP-Iris' capture (the Mosaico's console) gets it too. */
    ESP_LOGI(TAG, "dump: %s, %u attributes", s_served ? "served" : (s_why ? s_why : "?"), (unsigned)s_nattrs);
    for (size_t i = 0; s_attrs && i < s_nattrs; i++) {
        const gatt_dbhash_attr_t *a = &s_attrs[i];
        char hex[2 * GATT_DBHASH_VALUE_MAX + 1] = "";
        for (int j = 0; j < a->len; j++) snprintf(hex + 2 * j, 3, "%02x", a->value[j]);
        ESP_LOGI(TAG, "attr %u %04x %s", a->handle, a->type == VALUE_MARK ? 0 : a->type, hex);
    }
    char h[33];
    for (int i = 0; i < 16; i++) snprintf(h + 2 * i, 3, "%02x", s_hash[i]);
    ESP_LOGI(TAG, "hash %s", h);
}
