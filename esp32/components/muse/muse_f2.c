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
#include "muse_f2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_timer.h"
#include "muse_board.h"
#include "muse_state.h"
#include "muse_console.h"
#include "muse_voice.h"
#include "muse_ui.h"
#include "muse_input.h"
#include "muse_restart.h"
#include "sdkconfig.h"

static const char *TAG = "muse_f2";
static const char *s_boot_power = "?";   /* the power source this boot started on */
/* Where this boot's check is, for >f2.result while it runs: 0 not running,
 * 1 started, 2 waiting for the clock floor, 3 reading the app, 4 reading NVS. */
static volatile int s_stage;
/* What this boot's check did, for >f2.result (a battery boot leaves no
 * console to read until USB comes back, and nothing resets in between). */
static char s_this_boot[96] = "not armed this boot";
#define NS "muse_f2"

static void restart_now(void *arg)
{
    (void)arg;
    muse_restart(MUSE_RESTART_CONSOLE);
}

void muse_f2_arm(int restart_s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "arm", 1);
        nvs_commit(h);
        nvs_close(h);
        printf("@f2 armed: the next boot on battery checks flash 5 s in (unplug USB, then restart)\n");
        if (restart_s > 0 && restart_s <= 120) {
            static esp_timer_handle_t t;
            const esp_timer_create_args_t args = { .callback = restart_now, .name = "f2_restart" };
            if (t || esp_timer_create(&args, &t) == ESP_OK) {
                esp_timer_stop(t);
                esp_timer_start_once(t, (uint64_t)restart_s * 1000000);
                printf("@f2 restarting in %d s\n", restart_s);
                ESP_LOGI(TAG, "armed; restarting in %d s", restart_s);
            }
        }
    } else {
        printf("@f2 no NVS\n");
    }
    fflush(stdout);
}

void muse_f2_print_result(void)
{
    char res[220] = "none";
    size_t n = sizeof(res);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "res", res, &n) != ESP_OK) strlcpy(res, "none", sizeof(res));
        nvs_close(h);
    }
    printf("@f2 this boot: %s\n", s_this_boot);
    int stage = s_stage;
    if (stage) {
        printf("@f2 running: stage %d (%s), cpu %u MHz\n", stage,
               stage == 1 ? "started" : stage == 2 ? "waiting for the clock floor" : stage == 3 ? "reading the app" : "reading NVS",
               (unsigned)(esp_clk_cpu_freq() / 1000000));
    }
    printf("@f2 %s\n", res);
    fflush(stdout);
    ESP_LOGI(TAG, "last flash check: %s", res);   /* for log captures that don't take stdout (ESP-Iris) */
}

