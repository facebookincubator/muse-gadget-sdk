// SPDX-License-Identifier: Apache-2.0
#include "link_power.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_counts[LINK_POWER_OWNER_COUNT];
static uint32_t s_total;
static const char *const TAG = "link.power";
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_pm_locks[LINK_PM_LOCK_COUNT];
static bool s_online_pm_ready;
#endif

bool link_power_enable_wifi_modem_sleep(void) {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi online power save unavailable: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool link_power_enable_online_pm(void) {
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
    if (s_online_pm_ready) return true;
    static const char *const names[] = {"link_panel", "link_sd", "link_mic", "link_beep"};
    for (int i = 0; i < LINK_PM_LOCK_COUNT; ++i) {
        if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, names[i], &s_pm_locks[i]) != ESP_OK) {
            ESP_LOGW(TAG, "PM lock creation failed: %s", names[i]);
            goto fail;
        }
    }
    const esp_pm_config_t config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "automatic Light Sleep unavailable: %s", esp_err_to_name(err));
        goto fail;
    }
    s_online_pm_ready = true;
    ESP_LOGI(TAG, "online PM enabled: DFS 80-%d MHz, automatic Light Sleep",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    return true;
fail:
    for (int i = 0; i < LINK_PM_LOCK_COUNT; ++i) {
        if (s_pm_locks[i]) {
            esp_pm_lock_delete(s_pm_locks[i]);
            s_pm_locks[i] = NULL;
        }
    }
    return false;
#else
    ESP_LOGW(TAG, "online PM requires PM_ENABLE and FreeRTOS tickless idle");
    return false;
#endif
}

bool link_power_pm_enter(link_pm_operation_t operation) {
    if (operation >= LINK_PM_LOCK_COUNT) return false;
#if CONFIG_PM_ENABLE
    return !s_online_pm_ready || esp_pm_lock_acquire(s_pm_locks[operation]) == ESP_OK;
#else
    return true;
#endif
}

void link_power_pm_leave(link_pm_operation_t operation) {
    if (operation >= LINK_PM_LOCK_COUNT) return;
#if CONFIG_PM_ENABLE
    if (s_online_pm_ready) esp_pm_lock_release(s_pm_locks[operation]);
#endif
}

bool link_power_light_sleep_stats(uint32_t *count, uint64_t *total_us,
                                  uint32_t *rejected, char **raw_dump,
                                  const char **reason) {
    if (raw_dump) *raw_dump = NULL;
    if (reason) *reason = "invalid_arguments";
#if CONFIG_PM_ENABLE && CONFIG_PM_PROFILING
    if (!count || !total_us || !rejected || !raw_dump || !reason) return false;
    if (!s_online_pm_ready) {
        *reason = "online_pm_not_ready";
        return false;
    }
    // The Watcher captures this IDF diagnostic with fmemopen too. A bounded
    // buffer avoids changing allocation size while the PM locks are dumped.
    enum { PM_DUMP_CAPACITY = 4096 };
    char *dump = calloc(1, PM_DUMP_CAPACITY);
    if (!dump) {
        *reason = "out_of_memory";
        return false;
    }
    FILE *stream = fmemopen(dump, PM_DUMP_CAPACITY - 1, "w");
    if (!stream) {
        free(dump);
        *reason = "capture_open_failed";
        return false;
    }
    esp_err_t err = esp_pm_dump_locks(stream);
    int close_result = fclose(stream);
    if (err != ESP_OK || close_result != 0) {
        free(dump);
        *reason = "pm_dump_failed";
        return false;
    }
    *raw_dump = dump; // Keep the exact IDF text even when parsing fails.
    // IDF 6.0.1 prints "SLEEP  <freq>M  <Time(us)>  <Time(%)>" and a
    // separate successful/rejected count line. Do not parse the percent.
    const char *mode = strstr(dump, "\nSLEEP");
    const char *sleep = strstr(dump, "light_sleep_counts:");
    if (!mode || !sleep) {
        *reason = !mode ? "sleep_mode_row_missing" : "sleep_counter_row_missing";
        return false;
    }
    const char *p = mode + strlen("\nSLEEP");
    while (*p == ' ' || *p == '\t') ++p;
    char *end;
    (void)strtoul(p, &end, 10); // CPU frequency, e.g. "80M" or "80 M"
    if (end == p) {
        *reason = "sleep_mode_parse_failed";
        return false;
    }
    p = end;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == 'M') ++p;
    else {
        *reason = "sleep_mode_parse_failed";
        return false;
    }
    unsigned long long duration = strtoull(p, &end, 10);
    if (end == p) {
        *reason = "sleep_mode_parse_failed";
        return false;
    }
    p = sleep + strlen("light_sleep_counts:");
    unsigned long successful = strtoul(p, &end, 10);
    if (end == p || successful > UINT32_MAX) {
        *reason = "sleep_count_parse_failed";
        return false;
    }
    p = strstr(end, "light_sleep_reject_counts:");
    if (!p) {
        *reason = "sleep_reject_row_missing";
        return false;
    }
    p += strlen("light_sleep_reject_counts:");
    unsigned long failed = strtoul(p, &end, 10);
    if (end == p || failed > UINT32_MAX) {
        *reason = "sleep_reject_parse_failed";
        return false;
    }
    *count = (uint32_t)successful;
    *total_us = (uint64_t)duration;
    *rejected = (uint32_t)failed;
    *reason = "ok";
    return true;
#else
    (void)count;
    (void)total_us;
    (void)rejected;
    if (reason) *reason = "pm_profiling_not_compiled";
    return false;
#endif
}

void link_power_init(void) {
    portENTER_CRITICAL(&s_lock);
    memset(s_counts, 0, sizeof(s_counts));
    s_total = 0;
    portEXIT_CRITICAL(&s_lock);
}

bool link_power_acquire(link_power_owner_t owner) {
    if (owner >= LINK_POWER_OWNER_COUNT) return false;
    portENTER_CRITICAL(&s_lock);
    bool ok = s_total != UINT32_MAX && s_counts[owner] != UINT32_MAX;
    if (ok) {
        s_counts[owner]++;
        s_total++;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

void link_power_release(link_power_owner_t owner) {
    if (owner >= LINK_POWER_OWNER_COUNT) return;
    portENTER_CRITICAL(&s_lock);
    if (s_counts[owner]) {
        s_counts[owner]--;
        --s_total;
    }
    portEXIT_CRITICAL(&s_lock);
}

link_power_snapshot_t link_power_snapshot(void) {
    portENTER_CRITICAL(&s_lock);
    link_power_snapshot_t result = {
        .active_owners = s_total,
    };
    portEXIT_CRITICAL(&s_lock);
    return result;
}
