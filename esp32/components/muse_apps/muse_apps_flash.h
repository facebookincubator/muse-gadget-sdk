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
 * The gadget's own files (muse_apps_flash.c): images and anything else Muse
 * puts on the gadget, in the "files" LittleFS partition, for apps to read.
 * Plain C over stdio, so tests/test_app_files.py runs it on the host. Not
 * thread-safe: muse_apps_store.c writes from one task, and reads from one
 * other (the filesystem has its own lock).
 *
 * A name is a path in the partition: segments of A-Z, a-z, 0-9, '.', '-'
 * and '_' joined by '/', none empty, none starting with '.', each under 64
 * characters, at most 8 folders deep and all of it under
 * MUSE_APPS_FLASH_NAME_MAX. Apps write it
 * with a leading '/': "/photos/cat.jpg".
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cJSON.h"

#define MUSE_APPS_FLASH_NAME_MAX 96
#define MUSE_APPS_FLASH_LIST_MAX 256  /* files listed */

bool muse_apps_flash_name_ok(const char *name);

/* Each returns NULL on success, else an error code for the reply
 * ("bad_name", "not_found", "too_large", "no_space", "storage_failed") with a
 * message in err. */

/* Writing a file: create opens a temporary file beside the name (making the
 * folders it's in), the caller writes it, then commit replaces the name with
 * it whole, or abort deletes it. */
const char *muse_apps_flash_create(const char *root, const char *name, FILE **f, char *tmp, size_t tmp_cap,
                                   char *err, size_t err_len);
const char *muse_apps_flash_commit(const char *root, const char *name, const char *tmp, bool *replaced, char *err,
                                   size_t err_len);
void muse_apps_flash_abort(const char *tmp);
/* All of it in one call. free_bytes is the room left in the partition. */
const char *muse_apps_flash_put(const char *root, const char *name, const uint8_t *data, size_t len,
                                size_t free_bytes, bool *replaced, char *err, size_t err_len);
/* *removed is false if there was no such file. Empty folders go with it. */
const char *muse_apps_flash_remove(const char *root, const char *name, bool *removed, char *err, size_t err_len);
/* Up to max bytes from offset; *out is NULL and *len 0 at or past the end.
 * The caller frees *out. */
const char *muse_apps_flash_read(const char *root, const char *name, size_t offset, size_t max, uint8_t **out,
                                 size_t *len, char *err, size_t err_len);
/* [{"name", "bytes"}, ...], sorted by name, up to MUSE_APPS_FLASH_LIST_MAX;
 * *total is the bytes of every file, *more whether some weren't listed. */
cJSON *muse_apps_flash_list(const char *root, size_t *total, bool *more);
/* Deletes what a write cut short (temporary files), at mount. */
void muse_apps_flash_tidy(const char *root);
/* Deletes everything (the gadget was unpaired); false if anything is left
 * (a file a reader held open), for a later try. */
bool muse_apps_flash_wipe(const char *root);
