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

/*
 * WeatherXM WG1200 Boot Guard, NVS Isolation & Dual-Boot Partition Switcher
 */
#include "wxm_boot_mgr.h"

#include <stdatomic.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_flash_partitions.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/gpio_sig_map.h"

#include "boards/board_wxm_wg1200.h"
#include "muse_state.h"

static const char *TAG = "wxm_boot";

#define MUSE_NVS_PART "muse_nvs"

/* ---------- NVS Isolation Wrappers ----------
 * Redirect all default NVS operations (Muse settings, Link, Wi-Fi, NimBLE)
 * to the dedicated "muse_nvs" partition at 0xF21000 so WeatherXM's "nvs"
 * partition at 0xF000 is never modified or erased.
 */

esp_err_t __real_nvs_flash_init(void);
esp_err_t __real_nvs_flash_init_partition(const char *partition_label);
esp_err_t __real_nvs_flash_erase(void);
esp_err_t __real_nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
esp_err_t __real_nvs_open_from_partition(const char *part_name, const char *name,
                                          nvs_open_mode_t open_mode, nvs_handle_t *out_handle);

esp_err_t __wrap_nvs_flash_init_partition(const char *partition_label)
{
    if (!partition_label || strcmp(partition_label, NVS_DEFAULT_PART_NAME) == 0) {
        partition_label = MUSE_NVS_PART;
    }
    esp_err_t err = __real_nvs_flash_init_partition(partition_label);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing isolated %s partition (WeatherXM nvs untouched)", partition_label);
        ESP_ERROR_CHECK(nvs_flash_erase_partition(partition_label));
        err = __real_nvs_flash_init_partition(partition_label);
    }
    return err;
}

esp_err_t __wrap_nvs_flash_init(void)
{
    return __wrap_nvs_flash_init_partition(MUSE_NVS_PART);
}

esp_err_t __wrap_nvs_flash_erase(void)
{
    ESP_LOGW(TAG, "erasing isolated %s partition (WeatherXM nvs untouched)", MUSE_NVS_PART);
    return nvs_flash_erase_partition(MUSE_NVS_PART);
}

esp_err_t __wrap_nvs_open_from_partition(const char *part_name, const char *name,
                                          nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    if (!part_name || strcmp(part_name, NVS_DEFAULT_PART_NAME) == 0) {
        part_name = MUSE_NVS_PART;
    }
    return __real_nvs_open_from_partition(part_name, name, open_mode, out_handle);
}

esp_err_t __wrap_nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    return __real_nvs_open_from_partition(MUSE_NVS_PART, name, open_mode, out_handle);
}

/* ---------- Bootloader GPIO10 + GPIO38 Factory-Reset Protection ---------- */

static atomic_bool s_restart_gpios_prepared = false;

void wxm_prepare_safe_restart_gpios(void)
{
    if (atomic_exchange(&s_restart_gpios_prepared, true)) {
        return;
    }
    ESP_LOGI(TAG, "Preparing GPIO 10 and 38 for safe restart...");
    /*
     * GPIO10 is LCD RGB data G0 and the factory-reset input on historical
     * WG1200 bootloaders (v0.1.9). Disconnect it from the RGB matrix, force
     * it HIGH with input sampling enabled, and hold the pad across reset.
     */
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_reset_pin(GPIO_NUM_10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction(GPIO_NUM_10, GPIO_MODE_INPUT_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(GPIO_NUM_10, 1));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_hold_en(GPIO_NUM_10));

    /*
     * GPIO38 is the active-low physical user button. Keep it as input with pull-up.
     */
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_reset_pin(GPIO_NUM_38));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction(GPIO_NUM_38, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_pull_mode(GPIO_NUM_38, GPIO_PULLUP_ONLY));

    esp_rom_delay_us(100000);
}

