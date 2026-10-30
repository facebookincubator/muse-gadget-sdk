// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "driver/i2c_master.h"

// Existing profile selection is the board identity; feature toggles never
// select a different wiring table. Sticky does not include this header.
#if CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951
#define RETERMINAL_E100X_BOARD_ID 3
#define E100X_WIDTH 1872
#define E100X_HEIGHT 1404
#define E100X_BATTERY_ENABLE_GPIO 40
#define E100X_SD_ENABLE_GPIO 39
#define E100X_BATTERY_KEEP_ENABLED 1
#else
#if CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179
#define RETERMINAL_E100X_BOARD_ID 1
#elif CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6
#define RETERMINAL_E100X_BOARD_ID 2
#else
#error "E100x board identity requires an E1001, E1002 or E1003 profile"
#endif
#define E100X_WIDTH 800
#define E100X_HEIGHT 480
#define E100X_BATTERY_ENABLE_GPIO 21
#define E100X_SD_ENABLE_GPIO 16
#define E100X_BATTERY_KEEP_ENABLED 0
#endif
#define E100X_KEY0_GPIO 3
#define E100X_KEY1_GPIO 4
#define E100X_KEY2_GPIO 5
#define E100X_I2C_SDA_GPIO 19
#define E100X_I2C_SCL_GPIO 20
#define E100X_SPI_SCLK_GPIO 7
#define E100X_SPI_MISO_GPIO 8
#define E100X_SPI_MOSI_GPIO 9
#define E100X_PANEL_CS_GPIO 10
#define E100X_PANEL_DC_OR_DRIVE_GPIO 11
#define E100X_PANEL_RESET_GPIO 12
#define E100X_PANEL_READY_GPIO 13
#define E100X_SD_CS_GPIO 14
#define E100X_SD_DETECT_GPIO 15
#define E100X_BATTERY_ADC_GPIO 1
#define E100X_BUZZER_GPIO 45
#define E100X_MIC_ENABLE_GPIO 38
#define E100X_MIC_CLOCK_GPIO 42
#define E100X_MIC_DATA_GPIO 41
#define E100X_USER_LED_GPIO 16
#define E100X_TOUCH_INT_GPIO 2
#define E100X_TOUCH_RESET_GPIO 48
#define E100X_TCON_ENABLE_GPIO 21

typedef struct {
    int model, width, height;
    int keys[3], sd_enable_gpio, battery_enable_gpio;
    bool battery_keep_enabled;
    const uint16_t *battery_curve_mv;
} reterminal_e100x_board_config_t;
const reterminal_e100x_board_config_t *reterminal_e100x_board(void);
int reterminal_e100x_battery_percent(int mv);
i2c_master_bus_handle_t reterminal_e100x_i2c_bus_get(void);
// Whole IT8951 sequences and SD operations; the panel still owns SPI2 init.
bool reterminal_e100x_spi_prepare(void);
bool reterminal_e100x_spi_lock(void);
void reterminal_e100x_spi_unlock(void);

typedef struct {
    bool key0_down;
    bool key1_down;
    bool key2_down;
    uint32_t key0_presses;
    uint32_t key1_presses;
    uint32_t key2_presses;
} reterminal_e100x_buttons_state_t;

bool reterminal_e100x_buttons_init(void);
bool reterminal_e100x_buttons_read(reterminal_e100x_buttons_state_t *state);

bool reterminal_e100x_buzzer_beep(int frequency_hz, int duration_ms);

bool reterminal_e100x_led_init(void);
bool reterminal_e100x_led_set(bool on);
bool reterminal_e100x_led_read(bool *on);
