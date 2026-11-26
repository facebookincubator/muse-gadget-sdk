# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""The factory-test service (main/factory_test.c) answers only while the
gadget isn't set up or its owner's setup window is open: on the host against
fakes, both builds (with the Muse UI and Link only)."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FAKES = r"""
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
typedef struct { const char *version; } esp_app_desc_t;
static const esp_app_desc_t s_desc = { "1.2.3" };
static inline const esp_app_desc_t *esp_app_get_description(void) { return &s_desc; }
typedef struct { int u; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
#define BLE_UUID128_INIT(...) { { 0 }, { __VA_ARGS__ } }
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_ATT_ERR_UNLIKELY 0x0e
#define BLE_ATT_ERR_INSUFFICIENT_RES 0x11
#define BLE_ATT_ERR_INSUFFICIENT_AUTHOR 0x08
#define BLE_GATT_CHR_F_READ 0x02
#define BLE_GATT_CHR_F_NOTIFY 0x10
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_HS_CONN_HANDLE_NONE 0xffff
struct os_mbuf { char data[512]; size_t len; };
static inline int os_mbuf_append(struct os_mbuf *om, const void *d, size_t n)
{
    if (om->len + n > sizeof(om->data)) return 1;
    memcpy(om->data + om->len, d, n);
    om->len += n;
    return 0;
}
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; };
typedef int ble_gatt_access_fn(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
struct ble_gatt_chr_def { const ble_uuid_t *uuid; ble_gatt_access_fn *access_cb; unsigned flags; uint16_t *val_handle; };
struct ble_gatt_svc_def { int type; const ble_uuid_t *uuid; const struct ble_gatt_chr_def *characteristics; };
extern int notifies;
static inline struct os_mbuf *ble_hs_mbuf_from_flat(const void *d, size_t n)
{
    static struct os_mbuf m;
    m.len = 0;
    os_mbuf_append(&m, d, n);
    return &m;
}
static inline int ble_gatts_notify_custom(uint16_t c, uint16_t h, struct os_mbuf *om)
{
    (void)c; (void)h; (void)om;
    notifies++;
    return 0;
}
typedef struct cJSON { char buf[400]; } cJSON;
static inline cJSON *cJSON_CreateObject(void) { cJSON *j = (cJSON *)calloc(1, sizeof(cJSON)); strcpy(j->buf, "{"); return j; }
static inline void cJSON_AddStringToObject(cJSON *j, const char *k, const char *v)
{
    size_t n = strlen(j->buf);
    snprintf(j->buf + n, sizeof(j->buf) - n, "%s\"%s\":\"%s\"", n > 1 ? "," : "", k, v);
}
static inline char *cJSON_PrintUnformatted(cJSON *j)
{
    size_t n = strlen(j->buf) + 2;
    char *s = (char *)malloc(n);
    snprintf(s, n, "%s}", j->buf);
    return s;
}
static inline void cJSON_Delete(cJSON *j) { free(j); }
#define CUSTOMER_PRODUCT_INFO_SN_LEN 16
#define CUSTOMER_PRODUCT_INFO_TEXT_LEN 16
static inline void customer_product_info_get_sn(char *o, size_t n) { snprintf(o, n, "SN1"); }
static inline void customer_product_info_get_colour_text(char *o, size_t n) { snprintf(o, n, "white"); }
static inline void customer_product_info_get_region_text(char *o, size_t n) { snprintf(o, n, "us"); }
static inline const char *identity_node_id(void) { return "gadget-1"; }
static inline const char *link_pairing_sign_factory_test(void) { return "sig"; }
extern bool fake_setup_complete, fake_provisioned, fake_window;
static inline bool config_setup_complete_or_unreadable(void) { return fake_setup_complete; }
static inline bool config_is_provisioned(void) { return fake_provisioned; }
static inline bool muse_ble_setup_window_open(void) { return fake_window; }
"""

DRIVER = r"""
#include <assert.h>
#include "factory_test.c"
int notifies;
bool fake_setup_complete, fake_provisioned, fake_window;

static int read_info(struct os_mbuf *om)
{
    om->len = 0;
    struct ble_gatt_access_ctxt c = { BLE_GATT_ACCESS_OP_READ_CHR, om };
    return s_ft_chrs[0].access_cb(7, 1, &c, NULL);
}
static int read_button(struct os_mbuf *om)
{
    om->len = 0;
    struct ble_gatt_access_ctxt c = { BLE_GATT_ACCESS_OP_READ_CHR, om };
    return s_ft_chrs[1].access_cb(7, 2, &c, NULL);
}

int main(void)
{
    struct os_mbuf om;
    /* Set up, no window: device info (identity) answers; the button read is refused; a release build notifies no
     * press, a bench one does. */
    fake_setup_complete = true;
    assert(read_info(&om) == 0 && om.len > 0 && s_armed && strstr(om.data, "\"node\":\"gadget-1\""));
    assert(read_button(&om) == BLE_ATT_ERR_INSUFFICIENT_AUTHOR && om.len == 0);
    factory_test_on_button_press();
#if CONFIG_MUSE_GADGET_AUTOMATION
    assert(notifies == 1 && s_button_value == 1);
    puts("bench build: a set-up unit's press notifies");
    return 0;
#endif
    assert(notifies == 0 && s_button_value == 0 && !s_armed);
    factory_test_on_disconnect();
    /* A device token alone counts as set up too. */
    fake_setup_complete = false;
    fake_provisioned = true;
    assert(read_button(&om) == BLE_ATT_ERR_INSUFFICIENT_AUTHOR && om.len == 0);

    /* The owner's setup window open: read OK, armed, the press notifies. */
#if CONFIG_MUSE_ENABLED
    fake_window = true;
    assert(read_info(&om) == 0 && om.len > 0 && s_armed && strstr(om.data, "\"node\":\"gadget-1\""));
    factory_test_on_button_press();
    assert(notifies == 1 && s_button_value == 1);
    assert(read_button(&om) == 0 && om.len == 1 && om.data[0] == 1);
    factory_test_on_disconnect();
    /* Armed in the window, which closes before the press: no notify. */
    assert(read_info(&om) == 0 && s_armed);
    fake_window = false;
    factory_test_on_button_press();
    assert(notifies == 1 && !s_armed);
    factory_test_on_disconnect();
#else
    fake_window = true;   /* no such window without the Muse UI */
    assert(read_button(&om) == BLE_ATT_ERR_INSUFFICIENT_AUTHOR);
    fake_window = false;
#endif

    /* Not set up: the jig works as before. */
    fake_provisioned = false;
    int before = notifies;
    assert(read_info(&om) == 0 && om.len > 0 && s_armed);
    factory_test_on_button_press();
    assert(notifies == before + 1);
    assert(read_button(&om) == 0 && om.data[0] == 1);
    assert(factory_test_allowed(false, false) && factory_test_allowed(true, true) && !factory_test_allowed(true, false));
    puts("factory test: device info open; the button refused when set up outside the window; window and unset gadgets answer");
    return 0;
}
"""


class FactoryTestAccess(unittest.TestCase):
    def run_build(self, muse: bool, bench: bool = False) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "fakes.h").write_text(FAKES)
            for name in ("esp_log.h", "esp_app_desc.h", "host/ble_hs.h", "host/ble_uuid.h", "host/ble_gatt.h",
                         "cJSON.h", "identity.h", "link_pairing.h", "customer_product_info.h", "config_store.h",
                         "muse_ble.h"):
                p = d / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('#include "fakes.h"\n')
            (d / "sdkconfig.h").write_text(("#define CONFIG_MUSE_ENABLED 1\n" if muse else "\n") +
                                           ("#define CONFIG_MUSE_GADGET_AUTOMATION 1\n" if bench else ""))
            (d / "factory_test.h").write_text((ROOT / "main" / "factory_test.h").read_text())
            (d / "factory_test.c").write_text((ROOT / "main" / "factory_test.c").read_text())
            (d / "driver.c").write_text(DRIVER)
            exe = str(d / "ft")
            cc = shlex.split(os.environ.get("CC", "cc"))
            subprocess.run(cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                                 "-fsanitize=address,undefined", "-I", tmp, str(d / "driver.c"), "-o", exe],
                           check=True)
            return subprocess.run([exe], check=True, capture_output=True, text=True,
                                  env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1"}).stdout

    def test_with_the_muse_ui(self) -> None:
        self.assertIn("refused when set up outside the window", self.run_build(True))

    def test_link_only(self) -> None:
        self.assertIn("refused when set up outside the window", self.run_build(False))

    def test_a_bench_build_notifies_a_set_up_units_press(self) -> None:
        self.assertIn("bench build: a set-up unit's press notifies", self.run_build(False, bench=True))

    def test_the_service_stays_registered(self) -> None:
        # NimBLE can't drop a service without restarting the stack: it stays, and answers per the rule.
        ble = (ROOT / "main" / "ble_server.c").read_text()
        self.assertIn("const struct ble_gatt_svc_def *ft = factory_test_svcs();", ble)


if __name__ == "__main__":
    unittest.main()
