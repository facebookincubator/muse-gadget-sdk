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

// Apps for the agent and scripts (app.*): pages below the face, built by
// components/muse/muse_apps.c from JSON. Their descriptions are kept on the
// scripts' LittleFS partition, /scripts/apps/<id>.json, and built again at
// boot before scripts start; an app a script defines belongs to it, and goes
// when the script is deleted. Files are written on the esp_timer task: a
// script's own task mustn't write flash.
//
// The firmware carries a library of example apps (main/apps: a page and its
// script each), installed at the first boot for people to use and for the
// agent to learn from (app.library, app.example). A newer firmware updates
// the ones nobody has changed, and doesn't bring back ones taken away.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "muse_apps.h"
#include "muse_hw_commands_priv.h"
#include "muse_mem.h"

static const char *TAG = "link.apps";

#define DIR_PATH "/scripts/apps"
#define LIBRARY_STATE "/scripts/library.json"   // what was installed: {"apps": {id: [page hash, script hash]}}
#define DEF_MAX (24 * 1024)
#define IMAGE_MAX (1024 * 1024)
#define SAVE_DELAY_US 300000

// ---- Saving, on the esp_timer task -------------------------------------------

typedef struct pending {
    char id[16];
    char owner[32];        // the script it belongs to, or ""
    char *json;            // NULL: delete the file
    struct pending *next;
} pending_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_save_lock;   // held through a whole save, for remove_owned
static pending_t *s_pending;
static esp_timer_handle_t s_saver;

static void path_of(char *out, size_t n, const char *id) {
    snprintf(out, n, DIR_PATH "/%s.json", id);
}

static void save_now(void *arg) {
    (void)arg;
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pending_t *p = s_pending;
    s_pending = NULL;
    xSemaphoreGive(s_lock);
    mkdir(DIR_PATH, 0775);
    while (p) {
        char path[64];
        path_of(path, sizeof(path), p->id);
        if (p->json) {
            FILE *f = fopen(path, "wb");
            if (!f || fwrite(p->json, 1, strlen(p->json), f) != strlen(p->json)) {
                ESP_LOGW(TAG, "couldn't save app %s", p->id);
            }
            if (f) fclose(f);
        } else {
            unlink(path);
        }
        pending_t *next = p->next;
        free(p->json);
        free(p);
        p = next;
    }
    xSemaphoreGive(s_save_lock);
}

// Queues a save (json) or delete (NULL) of the app's file; takes json.
static void queue_save(const char *id, char *json, const char *owner) {
    if (!muse_script_storage()) {
        free(json);
        return;
    }
    pending_t *p = calloc(1, sizeof(*p));
    if (!p) {
        free(json);
        return;
    }
    strlcpy(p->id, id, sizeof(p->id));
    strlcpy(p->owner, owner ? owner : "", sizeof(p->owner));
    p->json = json;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (pending_t **q = &s_pending; *q; q = &(*q)->next) {   // a newer save replaces an older one
        if (!strcmp((*q)->id, id)) {
            pending_t *old = *q;
            *q = old->next;
            free(old->json);
            free(old);
            break;
        }
    }
    p->next = s_pending;
    s_pending = p;
    xSemaphoreGive(s_lock);
    esp_timer_stop(s_saver);
    esp_timer_start_once(s_saver, SAVE_DELAY_US);
}

// ---- Images from the SD card or the web ----------------------------------------

typedef struct {
    char app[16], widget[16];
    char *src;
} image_job_t;

static QueueHandle_t s_images;

static void image_task(void *arg) {
    (void)arg;
    for (;;) {
        image_job_t job;
        if (xQueueReceive(s_images, &job, portMAX_DELAY) != pdTRUE) continue;
        uint8_t *data = NULL;
        size_t len = 0;
        const char *err;
        if (!strncmp(job.src, "sd:", 3)) data = hw_storage_load(job.src + 3, IMAGE_MAX, &len, &err);
        else err = hw_download(job.src + 4, IMAGE_MAX, &data, &len);
        if (data && !muse_apps_set_jpeg(job.app, job.widget, data, len)) err = "not a baseline JPEG";
        if (err) ESP_LOGW(TAG, "%s.%s: %s: %s", job.app, job.widget, job.src, err);
        free(data);
        free(job.src);
    }
}

