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

// The board's SD card for the agent (storage.*), and for photos and
// recordings saved there. Mounted (FAT) on first use and unmounted after a
// minute idle, powering its rails down with it. Paths are relative to the
// card; ".." is refused.

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

#include "muse_board.h"
#include "muse_hw_commands_priv.h"

static const char *TAG = "link.storage";

#define MOUNT "/sdcard"
#define IDLE_US (60 * 1000000LL)
#define READ_MAX (180 * 1024)          // a result stays under the session's 256 KB in base64
#define PATH_MAX_LEN 200

static SemaphoreHandle_t s_lock;
static sdmmc_card_t *s_card;
static int64_t s_last_use;
static esp_timer_handle_t s_idle;

// ---- Host-tested: paths ------------------------------------------------------

// "photos/a.jpg" or "/photos/a.jpg" into "/sdcard/photos/a.jpg"; false for
// "..", empty parts past the first, or too long.
static bool card_path(const char *rel, char *out, size_t cap) {
    if (!rel) return false;
    while (*rel == '/') rel++;
    for (const char *p = rel; *p;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if ((n == 2 && !strncmp(p, "..", 2)) || (n == 0 && e)) return false;
        p += n + (e ? 1 : 0);
    }
    int w = snprintf(out, cap, MOUNT "%s%s", *rel ? "/" : "", rel);
    return w > 0 && (size_t)w < cap;
}

// ---- Host-tested end

static const muse_ports_t *ports(void) {
    return muse_board && muse_board->ports ? muse_board->ports() : NULL;
}

static void unmount_locked(void) {
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(MOUNT, s_card);
    s_card = NULL;
    ports()->sd_power(false);
    ESP_LOGI(TAG, "SD card unmounted");
}

// On the esp_timer task: never waits out a transfer (a big file takes seconds,
// and every timer would stall meanwhile); busy now, the next check will do.
static void idle_check(void *arg) {
    (void)arg;
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) return;
    if (s_card && esp_timer_get_time() - s_last_use > IDLE_US) unmount_locked();
    xSemaphoreGive(s_lock);
}

