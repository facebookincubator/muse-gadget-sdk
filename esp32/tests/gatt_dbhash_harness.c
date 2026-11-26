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
/* main/gatt_dbhash_core.c on the host (tests/test_gatt_dbhash.py): prints the
 * serialization of a fixed table and CMACs, for the Python reference. */
#include <stdio.h>
#include <string.h>

#include "gatt_dbhash_core.h"

static void hex(const char *tag, const uint8_t *b, size_t n)
{
    printf("%s ", tag);
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

int main(void)
{
    /* Sorted by handle, as the walk hands it over. */
    gatt_dbhash_attr_t t[] = {
        { 1, 0x2800, 2, { 0x00, 0x18 } },                                   /* GAP */
        { 2, 0x2803, 5, { 0x02, 0x03, 0x00, 0x00, 0x2a } },                 /* Device Name decl */
        { 3, 0, 0, { 0 } },                                                 /* its value: skipped */
        { 4, 0x2800, 2, { 0x01, 0x18 } },                                   /* GATT */
        { 5, 0x2803, 5, { 0x20, 0x06, 0x00, 0x05, 0x2a } },                 /* Service Changed decl */
        { 6, 0, 0, { 0 } },
        { 7, 0x2902, 0, { 0 } },                                            /* CCC: handle and type only */
        { 8, 0x2800, 16, { 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e } },
        { 9, 0x2803, 19, { 0x10, 0x0a, 0x00, 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e } },
        { 10, 0, 0, { 0 } },
        { 11, 0x2902, 0, { 0 } },
        { 12, 0x2901, 0, { 0 } },                                           /* user description: no value */
        { 13, 0x2900, 2, { 0x01, 0x00 } },                                  /* extended properties: with value */
        { 14, 0x2908, 0, { 0 } },                                           /* another descriptor: skipped */
    };
    uint8_t buf[512];
    size_t n = gatt_dbhash_serialize(t, sizeof(t) / sizeof(t[0]), buf, sizeof(buf));
    hex("serial", buf, n);
    printf("small %zu\n", gatt_dbhash_serialize(t, sizeof(t) / sizeof(t[0]), buf, 10));   /* doesn't fit: 0 */
    static const uint8_t zero[16];
    uint8_t h[16];
    if (!gatt_dbhash_cmac(zero, buf, n, h)) { printf("cmac failed\n"); return 1; }
    hex("hash", h, 16);
    /* RFC 4493 example 2 (16-byte message), reversed as the characteristic carries it. */
    static const uint8_t k[16] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
    static const uint8_t m[16] = { 0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a };
    if (!gatt_dbhash_cmac(k, m, 16, h)) { printf("cmac failed\n"); return 1; }
    hex("rfc4493", h, 16);
    printf("PASS gatt_dbhash\n");
    return 0;
}
