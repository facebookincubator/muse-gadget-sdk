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
#include "muse_settings.c"
static bool present, fail_set, fail_commit;
static uint8_t stored, pending;
static unsigned commits, notices;
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { assert(!"must preserve existing settings"); return ESP_FAIL; }
esp_err_t nvs_open(const char *ns,nvs_open_mode_t mode,nvs_handle_t *h) {
    assert(!strcmp(ns,"muse") && mode==NVS_READWRITE); *h=1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *v) {
    assert(h==1);
    if(!strcmp(key,"avatar_src_v1") && present) { *v=stored;return ESP_OK; }
    return ESP_FAIL;
}
esp_err_t nvs_get_u16(nvs_handle_t h,const char *key,uint16_t *v) { (void)h;(void)key;(void)v;return ESP_FAIL; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *v,size_t *n) { (void)h;(void)key;(void)v;(void)n;return ESP_FAIL; }
esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v) {
    assert(h==1 && !strcmp(key,"avatar_src_v1")); if(fail_set)return ESP_FAIL;pending=v;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { assert(h==1);++commits;if(fail_commit)return ESP_FAIL;stored=pending;present=true;return ESP_OK; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t wait) { (void)wait; assert(h==(void *)1); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { assert(h==(void *)1); return pdTRUE; }
static void changed(muse_setting_t what) { assert(what==MUSE_SETTING_AVATAR);++notices; }
int main(void) {
    assert(muse_settings_init()==ESP_OK);
    assert(muse_settings_avatar_source()==MUSE_AVATAR_BUILTIN && !commits);
    muse_settings_set_listener(changed);
    assert(muse_settings_set_avatar_source(MUSE_AVATAR_PERSONAL)==ESP_OK);
    assert(muse_settings_avatar_source()==MUSE_AVATAR_PERSONAL && stored==1 && commits==1);
    assert(muse_settings_init()==ESP_OK && muse_settings_avatar_source()==MUSE_AVATAR_PERSONAL);
    assert(muse_settings_set_avatar_source(MUSE_AVATAR_PERSONAL)==ESP_OK && commits==1);
    unsigned before=notices;
    assert(muse_settings_set_avatar_source((muse_avatar_source_t)99)==ESP_ERR_INVALID_ARG && notices==before && commits==1);
    fail_set=true;
    assert(muse_settings_set_avatar_source(MUSE_AVATAR_BUILTIN)==ESP_FAIL && muse_settings_avatar_source()==MUSE_AVATAR_PERSONAL && notices==before);
    fail_set=false;fail_commit=true;
    assert(muse_settings_set_avatar_source(MUSE_AVATAR_BUILTIN)==ESP_FAIL && muse_settings_avatar_source()==MUSE_AVATAR_PERSONAL && notices==before);
    fail_commit=false;
    assert(muse_settings_set_avatar_source(MUSE_AVATAR_BUILTIN)==ESP_OK && muse_settings_avatar_source()==MUSE_AVATAR_BUILTIN);
    stored=255;
    assert(muse_settings_init()==ESP_OK && muse_settings_avatar_source()==MUSE_AVATAR_BUILTIN);
    puts("avatar settings: upgrade default, saved source, invalid values, write failures, unchanged-value writes passed");
}
