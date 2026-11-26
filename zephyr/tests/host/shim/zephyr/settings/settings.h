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

/* Host shim of Zephyr settings: an in-memory store with failure injection
 * (host_zephyr.c). */

#ifndef HOST_SHIM_SETTINGS_H_
#define HOST_SHIM_SETTINGS_H_

#include <stddef.h>
#include <sys/types.h>

typedef ssize_t (*settings_read_cb)(void *cb_arg, void *data, size_t len);
typedef int (*settings_load_direct_cb)(const char *key, size_t len, settings_read_cb read_cb,
				       void *cb_arg, void *param);

int settings_save_one(const char *name, const void *value, size_t val_len);
int settings_delete(const char *name);
int settings_load_subtree_direct(const char *subtree, settings_load_direct_cb cb, void *param);
ssize_t settings_get_val_len(const char *name);

/* Test side. */
void host_settings_clear(void);
/* The nth (1-based) save from now fails with -EIO; 0 turns it off. */
void host_settings_fail_save(int nth);
/* The nth settings_delete() from now fails (-EIO); 0 turns it off. */
void host_settings_fail_delete(int nth);
/* The nth entry read from now fails (-EIO); 0 turns it off. */
void host_settings_fail_read(int nth);
/* A listing (settings_load_subtree_direct) ends, silently and returning 0,
 * after n entries, as ZMS's does at a list link it can't read; -1 turns it
 * off. Lookups by name still find every entry. */
void host_settings_list_only(int n);
/* The nth settings_get_val_len() from now fails (-EIO); 0 turns it off. */
void host_settings_fail_len(int nth);
/* Length of a stored value, or -1. */
int host_settings_len(const char *name);
int host_settings_count(void);
const void *host_settings_get(const char *name);

#endif
