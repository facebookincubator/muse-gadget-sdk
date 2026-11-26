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
#include <assert.h>
#include "esp_log.h"
/* Retain log-tag references when compiling the settings source with host stubs. */
#undef ESP_LOGI
#undef ESP_LOGW
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#include "muse_settings.c"
#include "muse_theme.c"
static bool present[2], staged[2], fail_commit;
static uint8_t stored[2], pending[2];
static int key_index(const char *key) {
    if (!strcmp(key, "avatar_src_v1")) return 0;
    if (!strcmp(key, "ui_theme_v1")) return 1;
    return -1;
}
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { abort(); }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) {
    assert(!strcmp(ns,"muse") && mode==NVS_READWRITE); *h=1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *value) {
    assert(h==1);
    int i=key_index(key);
    if(i<0 || !present[i]) return ESP_FAIL;
    *value=stored[i]; return ESP_OK;
}
esp_err_t nvs_get_u16(nvs_handle_t h,const char *key,uint16_t *v) { (void)h;(void)key;(void)v;return ESP_FAIL; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *v,size_t *n) { (void)h;(void)key;(void)v;(void)n;return ESP_FAIL; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value) {
    assert(h==1); int i=key_index(key); assert(i>=0);
    staged[i]=true; pending[i]=value; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h==1); if(fail_commit) return ESP_FAIL;
    for(unsigned i=0;i<2;i++) if(staged[i]) { stored[i]=pending[i];present[i]=true;staged[i]=false; }
    return ESP_OK;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h,TickType_t wait) { (void)wait; assert(h==(void *)1); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { assert(h==(void *)1); return pdTRUE; }
int main(void) {
    assert(muse_settings_init()==ESP_OK); muse_theme_start();
    for(unsigned repeat=0;repeat<20;repeat++) for(int mode=0;mode<3;mode++) for(int source=0;source<2;source++) {
        muse_theme_request_mode((muse_theme_mode_t)mode);
        assert(muse_settings_set_avatar_source((muse_avatar_source_t)source)==ESP_OK);
        assert(muse_settings_init()==ESP_OK); muse_theme_start();
        assert(muse_theme_mode()==(muse_theme_mode_t)mode);
        assert(muse_settings_avatar_source()==(muse_avatar_source_t)source);
    }
    assert(muse_settings_set_avatar_source(MUSE_AVATAR_BUILTIN)==ESP_OK);
    muse_theme_request_mode(MUSE_THEME_LIGHT);
    fail_commit=true;
    assert(muse_settings_set_avatar_source(MUSE_AVATAR_PERSONAL)==ESP_FAIL);
    assert(muse_settings_avatar_source()==MUSE_AVATAR_BUILTIN);
    fail_commit=false;
    muse_theme_request_mode(MUSE_THEME_DARK); /* must not persist a failed source change */
    assert(muse_settings_init()==ESP_OK); muse_theme_start();
    assert(muse_settings_avatar_source()==MUSE_AVATAR_BUILTIN);
    assert(muse_theme_mode()==MUSE_THEME_DARK);
    fail_commit=true; muse_theme_request_mode(MUSE_THEME_LIGHT);
    assert(muse_theme_mode()==MUSE_THEME_DARK);
    fail_commit=false;
    assert(muse_settings_set_avatar_source(MUSE_AVATAR_PERSONAL)==ESP_OK);
    assert(muse_settings_init()==ESP_OK); muse_theme_start();
    assert(muse_theme_mode()==MUSE_THEME_DARK && muse_settings_avatar_source()==MUSE_AVATAR_PERSONAL);
    puts("PASS mixed settings: all source/theme pairs persist independently; failed commits cannot leak through another setting");
}