void __real_esp_panic_handler(void *info);
void IRAM_ATTR __wrap_esp_panic_handler(void *info)
{
    esp_rom_gpio_connect_out_signal(GPIO_NUM_10, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_input_enable(&GPIO, GPIO_NUM_10);
    gpio_ll_output_enable(&GPIO, GPIO_NUM_10);
    gpio_ll_set_level(&GPIO, GPIO_NUM_10, 1);
    gpio_ll_hold_en(&GPIO, GPIO_NUM_10);
    __real_esp_panic_handler(info);
}

void wxm_boot_guard_init(void)
{
    static bool s_initialized = false;
    atomic_store(&s_restart_gpios_prepared, false);
    /* Release GPIO10 pad hold if we arrived here from a safe soft-restart. */
    gpio_hold_dis(GPIO_NUM_10);
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    esp_register_shutdown_handler(wxm_prepare_safe_restart_gpios);
    ESP_LOGI(TAG, "WG1200 GPIO10 safe-restart guard + isolated %s active", MUSE_NVS_PART);
}

/* ---------- WeatherXM Partition Discovery & Boot Switcher ---------- */

static bool is_wxm_project(const char *proj)
{
    if (!proj || !proj[0]) {
        return false;
    }
    return strncasecmp(proj, "wg1", 3) == 0 ||
           strncasecmp(proj, "weatherxm", 9) == 0 ||
           strncasecmp(proj, "wxm", 3) == 0;
}

void wxm_boot_get_info(wxm_boot_info_t *out)
{
    memset(out, 0, sizeof(*out));
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    const esp_partition_t *ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);

    esp_app_desc_t desc_factory = {0}, desc_ota0 = {0};
    bool ok_factory = factory && (factory != running) &&
                      (esp_ota_get_partition_description(factory, &desc_factory) == ESP_OK);
    bool ok_ota0 = ota0 && (ota0 != running) &&
                   (esp_ota_get_partition_description(ota0, &desc_ota0) == ESP_OK);

    if (ok_factory) {
        out->factory_part = factory;
        strlcpy(out->factory_project, desc_factory.project_name, sizeof(out->factory_project));
        strlcpy(out->factory_version, desc_factory.version, sizeof(out->factory_version));
    }
    if (ok_ota0) {
        out->ota0_part = ota0;
        strlcpy(out->ota0_project, desc_ota0.project_name, sizeof(out->ota0_project));
        strlcpy(out->ota0_version, desc_ota0.version, sizeof(out->ota0_version));
    }

    /* Prefer ota_0 if it holds a WeatherXM image; otherwise factory if it holds WeatherXM. */
    if (ok_ota0 && is_wxm_project(desc_ota0.project_name)) {
        out->wxm_part = ota0;
        strlcpy(out->wxm_label, ota0->label, sizeof(out->wxm_label));
        strlcpy(out->wxm_project, desc_ota0.project_name, sizeof(out->wxm_project));
        strlcpy(out->wxm_version, desc_ota0.version, sizeof(out->wxm_version));
        if (ok_factory) {
            out->alt_part = factory;
            strlcpy(out->alt_label, factory->label, sizeof(out->alt_label));
            strlcpy(out->alt_project, desc_factory.project_name, sizeof(out->alt_project));
            strlcpy(out->alt_version, desc_factory.version, sizeof(out->alt_version));
        }
    } else if (ok_factory) {
        out->wxm_part = factory;
        strlcpy(out->wxm_label, factory->label, sizeof(out->wxm_label));
        strlcpy(out->wxm_project, desc_factory.project_name, sizeof(out->wxm_project));
        strlcpy(out->wxm_version, desc_factory.version, sizeof(out->wxm_version));
        if (ok_ota0) {
            out->alt_part = ota0;
            strlcpy(out->alt_label, ota0->label, sizeof(out->alt_label));
            strlcpy(out->alt_project, desc_ota0.project_name, sizeof(out->alt_project));
            strlcpy(out->alt_version, desc_ota0.version, sizeof(out->alt_version));
        }
    } else if (ok_ota0) {
        out->wxm_part = ota0;
        strlcpy(out->wxm_label, ota0->label, sizeof(out->wxm_label));
        strlcpy(out->wxm_project, desc_ota0.project_name, sizeof(out->wxm_project));
        strlcpy(out->wxm_version, desc_ota0.version, sizeof(out->wxm_version));
    }
}

static atomic_bool s_boot_in_progress = false;

#define WXM_BOOT_TASK_STACK_BYTES 6144

