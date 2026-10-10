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

/* Drives the battery meter (muse_battery.c) for test_muse_battery.py, one command a line:
 *   t <us>                 the clock
 *   dump ... end           the lines esp_pm_dump_locks() prints from now on
 *   p <usb> <pct> <mv>     a battery reading
 *   s <off> <resting>      the screen and CPU state
 *   w <us>                 a wake from light sleep, after <us> asleep
 *   h <free> <min> <largest> [<ret_free> <ret_largest>]   internal RAM: free now, lowest since boot,
 *                          largest block; retention-capable free and largest block
 *   c <0|1>                muse_battery_note_cpu_pd()
 *   x                      muse_battery_reset()
 *   j                      prints muse_battery_json()
 *   r                      prints muse_battery_read() as JSON
 *   k                      prints muse_battery_saved_json(), or null
 *   save <file>            writes the RTC memory, for a later run to boot with
 * and before any of those, a boot:
 *   load <file>            the RTC memory a run saved
 *   flash <file>           the NVS, kept in the file as it's committed (none: no NVS)
 *   boot <reason>          esp_reset_reason(), ESP_RST_POWERON by default */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_pm.h"
#include "esp_system.h"

/* Built in, for its RTC memory (s_saved). */
#include "muse_battery.c"

static int64_t s_clock;
static esp_reset_reason_t s_boot_reason = ESP_RST_POWERON;

esp_reset_reason_t esp_reset_reason(void)
{
    return s_boot_reason;
}
static char s_dump_text[4096];
static size_t s_heap_free = 60000, s_heap_min = 40000, s_heap_largest = 30000;
static size_t s_ret_free = 20000, s_ret_largest = 12000;

size_t heap_caps_get_free_size(int caps)
{
    return caps == MALLOC_CAP_INTERNAL ? s_heap_free : caps == MALLOC_CAP_RETENTION ? s_ret_free : 0;
}

size_t heap_caps_get_minimum_free_size(int caps)
{
    return caps == MALLOC_CAP_INTERNAL ? s_heap_min : 0;
}

size_t heap_caps_get_largest_free_block(int caps)
{
    return caps == MALLOC_CAP_INTERNAL ? s_heap_largest : caps == MALLOC_CAP_RETENTION ? s_ret_largest : 0;
}
static esp_pm_light_sleep_cb_t s_wake_cb;

int64_t esp_timer_get_time(void)
{
    return s_clock;
}

esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *cbs)
{
    s_wake_cb = cbs->exit_cb;
    return ESP_OK;
}

esp_err_t esp_pm_dump_locks(FILE *stream)
{
    fputs(s_dump_text, stream);
    return ESP_OK;
}

/* One namespace, a few keys; a u16 is a two-byte blob. */
typedef struct {
    char key[16];
    size_t len;
    char value[SAVED_MAX];
} nvs_key_t;
static nvs_key_t s_nvs_keys[4];
static char s_flash[512];

static nvs_key_t *nvs_find(const char *key, bool add)
{
    nvs_key_t *unused = NULL;
    for (size_t i = 0; i < sizeof(s_nvs_keys) / sizeof(s_nvs_keys[0]); i++) {
        if (!strcmp(s_nvs_keys[i].key, key)) {
            return &s_nvs_keys[i];
        }
        if (!unused && !s_nvs_keys[i].key[0]) {
            unused = &s_nvs_keys[i];
        }
    }
    if (!add || !unused) {
        return NULL;
    }
    strlcpy(unused->key, key, sizeof(unused->key));
    return unused;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    (void)name;
    (void)mode;
    *handle = 1;
    return s_flash[0] ? ESP_OK : ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    (void)handle;
    nvs_key_t *k = nvs_find(key, false);
    if (!k) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (value && *length < k->len) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (value) {
        memcpy(value, k->value, k->len);
    }
    *length = k->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    nvs_key_t *k = nvs_find(key, true);
    if (!k || length > sizeof(k->value)) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(k->value, value, length);
    k->len = length;
    return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value)
{
    size_t length = sizeof(*value);
    return nvs_get_blob(handle, key, value, &length);
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    return nvs_set_blob(handle, key, &value, sizeof(value));
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    nvs_key_t *k = nvs_find(key, false);
    if (!k) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memset(k, 0, sizeof(*k));
    return ESP_OK;
}

