/*
 * smartring_bsp.c — 显示 / 触摸 / 共享 I2C / LVGL 适配（ST77916 QSPI + CST816）
 *
 * 对外 API 见 bsp/smartring_plus.h：bsp_display_*、bsp_touch_get_handle、bsp_i2c_get_bus。
 */
#include "bsp/smartring_plus.h"
#include "bsp/board_config.h"
#include "lcd_init_seq.h"

#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lv_adapter.h"

static const char *TAG = "bsp";

/* SPI 总线单次传输上限按整屏 RGB565 预留（适配器默认条带高度可为整屏） */
#define LCD_SPI_MAX_TRANSFER_LINES  BOARD_LCD_V_RES

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_touch_handle_t s_tp;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static bool s_swap_bytes = true;   /* 本屏需字节交换；适配器在 RGB565+OTHER 路径会自动 swap */

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_IO,
        .scl_io_num = BOARD_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static esp_err_t touch_init(void)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io),
                        TAG, "create touch io failed");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = BOARD_TP_RST_IO,
        .int_gpio_num = BOARD_TP_INT_IO,
        .levels = {
            .reset = 0,      /* CST816 复位低有效 */
            .interrupt = 0,  /* 中断低有效 */
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_tp),
                        TAG, "create cst816s failed");
    return ESP_OK;
}

static esp_err_t lcd_init(esp_lcd_panel_io_handle_t *out_io, esp_lcd_panel_handle_t *out_panel)
{
    const spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        BOARD_LCD_QSPI_SCL_IO, BOARD_LCD_QSPI_D0_IO, BOARD_LCD_QSPI_D1_IO,
        BOARD_LCD_QSPI_D2_IO, BOARD_LCD_QSPI_D3_IO,
        BOARD_LCD_H_RES * LCD_SPI_MAX_TRANSFER_LINES * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg =
        ST77916_PANEL_IO_QSPI_CONFIG(BOARD_LCD_QSPI_CS_IO, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                                                 &io_cfg, &io),
                        TAG, "create panel io failed");

    /* 官方 A/B 初始化表（lcd_init_seq.h，BOARD_LCD_VERSION_A 选版，本板已确认 B 版）。
     * 组件自带默认序列与本屏不匹配（实测竖纹花屏），必须用厂商表。 */
    st77916_vendor_config_t vendor_cfg = {
        .init_cmds = lcd_init_seq,
        .init_cmds_size = LCD_INIT_SEQ_SIZE,
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST_IO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(io, &panel_cfg, &panel),
                        TAG, "create st77916 panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel disp on failed");

    *out_io = io;
    *out_panel = panel;
    return ESP_OK;
}

static void backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = BOARD_LCD_BK_LEDC_TIMER,
        .freq_hz = BOARD_LCD_BK_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    const ledc_channel_config_t chan_cfg = {
        .gpio_num = BOARD_LCD_BK_IO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BOARD_LCD_BK_LEDC_CHANNEL,
        .timer_sel = BOARD_LCD_BK_LEDC_TIMER,
        .duty = 255,     /* 默认 100% 点亮 */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chan_cfg));
}

void bsp_display_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BOARD_LCD_BK_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, BOARD_LCD_BK_LEDC_CHANNEL));
}

esp_err_t bsp_display_init(void)
{
    return bsp_display_init_with_task(ESP_LV_ADAPTER_DEFAULT_TASK_CORE_ID,
                                      ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY);
}

/* muse-gadget-sdk: same as bsp_display_init(), with the LVGL task's core and priority. */
esp_err_t bsp_display_init_with_task(int task_core_id, uint32_t task_priority)
{
    if (s_disp != NULL) {
        ESP_LOGW(TAG, "bsp_display 已初始化，跳过");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c bus init failed");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch init failed");
    ESP_LOGI(TAG, "touch CST816 ready (addr 0x%02X)", BOARD_ADDR_TOUCH_CST816S);

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(lcd_init(&io, &panel), TAG, "lcd init failed");
    ESP_LOGI(TAG, "ST77916 panel ready (QSPI 40MHz, %s 版初始化表)",
             BOARD_LCD_VERSION_A ? "A 老屏" : "B 新屏");

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = task_core_id;
    adapter_cfg.task_priority = task_priority;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter_cfg), TAG, "lvgl adapter init failed");

    /* QSPI + PSRAM：适配器 OTHER 接口在 RGB565 flush 时自动字节交换，匹配本屏 */
    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
        panel, io, BOARD_LCD_H_RES, BOARD_LCD_V_RES, ESP_LV_ADAPTER_ROTATE_0);
    /* QSPI + PSRAM：条带高度不宜过大。
     * 整屏(360) 单次 flush≈259KB，SPI DMA 私有缓冲必须占内部 RAM，会 ESP_ERR_NO_MEM 花屏。
     * 40 行双缓冲≈56KB，本板已验证稳定。 */
    disp_cfg.profile.buffer_height = 40;
    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "esp_lv_adapter_register_display failed");

    esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, s_tp);
    s_indev = esp_lv_adapter_register_touch(&touch_cfg);
    ESP_RETURN_ON_FALSE(s_indev != NULL, ESP_FAIL, TAG, "esp_lv_adapter_register_touch failed");

    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "lvgl adapter start failed");

    backlight_init();
    ESP_LOGI(TAG, "bsp display init done (esp_lvgl_adapter), backlight 100%%, swap_bytes=%d",
             s_swap_bytes);
    return ESP_OK;
}

void bsp_display_set_swap_bytes(bool enable)
{
    s_swap_bytes = enable;
    if (!s_disp) {
        return;
    }
    /* 适配器 OTHER+RGB565 路径会在 flush 时 swap；RGB565_SWAPPED 则跳过该步。
     * 本屏默认需要交换：enable=true → RGB565（由适配器 swap）。 */
    if (esp_lv_adapter_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "set swap: lvgl lock timeout");
        return;
    }
    lv_display_set_color_format(s_disp, enable ? LV_COLOR_FORMAT_RGB565
                                               : LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_obj_invalidate(lv_screen_active());
    esp_lv_adapter_unlock();
}

bool bsp_display_get_swap_bytes(void)
{
    return s_swap_bytes;
}

lv_display_t *bsp_display_get_handle(void)
{
    return s_disp;
}

esp_lcd_touch_handle_t bsp_touch_get_handle(void)
{
    return s_tp;
}

i2c_master_bus_handle_t bsp_i2c_get_bus(void)
{
    return s_i2c_bus;
}
