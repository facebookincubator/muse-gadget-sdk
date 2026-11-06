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

#include "muse_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"

#include "muse_chat.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_voice.h"
#include "muse_wifi.h"

static const char *TAG = "muse_ble";

#define CMD_MAX 600

void ble_store_config_init(void);

/* 4d757365-000X-4000-8000-6a6f6c6c7900, little-endian. */
#define MUSE_UUID(x) BLE_UUID128_INIT(0x00, 0x79, 0x6c, 0x6c, 0x6f, 0x6a, 0x00, 0x80, 0x00, 0x40, x, 0x00, 0x65, 0x73, 0x75, 0x4d)
static const ble_uuid128_t SVC_UUID = MUSE_UUID(0x01);
static const ble_uuid128_t CMD_UUID = MUSE_UUID(0x02);
static const ble_uuid128_t STATUS_UUID = MUSE_UUID(0x03);

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_handle;
static volatile uint32_t s_passkey;
static volatile bool s_secure;
static char s_name[32];   /* the whole "MuseGadget-XXXXXX", not a prefix of it */
static char s_last[64] = "ready";

/* ---- commands ---- */

static const char *wifi_state_name(muse_wifi_state_t st)
{
    static const char *const names[] = { "off", "no_network", "connecting", "connected", "failed", "not_nearby" };
    return (unsigned)st < sizeof(names) / sizeof(names[0]) ? names[st] : "unknown";
}

/* Escapes only what can appear in SSIDs/hosts; fine for a status blob. */
static void json_str(char *out, size_t len, const char *s)
{
    size_t o = 0;
    for (; *s && o + 3 < len; s++) {
        if (*s == '"' || *s == '\\') {
            out[o++] = '\\';
        }
        out[o++] = (unsigned char)*s < 0x20 ? '?' : *s;
    }
    out[o] = '\0';
}

static int build_status(char *out, size_t len)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    muse_power_t p = muse_state_power();
    char host[MUSE_HOST_MAX + 1], vm[MUSE_VM_MAX + 1];
    muse_settings_hatch_host(host);
    muse_settings_hatch_vm(vm);

    char ssid_e[2 * MUSE_SSID_MAX + 1], host_e[2 * MUSE_HOST_MAX + 1], vm_e[2 * MUSE_VM_MAX + 1], last_e[128];
    json_str(ssid_e, sizeof(ssid_e), w.ssid);
    json_str(host_e, sizeof(host_e), host);
    json_str(vm_e, sizeof(vm_e), vm);
    json_str(last_e, sizeof(last_e), s_last);

    return snprintf(out, len,
                    "{\"name\":\"%s\",\"fw\":\"%s\",\"battery\":%d,"
                    "\"wifi\":{\"on\":%s,\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},"
                    "\"hatch\":{\"host\":\"%s\",\"vm\":\"%s\",\"token\":%s,\"state\":\"%s\"},"
                    "\"link\":{\"paired\":%s,\"state\":\"%s\"},"
                    "\"volume\":%d,\"speaker\":%s,\"mic_gain\":%d,\"brightness\":%d,\"sleep\":%d,\"last\":\"%s\"}",
                    s_name, esp_app_get_description()->version, p.battery_pct,
                    muse_settings_wifi_on() ? "true" : "false", wifi_state_name(w.state), ssid_e, w.ip, w.rssi,
                    host_e, vm_e, muse_settings_hatch_token_len() ? "true" : "false", muse_hatch_state_name(h.state),
                    muse_link_hatch_linked() ? "true" : "false", muse_link_state_name(muse_link_state()),
                    muse_settings_volume(), muse_settings_speaker_on() ? "true" : "false",
                    muse_settings_mic_gain(), muse_settings_brightness(),
                    muse_settings_sleep_s(), last_e);
}

static bool parse_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    long n = strtol(v, &end, 10);
    if (!*v || *end || n < lo || n > hi) {
        return false;
    }
    *out = (int)n;
    return true;
}

/* Wi-Fi credentials arrive as two writes; they're applied on wifi.connect. */
static char s_pending_ssid[MUSE_SSID_MAX + 1];
static char s_pending_pass[MUSE_PASS_MAX + 1];

