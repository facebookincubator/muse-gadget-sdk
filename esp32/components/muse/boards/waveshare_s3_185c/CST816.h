/**
 * @file
 * @brief ESP LCD touch: CST820
 */

#pragma once
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"


/**
 * @brief Create a new CST816S touch driver
 *
 * @note  The I2C communication should be initialized before use this function.
 *
 * @param io LCD panel IO handle, it should be created by `esp_lcd_new_panel_io_i2c()`
 * @param config Touch panel configuration
 * @param tp Touch panel handle
 * @return
 *      - ESP_OK: on success
 */
esp_err_t esp_lcd_touch_new_i2c_cst816(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *tp);

/**
 * @brief I2C address of the CST816S controller
 *
 */
#define ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS    (0x15)

/**
 * @brief Touch IO configuration structure
 *
 */
#define ESP_LCD_TOUCH_IO_I2C_CST816_CONFIG()             \
    {                                                    \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS, \
        .scl_speed_hz = (400000),                        \
        .control_phase_bytes = 1,                        \
        .dc_bit_offset = 0,                              \
        .lcd_cmd_bits = 8,                              \
        .flags =                                         \
        {                                                \
            .disable_control_phase = 1,                  \
        }                                                \
    }
    

// I2C settings
#define I2C_Touch_SDA_IO            11               /*!< GPIO number used for I2C master data  */
#define I2C_Touch_SCL_IO            10               /*!< GPIO number used for I2C master clock */
#define I2C_Touch_INT_IO            4               /*!< GPIO number used for I2C master data  */
#define I2C_Touch_RST_IO            -1              /*!< GPIO number used for I2C master clock */
#define I2C_Touch_MASTER_NUM        0               /*!< I2C master i2c port number, the number of i2c peripheral interfaces available will depend on the chip */
#define I2C_Touch_MASTER_FREQ_HZ    400000          /*!< I2C master clock frequency */

extern esp_lcd_touch_handle_t tp;

void Touch_Init(void);
