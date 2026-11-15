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

/* tests/test_app_store.py: the installed-apps catalog (muse_apps_catalog.c) on
 * the host, in a temporary directory standing in for the "apps" partition. */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "muse_apps_catalog.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static const char *dir;
static char err[160];

/* A module: the wasm header, then a tag so each one hashes differently. */
static size_t module(uint8_t *buf, const char *tag) {
    memcpy(buf, "\0asm\1\0\0\0", 8);
    size_t n = strlen(tag);
    memcpy(buf + 8, tag, n);
    return 8 + n;
}

static const char *install(const char *id, const char *name, const char *tag, const char *want, size_t room, bool *changed) {
    uint8_t buf[64];
    size_t n = module(buf, tag);
    return muse_apps_catalog_install(id, name, NULL, NULL, buf, n, want, room, changed, err, sizeof(err));
}

static const char *install_look(const char *id, const char *icon, const char *color, const char *tag, bool *changed) {
    uint8_t buf[64];
    size_t n = module(buf, tag);
    return muse_apps_catalog_install(id, NULL, icon, color, buf, n, NULL, 1 << 20, changed, err, sizeof(err));
}

static void sha_of(const char *tag, char out[MUSE_APPS_SHA_HEX + 1]) {
    uint8_t buf[64];
    size_t n = module(buf, tag);
    muse_apps_sha256_hex(buf, n, out);
}

static bool has_file(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    return stat(path, &st) == 0;
}

static bool has_module(const char *tag) {
    char sha[MUSE_APPS_SHA_HEX + 1], name[80];
    sha_of(tag, sha);
    snprintf(name, sizeof(name), "%s.wasm", sha);
    return has_file(name);
}

static int files(void) {
    int n = 0;
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) n += e->d_name[0] != '.';
    if (d) closedir(d);
    return n;
}

