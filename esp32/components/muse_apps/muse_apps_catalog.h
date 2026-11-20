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

#pragma once

/*
 * The apps installed on this gadget (muse_apps_catalog.c): which there are and
 * their files. Plain C over stdio, so tests/test_app_store.py runs it on the
 * host. Not thread-safe: muse_apps_store.c calls it from one task.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "muse_apps_icons.h"

#define MUSE_APPS_MAX 24
#define MUSE_APPS_ID_MAX 32    /* "pomodoro": a-z, 0-9, - and _, under 32 */
#define MUSE_APPS_NAME_MAX 48  /* what the launcher shows, UTF-8 */
#define MUSE_APPS_SHA_HEX 64

typedef struct {
    char id[MUSE_APPS_ID_MAX];
    char name[MUSE_APPS_NAME_MAX];
    char sha256[MUSE_APPS_SHA_HEX + 1];
    uint32_t bytes;
    char icon[MUSE_APPS_LOOK_MAX];   /* a glyph's name (muse_apps_icons.h), or "" */
    char color[MUSE_APPS_LOOK_MAX];  /* a colour's name, or "" */
} muse_apps_entry_t;

/* Loads the catalog from a directory (a mounted partition), dropping entries
 * whose file is missing and deleting files no entry names; with NULL, apps
 * live in memory until restart. */
void muse_apps_catalog_open(const char *dir);
bool muse_apps_catalog_persistent(void);
int muse_apps_catalog_count(void);
const muse_apps_entry_t *muse_apps_catalog_get(int i);

/* Each returns NULL on success, else an error code for the reply ("bad_id",
 * "bad_icon", "bad_color", "invalid_app", "hash_mismatch", "too_many_apps", "no_space",
 * "storage_failed", "not_installed", "damaged") with a message in err. */

/* Installs data as app id, or updates it. name, icon and color NULL or ""
 * keep what it had (the name defaults to the id). want_sha256, if given, must
 * be the data's. free_bytes is the room left in the directory. *changed is
 * false if it had all of these already. */
const char *muse_apps_catalog_install(const char *id, const char *name, const char *icon, const char *color,
                                      const uint8_t *data, size_t len, const char *want_sha256,
                                      size_t free_bytes, bool *changed, char *err, size_t err_len);
/* *removed is false if there was no such app. */
const char *muse_apps_catalog_remove(const char *id, bool *removed, char *err, size_t err_len);
/* The app's bytes, checked against its hash; the caller frees *out. A damaged
 * app is removed. */
const char *muse_apps_catalog_read(const char *id, uint8_t **out, size_t *len, char *err, size_t err_len);
/* Removes every app (the gadget was unpaired); false if one stayed on flash. */
bool muse_apps_catalog_wipe(void);

/* [{"id", "name", "sha256", "bytes", "icon", "color"}, ...] (icon and color if set), in the order they were installed;
 * NULL when memory runs out. */
cJSON *muse_apps_catalog_json(void);
/* The SHA-256 of data as 64 lowercase hex digits. */
void muse_apps_sha256_hex(const uint8_t *data, size_t len, char out[MUSE_APPS_SHA_HEX + 1]);