// From muse_apps.c, on any task: fetched here, off the caller's task.
static void load_image(const char *app, const char *widget, const char *src) {
    if (!s_images) {
        s_images = xQueueCreate(8, sizeof(image_job_t));
        // Stack in PSRAM: downloads and card reads never write internal flash.
        if (!s_images || xTaskCreatePinnedToCoreWithCaps(image_task, "app_images", 8192, NULL, 3, NULL,
                                                         tskNO_AFFINITY, MUSE_BIG_CAPS) != pdPASS) {
            if (s_images) vQueueDelete(s_images);
            s_images = NULL;   // all or nothing: the next image tries again
            return;
        }
    }
    image_job_t job = { .src = strdup(src) };
    strlcpy(job.app, app, sizeof(job.app));
    strlcpy(job.widget, widget, sizeof(job.widget));
    if (!job.src || xQueueSend(s_images, &job, 0) != pdTRUE) free(job.src);
}

// ---- The library ---------------------------------------------------------------

#define LIBRARY(X) X(apps) X(clock) X(timer) X(camera) X(sound) X(light) X(system) X(dice)

typedef struct {
    const char *id, *json, *lua;
} example_t;

#define DECLARE(id) extern const char _binary_##id##_json_start[], _binary_##id##_lua_start[];
LIBRARY(DECLARE)
#define ENTRY(id) { #id, _binary_##id##_json_start, _binary_##id##_lua_start },
static const example_t EXAMPLES[] = { LIBRARY(ENTRY) };
#define EXAMPLES_N (sizeof(EXAMPLES) / sizeof(EXAMPLES[0]))

static const example_t *example(const char *id) {
    for (size_t i = 0; id && i < EXAMPLES_N; i++) {
        if (!strcmp(EXAMPLES[i].id, id)) return &EXAMPLES[i];
    }
    return NULL;
}

static uint32_t fnv(const char *p, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ (uint8_t)*p++) * 16777619u;
    return h;
}

// A file's contents (PSRAM, NUL-terminated), or NULL.
static char *read_file(const char *path, size_t max) {
    FILE *f = fopen(path, "rb");
    char *buf = f ? heap_caps_malloc(max + 1, MUSE_BIG_CAPS) : NULL;
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    if (f) fclose(f);
    if (buf) buf[n] = '\0';
    return buf;
}

static uint32_t file_hash(const char *path) {
    char *s = read_file(path, DEF_MAX);
    uint32_t h = s ? fnv(s, strlen(s)) : 0;
    free(s);
    return h;
}

static bool write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(data, 1, strlen(data), f) == strlen(data);
    if (f) ok = fclose(f) == 0 && ok;
    return ok;
}

static void script_path(char *out, size_t n, const char *id) {
    snprintf(out, n, "/scripts/%s.lua", id);
}

// An example's page as it's kept: owned by its script, which has its name.
static char *example_page(const example_t *ex) {
    cJSON *def = cJSON_Parse(ex->json);
    if (!def) return NULL;
    cJSON_DeleteItemFromObject(def, "_owner");
    cJSON_AddStringToObject(def, "_owner", ex->id);
    char *json = cJSON_PrintUnformatted(def);
    cJSON_Delete(def);
    return json;
}

// Writes an example's page and script (to start at boot); starts the script
// if run. The page is built by the caller, or at boot by muse_hw_apps_load.
static bool put_example(const example_t *ex, bool run, uint32_t hashes[2], char *err, size_t errlen) {
    char *page = example_page(ex);
    char path[64];
    path_of(path, sizeof(path), ex->id);
    mkdir(DIR_PATH, 0775);
    bool ok = page && write_file(path, page);
    if (ok) hashes[0] = fnv(page, strlen(page));
    cJSON_free(page);
    if (!ok) {
        snprintf(err, errlen, "couldn't save its page");
        return false;
    }
    hashes[1] = fnv(ex->lua, strlen(ex->lua));
    return muse_script_put(ex->id, ex->lua, run, err, errlen);
}

static cJSON *library_state(void) {
    char *s = read_file(LIBRARY_STATE, DEF_MAX);
    cJSON *st = s ? cJSON_Parse(s) : NULL;
    free(s);
    if (!cJSON_IsObject(st)) {
        cJSON_Delete(st);
        st = cJSON_CreateObject();
    }
    if (!cJSON_IsObject(cJSON_GetObjectItem(st, "apps"))) {
        cJSON_DeleteItemFromObject(st, "apps");
        cJSON_AddObjectToObject(st, "apps");
    }
    return st;
}

