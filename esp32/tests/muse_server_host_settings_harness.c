/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* The Muse host setting takes only the servers muse_server_hosts.h allows
 * (test_link_server_hosts.py). */
#include <assert.h>
#include "esp_log.h"
#undef ESP_LOGI
#undef ESP_LOGW
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#include "muse_settings.c"

static char saved[MUSE_HOST_MAX + 1];
static bool has_saved;
static unsigned writes, notices;

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { assert(!"must preserve existing settings"); return ESP_FAIL; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) {
    assert(!strcmp(ns, "muse") && mode == NVS_READWRITE);
    *h = 1;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) { (void)h; (void)key; (void)v; return ESP_FAIL; }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v) { (void)h; (void)key; (void)v; return ESP_FAIL; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *v, size_t *n) {
    assert(h == 1);
    if (strcmp(key, "host") || !has_saved) return ESP_FAIL;
    assert(*n > strlen(saved));
    strcpy(v, saved);
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v) {
    assert(h == 1 && !strcmp(key, "host"));
    snprintf(saved, sizeof(saved), "%s", v);
    has_saved = true;
    writes++;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { assert(h == 1); return ESP_OK; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t wait) { (void)wait; assert(h == (void *)1); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { assert(h == (void *)1); return pdTRUE; }
static void changed(muse_setting_t what) { assert(what == MUSE_SETTING_HATCH); notices++; }

static void expect_host(const char *want) {
    char host[MUSE_HOST_MAX + 1];
    muse_settings_hatch_host(host);
    assert(!strcmp(host, want));
}

int main(void) {
    static const char *const refused[] = {
        "evilmetaaivm.com", "metaaivm.com.evil.com", "evil.hatch.metaaivm.com",
        "vm-1.metaaivm.com", "hatch.metaaivm.com:8443", "user@hatch.metaaivm.com",
        "https://hatch.metaaivm.com", "hatch.metaaivm.com/", "evil.example",
    };
    const size_t n = sizeof(refused) / sizeof(refused[0]);

    assert(muse_settings_init() == ESP_OK);
    expect_host(DEFAULT_HOST);
    muse_settings_set_listener(changed);

    // A server this build doesn't allow is refused and changes nothing.
    for (size_t i = 0; i < n; i++) {
        assert(muse_settings_set_hatch_host(refused[i]) == (bool)CUSTOM_HOSTS);
        if (!CUSTOM_HOSTS) {
            assert(!writes && !notices);
            expect_host(DEFAULT_HOST);
        }
    }
    assert(muse_settings_set_hatch_host("hatch.metaaivm.com"));
    expect_host("hatch.metaaivm.com");
    assert(muse_settings_set_hatch_host(""));
    expect_host(DEFAULT_HOST);

    // One saved by earlier firmware gives way to the default on load.
    for (size_t i = 0; i < n; i++) {
        snprintf(saved, sizeof(saved), "%s", refused[i]);
        has_saved = true;
        assert(muse_settings_init() == ESP_OK);
        expect_host(CUSTOM_HOSTS ? refused[i] : DEFAULT_HOST);
    }
    snprintf(saved, sizeof(saved), "%s", "hatch.metaaivm.com");
    assert(muse_settings_init() == ESP_OK);
    expect_host("hatch.metaaivm.com");
    puts("muse server host setting: refused writes, saved values and defaults passed");
    return 0;
}