static esp_err_t wxm_write_otadata_for_partition(const esp_partition_t *part)
{
    if (!part) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!otadata) {
        ESP_LOGE(TAG, "otadata partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Erase both 4 KB sectors of otadata (8 KB total at 0x13000).
     * When both sectors are 0xFF, the ESP-IDF bootloader boots 'factory' (0x20000).
     */
    esp_err_t err = esp_partition_erase_range(otadata, 0, otadata->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase otadata: %s", esp_err_to_name(err));
        return err;
    }

    if (part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        ESP_LOGI(TAG, "otadata cleared to 0xFF -> bootloader will boot factory (0x%lx)",
                 (unsigned long)part->address);
        return ESP_OK;
    }

    if (part->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        part->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        uint32_t slot = (uint32_t)(part->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0);
        esp_ota_select_entry_t entry;
        memset(&entry, 0xFF, sizeof(entry));
        /*
         * In ESP-IDF bootloader: (ota_seq - 1) % num_ota_partitions == slot
         * With 2 OTA partitions (ota_0, ota_1):
         *   ota_seq = 1 -> (1 - 1) % 2 = 0 (ota_0)
         *   ota_seq = 2 -> (2 - 1) % 2 = 1 (ota_1)
         */
        entry.ota_seq = slot + 1;
        entry.ota_state = ESP_OTA_IMG_VALID;
        entry.crc = esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)&entry.ota_seq, sizeof(entry.ota_seq));

        err = esp_partition_write(otadata, 0, &entry, sizeof(entry));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write otadata for slot %lu: %s",
                     (unsigned long)slot, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "otadata set to ota_%lu (seq=%lu, crc=0x%08lx) -> 0x%lx",
                 (unsigned long)slot, (unsigned long)entry.ota_seq,
                 (unsigned long)entry.crc, (unsigned long)part->address);
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static void wxm_boot_worker_task(void *arg)
{
    const esp_partition_t *part = (const esp_partition_t *)arg;
    /* Wait briefly so any HTTP/WS/serial/UI caller can flush its response first. */
    vTaskDelay(pdMS_TO_TICKS(350));
    wg1200_buzzer_beep(80);

    esp_err_t err = wxm_write_otadata_for_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wxm_write_otadata_for_partition(%s) failed: %s",
                 part->label, esp_err_to_name(err));
        muse_state_set_mode(MUSE_MODE_ERROR);
        muse_state_set_caption("BOOT SWITCH FAILED");
        atomic_store(&s_boot_in_progress, false);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "Boot partition set to '%s' (0x%lx); restarting now...",
             part->label, (unsigned long)part->address);
    vTaskDelay(pdMS_TO_TICKS(100));
    wxm_prepare_safe_restart_gpios();
    esp_restart();
}

esp_err_t wxm_boot_to_partition(const esp_partition_t *part)
{
    if (!part) {
        return ESP_ERR_NOT_FOUND;
    }
    if (atomic_exchange(&s_boot_in_progress, true)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Switching boot partition to '%s' (0x%lx) and rebooting...",
             part->label, (unsigned long)part->address);
    muse_state_poke();
    muse_state_set_caption("BOOTING %s...", part->label);

    /* Allocate internal-RAM task stack dynamically on demand: saves 4 KB of
     * permanent .bss, gives esp_restart() shutdown handlers 6 KB of stack,
     * and avoids static TCB reuse hazards on vTaskDelete(NULL). */
    if (xTaskCreate(wxm_boot_worker_task, "wxm_boot", WXM_BOOT_TASK_STACK_BYTES,
                    (void *)part, 10, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn wxm_boot_worker_task");
        atomic_store(&s_boot_in_progress, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wxm_boot_return_to_weatherxm(void)
{
    wxm_boot_info_t info;
    wxm_boot_get_info(&info);
    if (!info.wxm_part) {
        ESP_LOGE(TAG, "No WeatherXM partition found in factory or ota_0!");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "Returning to WeatherXM (%s: %s %s @ 0x%lx)",
             info.wxm_label, info.wxm_project, info.wxm_version,
             (unsigned long)info.wxm_part->address);
    return wxm_boot_to_partition(info.wxm_part);
}
