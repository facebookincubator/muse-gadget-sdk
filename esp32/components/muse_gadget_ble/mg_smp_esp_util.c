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

/* The pure parts of mg_smp_esp.c (no IDF): host-tested by tests/test_mg_smp.py (EspUtil). */

#include "mg_smp_esp.h"

#include <ctype.h>
#include <stdlib.h>

/* esp_ota_img_states_t, as esp_ota_ops.h numbers them (checked by a static
 * assert in mg_smp_esp.c, where the real enum is in scope). */
#define ST_NEW 0x0
#define ST_PENDING_VERIFY 0x1

void mg_smp_esp_parse_version(const char *s, mg_smp_version_t *out)
{
    unsigned long v[3] = { 0, 0, 0 };
    if (s && (*s == 'v' || *s == 'V')) {
        s++;
    }
    for (int i = 0; s && i < 3; i++) {
        if (!isdigit((unsigned char)*s)) {
            break;
        }
        char *end;
        v[i] = strtoul(s, &end, 10);
        s = end;
        if (*s != '.') {
            break;
        }
        s++;
    }
    out->major = (uint8_t)(v[0] > 0xff ? 0xff : v[0]);
    out->minor = (uint8_t)(v[1] > 0xff ? 0xff : v[1]);
    out->revision = (uint16_t)(v[2] > 0xffff ? 0xffff : v[2]);
    out->build = 0;   /* the build lives in the build id, not in the version */
}

#define KEEP_TRIES(m) (((unsigned)(m) >> 2) & 7u)
#define KEEP_SLOT(u) (((u) / MG_SMP_ESP_KEEP_PERIOD) & 7u)

bool mg_smp_esp_keep_due(uint8_t marks, uint32_t uptime)
{
    return KEEP_TRIES(marks) == 0 || KEEP_SLOT(uptime) != ((unsigned)marks >> 5);
}

uint8_t mg_smp_esp_keep_tried(uint8_t marks, uint32_t uptime)
{
    unsigned tries = KEEP_TRIES(marks) < MG_SMP_ESP_KEEP_TRIES ? KEEP_TRIES(marks) + 1 : MG_SMP_ESP_KEEP_TRIES;
    return (uint8_t)((marks & 0x03u) | (tries << 2) | (KEEP_SLOT(uptime) << 5));
}

bool mg_smp_esp_keep_spent(uint8_t marks)
{
    return KEEP_TRIES(marks) >= MG_SMP_ESP_KEEP_TRIES;
}

uint32_t mg_smp_esp_build_id(const uint8_t elf_sha256[32])
{
    return (uint32_t)elf_sha256[0] | (uint32_t)elf_sha256[1] << 8 | (uint32_t)elf_sha256[2] << 16 |
           (uint32_t)elf_sha256[3] << 24;
}

mg_smp_rb_t mg_smp_esp_verdict(int state, bool kept_ok, mg_smp_rb_t kept)
{
    if (state == ST_PENDING_VERIFY) {
        return MG_SMP_RB_PROVEN;   /* the bootloader turned NEW into PENDING_VERIFY */
    }
    if (state == ST_NEW) {
        return MG_SMP_RB_ABSENT;   /* the app asked for a monitored boot; the bootloader ignored it */
    }
    return kept_ok ? kept : MG_SMP_RB_UNPROVEN;
}

const char *mg_smp_esp_rollback_name(mg_smp_rb_t rb)
{
    switch (rb) {
    case MG_SMP_RB_PROVEN: return "rollback proven";
    case MG_SMP_RB_ABSENT: return "NO rollback (updates are permanent)";
    default: return "rollback unproven (no test boot yet)";
    }
}

const char *mg_smp_esp_stop_text(bool installed)
{
    return installed ? "Installed; restart to finish" : "Nothing was installed";
}
