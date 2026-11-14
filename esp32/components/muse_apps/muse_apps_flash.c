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
 * The gadget's own files, in a directory (the mounted "files" partition).
 * A file is written to "<name>.part" and renamed over its name only once it's
 * whole, so a reader sees the old file or the new one, never half of one.
 */

#include "muse_apps_flash.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "muse_apps_flash";

#define SEGMENT_MAX 63   /* LittleFS's default name length, less one */
#define FOLDERS_MAX 8    /* folders a name may have: as deep as listing and wiping go */
#define PART ".part"
#define PATH_CAP (MUSE_APPS_FLASH_NAME_MAX + 64)

static const char *fail(char *err, size_t err_len, const char *code, const char *message) {
    snprintf(err, err_len, "%s", message);
    return code;
}

bool muse_apps_flash_name_ok(const char *name) {
    if (!name || !*name || strlen(name) >= MUSE_APPS_FLASH_NAME_MAX) return false;
    size_t seg = 0;
    int folders = 0;
    for (const char *p = name;; p++) {
        char c = *p;
        if (c == '/' || c == '\0') {
            if (seg == 0 || seg > SEGMENT_MAX) return false;   // empty, or too long
            if (c == '\0') break;
            if (++folders > FOLDERS_MAX) return false;   // deeper than walk() and sweep() reach
            seg = 0;
            continue;
        }
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                  c == '_' || c == '.';
        if (!ok || (seg == 0 && c == '.')) return false;   // no ".", "..", ".hidden"
        seg++;
    }
    size_t n = strlen(name);
    return !(n >= sizeof(PART) - 1 && !strcmp(name + n - (sizeof(PART) - 1), PART));   // ours, while writing
}

static bool join(char *out, size_t cap, const char *root, const char *name, const char *suffix) {
    int n = snprintf(out, cap, "%s/%s%s", root, name, suffix ? suffix : "");
    return n > 0 && (size_t)n < cap;
}

// Makes the folders a file at path sits in.
static bool make_parents(char *path, size_t root_len) {
    for (char *p = path + root_len + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        bool ok = mkdir(path, 0775) == 0 || errno == EEXIST;
        *p = '/';
        if (!ok) return false;
    }
    return true;
}

const char *muse_apps_flash_create(const char *root, const char *name, FILE **f, char *tmp, size_t tmp_cap,
                                   char *err, size_t err_len) {
    *f = NULL;
    if (!muse_apps_flash_name_ok(name)) {
        return fail(err, err_len, "bad_name",
                    "a name is folders and a file of A-Z, a-z, 0-9, '.', '-' and '_', joined by '/'");
    }
    if (!join(tmp, tmp_cap, root, name, PART)) return fail(err, err_len, "bad_name", "the name is too long");
    if (!make_parents(tmp, strlen(root))) {
        return fail(err, err_len, "storage_failed", "couldn't make the file's folders (is a file in the way?)");
    }
    struct stat st;
    char dest[PATH_CAP];
    if (join(dest, sizeof(dest), root, name, NULL) && stat(dest, &st) == 0 && S_ISDIR(st.st_mode)) {
        return fail(err, err_len, "bad_name", "that name is a folder");
    }
    *f = fopen(tmp, "wb");
    if (!*f) return fail(err, err_len, "storage_failed", "couldn't create the file");
    return NULL;
}

void muse_apps_flash_abort(const char *tmp) {
    if (tmp && *tmp) remove(tmp);
}