static void note_installed(cJSON *apps, const char *id, const uint32_t hashes[2]) {
    cJSON_DeleteItemFromObject(apps, id);
    cJSON *h = cJSON_AddArrayToObject(apps, id);
    cJSON_AddItemToArray(h, cJSON_CreateNumber(hashes[0]));
    cJSON_AddItemToArray(h, cJSON_CreateNumber(hashes[1]));
}

static void save_library_state(cJSON *st) {
    char *json = cJSON_PrintUnformatted(st);
    if (json && !write_file(LIBRARY_STATE, json)) ESP_LOGW(TAG, "couldn't save the library's state");
    cJSON_free(json);
}

// At boot, before the pages are built: new examples in, unchanged ones updated.
static void library_sync(void) {
    cJSON *st = library_state(), *apps = cJSON_GetObjectItem(st, "apps");
    bool changed = false;
    for (size_t i = 0; i < EXAMPLES_N; i++) {
        const example_t *ex = &EXAMPLES[i];
        const cJSON *had = cJSON_GetObjectItem(apps, ex->id);
        char err[80];
        uint32_t hashes[2];
        if (cJSON_GetArraySize(had) == 2) {
            char *page = example_page(ex);
            uint32_t want[2] = { page ? fnv(page, strlen(page)) : 0, fnv(ex->lua, strlen(ex->lua)) };
            cJSON_free(page);
            uint32_t was[2] = { (uint32_t)cJSON_GetArrayItem(had, 0)->valuedouble,
                                (uint32_t)cJSON_GetArrayItem(had, 1)->valuedouble };
            if (want[0] == was[0] && want[1] == was[1]) continue;   // as shipped
            char path[64];
            path_of(path, sizeof(path), ex->id);
            uint32_t now_page = file_hash(path);
            script_path(path, sizeof(path), ex->id);
            if (now_page != was[0] || file_hash(path) != was[1]) continue;   // changed, or taken away
        }
        if (put_example(ex, false, hashes, err, sizeof(err))) {
            note_installed(apps, ex->id, hashes);
            changed = true;
            ESP_LOGI(TAG, "library: %s %s", ex->id, had ? "updated" : "installed");
        } else {
            ESP_LOGW(TAG, "library: %s: %s", ex->id, err);
        }
    }
    if (changed) save_library_state(st);
    cJSON_Delete(st);
}

// ---- At boot -------------------------------------------------------------------

void muse_hw_apps_init(void) {
    s_lock = xSemaphoreCreateMutex();
    s_save_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t a = { .callback = save_now, .name = "app_save" };
    esp_timer_create(&a, &s_saver);
    muse_apps_set_loader(load_image);
}

// On the esp_timer task (muse_script.c's boot timer), before scripts start.
void muse_hw_apps_load(void) {
    if (muse_script_storage()) library_sync();
    DIR *d = muse_script_storage() ? opendir(DIR_PATH) : NULL;
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".json")) continue;
        char path[300];
        snprintf(path, sizeof(path), DIR_PATH "/%s", e->d_name);
        FILE *f = fopen(path, "rb");
        char *json = f ? heap_caps_malloc(DEF_MAX + 1, MUSE_BIG_CAPS) : NULL;
        size_t got = json ? fread(json, 1, DEF_MAX, f) : 0;
        if (f) fclose(f);
        if (!json) continue;
        json[got] = '\0';
        cJSON *def = cJSON_Parse(json);
        free(json);
        char err[120];
        if (def && muse_apps_define(def, err, sizeof(err))) n++;
        else ESP_LOGW(TAG, "%s: %s", e->d_name, def ? err : "not JSON");
        cJSON_Delete(def);
    }
    if (d) closedir(d);
    if (n) ESP_LOGI(TAG, "%d apps restored", n);
}

