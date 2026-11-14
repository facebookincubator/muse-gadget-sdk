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

/*
 * The installed apps. In a directory, each app's module is <sha256>.wasm,
 * written whole (to a .tmp, then renamed) before catalog.json names it, and
 * catalog.json is replaced whole the same way: an install, update or removal
 * either happened or it didn't, and the old version stays until the new one
 * is in. Files catalog.json doesn't name, such as an install cut short, are
 * deleted when it loads. Without a directory the modules are kept in memory
 * until the gadget restarts.
 */

#include "muse_apps_catalog.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/md.h"

static const char *TAG = "muse_apps";

#define CATALOG "catalog.json"
#define CATALOG_MAX (16 * 1024)
#define HEADROOM (32 * 1024)      // left free in the directory for its own bookkeeping
#define MEMORY_MAX (1024 * 1024)  // all apps together, without a directory

typedef struct {
    muse_apps_entry_t e;
    uint8_t *ram;  // the module, without a directory
} slot_t;

static EXT_RAM_BSS_ATTR slot_t s_apps[MUSE_APPS_MAX];  // PSRAM: internal RAM is the audio's and TLS's
static int s_count;
static char s_dir[128];
static bool s_persistent;
static bool s_unread;   // the catalog is there but couldn't be read: saving one now would drop its apps

static void *alloc(size_t n) {
    return heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static const char *fail(char *err, size_t err_len, const char *code, const char *message) {
    snprintf(err, err_len, "%s", message);
    return code;
}

void muse_apps_sha256_hex(const uint8_t *data, size_t len, char out[MUSE_APPS_SHA_HEX + 1]) {
    uint8_t sum[32];
    out[0] = '\0';
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md(md, data, len, sum) != 0) return;
    for (int i = 0; i < 32; i++) {
        out[i * 2] = "0123456789abcdef"[sum[i] >> 4];
        out[i * 2 + 1] = "0123456789abcdef"[sum[i] & 15];
    }
    out[MUSE_APPS_SHA_HEX] = '\0';
}

static bool valid_id(const char *id) {
    size_t n = strlen(id);
    if (!n || n >= MUSE_APPS_ID_MAX) return false;
    for (const char *c = id; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) return false;
    }
    return true;
}

static bool valid_sha(const char *s) {
    if (strlen(s) != MUSE_APPS_SHA_HEX) return false;
    for (const char *c = s; *c; c++) {
        if (!strchr("0123456789abcdef", *c)) return false;
    }
    return true;
}

// A name cut to fit, never inside a UTF-8 character.
static void set_name(char *dst, const char *src) {
    size_t n = strlen(src);
    if (n >= MUSE_APPS_NAME_MAX) {
        n = MUSE_APPS_NAME_MAX - 1;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// A glyph's or colour's name, if it's one the launcher has; else none.
static void set_look(char *dst, const char *src, bool known) {
    snprintf(dst, MUSE_APPS_LOOK_MAX, "%s", known ? src : "");
}

static void app_path(char *out, size_t cap, const char *sha) {
    snprintf(out, cap, "%s/%s.wasm", s_dir, sha);
}

static int find(const char *id) {
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_apps[i].e.id, id)) return i;
    }
    return -1;
}

static bool sha_in_use(const char *sha) {
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_apps[i].e.sha256, sha)) return true;
    }
    return false;
}