const char *muse_apps_flash_commit(const char *root, const char *name, const char *tmp, bool *replaced, char *err,
                                   size_t err_len) {
    char dest[PATH_CAP];
    if (!join(dest, sizeof(dest), root, name, NULL)) {
        muse_apps_flash_abort(tmp);
        return fail(err, err_len, "bad_name", "the name is too long");
    }
    struct stat st;
    *replaced = stat(dest, &st) == 0;
    // POSIX rename replaces dest in one step (LittleFS's does too); some
    // hosts' refuses an existing one (EEXIST), so try again without it. Any
    // other failure (an open file: EBUSY) keeps dest as it was.
    if (rename(tmp, dest) != 0 && !(errno == EEXIST && remove(dest) == 0 && rename(tmp, dest) == 0)) {
        muse_apps_flash_abort(tmp);
        return fail(err, err_len, "storage_failed", "couldn't move the file into place");
    }
    return NULL;
}

const char *muse_apps_flash_put(const char *root, const char *name, const uint8_t *data, size_t len,
                                size_t free_bytes, bool *replaced, char *err, size_t err_len) {
    *replaced = false;
    // A file takes its size plus a block or two of LittleFS's own.
    if (len + 8192 > free_bytes) return fail(err, err_len, "no_space", "not enough room on the gadget");
    FILE *f;
    char tmp[PATH_CAP];
    const char *code = muse_apps_flash_create(root, name, &f, tmp, sizeof(tmp), err, err_len);
    if (code) return code;
    bool ok = (!len || fwrite(data, 1, len, f) == len);
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        muse_apps_flash_abort(tmp);
        return fail(err, err_len, "no_space", "the write failed: the gadget may be full");
    }
    return muse_apps_flash_commit(root, name, tmp, replaced, err, err_len);
}

const char *muse_apps_flash_remove(const char *root, const char *name, bool *removed, char *err, size_t err_len) {
    *removed = false;
    if (!muse_apps_flash_name_ok(name)) return fail(err, err_len, "bad_name", "not a file name");
    char path[PATH_CAP];
    if (!join(path, sizeof(path), root, name, NULL)) return fail(err, err_len, "bad_name", "the name is too long");
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    if (S_ISDIR(st.st_mode)) return fail(err, err_len, "bad_name", "that name is a folder");
    if (remove(path) != 0) return fail(err, err_len, "storage_failed", "couldn't remove the file");
    *removed = true;
    // Folders it leaves empty go too.
    size_t root_len = strlen(root);
    for (char *slash = strrchr(path, '/'); slash && (size_t)(slash - path) > root_len; slash = strrchr(path, '/')) {
        *slash = '\0';
        if (rmdir(path) != 0) break;   // not empty
    }
    return NULL;
}

const char *muse_apps_flash_read(const char *root, const char *name, size_t offset, size_t max, uint8_t **out,
                                 size_t *len, char *err, size_t err_len) {
    *out = NULL;
    *len = 0;
    if (!muse_apps_flash_name_ok(name)) return fail(err, err_len, "bad_name", "not a file name");
    char path[PATH_CAP];
    if (!join(path, sizeof(path), root, name, NULL)) return fail(err, err_len, "bad_name", "the name is too long");
    // The size of the file opened, which the store task may replace meanwhile.
    FILE *f = fopen(path, "rb");
    struct stat st;
    if (!f || fstat(fileno(f), &st) != 0 || S_ISDIR(st.st_mode)) {
        if (f) fclose(f);
        return fail(err, err_len, "not_found", "no such file");
    }
    size_t size = (size_t)st.st_size;
    size_t want = offset < size ? (size - offset < max ? size - offset : max) : 0;
    if (!want) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return fail(err, err_len, "too_large", "no memory for that much of it");
    }
    size_t got = fseek(f, (long)offset, SEEK_SET) == 0 ? fread(buf, 1, want, f) : 0;
    fclose(f);
    if (got != want) {
        free(buf);
        return fail(err, err_len, "storage_failed", "couldn't read the file");
    }
    *out = buf;
    *len = got;
    return NULL;
}

typedef struct {
    cJSON *list;
    int count;
    size_t total;
    bool more;
} walk_t;