/* Every NVS entry read back (each read checks the entry's CRC). */
static void nvs_walk(int *entries, int *errors)
{
    /* From the heap, only while the check runs, PSRAM first: a bench check's
     * 4 KB never takes internal RAM for good. */
    enum { BUF_LEN = 4096 };
    uint8_t *buf = heap_caps_malloc(BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    buf = buf ? buf : malloc(BUF_LEN);
    if (!buf) {
        (*errors)++;
        ESP_LOGW(TAG, "nvs walk: no memory");
        return;
    }
    nvs_iterator_t it = NULL;
    esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
    while (e == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        (*entries)++;
        nvs_handle_t h;
        if (nvs_open(info.namespace_name, NVS_READONLY, &h) == ESP_OK) {
            size_t len = BUF_LEN;
            esp_err_t r = ESP_OK;
            if (info.type == NVS_TYPE_STR) {
                r = nvs_get_str(h, info.key, (char *)buf, &len);
            } else if (info.type == NVS_TYPE_BLOB) {
                r = nvs_get_blob(h, info.key, buf, &len);
                if (r == ESP_ERR_NVS_INVALID_LENGTH) r = ESP_OK;   /* larger than the buffer: its CRC still checked */
            } else {
                union { uint8_t u8; int8_t i8; uint16_t u16; int16_t i16; uint32_t u32; int32_t i32;
                        uint64_t u64; int64_t i64; } v;
                switch (info.type) {
                case NVS_TYPE_U8: r = nvs_get_u8(h, info.key, &v.u8); break;
                case NVS_TYPE_I8: r = nvs_get_i8(h, info.key, &v.i8); break;
                case NVS_TYPE_U16: r = nvs_get_u16(h, info.key, &v.u16); break;
                case NVS_TYPE_I16: r = nvs_get_i16(h, info.key, &v.i16); break;
                case NVS_TYPE_U32: r = nvs_get_u32(h, info.key, &v.u32); break;
                case NVS_TYPE_I32: r = nvs_get_i32(h, info.key, &v.i32); break;
                case NVS_TYPE_U64: r = nvs_get_u64(h, info.key, &v.u64); break;
                case NVS_TYPE_I64: r = nvs_get_i64(h, info.key, &v.i64); break;
                default: r = ESP_OK; break;
                }
            }
            if (r != ESP_OK) {
                (*errors)++;
                ESP_LOGW(TAG, "nvs %s/%s (type 0x%02x): %s", info.namespace_name, info.key, (unsigned)info.type,
                         esp_err_to_name(r));
            }
            nvs_close(h);
        } else {
            (*errors)++;
        }
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    memset(buf, 0, BUF_LEN);
    free(buf);
}

/* Waits for this boot to run on battery (detect the unplug, don't
 * time it): the power chip can still read USB for a while after the
 * cable goes (the StopWatch's M5PM1 does). 1 battery (*after_ms how
 * long it took), 0 USB the whole F2_BATTERY_WAIT_S, -1 the board can't tell. */
#define F2_BATTERY_WAIT_S 60
static int boot_on_battery(int *after_ms)
{
    bool known = false;
    for (int i = 0; i < F2_BATTERY_WAIT_S * 2; i++) {
        muse_power_t p = { 0 };
        if (muse_board && muse_board->read_power && muse_board->read_power(&p) == ESP_OK &&
            (p.usb || p.battery_mv > 0 || p.battery_pct >= 0)) {
            known = true;
            if (!p.usb) {
                *after_ms = i * 500;
                return 1;
            }
        } else if (i >= 20 && !known) {
            return -1;   /* 10 s and still nothing to read */
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return known ? 0 : -1;
}

static void f2_task(void *arg)
{
    (void)arg;
    /* F2 asks for a boot on battery: a boot on USB (a late unplug) keeps the
     * arm for the next one rather than recording a USB run as the battery's. */
    s_stage = 1;
    int after_ms = 0;
    int battery = boot_on_battery(&after_ms);
    if (battery == 0) {
        ESP_LOGW(TAG, "flash check armed: this boot is on USB, waiting for a boot on battery");
        strlcpy(s_this_boot, "armed, but USB for the whole 60 s: kept for a battery boot", sizeof(s_this_boot));
        s_stage = 0;
        vTaskDelete(NULL);
        return;
    }
    static char power[40];
    if (battery == 1 && after_ms == 0) {
        strlcpy(power, "battery", sizeof(power));
    } else if (battery == 1) {
        snprintf(power, sizeof(power), "battery (read %d.%d s in)", after_ms / 1000, after_ms % 1000 / 100);
    } else {
        strlcpy(power, "power unknown (not a battery run)", sizeof(power));
    }
    s_boot_power = power;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "arm");   /* once */
        nvs_commit(h);
        nvs_close(h);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
    s_stage = 2;
    /* A gadget has failed reading flash at its minimum clock. On the ESP32 that
     * condition doesn't exist: with power management on, ESP-IDF holds a
     * full-speed lock on each core whenever it isn't idle ("rtos0/1",
     * esp_pm pm_impl.c), so code (flash reads included) always runs at the
     * top clock, and DFS's floor applies only while a core idles or
     * light-sleeps. Measured on the StopWatch: 240 MHz throughout, with
     * the screen dark and DFS set to the crystal (the conditions below). */
    muse_power_t p = { 0 };
    bool have_power = muse_board && muse_board->read_power && muse_board->read_power(&p) == ESP_OK;
    unsigned mhz = (unsigned)(esp_clk_cpu_freq() / 1000000);
    const esp_partition_t *run = esp_ota_get_running_partition();
    int app_ok = 0;
    s_stage = 3;
    for (int i = 0; i < 3 && run; i++) {
        esp_image_metadata_t md;
        const esp_partition_pos_t pos = { .offset = run->address, .size = run->size };
        if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) == ESP_OK) app_ok++;
    }
    int entries = 0, errors = 0;
    s_stage = 4;
    nvs_walk(&entries, &errors);
    unsigned mhz_end = (unsigned)(esp_clk_cpu_freq() / 1000000);
    /* Why the clock is where it is: the low-power conditions (muse_input.c's
     * update_power), so a check that stayed at full speed says which failed. */
    char why[64];
    snprintf(why, sizeof(why), "asleep %d dark %d resting %d host %d low %d", muse_state_asleep(), muse_ui_dark(),
             muse_voice_resting(), muse_console_host(), muse_input_cpu_low());
    char res[220];
    snprintf(res, sizeof(res), "%s: app %d/3 verified (%s), nvs %d entries %d errors, cpu %u-%u MHz (%s), on %s, now %s %d mV",
             app_ok == 3 && errors == 0 ? "PASS" : "FAIL", app_ok, run ? run->label : "?", entries, errors, mhz, mhz_end, why,
             s_boot_power,
             !have_power ? "power ?" : p.usb ? "USB" : "battery", have_power ? p.battery_mv : 0);
    ESP_LOGW(TAG, "flash check %s", res);
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "res", res);
        nvs_commit(h);
        nvs_close(h);
    }
    strlcpy(s_this_boot, "checked (the result below is this boot's)", sizeof(s_this_boot));
    s_stage = 0;
    vTaskDelete(NULL);
}

void muse_f2_boot(void)
{
    nvs_handle_t h;
    uint8_t arm = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    bool armed = nvs_get_u8(h, "arm", &arm) == ESP_OK && arm;
    nvs_close(h);
    if (!armed) {
        return;
    }
    if (xTaskCreate(f2_task, "muse_f2", 4096, NULL, 2, NULL) != pdPASS) {
        snprintf(s_this_boot, sizeof(s_this_boot), "armed, but its task didn't start (internal %u free, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        ESP_LOGE(TAG, "flash check not started: %s", s_this_boot);
    } else {
        strlcpy(s_this_boot, "armed, task started", sizeof(s_this_boot));
    }
}
