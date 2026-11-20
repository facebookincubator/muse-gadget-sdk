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
#include "muse_theme.h"
static bool present, fail_set, fail_commit;
static uint8_t stored, pending;
static unsigned commits;
#ifdef THEME_TEST_ASYNC
static void (*queued_task)(void *);
static void *queued_arg;
static unsigned workers, deleted;
static bool fail_spawn;
static int request_during_commit = -1;
BaseType_t xTaskCreateWithCaps(void (*task)(void *), const char *name, unsigned stack,
                             void *arg, unsigned priority, TaskHandle_t *handle, unsigned caps) {
    (void)name; (void)priority; (void)handle;
    assert(stack == 4096 && (caps & MALLOC_CAP_INTERNAL) && !queued_task);
    if (fail_spawn) return 0;
    queued_task = task; queued_arg = arg; ++workers;
    return pdPASS;
}
void vTaskDeleteWithCaps(TaskHandle_t task) { assert(!task); ++deleted; }
static void run_worker(void) {
    assert(queued_task);
    void (*task)(void *) = queued_task;
    void *arg = queued_arg;
    queued_task = NULL;
    task(arg);
}
#endif
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { assert(!"must preserve existing settings"); return ESP_FAIL; }
esp_err_t nvs_open(const char *ns,nvs_open_mode_t mode,nvs_handle_t *h) {
    assert(!strcmp(ns,"muse") && mode==NVS_READWRITE); *h=1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *v) {
    assert(h==1);
    if(!strcmp(key,"ui_theme_v1") && present) { *v=stored;return ESP_OK; }
    return ESP_FAIL;
}
esp_err_t nvs_get_u16(nvs_handle_t h,const char *key,uint16_t *v) { (void)h;(void)key;(void)v;return ESP_FAIL; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *v,size_t *n) { (void)h;(void)key;(void)v;(void)n;return ESP_FAIL; }
esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v) {
    assert(h==1 && !strcmp(key,"ui_theme_v1"));
    if (fail_set) {
        return ESP_FAIL;
    }
    pending=v;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h==1);++commits;
#ifdef THEME_TEST_ASYNC
    if (request_during_commit >= 0) {
        int mode = request_during_commit; request_during_commit = -1;
        muse_theme_request_mode((muse_theme_mode_t)mode);
    }
#endif
    if (fail_commit) {
        return ESP_FAIL;
    }
    stored=pending;
    present=true;
    return ESP_OK;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t wait) { (void)wait; assert(h==(void *)1); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { assert(h==(void *)1); return pdTRUE; }
#include "muse_theme.c"
int main(void) {
    assert(muse_settings_init()==ESP_OK && muse_settings_theme()==MUSE_THEME_AUTO);
    muse_theme_start();assert(muse_theme_mode()==MUSE_THEME_AUTO);
#ifdef THEME_TEST_ASYNC
    for (unsigned change = 0; change < 1000; ++change)
        muse_theme_request_mode((muse_theme_mode_t)(change % 3));
    muse_theme_request_mode(MUSE_THEME_LIGHT);
    assert(workers == 1 && !commits && muse_settings_theme() == MUSE_THEME_AUTO);
    run_worker();
    assert(stored == MUSE_THEME_LIGHT && commits == 1 && deleted == 1 && !queued_task);
    request_during_commit = MUSE_THEME_AUTO;
    muse_theme_request_mode(MUSE_THEME_DARK);
    run_worker();
    assert(stored == MUSE_THEME_AUTO && muse_theme_mode() == MUSE_THEME_AUTO && !queued_task);
    fail_set = true;
    muse_theme_request_mode(MUSE_THEME_DARK); run_worker();
    assert(muse_theme_mode() == MUSE_THEME_AUTO && muse_theme_save_error() && !queued_task);
    fail_set = false; fail_commit = true; request_during_commit = MUSE_THEME_LIGHT;
    muse_theme_request_mode(MUSE_THEME_DARK); run_worker();
    assert(muse_theme_mode() == MUSE_THEME_LIGHT && queued_task);
    fail_commit = false; run_worker();
    assert(stored == MUSE_THEME_LIGHT && !muse_theme_save_error() && workers == deleted);
    fail_spawn = true; muse_theme_request_mode(MUSE_THEME_DARK);
    assert(muse_theme_mode() == MUSE_THEME_LIGHT && muse_theme_save_error() && !queued_task);
    assert(muse_settings_init() == ESP_OK && muse_settings_theme() == MUSE_THEME_LIGHT);
    puts("PASS async theme: rapid requests coalesce, latest change wins during commit/failure, low-memory rollback, restart persistence");
#else
    muse_theme_request_mode(MUSE_THEME_LIGHT);
    assert(muse_settings_theme()==MUSE_THEME_LIGHT && stored==MUSE_THEME_LIGHT && commits==1);
    assert(muse_settings_init()==ESP_OK && muse_settings_theme()==MUSE_THEME_LIGHT);
    muse_theme_request_mode(MUSE_THEME_LIGHT);assert(commits==1);
    assert(muse_settings_set_theme((muse_theme_mode_t)99)==ESP_ERR_INVALID_ARG && commits==1);
    fail_set=true;muse_theme_request_mode(MUSE_THEME_DARK);
    assert(muse_theme_mode()==MUSE_THEME_LIGHT && muse_settings_theme()==MUSE_THEME_LIGHT && muse_theme_save_error());
    fail_set=false;fail_commit=true;muse_theme_request_mode(MUSE_THEME_DARK);
    assert(muse_theme_mode()==MUSE_THEME_LIGHT && muse_settings_theme()==MUSE_THEME_LIGHT && muse_theme_save_error() && pending==MUSE_THEME_LIGHT);
    fail_commit=false;muse_theme_request_mode(MUSE_THEME_DARK);
    assert(muse_theme_mode()==MUSE_THEME_DARK && stored==MUSE_THEME_DARK && !muse_theme_save_error());
    stored=255;assert(muse_settings_init()==ESP_OK && muse_settings_theme()==MUSE_THEME_AUTO);
    puts("PASS theme setting: upgrade default, persistence, invalid value, failed write/commit rollback");
#endif
}