// Lists dir/rel (rel "" for the root) into w, depth first.
static void walk(const char *root, const char *rel, walk_t *w, int depth) {
    char dir_path[PATH_CAP];
    if (!join(dir_path, sizeof(dir_path), root, rel, NULL)) return;
    if (!*rel) snprintf(dir_path, sizeof(dir_path), "%s", root);
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    for (struct dirent *d; (d = readdir(dir));) {
        if (d->d_name[0] == '.') continue;
        char name[MUSE_APPS_FLASH_NAME_MAX + 64], path[PATH_CAP + 128];
        int n = snprintf(name, sizeof(name), "%s%s%s", rel, *rel ? "/" : "", d->d_name);
        if (n <= 0 || (size_t)n >= sizeof(name)) continue;   // longer than any name of ours
        n = snprintf(path, sizeof(path), "%s/%s", root, name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < FOLDERS_MAX) walk(root, name, w, depth + 1);
            continue;
        }
        size_t len = strlen(name);
        if (len >= sizeof(PART) - 1 && !strcmp(name + len - (sizeof(PART) - 1), PART)) continue;   // being written
        w->total += (size_t)st.st_size;
        if (w->count >= MUSE_APPS_FLASH_LIST_MAX) {
            w->more = true;
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        if (!cJSON_AddStringToObject(entry, "name", name) || !cJSON_AddNumberToObject(entry, "bytes", (double)st.st_size)) {
            cJSON_Delete(entry);   /* out of memory: the list is short, not broken */
            w->more = true;
            continue;
        }
        cJSON_AddItemToArray(w->list, entry);
        w->count++;
    }
    closedir(dir);
}

static int by_name(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *)a, *y = *(const cJSON *const *)b;
    return strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(x, "name")),
                  cJSON_GetStringValue(cJSON_GetObjectItem(y, "name")));
}

cJSON *muse_apps_flash_list(const char *root, size_t *total, bool *more) {
    walk_t w = {.list = cJSON_CreateArray()};
    walk(root, "", &w, 0);
    *total = w.total;
    *more = w.more;
    // Sorted, so the list reads the same however the filesystem orders it.
    int n = cJSON_GetArraySize(w.list);
    cJSON **items = n ? malloc(sizeof(cJSON *) * (size_t)n) : NULL;
    if (!items) return w.list;
    for (int i = 0; i < n; i++) items[i] = cJSON_DetachItemFromArray(w.list, 0);
    qsort(items, (size_t)n, sizeof(cJSON *), by_name);
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(w.list, items[i]);
    free(items);
    return w.list;
}

// Deletes everything under path (all) or what writes cut short, and the
// folders that leaves empty, but never path itself. False if something it
// meant to delete is still there (all: anything at all).
static bool sweep(const char *path, bool all, int depth) {
    DIR *dir = opendir(path);
    if (!dir) return !all || (depth == 0 && errno == ENOENT);   // no partition root: nothing to wipe
    bool ok = true;
    for (struct dirent *d; (d = readdir(dir));) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
        char child[PATH_CAP + 64];
        int n = snprintf(child, sizeof(child), "%s/%s", path, d->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) continue;
        struct stat st;
        if (stat(child, &st) != 0) {
            ok = ok && !all;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth < FOLDERS_MAX) ok = sweep(child, all, depth + 1) && ok;
            // fails, harmlessly when tidying, unless it's now empty
            if (rmdir(child) != 0 && all) ok = false;
            continue;
        }
        size_t len = strlen(d->d_name);
        bool partial = len >= sizeof(PART) - 1 && !strcmp(d->d_name + len - (sizeof(PART) - 1), PART);
        if (all || partial) {
            if (remove(child) != 0) ok = false;   // a reader may hold it open
            else if (partial) ESP_LOGI(TAG, "deleted %s, a write cut short", child);
        }
    }
    closedir(dir);
    return ok;
}

void muse_apps_flash_tidy(const char *root) {
    (void)sweep(root, false, 0);
}

bool muse_apps_flash_wipe(const char *root) {
    return sweep(root, true, 0);
}