// Mounts the card if needed; with s_lock held. NULL or an error message.
static const char *mount_locked(void) {
    const muse_ports_t *p = ports();
    if (!p || !p->sd_power) return "this board has no SD card slot";
    s_last_use = esp_timer_get_time();
    if (s_card) return NULL;
    if (p->sd_present && !p->sd_present()) return "no SD card in the slot";
    if (p->sd_power(true) != ESP_OK) {
        p->sd_power(false);   // the rails may have counted this on before failing
        return "couldn't power the card";
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = p->sd_spi_host;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = p->sd_cs;
    slot.host_id = p->sd_spi_host;
    const esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        p->sd_power(false);
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        return err == ESP_FAIL ? "the card isn't FAT formatted" : "the card didn't answer";
    }
    if (!s_idle) {
        const esp_timer_create_args_t a = { .callback = idle_check, .name = "sd_idle" };
        if (esp_timer_create(&a, &s_idle) == ESP_OK) esp_timer_start_periodic(s_idle, 10 * 1000000LL);
    }
    ESP_LOGI(TAG, "SD card mounted: %s, %llu MB", s_card->cid.name,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    return NULL;
}

// Created once at init: made on first use, two tasks at once could each make one.
void muse_hw_storage_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static bool lock(void) {
    return s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

// Makes the directories above a file.
static void make_parents(char *path) {
    for (char *p = path + strlen(MOUNT) + 1; (p = strchr(p, '/')); p++) {
        *p = '\0';
        mkdir(path, 0775);
        *p = '/';
    }
}

// ---- For other commands: save and load files ---------------------------------

const char *hw_storage_save(const char *rel, const void *data, size_t len, bool append) {
    char path[PATH_MAX_LEN];
    if (!card_path(rel, path, sizeof(path)) || !strcmp(path, MOUNT)) return "not a file path on the card";
    if (!lock()) return "out of memory";
    const char *err = mount_locked();
    if (!err) {
        make_parents(path);
        FILE *f = fopen(path, append ? "ab" : "wb");
        if (!f) {
            err = "couldn't open the file for writing";
        } else {
            if (fwrite(data, 1, len, f) != len) err = "the card is full, or failed";
            if (fclose(f) != 0 && !err) err = "couldn't finish writing";
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

// PSRAM, for the caller to free; NULL with *err set.
uint8_t *hw_storage_load(const char *rel, size_t max, size_t *len, const char **err) {
    char path[PATH_MAX_LEN];
    *err = NULL;
    if (!card_path(rel, path, sizeof(path))) {
        *err = "not a path on the card";
        return NULL;
    }
    if (!lock()) {
        *err = "out of memory";
        return NULL;
    }
    uint8_t *buf = NULL;
    *err = mount_locked();
    if (!*err) {
        FILE *f = fopen(path, "rb");
        struct stat st;
        if (!f || fstat(fileno(f), &st) != 0) {
            *err = "no such file";
        } else if ((size_t)st.st_size > max) {
            *err = "the file is too large";
        } else if (!(buf = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM))) {
            *err = "out of memory";
        } else {
            *len = fread(buf, 1, st.st_size, f);
            buf[*len] = 0;
        }
        if (f) fclose(f);
    }
    xSemaphoreGive(s_lock);
    return buf;
}

// ---- storage.* ---------------------------------------------------------------

static cJSON *info(void) {
    const muse_ports_t *p = ports();
    if (!p || !p->sd_power) return hw_error("unsupported", "this board has no SD card slot");
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddBoolToObject(pl, "card", !p->sd_present || p->sd_present());
    if (!lock()) return hw_error("out_of_memory", "failed to allocate");
    const char *err = mount_locked();
    if (!err) {
        uint64_t total = 0, free_bytes = 0;
        esp_vfs_fat_info(MOUNT, &total, &free_bytes);
        cJSON_AddBoolToObject(pl, "mounted", true);
        cJSON_AddStringToObject(pl, "name", s_card->cid.name);
        cJSON_AddNumberToObject(pl, "total_bytes", (double)total);
        cJSON_AddNumberToObject(pl, "free_bytes", (double)free_bytes);
    } else {
        cJSON_AddBoolToObject(pl, "mounted", false);
        cJSON_AddStringToObject(pl, "error", err);
    }
    xSemaphoreGive(s_lock);
    return hw_ok(pl);
}

static cJSON *list(cJSON *params) {
    char path[PATH_MAX_LEN];
    const char *rel = hw_str(params, "path");
    if (!card_path(rel ? rel : "", path, sizeof(path))) return hw_error("invalid_params", "not a path on the card");
    if (!lock()) return hw_error("out_of_memory", "failed to allocate");
    const char *err = mount_locked();
    cJSON *result;
    DIR *d = err ? NULL : opendir(path);
    if (err) {
        result = hw_error("unavailable", err);
    } else if (!d) {
        result = hw_error("not_found", "no such directory");
    } else {
        cJSON *pl = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(pl, "entries");
        int n = 0;
        for (struct dirent *e; (e = readdir(d)) && n < 200; n++) {
            char full[PATH_MAX_LEN + 260];
            snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            struct stat st;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", e->d_name);
            bool dir = e->d_type == DT_DIR;
            cJSON_AddBoolToObject(o, "dir", dir);
            if (!dir && stat(full, &st) == 0) cJSON_AddNumberToObject(o, "bytes", (double)st.st_size);
            cJSON_AddItemToArray(arr, o);
        }
        closedir(d);
        result = hw_ok(pl);
    }
    xSemaphoreGive(s_lock);
    return result;
}

static cJSON *read_file(cJSON *params) {
    const char *rel = hw_str(params, "path");
    const char *as = hw_str(params, "as");
    bool text = as && !strcmp(as, "text");
    size_t len = 0;
    const char *err;
    uint8_t *buf = hw_storage_load(rel, READ_MAX, &len, &err);
    if (!buf) return hw_error(strstr(err, "no such") ? "not_found" : "failed", err);
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddNumberToObject(pl, "bytes", (double)len);
    if (text) {
        cJSON_AddStringToObject(pl, "text", (const char *)buf);
    } else {
        char *b64 = heap_caps_malloc((len + 2) / 3 * 4 + 1, MALLOC_CAP_SPIRAM);
        if (b64) {
            hw_base64(buf, len, b64);
            cJSON_AddStringToObject(pl, "base64", b64);
            free(b64);
        }
    }
    free(buf);
    return hw_ok(pl);
}

static cJSON *write_file(cJSON *params) {
    const char *rel = hw_str(params, "path");
    const char *text = hw_str(params, "text");
    const char *b64 = hw_str(params, "base64");
    bool append = cJSON_IsTrue(cJSON_GetObjectItem(params, "append"));
    uint8_t *data = NULL;
    size_t len;
    if (text) {
        len = strlen(text);
    } else if (b64) {
        data = heap_caps_malloc(strlen(b64) / 4 * 3 + 3, MALLOC_CAP_SPIRAM);
        if (!data) return hw_error("out_of_memory", "failed to allocate");
        len = hw_base64_decode(b64, strlen(b64), data);
    } else {
        return hw_error("missing_param", "text or base64 is required");
    }
    const char *err = hw_storage_save(rel, data ? data : (const uint8_t *)text, len, append);
    free(data);
    if (err) return hw_error("failed", err);
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddNumberToObject(pl, "bytes", (double)len);
    return hw_ok(pl);
}

static cJSON *delete_path(cJSON *params) {
    char path[PATH_MAX_LEN];
    if (!card_path(hw_str(params, "path"), path, sizeof(path)) || !strcmp(path, MOUNT)) {
        return hw_error("invalid_params", "not a path on the card");
    }
    if (!lock()) return hw_error("out_of_memory", "failed to allocate");
    const char *err = mount_locked();
    cJSON *result;
    if (err) {
        result = hw_error("unavailable", err);
    } else if (unlink(path) == 0 || rmdir(path) == 0) {
        result = hw_ok(NULL);
    } else {
        result = hw_error(errno == ENOTEMPTY ? "not_empty" : "not_found",
                          errno == ENOTEMPTY ? "the directory isn't empty" : "no such file or directory");
    }
    xSemaphoreGive(s_lock);
    return result;
}

void muse_hw_storage_register(cJSON *commands) {
    const muse_ports_t *p = ports();
    if (!p || !p->sd_power) return;
    hw_add(commands, "storage.info", "The SD card: whether there is one, its size and free space.",
           NULL, NULL, 10000);
    hw_add(commands, "storage.list", "List a directory on the SD card (name, dir, bytes).", NULL,
           hw_params("path", hw_param("string", "A directory; default the top.")), 10000);
    hw_add(commands, "storage.read", "Read a file from the SD card, up to 180 KB, as base64 (default) or text.",
           hw_params("path", hw_param("string", "The file, e.g. photos/door.jpg.")),
           hw_params("as", hw_param("string", "text or base64.")), 15000);
    cJSON *w = cJSON_CreateObject();
    cJSON_AddItemToObject(w, "text", hw_param("string", "The contents as text; or base64."));
    cJSON_AddItemToObject(w, "base64", hw_param("string", "The contents as base64."));
    cJSON_AddItemToObject(w, "append", hw_param("boolean", "Add to the end; default replaces."));
    hw_add(commands, "storage.write", "Write a file on the SD card, making its directories.",
           hw_params("path", hw_param("string", "The file.")), w, 15000);
    hw_add(commands, "storage.delete", "Delete a file, or an empty directory, on the SD card.",
           hw_params("path", hw_param("string", "The file or directory.")), NULL, 10000);
}

cJSON *muse_hw_storage_command(const char *command, cJSON *params) {
    if (!strcmp(command, "storage.info")) return info();
    if (!strcmp(command, "storage.list")) return list(params);
    if (!strcmp(command, "storage.read")) return read_file(params);
    if (!strcmp(command, "storage.write")) return write_file(params);
    if (!strcmp(command, "storage.delete")) return delete_path(params);
    return NULL;
}
