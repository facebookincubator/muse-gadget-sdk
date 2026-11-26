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

/* muse_settings.c's Muse server, VM and device token on the host
 * (tests/test_link_reset_forgets_pairings.py): an unpair or reset puts all
 * three back to their defaults. */
#include <assert.h>
#include "esp_log.h"
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif
#ifndef ESP_LOGE
#define ESP_LOGE(tag, ...) ((void)(tag))
#endif
/* Keep the log tag referenced with the host stubs. */
#undef ESP_LOGI
#undef ESP_LOGW
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#include "muse_settings.c"

static struct { char key[16]; char val[MUSE_TOKEN_MAX + 1]; } nv[8];
static unsigned notices;

static int find(const char *key)
{
    for (int i = 0; i < 8; i++) {
        if (!strcmp(nv[i].key, key)) return i;
    }
    return -1;
}
static const char *stored(const char *key)
{
    int i = find(key);
    return i < 0 ? NULL : nv[i].val;
}
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { assert(!"must preserve existing settings"); return ESP_FAIL; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) { (void)h; (void)key; (void)v; return ESP_FAIL; }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v) { (void)h; (void)key; (void)v; return ESP_FAIL; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *v, size_t *n)
{
    (void)h;
    int i = find(key);
    if (i < 0) return ESP_FAIL;
    assert(strlen(nv[i].val) < *n);
    strcpy(v, nv[i].val);
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v)
{
    (void)h;
    int i = find(key);
    for (int j = 0; i < 0 && j < 8; j++) {
        if (!nv[j].key[0]) { i = j; strcpy(nv[i].key, key); }
    }
    assert(i >= 0 && strlen(v) <= MUSE_TOKEN_MAX);
    strcpy(nv[i].val, v);
    return ESP_OK;
}
static int fail_commit;   /* the store refuses the commit */
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return fail_commit ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    int i = find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    memset(&nv[i], 0, sizeof(nv[i]));
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t wait) { (void)wait; assert(h == (void *)1); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { assert(h == (void *)1); return pdTRUE; }
static void changed(muse_setting_t what) { assert(what == MUSE_SETTING_HATCH); ++notices; }

int main(void)
{
    assert(muse_settings_init() == ESP_OK);
    muse_settings_set_listener(changed);
    char host[MUSE_HOST_MAX + 1], vm[MUSE_VM_MAX + 1];
    assert(muse_settings_set_hatch_host("other.example.com"));   /* a build that allows custom hosts */
    muse_settings_set_hatch_vm("vm-b");
    assert(muse_settings_set_hatch_token("tok-5", false) == ESP_OK);
    unsigned before = notices;
    fail_commit = 1;   /* a store that doesn't take it: reported, never a success */
    assert(!muse_settings_forget_server());
    fail_commit = 0;
    assert(muse_settings_set_hatch_token("tok-5", false) == ESP_OK);
    before = notices;
    assert(muse_settings_forget_server());
    muse_settings_hatch_host(host);
    muse_settings_hatch_vm(vm);
    assert(!strcmp(host, "hatch.metaaivm.com") && !vm[0] && muse_settings_hatch_token_len() == 0);
    assert(!stored("host") && !stored("vm") && !stored("token") && notices == before + 1);
    assert(muse_settings_init() == ESP_OK);   /* after the restart a reset ends with */
    muse_settings_hatch_host(host);
    assert(!strcmp(host, "hatch.metaaivm.com") && muse_settings_hatch_token_len() == 0);
    puts("settings reset: Muse server, VM and device token back to their defaults");
    return 0;
}