static uint8_t *read_all(const char *path, size_t max, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (fstat(fileno(f), &st) == 0 && (size_t)st.st_size <= max && (buf = alloc(st.st_size + 1))) {
        *len = fread(buf, 1, st.st_size, f);
        buf[*len] = '\0';
        if (*len != (size_t)st.st_size) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

// The whole file or nothing: written beside it, then renamed over it.
static bool write_all(const char *path, const void *data, size_t len) {
    char tmp[240];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

static bool save(void) {
    if (!s_persistent) return true;
    // Every piece or nothing: a catalog missing one would drop its modules at the next start.
    cJSON *root = cJSON_CreateObject(), *apps = muse_apps_catalog_json();
    bool built = root && apps && cJSON_AddNumberToObject(root, "version", 1) && cJSON_AddItemToObject(root, "apps", apps);
    if (!built) {
        cJSON_Delete(apps);   // not in root unless built
        cJSON_Delete(root);
        ESP_LOGE(TAG, "no memory to save %s", CATALOG);
        return false;
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    char path[160];
    snprintf(path, sizeof(path), "%s/" CATALOG, s_dir);
    bool ok = text && write_all(path, text, strlen(text));
    cJSON_free(text);
    if (!ok) ESP_LOGE(TAG, "couldn't write %s", path);
    return ok;
}

// Deletes what catalog.json doesn't name: modules of removed apps, installs
// cut short, leftover .tmp files. False if one couldn't be.
static bool delete_unnamed(void) {
    DIR *dir = opendir(s_dir);
    if (!dir) return errno == ENOENT;
    bool all = true;
    for (struct dirent *d; (d = readdir(dir));) {
        const char *name = d->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, CATALOG)) continue;
        size_t n = strlen(name);
        char sha[MUSE_APPS_SHA_HEX + 1] = "";
        if (n == MUSE_APPS_SHA_HEX + 5 && !strcmp(name + MUSE_APPS_SHA_HEX, ".wasm")) {
            memcpy(sha, name, MUSE_APPS_SHA_HEX);
            sha[MUSE_APPS_SHA_HEX] = '\0';
            if (sha_in_use(sha)) continue;
        }
        char path[sizeof(s_dir) + sizeof(d->d_name) + 1];
        int len = snprintf(path, sizeof(path), "%s/%s", s_dir, name);
        if (len < 0 || (size_t)len >= sizeof(path)) continue;
        if (remove(path) != 0) {
            all = false;
            continue;
        }
        ESP_LOGI(TAG, "deleted %s, which no installed app uses", name);
    }
    closedir(dir);
    return all;
}

static void forget_all(void) {
    for (int i = 0; i < s_count; i++) free(s_apps[i].ram);
    memset(s_apps, 0, sizeof(s_apps));
    s_count = 0;
}

void muse_apps_catalog_open(const char *dir) {
    forget_all();
    s_unread = false;
    s_persistent = dir != NULL;
    snprintf(s_dir, sizeof(s_dir), "%s", dir ? dir : "");
    if (!s_persistent) return;
    char path[160];
    snprintf(path, sizeof(path), "%s/" CATALOG, s_dir);
    size_t len = 0;
    struct stat cat;
    bool exists = stat(path, &cat) == 0;
    char *text = (char *)read_all(path, CATALOG_MAX, &len);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    /* A catalog that's there but couldn't be read (no memory, a flash error)
     * names nothing: its modules aren't unused, so they stay. */
    bool trusted = !exists || root;
    s_unread = !trusted;
    bool dropped = false;
    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(root, "apps")) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        const char *sha = cJSON_GetStringValue(cJSON_GetObjectItem(item, "sha256"));
        const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(item, "icon"));
        const char *color = cJSON_GetStringValue(cJSON_GetObjectItem(item, "color"));
        cJSON *bytes = cJSON_GetObjectItem(item, "bytes");
        char file[224];
        struct stat st;
        if (id && name && sha && valid_id(id) && valid_sha(sha) && cJSON_IsNumber(bytes) && find(id) < 0 &&
            s_count < MUSE_APPS_MAX && (app_path(file, sizeof(file), sha), stat(file, &st) == 0) &&
            st.st_size == bytes->valuedouble) {
            slot_t *s = &s_apps[s_count++];
            snprintf(s->e.id, sizeof(s->e.id), "%s", id);
            set_name(s->e.name, name);
            snprintf(s->e.sha256, sizeof(s->e.sha256), "%s", sha);
            s->e.bytes = (uint32_t)bytes->valuedouble;
            set_look(s->e.icon, icon, muse_apps_glyph_find(icon) >= 0);
            set_look(s->e.color, color, muse_apps_color_find(color) >= 0);
        } else {
            ESP_LOGW(TAG, "dropped %s from the installed apps: its module is missing", id ? id : "an entry");
            dropped = true;
        }
    }
    cJSON_Delete(root);
    if (dropped) save();
    if (trusted) {
        delete_unnamed();
    } else {
        ESP_LOGW(TAG, "couldn't read %s: its apps' modules kept", CATALOG);
    }
    ESP_LOGI(TAG, "%d apps installed", s_count);
}

bool muse_apps_catalog_persistent(void) {
    return s_persistent;
}

int muse_apps_catalog_count(void) {
    return s_count;
}

const muse_apps_entry_t *muse_apps_catalog_get(int i) {
    return i >= 0 && i < s_count ? &s_apps[i].e : NULL;
}

