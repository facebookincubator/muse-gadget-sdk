/*
 * bsp_power — 软关机（GPIO47 PW_OFF）
 *
 * V1/V2 共用：拉高 BOARD_PW_OFF_IO 保持 BOARD_PW_OFF_HOLD_MS（约 3.5s），
 * 经 Q4 拉低电源键模拟长按，整机断电。AXP2101 不参与关机。
 */
#include "bsp/smartring_plus.h"
#include "bsp/board_config.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";
static bool s_ready;

esp_err_t bsp_power_init(void)
{
    if (s_ready) {
        ESP_LOGW(TAG, "bsp_power 已初始化，跳过");
        return ESP_OK;
    }

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PW_OFF_IO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(BOARD_PW_OFF_IO, 0);   /* 默认不触发 */
    s_ready = true;
    ESP_LOGI(TAG, "power mgmt ready (PW_OFF=GPIO%d, V1/V2 same path)", BOARD_PW_OFF_IO);
    return ESP_OK;
}

void bsp_power_shutdown(void)
{
    ESP_LOGW(TAG, "PW_OFF 拉高 %d ms，设备应随即断电...", BOARD_PW_OFF_HOLD_MS);
    gpio_set_level(BOARD_PW_OFF_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOARD_PW_OFF_HOLD_MS));

    /* 能走到这里说明未断电 */
    gpio_set_level(BOARD_PW_OFF_IO, 0);
    ESP_LOGE(TAG, "保持 %d ms 后设备仍在运行，软关机未生效", BOARD_PW_OFF_HOLD_MS);
}