static void put(const char *name, const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    dir = argv[1];
    bool changed, removed;
    char sha[MUSE_APPS_SHA_HEX + 1];
    const size_t ROOM = 1 << 20;

    muse_apps_sha256_hex((const uint8_t *)"abc", 3, sha);
    CHECK(!strcmp(sha, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    muse_apps_catalog_open(dir);
    CHECK(muse_apps_catalog_persistent() && muse_apps_catalog_count() == 0);

    /* Install, again unchanged, rename, update. */
    CHECK(install("hello", "Hello", "v1", NULL, ROOM, &changed) == NULL && changed);
    CHECK(muse_apps_catalog_count() == 1 && has_module("v1") && has_file("catalog.json"));
    CHECK(install("hello", "Hello", "v1", NULL, ROOM, &changed) == NULL && !changed);
    CHECK(install("hello", NULL, "v1", NULL, ROOM, &changed) == NULL && !changed);   /* keeps its name */
    CHECK(install("hello", "Hi there", "v1", NULL, ROOM, &changed) == NULL && changed);
    CHECK(!strcmp(muse_apps_catalog_get(0)->name, "Hi there"));
    sha_of("v2", sha);
    CHECK(install("hello", NULL, "v2", sha, ROOM, &changed) == NULL && changed);
    CHECK(has_module("v2") && !has_module("v1") && !strcmp(muse_apps_catalog_get(0)->sha256, sha));

    /* Its look: a glyph and a colour the launcher has, kept until changed, and kept over a restart. */
    CHECK(install_look("hello", "timer", "red", "v2", &changed) == NULL && changed);
    CHECK(install_look("hello", NULL, NULL, "v2", &changed) == NULL && !changed);
    CHECK(install_look("hello", NULL, "blue", "v2", &changed) == NULL && changed);
    CHECK(!strcmp(muse_apps_catalog_get(0)->icon, "timer") && !strcmp(muse_apps_catalog_get(0)->color, "blue"));
    CHECK(!strcmp(install_look("hello", "rocket", NULL, "v2", &changed), "bad_icon") && !changed);
    CHECK(!strcmp(install_look("hello", NULL, "neon", "v2", &changed), "bad_color") && !changed);
    muse_apps_catalog_open(dir);
    CHECK(!strcmp(muse_apps_catalog_get(0)->icon, "timer") && !strcmp(muse_apps_catalog_get(0)->color, "blue"));

    /* Refused, and nothing changes. */
    CHECK(!strcmp(install("hello", NULL, "v3", sha, ROOM, &changed), "hash_mismatch") && !changed);
    CHECK(!strcmp(install("Hello!", NULL, "v3", NULL, ROOM, &changed), "bad_id"));
    CHECK(!strcmp(install("", NULL, "v3", NULL, ROOM, &changed), "bad_id"));
    CHECK(!strcmp(muse_apps_catalog_install("junk", NULL, NULL, NULL, (const uint8_t *)"not wasm", 8, NULL, ROOM, &changed, err, sizeof(err)), "invalid_app"));
    CHECK(!strcmp(install("big", NULL, "v3", NULL, 100, &changed), "no_space") && !has_module("v3"));
    CHECK(muse_apps_catalog_count() == 1 && has_module("v2"));

    /* Two apps with the same bytes share a file; it goes with the last of them. */
    CHECK(install("timer", "Timer", "v2", NULL, ROOM, &changed) == NULL && changed);
    CHECK(muse_apps_catalog_count() == 2 && files() == 2);
    CHECK(muse_apps_catalog_remove("hello", &removed, err, sizeof(err)) == NULL && removed);
    CHECK(muse_apps_catalog_count() == 1 && has_module("v2"));
    CHECK(muse_apps_catalog_remove("hello", &removed, err, sizeof(err)) == NULL && !removed);

    /* A restart keeps the apps and clears out what no app uses. */
    put("leftover.wasm.tmp", "half an install");
    put("0000000000000000000000000000000000000000000000000000000000000000.wasm", "unused");
    muse_apps_catalog_open(dir);
    CHECK(muse_apps_catalog_count() == 1 && !strcmp(muse_apps_catalog_get(0)->id, "timer"));
    CHECK(!strcmp(muse_apps_catalog_get(0)->name, "Timer") && files() == 2);

    /* Reading checks the hash: a damaged copy is removed. */
    uint8_t *data;
    size_t len;
    CHECK(muse_apps_catalog_read("timer", &data, &len, err, sizeof(err)) == NULL && len == 10 && !memcmp(data + 8, "v2", 2));
    free(data);
    CHECK(!strcmp(muse_apps_catalog_read("nope", &data, &len, err, sizeof(err)), "not_installed"));
    sha_of("v2", sha);
    char name[320];
    snprintf(name, sizeof(name), "%s.wasm", sha);
    put(name, "\0asm\1\0\0\0XX");
    CHECK(!strcmp(muse_apps_catalog_read("timer", &data, &len, err, sizeof(err)), "damaged") && data == NULL);
    CHECK(muse_apps_catalog_count() == 0 && files() == 1);

    /* An entry whose module is gone is dropped when the catalog loads. */
    CHECK(install("a", NULL, "a1", NULL, ROOM, &changed) == NULL);
    sha_of("a1", sha);
    snprintf(name, sizeof(name), "%s/%s.wasm", dir, sha);
    remove(name);
    muse_apps_catalog_open(dir);
    CHECK(muse_apps_catalog_count() == 0);

    /* At most MUSE_APPS_MAX. */
    for (int i = 0; i < MUSE_APPS_MAX; i++) {
        char id[8], tag[8];
        snprintf(id, sizeof(id), "app%d", i);
        snprintf(tag, sizeof(tag), "t%d", i);
        CHECK(install(id, NULL, tag, NULL, ROOM, &changed) == NULL);
    }
    CHECK(!strcmp(install("one-more", NULL, "x", NULL, ROOM, &changed), "too_many_apps"));
    CHECK(!strcmp(muse_apps_catalog_get(3)->name, "app3"));   /* no name: the id */
    cJSON *json = muse_apps_catalog_json();
    CHECK(cJSON_GetArraySize(json) == MUSE_APPS_MAX);
    cJSON_Delete(json);

    /* A write that fails leaves everything as it was. */
    if (geteuid() != 0) {
        chmod(dir, 0500);
        CHECK(!strcmp(install("app0", NULL, "new", NULL, ROOM, &changed), "storage_failed"));
        chmod(dir, 0700);
        CHECK(muse_apps_catalog_count() == MUSE_APPS_MAX && has_module("t0"));
        muse_apps_catalog_open(dir);
        CHECK(muse_apps_catalog_count() == MUSE_APPS_MAX);
    }

    /* A catalog or module that's there but can't be read (here unreadable,
     * on the gadget no memory or a flash error) isn't damage: nothing goes. */
    if (geteuid() != 0) {
        char cat[300];
        snprintf(cat, sizeof(cat), "%s/catalog.json", dir);
        int before = files();
        chmod(cat, 0000);
        muse_apps_catalog_open(dir);
        CHECK(muse_apps_catalog_count() == 0 && files() == before);
        /* and nothing saves over it meanwhile, which would drop its apps */
        CHECK(!strcmp(install("late", NULL, "late", NULL, ROOM, &changed), "storage_failed") && files() == before);
        chmod(cat, 0600);
        muse_apps_catalog_open(dir);
        CHECK(muse_apps_catalog_count() == MUSE_APPS_MAX);
        sha_of("t0", sha);
        snprintf(name, sizeof(name), "%s/%s.wasm", dir, sha);
        chmod(name, 0000);
        CHECK(!strcmp(muse_apps_catalog_read("app0", &data, &len, err, sizeof(err)), "no_memory") && data == NULL);
        chmod(name, 0600);
        CHECK(muse_apps_catalog_count() == MUSE_APPS_MAX && has_module("t0"));
    }

    /* Unpairing removes every app. */
    muse_apps_catalog_wipe();
    CHECK(muse_apps_catalog_count() == 0 && files() == 1);
    muse_apps_catalog_open(dir);
    CHECK(muse_apps_catalog_count() == 0);

    /* Without a partition: in memory, the same rules. */
    muse_apps_catalog_open(NULL);
    CHECK(!muse_apps_catalog_persistent());
    CHECK(install("hello", "Hello", "m1", NULL, (size_t)-1, &changed) == NULL && changed);
    CHECK(install("hello", NULL, "m2", NULL, (size_t)-1, &changed) == NULL && changed);
    CHECK(muse_apps_catalog_read("hello", &data, &len, err, sizeof(err)) == NULL && !memcmp(data + 8, "m2", 2));
    free(data);
    uint8_t *huge = calloc(1, 1100 * 1024);
    memcpy(huge, "\0asm\1\0\0\0", 8);
    CHECK(!strcmp(muse_apps_catalog_install("huge", NULL, NULL, NULL, huge, 1100 * 1024, NULL, (size_t)-1, &changed, err, sizeof(err)), "no_space"));
    free(huge);
    CHECK(muse_apps_catalog_remove("hello", &removed, err, sizeof(err)) == NULL && removed && muse_apps_catalog_count() == 0);

    if (!failures) printf("ok\n");
    return failures ? 1 : 0;
}