// A deleted script's apps go with it: those on flash, and those still waiting
// to be written (else the write lands after, and the page comes back at boot).
// On the link's or the console's command task (internal stacks: this touches
// flash, as store_delete does after it; scripts can't send script.delete).
// Holds off a save that has already taken the list, so each file is either
// still pending here or whole on flash.
void muse_hw_apps_remove_owned(const char *script) {
    char waiting[MUSE_APPS_MAX][16];
    int nwaiting = 0;
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (pending_t *p = s_pending; p; p = p->next) {
        if (p->json && !strcmp(p->owner, script)) {
            free(p->json);
            p->json = NULL;   // now a delete
            if (nwaiting < MUSE_APPS_MAX) strlcpy(waiting[nwaiting++], p->id, sizeof(waiting[0]));
        }
    }
    xSemaphoreGive(s_lock);
    for (int i = 0; i < nwaiting; i++) muse_apps_remove(waiting[i]);
    DIR *d = muse_script_storage() ? opendir(DIR_PATH) : NULL;
    for (struct dirent *e; d && (e = readdir(d));) {
        size_t len = strlen(e->d_name);
        if (len < 6 || len > 20 || strcmp(e->d_name + len - 5, ".json")) continue;
        char path[300], id[16];
        snprintf(path, sizeof(path), DIR_PATH "/%s", e->d_name);
        snprintf(id, sizeof(id), "%.*s", (int)(len - 5), e->d_name);
        FILE *f = fopen(path, "rb");
        char *json = f ? heap_caps_malloc(DEF_MAX + 1, MUSE_BIG_CAPS) : NULL;
        size_t got = json ? fread(json, 1, DEF_MAX, f) : 0;
        if (f) fclose(f);
        if (!json) continue;
        json[got] = '\0';
        cJSON *def = cJSON_Parse(json);
        free(json);
        cJSON *owner = cJSON_GetObjectItem(def, "_owner");
        if (cJSON_IsString(owner) && !strcmp(owner->valuestring, script)) {
            muse_apps_remove(id);
            unlink(path);
        }
        cJSON_Delete(def);
    }
    if (d) closedir(d);
    xSemaphoreGive(s_save_lock);
}

// ---- Commands --------------------------------------------------------------------

// The definition: params.app (an object, or JSON in a string), or params itself.
static cJSON *definition(cJSON *params) {
    cJSON *app = cJSON_GetObjectItem(params, "app");
    if (cJSON_IsObject(app)) return cJSON_Duplicate(app, true);
    if (cJSON_IsString(app)) return cJSON_Parse(app->valuestring);
    return cJSON_IsString(cJSON_GetObjectItem(params, "id")) ? cJSON_Duplicate(params, true) : NULL;
}

static cJSON *define(cJSON *params, const char *request_id, bool from_script) {
    cJSON *def = definition(params);
    if (!def) return hw_error("invalid_params", "app is a JSON description with an id");
    // A script's app is its own: "script:<token>:<name>".
    cJSON_DeleteItemFromObject(def, "_owner");
    const char *name = from_script ? strchr(request_id + 7, ':') : NULL;
    if (name) cJSON_AddStringToObject(def, "_owner", name + 1);
    char err[160];
    if (!muse_apps_define(def, err, sizeof(err))) {
        cJSON_Delete(def);
        return hw_error("invalid_app", err);
    }
    bool keep = !cJSON_IsFalse(cJSON_GetObjectItem(params, "keep"));
    char *json = keep ? cJSON_PrintUnformatted(def) : NULL;
    const char *id = cJSON_GetObjectItem(def, "id")->valuestring;
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "id", id);
    if (json && strlen(json) > DEF_MAX) {
        cJSON_free(json);
        json = NULL;
        cJSON_AddStringToObject(pl, "warning", "over 24 KB: shown, but not kept across restarts");
    }
    cJSON_AddBoolToObject(pl, "kept", json != NULL);
    if (json) queue_save(id, json, name ? name + 1 : "");
    cJSON_Delete(def);
    return hw_ok(pl);
}