static void run_command(char *cmd)
{
    char *v = strchr(cmd, '=');
    if (v) {
        *v++ = '\0';
    } else {
        v = "";
    }
    int n;
    const char *res = "ok";

    if (!strcmp(cmd, "wifi.ssid")) {
        strlcpy(s_pending_ssid, v, sizeof(s_pending_ssid));
        s_pending_pass[0] = '\0';
    } else if (!strcmp(cmd, "wifi.pass")) {
        strlcpy(s_pending_pass, v, sizeof(s_pending_pass));
    } else if (!strcmp(cmd, "wifi.connect")) {
        if (!s_pending_ssid[0]) {
            res = "error: send wifi.ssid first";
        } else {
            muse_settings_set_wifi_on(true);
            muse_settings_set_wifi(s_pending_ssid, s_pending_pass);
        }
    } else if (!strcmp(cmd, "wifi.forget")) {
        if (v[0]) {
            muse_wifi_forget(v);   /* just that one */
        } else {
            muse_settings_set_wifi("", "");   /* every saved network */
        }
    } else if (!strcmp(cmd, "hatch.host")) {
        muse_settings_set_hatch_host(v);
    } else if (!strcmp(cmd, "hatch.vm")) {
        muse_settings_set_hatch_vm(v);
    } else if (!strcmp(cmd, "hatch.token") || !strcmp(cmd, "hatch.token+")) {
        if (muse_settings_set_hatch_token(v, cmd[11] == '+') != ESP_OK) {
            res = "error: token too long";
        }
    } else if (!strcmp(cmd, "hatch.test")) {
        muse_hatch_test();
    } else if (!strcmp(cmd, "test.loopback")) {
        muse_voice_request_loopback();
    } else if (!strcmp(cmd, "volume") && parse_int(v, 0, 100, &n)) {
        muse_settings_set_volume(n);
    } else if (!strcmp(cmd, "speaker") && parse_int(v, 0, 1, &n)) {
        muse_settings_set_speaker_on(n);
    } else if (!strcmp(cmd, "mic_gain") && parse_int(v, 0, MUSE_MIC_GAIN_MAX, &n)) {
        muse_settings_set_mic_gain(n);
    } else if (!strcmp(cmd, "brightness") && parse_int(v, 10, 100, &n)) {
        muse_settings_set_brightness(n);
    } else if (!strcmp(cmd, "sleep") && parse_int(v, 0, 3600, &n)) {
        muse_settings_set_sleep_s(n);
    } else {
        res = "error: unknown command or bad value";
    }

    /* Never echo secrets back. */
    bool secret = !strcmp(cmd, "wifi.pass") || !strncmp(cmd, "hatch.token", 11);
    snprintf(s_last, sizeof(s_last), "%s: %s", cmd, res);
    ESP_LOGI(TAG, "cmd %s%s%s -> %s", cmd, secret ? "" : "=", secret ? "" : v, res);
    muse_state_poke();

    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_last, strlen(s_last));
        if (om) {
            ble_gatts_notify_custom(s_conn, s_status_handle, om);
        }
    }
}

void muse_ble_command(char *cmd)
{
    run_command(cmd);
}

int muse_ble_status_json(char *out, size_t len)
{
    return build_status(out, len);
}

static int on_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char buf[512];
        int n = build_status(buf, sizeof(buf));
        n = n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1;
        return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char *cmd = malloc(CMD_MAX + 1);
        if (!cmd) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, cmd, CMD_MAX, &len);
        if (rc == 0) {
            cmd[len] = '\0';
            while (len && (cmd[len - 1] == '\n' || cmd[len - 1] == '\r')) {
                cmd[--len] = '\0';
            }
            run_command(cmd);
        }
        free(cmd);
        return rc == 0 ? 0 : BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &CMD_UUID.u,
                .access_cb = on_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                .uuid = &STATUS_UUID.u,
                .access_cb = on_access,
                .val_handle = &s_status_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN |
                         BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- Hooks for Link's BLE server ---- */

const struct ble_gatt_svc_def *muse_ble_services(void)
{
    return SERVICES;
}

void muse_ble_configure_host(void)
{
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
}

void muse_ble_set_name(const char *name)
{
    strlcpy(s_name, name, sizeof(s_name));
}

/*
 * Link's connection is shared with the Muse app, which doesn't pair, so this
 * never asks for security itself: a phone that touches the Muse service gets
 * "insufficient authentication" and pairs, and then the passkey shows up.
 */
int muse_ble_gap_event(struct ble_gap_event *ev)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_secure = false;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_passkey = 0;
        s_secure = false;
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = 100000 + esp_random() % 900000 };
            s_passkey = io.passkey;
            muse_state_poke();
            ESP_LOGI(TAG, "pairing: showing passkey");
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        s_passkey = 0;
        struct ble_gap_conn_desc d;
        if (ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0) {
            s_secure = d.sec_state.encrypted && d.sec_state.authenticated;
        }
        ESP_LOGI(TAG, "encryption %s (%s)", ev->enc_change.status == 0 ? "on" : "failed",
                 s_secure ? "authenticated" : "not authenticated");
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The phone forgot us; drop the old bond and pair again. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
            ble_store_util_delete_peer(&d.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

void muse_ble_apply(void)
{
    if (!muse_settings_ble_on() && s_conn != BLE_HS_CONN_HANDLE_NONE && s_secure) {
        /* Drop a phone-setup session; the Muse app's unencrypted setup link stays. */
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    muse_link_ble_apply();
}

/* Link advertises while it is unpaired regardless of the setting. */
static bool link_advertising(void)
{
    muse_link_state_t st = muse_link_state();
    return !muse_link_hatch_linked() &&
           (st == MUSE_LINK_UNPAIRED || st == MUSE_LINK_PAIRING || st == MUSE_LINK_CONFIRM);
}

void muse_ble_status(muse_ble_status_t *out)
{
    out->passkey = s_passkey;
    out->secure = s_secure;
    strlcpy(out->name, s_name[0] ? s_name : "Muse", sizeof(out->name));
    if (!muse_link_ble_started() || !(muse_settings_ble_on() || link_advertising())) {
        out->state = s_conn != BLE_HS_CONN_HANDLE_NONE ? MUSE_BLE_CONNECTED : MUSE_BLE_OFF;
    } else if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        out->state = MUSE_BLE_CONNECTED;
    } else {
        out->state = MUSE_BLE_ADVERTISING;
    }
}

void muse_ble_forget_all(void)
{
    if (muse_link_ble_started()) {
        ble_store_clear();
        ESP_LOGI(TAG, "forgot all paired phones");
    }
}
