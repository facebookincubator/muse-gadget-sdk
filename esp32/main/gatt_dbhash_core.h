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
 * The GATT Database Hash (Core 5.1+ Vol 3 Part G 7.3): AES-CMAC with an all-zero key over the attributes in handle order,
 * each as handle, type and (declarations only) value, little-endian. No
 * NimBLE in here: gatt_dbhash.c walks the live table, the host test runs this
 * against tools/gatt_dbhash.py.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATT_DBHASH_VALUE_MAX 19   /* a characteristic declaration with a 128-bit UUID */

typedef struct {
    uint16_t handle;
    uint16_t type;    /* the attribute type (16-bit UUID); 0 for one the hash skips (a value, a 128-bit descriptor) */
    uint8_t len;
    uint8_t value[GATT_DBHASH_VALUE_MAX];
} gatt_dbhash_attr_t;

/* The hash's input from attributes sorted by handle: services (0x2800,
 * 0x2801), includes (0x2802), characteristic declarations (0x2803) and
 * extended properties (0x2900) with their values; user description, CCC,
 * SCC, presentation and aggregate formats (0x2901-0x2905) without; nothing
 * else. Returns the length, or 0 if it doesn't fit cap. */
size_t gatt_dbhash_serialize(const gatt_dbhash_attr_t *attrs, size_t n, uint8_t *out, size_t cap);

/* AES-CMAC of msg under key (all zeros for the Database Hash), as the
 * characteristic carries it: least significant octet first. */
bool gatt_dbhash_cmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t out[16]);

#ifdef __cplusplus
}
#endif
