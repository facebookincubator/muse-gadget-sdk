/*
 * bsp_sd — SDMMC 4-bit + VFS FAT 挂载，以及目录列举参考实现
 *
 * 本文件内容：
 *   硬件层
 *     bsp_sd_init / deinit / is_mounted / get_card
 *     引脚见 board_config（CLK/CMD/D0–D3），挂载点 BOARD_SD_MOUNT_POINT
 *   参考实现（纯 POSIX，可弃可换）
 *     bsp_sd_list / bsp_sd_free_list — 文件夹在前、文件在后，strcasecmp 排序
 *
 * 硬件注意：
 *   - 板载 10kΩ 上拉，VDD=3V3 常供；无卡检测脚，挂载失败无法区分“未插卡”
 *   - 使用 esp_vfs_fat_sdmmc_mount；d_type 在 FAT 上不可靠，list 用 stat 判目录
 */
#include "bsp/smartring_plus.h"
#include "bsp/board_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "bsp_sd";

static sdmmc_card_t *s_card;

esp_err_t bsp_sd_init(void)
{
    ESP_RETURN_ON_FALSE(s_card == NULL, ESP_ERR_INVALID_STATE, TAG, "already mounted");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = BOARD_SD_CLK_IO;
    slot.cmd   = BOARD_SD_CMD_IO;
    slot.d0    = BOARD_SD_D0_IO;
    slot.d1    = BOARD_SD_D1_IO;
    slot.d2    = BOARD_SD_D2_IO;
    slot.d3    = BOARD_SD_D3_IO;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   /* 板载 10kΩ 之外的冗余 */

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 16,   /* 录音/音乐/相册/目录遍历可能同时占多个 fd */
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "挂载 SD 卡（SDMMC 4-bit, CLK=%d CMD=%d D0-D3=%d/%d/%d/%d）...",
             BOARD_SD_CLK_IO, BOARD_SD_CMD_IO,
             BOARD_SD_D0_IO, BOARD_SD_D1_IO, BOARD_SD_D2_IO, BOARD_SD_D3_IO);

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BOARD_SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载失败: %s（确认卡已插好；本板无卡检测脚，软件无法区分）",
                 esp_err_to_name(err));
        s_card = NULL;
        return err;
    }

    ESP_LOGI(TAG, "挂载成功: %s", BOARD_SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool bsp_sd_is_mounted(void)
{
    return s_card != NULL;
}

void bsp_sd_deinit(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(BOARD_SD_MOUNT_POINT, s_card);
        s_card = NULL;
        ESP_LOGI(TAG, "已卸载");
    }
}

const sdmmc_card_t *bsp_sd_get_card(void)
{
    return s_card;
}

/* ---------------- 列目录（参考实现，可弃可换） ---------------- */

static int entry_cmp(const void *a, const void *b)
{
    const bsp_sd_entry_t *ea = a, *eb = b;
    if (ea->type != eb->type) {
        return (ea->type == BSP_SD_ENTRY_DIR) ? -1 : 1;   /* 文件夹在前 */
    }
    return strcasecmp(ea->name, eb->name);
}

esp_err_t bsp_sd_list(const char *path, bsp_sd_entry_t **entries, size_t *count)
{
    ESP_RETURN_ON_FALSE(s_card != NULL, ESP_ERR_INVALID_STATE, TAG, "not mounted");
    ESP_RETURN_ON_FALSE(path && entries && count, ESP_ERR_INVALID_ARG, TAG, "bad args");

    *entries = NULL;
    *count = 0;

    DIR *dir = opendir(path);
    ESP_RETURN_ON_FALSE(dir != NULL, ESP_ERR_NOT_FOUND, TAG, "opendir failed: %s", path);

    /* 第一遍数条目，第二遍填数组 */
    size_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
            n++;
        }
    }
    rewinddir(dir);

    if (n == 0) {
        closedir(dir);
        return ESP_OK;
    }

    bsp_sd_entry_t *list = calloc(n, sizeof(bsp_sd_entry_t));
    if (!list) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    size_t i = 0;
    while ((ent = readdir(dir)) != NULL && i < n) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }
        list[i].name = strdup(ent->d_name);
        if (!list[i].name) {
            bsp_sd_free_list(list, i);
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        /* d_type 在 FAT VFS 不可靠，用 stat 判定目录 */
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        list[i].type = (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                       ? BSP_SD_ENTRY_DIR : BSP_SD_ENTRY_FILE;
        i++;
    }
    closedir(dir);

    qsort(list, i, sizeof(bsp_sd_entry_t), entry_cmp);

    *entries = list;
    *count = i;
    return ESP_OK;
}

void bsp_sd_free_list(bsp_sd_entry_t *entries, size_t count)
{
    if (!entries) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
    }
    free(entries);
}