cJSON *muse_hw_apps_command(const char *command, cJSON *params, const char *request_id,
                            noise_ctrl_session_generation_t session_generation) {
    const char *app = hw_str(params, "app");
    if (!strcmp(command, "app.define")) return define(params, request_id, session_generation == HW_SCRIPT_SESSION);
    if (!strcmp(command, "app.update")) {
        cJSON *set = cJSON_GetObjectItem(params, "set");
        cJSON *parsed = cJSON_IsString(set) ? cJSON_Parse(set->valuestring) : NULL;
        if (!cJSON_IsObject(parsed ? parsed : set)) {
            cJSON_Delete(parsed);
            return hw_error("invalid_params", "set is {widget_id: {prop: value}}");
        }
        char err[120];
        bool ok = muse_apps_update(app, parsed ? parsed : set, err, sizeof(err));
        cJSON_Delete(parsed);
        return ok ? hw_ok(NULL) : hw_error("not_found", err);
    }
    if (!strcmp(command, "app.remove")) {
        if (!app || !muse_apps_remove(app)) return hw_error("not_found", "no such app");
        queue_save(app, NULL, "");
        return hw_ok(NULL);
    }
    if (!strcmp(command, "app.show")) {
        if (!muse_apps_show(app)) return hw_error("not_found", "no such app");
        return hw_ok(NULL);
    }
    if (!strcmp(command, "app.library")) {
        cJSON *pl = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(pl, "examples");
        cJSON *st = muse_script_storage() ? library_state() : cJSON_CreateObject();
        const cJSON *apps = cJSON_GetObjectItem(st, "apps");
        for (size_t i = 0; i < EXAMPLES_N; i++) {
            cJSON *def = cJSON_Parse(EXAMPLES[i].json), *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", EXAMPLES[i].id);
            const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(def, "title"));
            const char *about = cJSON_GetStringValue(cJSON_GetObjectItem(def, "_about"));
            cJSON_AddStringToObject(o, "title", title ? title : EXAMPLES[i].id);
            if (about) cJSON_AddStringToObject(o, "about", about);
            char path[64];
            path_of(path, sizeof(path), EXAMPLES[i].id);
            struct stat sb;
            bool on_device = stat(path, &sb) == 0;
            cJSON_AddBoolToObject(o, "installed", on_device);
            const cJSON *had = cJSON_GetObjectItem(apps, EXAMPLES[i].id);
            if (on_device && cJSON_GetArraySize(had) == 2) {
                cJSON_AddBoolToObject(o, "changed", file_hash(path) != (uint32_t)cJSON_GetArrayItem(had, 0)->valuedouble);
            }
            cJSON_AddItemToArray(arr, o);
            cJSON_Delete(def);
        }
        cJSON_Delete(st);
        cJSON_AddStringToObject(pl, "pet", "The pet page (app.show pet) is built in: pet.status, pet.care, pet.name, "
                                           "and \"pet\" events.");
        return hw_ok(pl);
    }
    if (!strcmp(command, "app.example")) {
        const example_t *ex = example(hw_str(params, "id"));
        if (!ex) return hw_error("not_found", "no such example; app.library lists them");
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddStringToObject(pl, "id", ex->id);
        cJSON *def = cJSON_Parse(ex->json);
        cJSON_AddItemToObject(pl, "app", def ? def : cJSON_CreateNull());
        cJSON_AddStringToObject(pl, "script", ex->lua);
        return hw_ok(pl);
    }
    if (!strcmp(command, "app.install")) {
        if (session_generation == HW_SCRIPT_SESSION) return hw_error("unsupported", "not from a script");
        const example_t *ex = example(hw_str(params, "id"));
        if (!ex) return hw_error("not_found", "no such example; app.library lists them");
        if (!muse_script_storage()) return hw_error("unavailable", "no storage for apps");
        cJSON *def = cJSON_Parse(ex->json);
        char err[160];
        cJSON_AddStringToObject(def, "_owner", ex->id);
        bool built = def && muse_apps_define(def, err, sizeof(err));
        cJSON_Delete(def);
        if (!built) return hw_error("invalid_app", err);
        uint32_t hashes[2];
        bool running = put_example(ex, true, hashes, err, sizeof(err));
        cJSON *st = library_state();
        note_installed(cJSON_GetObjectItem(st, "apps"), ex->id, hashes);
        save_library_state(st);
        cJSON_Delete(st);
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddStringToObject(pl, "id", ex->id);
        cJSON_AddBoolToObject(pl, "running", running);
        if (!running) cJSON_AddStringToObject(pl, "error", err);
        return hw_ok(pl);
    }
    if (!strcmp(command, "app.get")) {
        char path[64];
        if (!app || !app[0] || strlen(app) > 15 || strchr(app, '/') || strchr(app, '.')) {
            return hw_error("invalid_params", "app is an app's id");
        }
        path_of(path, sizeof(path), app);
        char *json = read_file(path, DEF_MAX);
        cJSON *def = json ? cJSON_Parse(json) : NULL;
        free(json);
        if (!def) return hw_error("not_found", "no app kept by that id");
        cJSON *pl = cJSON_CreateObject();
        const char *owner = cJSON_GetStringValue(cJSON_GetObjectItem(def, "_owner"));
        if (owner) cJSON_AddStringToObject(pl, "script", owner);
        cJSON_AddItemToObject(pl, "app", def);
        return hw_ok(pl);
    }
    if (!strcmp(command, "app.list")) {
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddItemToObject(pl, "apps", muse_apps_list());
        cJSON_AddNumberToObject(pl, "max", MUSE_APPS_MAX);
        return hw_ok(pl);
    }
    return NULL;
}