const char *muse_apps_catalog_install(const char *id, const char *name, const char *icon, const char *color,
                                      const uint8_t *data, size_t len, const char *want_sha256,
                                      size_t free_bytes, bool *changed, char *err, size_t err_len) {
    *changed = false;
    if (s_unread) return fail(err, err_len, "storage_failed", "the installed apps couldn't be read: restart the gadget");
    if (!valid_id(id)) return fail(err, err_len, "bad_id", "an id is 1 to 31 of a-z, 0-9, - and _");
    if (icon && *icon && muse_apps_glyph_find(icon) < 0) {
        snprintf(err, err_len, "no icon %.16s: app.install's description lists them", icon);
        return "bad_icon";
    }
    if (color && *color && muse_apps_color_find(color) < 0) {
        snprintf(err, err_len, "no colour %.16s: app.install's description lists them", color);
        return "bad_color";
    }
    if (len < 8 || memcmp(data, "\0asm", 4)) return fail(err, err_len, "invalid_app", "not a WebAssembly module");
    char sha[MUSE_APPS_SHA_HEX + 1];
    muse_apps_sha256_hex(data, len, sha);
    if (want_sha256 && *want_sha256 && strcasecmp(want_sha256, sha)) {
        snprintf(err, err_len, "the file's SHA-256 is %s", sha);
        return "hash_mismatch";
    }
    int i = find(id);
    char new_name[MUSE_APPS_NAME_MAX];
    set_name(new_name, name && *name ? name : i >= 0 ? s_apps[i].e.name : id);
    char new_icon[MUSE_APPS_LOOK_MAX], new_color[MUSE_APPS_LOOK_MAX];
    set_look(new_icon, icon && *icon ? icon : i >= 0 ? s_apps[i].e.icon : "", true);
    set_look(new_color, color && *color ? color : i >= 0 ? s_apps[i].e.color : "", true);
    if (i >= 0 && !strcmp(s_apps[i].e.sha256, sha) && !strcmp(s_apps[i].e.name, new_name) &&
        !strcmp(s_apps[i].e.icon, new_icon) && !strcmp(s_apps[i].e.color, new_color)) {
        return NULL;
    }
    if (i < 0 && s_count >= MUSE_APPS_MAX) {
        snprintf(err, err_len, "%d apps are installed: remove one first", MUSE_APPS_MAX);
        return "too_many_apps";
    }

    slot_t old = i >= 0 ? s_apps[i] : (slot_t){0};
    bool same_bytes = i >= 0 && !strcmp(old.e.sha256, sha);
    char path[224] = "";
    bool wrote = false;
    uint8_t *ram = NULL;
    if (s_persistent) {
        app_path(path, sizeof(path), sha);
        struct stat st;
        if (stat(path, &st) != 0 || (size_t)st.st_size != len) {  // another app may have these bytes
            if (len + HEADROOM > free_bytes) {
                snprintf(err, err_len, "%u bytes don't fit: %u free", (unsigned)len,
                         (unsigned)(free_bytes > HEADROOM ? free_bytes - HEADROOM : 0));
                return "no_space";
            }
            if (!write_all(path, data, len)) return fail(err, err_len, "storage_failed", "couldn't write the app");
            wrote = true;
        }
    } else if (!same_bytes) {
        size_t used = 0;
        for (int k = 0; k < s_count; k++) used += k == i ? 0 : s_apps[k].e.bytes;
        if (used + len > MEMORY_MAX || !(ram = alloc(len))) {
            snprintf(err, err_len, "%u bytes don't fit in memory", (unsigned)len);
            return "no_space";
        }
        memcpy(ram, data, len);
    }

    slot_t *s = i >= 0 ? &s_apps[i] : &s_apps[s_count++];
    snprintf(s->e.id, sizeof(s->e.id), "%s", id);
    snprintf(s->e.name, sizeof(s->e.name), "%s", new_name);
    snprintf(s->e.icon, sizeof(s->e.icon), "%s", new_icon);
    snprintf(s->e.color, sizeof(s->e.color), "%s", new_color);
    snprintf(s->e.sha256, sizeof(s->e.sha256), "%s", sha);
    s->e.bytes = (uint32_t)len;
    if (ram) s->ram = ram;
    if (!save()) {
        if (i >= 0) {
            *s = old;
        } else {
            memset(s, 0, sizeof(*s));
            s_count--;
        }
        if (wrote && !sha_in_use(sha)) remove(path);
        free(ram);
        return fail(err, err_len, "storage_failed", "couldn't update the list of apps");
    }
    if (i >= 0 && !same_bytes) {  // the old version goes, unless another app has the same bytes
        if (s_persistent && !sha_in_use(old.e.sha256)) {
            app_path(path, sizeof(path), old.e.sha256);
            remove(path);
        }
        free(old.ram);
    }
    ESP_LOGI(TAG, "%s %s \"%s\": %u bytes", i >= 0 ? "updated" : "installed", id, s->e.name, (unsigned)len);
    *changed = true;
    return NULL;
}

