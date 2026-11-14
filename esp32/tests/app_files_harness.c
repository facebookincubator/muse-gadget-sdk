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

/* tests/test_app_files.py: the gadget's own files (muse_apps_flash.c) on the
 * host, in a temporary directory standing in for the "files" partition. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "muse_apps_flash.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)
#define ROOM (1 << 20)

static const char *dir;
static char err[160];

static const char *put(const char *name, const char *text, bool *replaced) {
    return muse_apps_flash_put(dir, name, (const uint8_t *)text, strlen(text), ROOM, replaced, err, sizeof(err));
}

static bool exists(const char *rel) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, rel);
    struct stat st;
    return stat(path, &st) == 0;
}

static const char *listed(int i, double *bytes, size_t *total, int *count) {
    static char name[128];
    bool more;
    cJSON *list = muse_apps_flash_list(dir, total, &more);
    *count = cJSON_GetArraySize(list);
    cJSON *e = cJSON_GetArrayItem(list, i);
    name[0] = '\0';
    if (e) {
        snprintf(name, sizeof(name), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(e, "name")));
        *bytes = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "bytes"));
    }
    cJSON_Delete(list);
    return name;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    dir = argv[1];

    /* Names: folders and a file, nothing that climbs out or hides. */
    const char *good[] = {"cat.jpg", "photos/cat.jpg", "A-b_c.1", "a/b/c/d.png", "x.part1", "1/2/3/4/5/6/7/8/f"};
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) CHECK(muse_apps_flash_name_ok(good[i]));
    char long_seg[80], long_name[120];
    memset(long_seg, 'a', 64);
    long_seg[64] = '\0';
    memset(long_name, 'b', 96);
    long_name[96] = '\0';
    for (int k = 30; k < 96; k += 31) long_name[k] = '/';
    const char *bad[] = {"", "/a", "a/", "a//b", ".a", "a/../b", "..", ".", "a/.b", "x.part", "a b", "a\\b",
                         "caf\xc3\xa9", long_seg, long_name,
                         "1/2/3/4/5/6/7/8/9/f"};   /* deeper than listing and wiping reach */
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (muse_apps_flash_name_ok(bad[i])) fprintf(stderr, "took bad name %zu\n", i);
        CHECK(!muse_apps_flash_name_ok(bad[i]));
    }
    CHECK(!muse_apps_flash_name_ok(NULL));
    bool replaced = true, removed = true;
    CHECK(!strcmp(put("../escape", "x", &replaced), "bad_name"));
    CHECK(!exists("../escape"));

    /* Put, replace, read whole and in parts. */
    CHECK(put("cat.jpg", "0123456789", &replaced) == NULL && !replaced);
    CHECK(put("cat.jpg", "abcdefghij", &replaced) == NULL && replaced);
    uint8_t *out = NULL;
    size_t len = 0;
    CHECK(muse_apps_flash_read(dir, "cat.jpg", 0, 100, &out, &len, err, sizeof(err)) == NULL);
    CHECK(len == 10 && out && !memcmp(out, "abcdefghij", 10));
    free(out);
    CHECK(muse_apps_flash_read(dir, "cat.jpg", 4, 3, &out, &len, err, sizeof(err)) == NULL);
    CHECK(len == 3 && out && !memcmp(out, "efg", 3));
    free(out);
    CHECK(muse_apps_flash_read(dir, "cat.jpg", 10, 5, &out, &len, err, sizeof(err)) == NULL && !out && !len);
    CHECK(!strcmp(muse_apps_flash_read(dir, "dog.jpg", 0, 5, &out, &len, err, sizeof(err)), "not_found"));
    CHECK(!strcmp(muse_apps_flash_read(dir, "../x", 0, 5, &out, &len, err, sizeof(err)), "bad_name"));
    CHECK(!exists("cat.jpg.part"));

    /* Folders: made as needed, listed sorted, pruned when emptied. */
    CHECK(put("photos/2026/b.png", "bb", &replaced) == NULL);
    CHECK(put("photos/2026/a.png", "a", &replaced) == NULL);
    CHECK(!strcmp(put("photos", "x", &replaced), "bad_name"));                 /* a folder */
    CHECK(!strcmp(put("cat.jpg/inside", "x", &replaced), "storage_failed"));  /* a file in the way */
    double bytes = 0;
    size_t total = 0;
    int count = 0;
    CHECK(!strcmp(listed(0, &bytes, &total, &count), "cat.jpg") && bytes == 10);
    CHECK(!strcmp(listed(1, &bytes, &total, &count), "photos/2026/a.png") && bytes == 1);
    CHECK(!strcmp(listed(2, &bytes, &total, &count), "photos/2026/b.png") && bytes == 2);
    CHECK(count == 3 && total == 13);
    CHECK(muse_apps_flash_remove(dir, "photos/2026/a.png", &removed, err, sizeof(err)) == NULL && removed);
    CHECK(exists("photos/2026"));
    CHECK(muse_apps_flash_remove(dir, "photos/2026/b.png", &removed, err, sizeof(err)) == NULL && removed);
    CHECK(!exists("photos"));
    CHECK(muse_apps_flash_remove(dir, "photos/2026/b.png", &removed, err, sizeof(err)) == NULL && !removed);
    CHECK(!strcmp(muse_apps_flash_remove(dir, "../cat.jpg", &removed, err, sizeof(err)), "bad_name"));

    /* Room: a file that won't fit isn't started. */
    CHECK(!strcmp(muse_apps_flash_put(dir, "big.bin", (const uint8_t *)"x", 1, 4096, &replaced, err, sizeof(err)),
                  "no_space"));
    CHECK(!exists("big.bin"));

    /* A write cut short: not listed, gone at the next mount; aborting cleans up. */
    FILE *f = NULL;
    char tmp[256];
    CHECK(muse_apps_flash_create(dir, "notes/half.txt", &f, tmp, sizeof(tmp), err, sizeof(err)) == NULL && f);
    fputs("half", f);
    fclose(f);
    CHECK(exists("notes/half.txt.part"));
    CHECK(!strcmp(listed(0, &bytes, &total, &count), "cat.jpg") && count == 1);
    muse_apps_flash_tidy(dir);
    CHECK(!exists("notes/half.txt.part") && !exists("notes") && exists("cat.jpg"));
    CHECK(muse_apps_flash_create(dir, "c.txt", &f, tmp, sizeof(tmp), err, sizeof(err)) == NULL && f);
    fclose(f);
    muse_apps_flash_abort(tmp);
    CHECK(!exists("c.txt.part") && !exists("c.txt"));
    CHECK(muse_apps_flash_create(dir, "d.txt", &f, tmp, sizeof(tmp), err, sizeof(err)) == NULL && f);
    fputs("done", f);
    fclose(f);
    CHECK(muse_apps_flash_commit(dir, "d.txt", tmp, &replaced, err, sizeof(err)) == NULL && !replaced);
    CHECK(exists("d.txt") && !exists("d.txt.part"));

    /* The deepest name is listed. */
    CHECK(put("1/2/3/4/5/6/7/8/f", "f", &replaced) == NULL);
    CHECK(!strcmp(listed(0, &bytes, &total, &count), "1/2/3/4/5/6/7/8/f") && count == 3);

    /* A file that can't be removed yet (on the gadget, one a reader holds
     * open) makes the wipe say so, so that it's tried again. */
    if (geteuid() != 0) {
        char keep[300];
        CHECK(put("keep/k.txt", "k", &replaced) == NULL);
        snprintf(keep, sizeof(keep), "%s/keep", dir);
        chmod(keep, 0500);
        CHECK(!muse_apps_flash_wipe(dir) && exists("keep/k.txt"));
        chmod(keep, 0700);
        CHECK(put("1/2/3/4/5/6/7/8/f", "f", &replaced) == NULL);
    }

    /* Unpairing: everything goes, the deepest too, the partition's root stays. */
    CHECK(put("deep/er/e.txt", "e", &replaced) == NULL);
    CHECK(muse_apps_flash_wipe(dir));
    CHECK(!exists("cat.jpg") && !exists("d.txt") && !exists("deep") && !exists("1") && exists(""));
    CHECK(listed(0, &bytes, &total, &count)[0] == '\0' && count == 0 && total == 0);

    if (failures) return 1;
    printf("ok\n");
    return 0;
}