void muse_hw_apps_register(cJSON *commands) {
    hw_add(commands, "app.define",
           "Add or replace an app: a page of widgets below Muse's face, swiped up to, kept across "
           "restarts. app = {id (a-z0-9_-), title, order, bg, layout (\"none\": place children with "
           "x/y/align; default a centred column), children: [widget]}. widget = {type, id, children, "
           "w, h (px, \"50%\", \"content\", \"grow\"), x, y, align (center, top, bottom_left...), bg, "
           "bg_opa, opa, radius, border, border_color, pad (a table's cells too), gap, justify/items (start, center, end, "
           "between), wrap, font (8-48), color, text_align, hidden, clickable, live}. Types: row, "
           "column, box; label (text, icon: play, bell, home, wifi, ok...; long: wrap/scroll/dots); "
           "button (text, icon); switch/checkbox (on); slider/arc/bar (min, max, value; arc: start, "
           "end, rotation, thickness, knob); spinner; image (src: \"sd:path\", \"url:https://..\", "
           "\"b64:<jpeg>\"; zoom, angle); chart (kind line/bar, min, max, points, series: [{color, "
           "values}]); led (color, brightness, on); roller/dropdown (options, selected, rows); line "
           "(points [[x,y]], width); canvas (w, h, fill, draw: [{op: rect/circle/line/arc/text, x, y, "
           "w, h, r, points, start, end, width, color, fill, text, font}]); qr (text, size); table "
           "(rows, col_w); input (text, placeholder; a keyboard opens). Presses and changes are "
           "input.read and script on(\"ui\") events: {app, id, event: click, long_press, change "
           "(value, text), submit (text), show, hide}.",
           NULL, hw_params("app", hw_param("string", "The description, as JSON.")), 10000);
    cJSON *up = hw_params("app", hw_param("string", "The app's id."));
    cJSON_AddItemToObject(up, "set", hw_param("string", "JSON {widget_id: {prop: value}}: text, value, on, "
                                                        "color, hidden, src, push (chart), draw (canvas)..."));
    hw_add(commands, "app.update", "Change an app's widgets in place.", up, NULL, 5000);
    hw_add(commands, "app.remove",
           "Remove an app's page. The script it belongs to keeps running: script.delete that script "
           "(app.get names it) to take both away.",
           hw_params("app", hw_param("string", "Its id.")), NULL, 5000);
    hw_add(commands, "app.show",
           "Bring up a page on the screen: an app, or a built-in page, \"face\" (Muse, the home "
           "page), \"pet\" (the pet, left of it) or \"settings\" (right of it). Use it when "
           "someone asks to open or see one (\"show me the timer\"); app.list has their ids.",
           NULL, hw_params("app", hw_param("string", "An app's id, face, pet or settings; empty for the face.")),
           5000);
    hw_add(commands, "app.library",
           "The example apps that come with the device: id, title, what each shows how to do, and "
           "whether it's installed. Read one with app.example before writing an app of your own: "
           "they cover pages, events, timers, the camera, the mic, the light, the canvas.",
           NULL, NULL, 5000);
    hw_add(commands, "app.example",
           "An example app from app.library: its page (the JSON for app.define) and its Lua script "
           "(for script.install), exactly as shipped.",
           hw_params("id", hw_param("string", "The example's id.")), NULL, 5000);
    hw_add(commands, "app.install",
           "Install an example app from app.library as shipped, page and script, replacing any "
           "changes to it; it starts at once and at every boot.",
           hw_params("id", hw_param("string", "The example's id.")), NULL, 10000);
    hw_add(commands, "app.get",
           "An installed app's page as kept (the JSON app.define takes) and the script it belongs "
           "to (script.read shows that): to change an app, get it, edit it, define it again.",
           hw_params("app", hw_param("string", "The app's id.")), NULL, 5000);
    hw_add(commands, "app.list",
           "The pages on the device: the built-in ones (face, pet, settings; builtin: true), then "
           "the apps below the face with id, title, order and widgets. shown marks the one on screen.",
           NULL, NULL, 5000);
}