/* Only what's committed is there for the next run. */
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    FILE *f = fopen(s_flash, "wb");
    if (!f || fwrite(s_nvs_keys, sizeof(s_nvs_keys), 1, f) != 1) {
        fprintf(stderr, "can't write %s\n", s_flash);
        exit(2);
    }
    fclose(f);
    return ESP_OK;
}

int main(void)
{
    char line[512];
    bool booted = false;
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        if (!booted && !strncmp(line, "load ", 5)) {
            FILE *f = fopen(line + 5, "rb");
            if (!f || fread(&s_saved, sizeof(s_saved), 1, f) != 1) {
                fprintf(stderr, "can't load %s\n", line + 5);
                return 2;
            }
            fclose(f);
            continue;
        }
        if (!booted && !strncmp(line, "flash ", 6)) {
            strlcpy(s_flash, line + 6, sizeof(s_flash));
            FILE *f = fopen(s_flash, "rb");   /* none yet: blank */
            if (f && fread(s_nvs_keys, sizeof(s_nvs_keys), 1, f) != 1) {
                fprintf(stderr, "can't read %s\n", s_flash);
                return 2;
            }
            if (f) {
                fclose(f);
            }
            continue;
        }
        if (!booted && !strncmp(line, "boot ", 5)) {
            s_boot_reason = (esp_reset_reason_t)atoi(line + 5);
            continue;
        }
        if (!booted) {
            muse_battery_init();
            booted = true;
        }
        if (!strncmp(line, "t ", 2)) {
            s_clock = strtoll(line + 2, NULL, 10);
        } else if (!strcmp(line, "dump")) {
            s_dump_text[0] = '\0';
            while (fgets(line, sizeof(line), stdin) && strcmp(line, "end\n")) {
                strlcat(s_dump_text, line, sizeof(s_dump_text));
            }
        } else if (!strncmp(line, "p ", 2)) {
            int usb;
            muse_power_t p = { 0 };
            sscanf(line + 2, "%d %d %d", &usb, &p.battery_pct, &p.battery_mv);
            p.usb = usb;
            muse_battery_note_power(&p, !p.usb && p.battery_pct >= 0);
        } else if (!strncmp(line, "s ", 2)) {
            int off, resting;
            sscanf(line + 2, "%d %d", &off, &resting);
            muse_battery_note_state(off, resting);
        } else if (!strncmp(line, "w ", 2)) {
            s_wake_cb(strtoll(line + 2, NULL, 10), NULL);
        } else if (!strncmp(line, "h ", 2)) {
            sscanf(line + 2, "%zu %zu %zu %zu %zu", &s_heap_free, &s_heap_min, &s_heap_largest, &s_ret_free,
                   &s_ret_largest);
        } else if (!strncmp(line, "c ", 2)) {
            muse_battery_note_cpu_pd(atoi(line + 2));
        } else if (!strcmp(line, "x")) {
            muse_battery_reset();
        } else if (!strcmp(line, "j")) {
            static char json[2048];
            muse_battery_json(json, sizeof(json));
            puts(json);
        } else if (!strcmp(line, "r")) {
            muse_battery_t m;
            muse_battery_read(&m);
            int rate10 = -1, full_h = -1;
            muse_battery_drain(&m, &rate10, &full_h);
            printf("{\"rate10\":%d,\"full_h\":%d,\"started\":%d,\"running\":%d,\"secs\":%lld,\"pct\":[%d,%d],"
                   "\"mv\":[%d,%d],\"screen_off_pm\":%d,\"resting_pm\":%d,\"slept_pm\":%d,\"sleeps\":%lu,"
                   "\"busy_pm\":%d,\"awake\":\"%s\"}\n",
                   rate10, full_h, m.started, m.running, (long long)m.secs, m.pct_start, m.pct_now, m.mv_start,
                   m.mv_now, m.screen_off_pm, m.resting_pm, m.slept_pm, (unsigned long)m.sleeps, m.busy_pm, m.awake);
        } else if (!strcmp(line, "k")) {
            const char *saved = muse_battery_saved_json();
            puts(saved ? saved : "null");
        } else if (!strncmp(line, "save ", 5)) {
            FILE *f = fopen(line + 5, "wb");
            if (!f || fwrite(&s_saved, sizeof(s_saved), 1, f) != 1) {
                fprintf(stderr, "can't save %s\n", line + 5);
                return 2;
            }
            fclose(f);
        } else {
            fprintf(stderr, "bad command: %s\n", line);
            return 2;
        }
    }
    return 0;
}
