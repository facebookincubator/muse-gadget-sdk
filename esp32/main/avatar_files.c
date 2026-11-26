/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "muse_avatar.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

static atomic_flag s_mount = ATOMIC_FLAG_INIT;
/* Shared mount owner for avatar + Apps builds. Do not format existing data
 * after a failed mount. A blank partition is checked in full before format. */
bool muse_avatar_files_mount(void) {
    while (atomic_flag_test_and_set(&s_mount)) vTaskDelay(1);
    bool ok = esp_littlefs_mounted("files");
    if (!ok) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "files");
        if (p) {
            const esp_vfs_littlefs_conf_t conf = {.base_path = "/files", .partition_label = "files",
                                               .format_if_mount_failed = false};
            ok = esp_vfs_littlefs_register(&conf) == ESP_OK;
            if (!ok) {
                uint32_t block[128];
                bool blank = true;
                for (size_t off = 0; blank && off < p->size; off += sizeof(block)) {
                    size_t n = p->size - off; if (n > sizeof(block)) n = sizeof(block);
                    if (esp_partition_read(p, off, block, n) != ESP_OK) { blank = false; break; }
                    for (size_t i = 0; i < n / sizeof(uint32_t); i++) if (block[i] != UINT32_MAX) { blank = false; break; }
                    taskYIELD();
                }
                if (blank && esp_littlefs_format("files") == ESP_OK) ok = esp_vfs_littlefs_register(&conf) == ESP_OK;
            }
        }
    }
    atomic_flag_clear(&s_mount);
    return ok;
}

#if CONFIG_MUSE_AVATAR_ANIMATIONS || CONFIG_MUSE_PIXEL_AVATAR
bool muse_avatar_files_space(size_t needed) {
    size_t total=0,used=0;
    return muse_avatar_files_mount() && esp_littlefs_info("files",&total,&used)==ESP_OK &&
        used<=total && needed<=total-used;
}
#endif