const char *muse_apps_catalog_remove(const char *id, bool *removed, char *err, size_t err_len) {
    *removed = false;
    int i = find(id);
    if (i < 0) return NULL;
    slot_t old = s_apps[i];
    memmove(&s_apps[i], &s_apps[i + 1], (s_count - i - 1) * sizeof(slot_t));
    s_count--;
    memset(&s_apps[s_count], 0, sizeof(slot_t));
    if (!save()) {
        memmove(&s_apps[i + 1], &s_apps[i], (s_count - i) * sizeof(slot_t));
        s_apps[i] = old;
        s_count++;
        return fail(err, err_len, "storage_failed", "couldn't update the list of apps");
    }
    if (s_persistent && !sha_in_use(old.e.sha256)) {
        char path[224];
        app_path(path, sizeof(path), old.e.sha256);
        remove(path);
    }
    free(old.ram);
    ESP_LOGI(TAG, "removed %s", id);
    *removed = true;
    return NULL;
}

const char *muse_apps_catalog_read(const char *id, uint8_t **out, size_t *len, char *err, size_t err_len) {
    *out = NULL;
    *len = 0;
    int i = find(id);
    if (i < 0) {
        snprintf(err, err_len, "no app %s is installed", id);
        return "not_installed";
    }
    const slot_t *s = &s_apps[i];
    uint8_t *data = NULL;
    size_t n = 0;
    if (s_persistent) {
        char path[224];
        app_path(path, sizeof(path), s->e.sha256);
        data = read_all(path, s->e.bytes, &n);
    } else if (s->ram && (data = alloc(s->e.bytes))) {
        memcpy(data, s->ram, s->e.bytes);
        n = s->e.bytes;
    }
    if (!data && s_persistent) {
        char path[224];
        struct stat st;
        app_path(path, sizeof(path), s->e.sha256);
        if (stat(path, &st) == 0 && st.st_size == (off_t)s->e.bytes) {
            /* there, but not read: no memory or a flash error, not damage */
            return fail(err, err_len, "no_memory", "couldn't read the app: try again");
        }
    } else if (!data) {
        return fail(err, err_len, "no_memory", "couldn't copy the app: try again");
    }
    char sha[MUSE_APPS_SHA_HEX + 1] = "";
    if (data && n == s->e.bytes) muse_apps_sha256_hex(data, n, sha);
    if (strcmp(sha, s->e.sha256)) {
        free(data);
        ESP_LOGW(TAG, "%s is damaged: removed", id);
        bool removed;
        char ignored[8];
        muse_apps_catalog_remove(id, &removed, ignored, sizeof(ignored));
        return fail(err, err_len, "damaged", "the copy on this gadget was damaged and is removed: install it again");
    }
    *out = data;
    *len = n;
    return NULL;
}

bool muse_apps_catalog_wipe(void) {
    forget_all();
    s_unread = false;   // unpairing removes every app, read or not
    if (!s_persistent) return true;
    bool saved = save();
    return delete_unnamed() && saved;
}

// NULL, not part of a list, when memory runs out.
cJSON *muse_apps_catalog_json(void) {
    cJSON *apps = cJSON_CreateArray();
    for (int i = 0; apps && i < s_count; i++) {
        const muse_apps_entry_t *e = &s_apps[i].e;
        cJSON *app = cJSON_CreateObject();
        bool ok = app && cJSON_AddStringToObject(app, "id", e->id) && cJSON_AddStringToObject(app, "name", e->name) &&
                  cJSON_AddStringToObject(app, "sha256", e->sha256) && cJSON_AddNumberToObject(app, "bytes", e->bytes) &&
                  (!e->icon[0] || cJSON_AddStringToObject(app, "icon", e->icon)) &&
                  (!e->color[0] || cJSON_AddStringToObject(app, "color", e->color));
        if (!ok || !cJSON_AddItemToArray(apps, app)) {
            cJSON_Delete(app);
            cJSON_Delete(apps);
            return NULL;
        }
    }
    return apps;
}
