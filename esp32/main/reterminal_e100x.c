// SPDX-License-Identifier: Apache-2.0
#include "reterminal_e100x.h"
#include "battery_adc.h"
#include "reterminal_e100x_board.h"
#include "link_power.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "reterminal_e100x_events.h"
#include "reterminal_e100x_sd.h"
#include "reterminal_e100x_microphone.h"
#include "stack_monitor.h"
#if CONFIG_HOMEHUB_RETERMINAL_E1003_INPUT
#include "e1003_touch.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// SPDX-License-Identifier: MIT
/*
 * Voltage table adapted from Seeed-reTerminal-E10xx-Firmware,
 * src/boards/reterminal_e1001/config.h (E1001/E1002 BATTERY_CURVE).
 *
 * MIT License
 *
 * Copyright (c) 2025 Seeed-reTerminal-E10xx-Firmware
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


// Both tables retain their original Seeed MIT attribution above.
// E1003 source: src/boards/reterminal_e1003/config.h (BATTERY_CURVE).
// ---- Board data (host-tested) ----
#if RETERMINAL_E100X_BOARD_ID != 3
static const uint16_t E1002_BATTERY_CURVE_MV[] = {
    2795, 2795, 2990, 3107, 3189, 3252, 3300, 3334, 3354, 3367,
    3378, 3389, 3399, 3409, 3418, 3426, 3434, 3442, 3449, 3457,
    3464, 3471, 3478, 3485, 3491, 3498, 3504, 3510, 3516, 3522,
    3528, 3534, 3540, 3546, 3552, 3558, 3564, 3569, 3575, 3581,
    3587, 3592, 3598, 3603, 3609, 3614, 3620, 3625, 3631, 3636,
    3642, 3647, 3653, 3658, 3664, 3669, 3675, 3680, 3686, 3691,
    3697, 3703, 3709, 3715, 3721, 3727, 3733, 3739, 3746, 3752,
    3759, 3765, 3772, 3779, 3786, 3793, 3801, 3808, 3816, 3824,
    3832, 3840, 3849, 3858, 3867, 3876, 3886, 3896, 3906, 3916,
    3927, 3984, 3995, 4007, 4019, 4032, 4045, 4059, 4074, 4090,
    4111,
};


#else
static const uint16_t E1003_BATTERY_CURVE_MV[101] = {
    3204, 3263, 3308, 3345, 3376, 3402, 3424, 3444, 3462, 3477,
    3492, 3505, 3517, 3528, 3539, 3550, 3560, 3570, 3579, 3588,
    3597, 3606, 3614, 3623, 3631, 3640, 3648, 3656, 3665, 3673,
    3682, 3691, 3700, 3709, 3718, 3727, 3736, 3745, 3754, 3764,
    3772, 3781, 3790, 3798, 3806, 3814, 3821, 3829, 3836, 3843,
    3849, 3856, 3862, 3868, 3874, 3880, 3885, 3890, 3895, 3900,
    3905, 3910, 3914, 3919, 3923, 3928, 3932, 3937, 3941, 3945,
    3949, 3954, 3958, 3962, 3966, 3971, 3975, 3980, 3984, 3989,
    3994, 3999, 4004, 4010, 4015, 4021, 4027, 4033, 4039, 4045,
    4051, 4057, 4063, 4069, 4075, 4081, 4087, 4093, 4098, 4104,
    4110,
};


#endif
const reterminal_e100x_board_config_t *reterminal_e100x_board(void) {
    static const reterminal_e100x_board_config_t board = {
        .model = RETERMINAL_E100X_BOARD_ID, .width = E100X_WIDTH, .height = E100X_HEIGHT,
        .keys = {E100X_KEY0_GPIO, E100X_KEY1_GPIO, E100X_KEY2_GPIO},
        .sd_enable_gpio = E100X_SD_ENABLE_GPIO,
        .battery_enable_gpio = E100X_BATTERY_ENABLE_GPIO,
        .battery_keep_enabled = E100X_BATTERY_KEEP_ENABLED,
#if RETERMINAL_E100X_BOARD_ID == 3
        .battery_curve_mv = E1003_BATTERY_CURVE_MV,
#else
        .battery_curve_mv = E1002_BATTERY_CURVE_MV,
#endif
    };
    return &board;
}
int reterminal_e100x_battery_percent(int mv) {
    const uint16_t *curve = reterminal_e100x_board()->battery_curve_mv;
    if (mv <= curve[0]) return 0;
    if (mv >= curve[100]) return 100;
    for (int i = 0; i < 100; ++i) {
        int low = curve[i], high = curve[i+1];
        if (mv >= low && mv <= high && high > low)
            return i + ((mv-low) + (high-low)/2) / (high-low);
    }
    return 0;
}
// ---- Board resources ----
#define KEY1_GPIO E100X_KEY1_GPIO
#define KEY2_GPIO E100X_KEY2_GPIO

typedef struct {
    bool stable;
    bool sample;
    uint8_t same;
    uint32_t presses;
} e100x_key_t;

static e100x_key_t s_keys[3];
static bool s_buttons_ready;
static portMUX_TYPE s_buttons_lock = portMUX_INITIALIZER_UNLOCKED;

static bool sample_key(e100x_key_t *key, bool down) {
    if (down != key->sample) {
        key->sample = down;
        key->same = 1;
    } else if (key->same < 3) {
        key->same++;
    }
    if (key->same == 3 && key->stable != down) {
        key->stable = down;
        if (down) key->presses++;
        return true;
    }
    return false;
}

static void poll_keys(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    for (;;) {
        static const int gpios[] = {E100X_KEY0_GPIO, KEY1_GPIO, KEY2_GPIO};
        for (int i = 0; i < 3; i++) {
            bool down = gpio_get_level(gpios[i]) == 0;
            portENTER_CRITICAL(&s_buttons_lock);
            bool changed = sample_key(&s_keys[i], down);
            uint32_t presses = s_keys[i].presses;
            portEXIT_CRITICAL(&s_buttons_lock);
            if (changed) {
                reterminal_e100x_events_button(i, gpios[i], down, presses);
                // Include the event/activity path in the measured stack margin.
                stack_monitor_record(&stack);
                ESP_LOGI("e100x.buttons", "KEY%d GPIO%d %s count=%lu",
                         i, gpios[i], down ? "pressed" : "released",
                         (unsigned long)presses);
            }
        }
        stack_monitor_poll(&stack);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool reterminal_e100x_buttons_init(void) {
    if (s_buttons_ready) return true;
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << E100X_KEY0_GPIO) | (1ULL << KEY1_GPIO) | (1ULL << KEY2_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) return false;
    // The event/activity callback and logging now share this task's stack.
    // Match the existing GPIO gesture task's bounded 4 KiB allocation.
    s_buttons_ready = xTaskCreate(poll_keys, "e100x_keys", 4096, NULL, 2, NULL) == pdPASS;
    return s_buttons_ready;
}

bool reterminal_e100x_buttons_read(reterminal_e100x_buttons_state_t *state) {
    if (!state || !s_buttons_ready) return false;
    portENTER_CRITICAL(&s_buttons_lock);
    state->key0_down = s_keys[0].stable;
    state->key1_down = s_keys[1].stable;
    state->key2_down = s_keys[2].stable;
    state->key0_presses = s_keys[0].presses;
    state->key1_presses = s_keys[1].presses;
    state->key2_presses = s_keys[2].presses;
    portEXIT_CRITICAL(&s_buttons_lock);
    return true;
}

#define BUZZER_GPIO E100X_BUZZER_GPIO
#define BUZZER_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_TIMER LEDC_TIMER_0
#define BUZZER_CHANNEL LEDC_CHANNEL_0

static SemaphoreHandle_t s_buzzer_lock;
static esp_timer_handle_t s_stop_timer;
static bool s_buzzer_ready;
static bool s_buzzer_pm_held;
static bool s_buzzer_owner_held;
static int64_t s_stop_at;

static void stop_tone(void *arg) {
    (void)arg;
    // A previous timer callback can race with a new beep. The new beep arms
    // its own timer; never let the old callback silence it.
    if (xSemaphoreTake(s_buzzer_lock, 0) == pdTRUE) {
        if (esp_timer_get_time() >= s_stop_at) {
            ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0);
            ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
            if (s_buzzer_pm_held) {
                link_power_pm_leave(LINK_PM_BUZZER);
                s_buzzer_pm_held = false;
            }
            if (s_buzzer_owner_held) {
                link_power_release(LINK_POWER_BUZZER);
                s_buzzer_owner_held = false;
            }
        }
        xSemaphoreGive(s_buzzer_lock);
    } else {
        // A beep is being rearmed. Its new timer will stop the tone; if that
        // rearm fails, retry so the PM lock and LEDC output cannot stick on.
        esp_timer_start_once(s_stop_timer, 10000);
    }
}

bool reterminal_e100x_buzzer_beep(int frequency_hz, int duration_ms) {
    if (frequency_hz < 100 || frequency_hz > 5000 || duration_ms < 20 || duration_ms > 1000) {
        return false;
    }
    if (!s_buzzer_lock) s_buzzer_lock = xSemaphoreCreateMutex();
    if (!s_buzzer_lock || xSemaphoreTake(s_buzzer_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (!s_buzzer_ready) {
        ledc_timer_config_t timer = {
            .speed_mode = BUZZER_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = BUZZER_TIMER,
            .freq_hz = 1000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_channel_config_t channel = {
            .gpio_num = BUZZER_GPIO,
            .speed_mode = BUZZER_MODE,
            .channel = BUZZER_CHANNEL,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = BUZZER_TIMER,
            .duty = 0,
        };
        const esp_timer_create_args_t stop = {
            .callback = stop_tone,
            .name = "e100x_buzzer_stop",
        };
        s_buzzer_ready = ledc_timer_config(&timer) == ESP_OK
               && ledc_channel_config(&channel) == ESP_OK
               && esp_timer_create(&stop, &s_stop_timer) == ESP_OK;
    }
    bool ok = false;
    if (s_buzzer_ready) {
        if (!s_buzzer_owner_held)
            s_buzzer_owner_held = link_power_acquire(LINK_POWER_BUZZER);
        if (!s_buzzer_pm_held) {
            s_buzzer_pm_held = link_power_pm_enter(LINK_PM_BUZZER);
        }
        esp_timer_stop(s_stop_timer);
        s_stop_at = esp_timer_get_time() + (int64_t)duration_ms * 1000;
        ok = s_buzzer_owner_held && s_buzzer_pm_held
          && ledc_set_freq(BUZZER_MODE, BUZZER_TIMER, frequency_hz) == ESP_OK
          && ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 512) == ESP_OK
          && ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL) == ESP_OK
          && esp_timer_start_once(s_stop_timer, (uint64_t)duration_ms * 1000) == ESP_OK;
        if (!ok) {
            ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0);
            ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
            if (s_buzzer_pm_held) {
                link_power_pm_leave(LINK_PM_BUZZER);
                s_buzzer_pm_held = false;
            }
            if (s_buzzer_owner_held) {
                link_power_release(LINK_POWER_BUZZER);
                s_buzzer_owner_held = false;
            }
        }
    }
    xSemaphoreGive(s_buzzer_lock);
    return ok;
}

#define USER_LED E100X_USER_LED_GPIO

static bool s_led_ready;
static bool s_led_on;

bool reterminal_e100x_led_init(void) {
    if (s_led_ready) return true;
    gpio_set_level(USER_LED, 1); // Off before switching the pin to output.
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << USER_LED,
        .mode = GPIO_MODE_OUTPUT,
    };
    s_led_ready = gpio_config(&cfg) == ESP_OK
           && gpio_set_level(USER_LED, 1) == ESP_OK;
    return s_led_ready;
}

bool reterminal_e100x_led_set(bool on) {
    if (!reterminal_e100x_led_init() || gpio_set_level(USER_LED, on ? 0 : 1) != ESP_OK)
        return false;
    s_led_on = on;
    return true;
}

bool reterminal_e100x_led_read(bool *on) {
    if (!on || !reterminal_e100x_led_init()) return false;
    *on = s_led_on;
    return true;
}

static i2c_master_bus_handle_t s_bus;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initializing;

i2c_master_bus_handle_t reterminal_e100x_i2c_bus_get(void) {
    // Initialization happens once; callers may come from health and command tasks.
    for (;;) {
        portENTER_CRITICAL(&s_init_lock);
        if (s_bus) {
            i2c_master_bus_handle_t bus = s_bus;
            portEXIT_CRITICAL(&s_init_lock);
            return bus;
        }
        if (!s_initializing) {
            s_initializing = true;
            portEXIT_CRITICAL(&s_init_lock);
            break;
        }
        portEXIT_CRITICAL(&s_init_lock);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = E100X_I2C_SDA_GPIO,
        .scl_io_num = E100X_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    portENTER_CRITICAL(&s_init_lock);
    if (err == ESP_OK) s_bus = bus;
    s_initializing = false;
    portEXIT_CRITICAL(&s_init_lock);
    if (err != ESP_OK) ESP_LOGW("e100x.i2c", "bus init: %s", esp_err_to_name(err));
    return err == ESP_OK ? bus : NULL;
}

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_battery_valid;
static int s_battery_mv, s_battery_pct;
static int64_t s_battery_at;
#if RETERMINAL_E100X_HARDWARE_ENABLED
static void battery_task(void *arg) {
    (void)arg;
    for (;;) {
        int mv = 0, pct = 0;
        bool valid = battery_adc_read(&mv, &pct);
        int64_t at = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        s_battery_valid = valid;
        s_battery_mv = mv;
        s_battery_pct = pct;
        s_battery_at = at;
        portEXIT_CRITICAL(&s_lock);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

#endif
void reterminal_e100x_battery_metrics(cJSON *payload) {
    portENTER_CRITICAL(&s_lock);
    bool valid = s_battery_valid;
    int mv = s_battery_mv, pct = s_battery_pct;
    int64_t at = s_battery_at;
    portEXIT_CRITICAL(&s_lock);
    valid = valid && esp_timer_get_time() - at < 60000000;
    if (valid) {
        cJSON_AddNumberToObject(payload, "battery_mv", mv);
        cJSON_AddNumberToObject(payload, "battery_pct", pct);
    } else {
        cJSON_AddNullToObject(payload, "battery_mv");
        cJSON_AddNullToObject(payload, "battery_pct");
    }
}


void reterminal_e100x_prepare_display(void) {
#if CONFIG_HOMEHUB_RETERMINAL_E100X_HARDWARE
    if (!reterminal_e100x_sd_prepare_bus()) ESP_LOGW("e100x", "SD bus pins unavailable");
#endif
}
void reterminal_e100x_init(void) {
#if RETERMINAL_E100X_INPUT_ENABLED
    if (!reterminal_e100x_buttons_init()) ESP_LOGW("e100x", "buttons unavailable");
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_INPUT
    if (!e1003_touch_init()) ESP_LOGW("e100x", "touch unavailable");
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE
    if (!reterminal_e100x_led_init()) ESP_LOGW("e100x", "user LED unavailable");
#endif
#if RETERMINAL_E100X_HARDWARE_ENABLED
    if (!reterminal_e100x_sd_monitor_init()) ESP_LOGW("e100x", "SD detection unavailable");
    if (!reterminal_e100x_microphone_init()) ESP_LOGW("e100x", "microphone power unavailable");
    if (xTaskCreate(battery_task, "e100x_battery", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGW("e100x", "battery sampling unavailable");
#endif
}

// E1003: preserve the former panel-owned mutex's creation point and scope.
// The panel initializes SPI2; this lock only serializes complete operations.
static SemaphoreHandle_t s_spi_lock;
bool reterminal_e100x_spi_prepare(void) {
    if (!s_spi_lock) s_spi_lock = xSemaphoreCreateMutex();
    return s_spi_lock != NULL;
}
bool reterminal_e100x_spi_lock(void) {
    return s_spi_lock && xSemaphoreTake(s_spi_lock, portMAX_DELAY) == pdTRUE;
}
void reterminal_e100x_spi_unlock(void) { xSemaphoreGive(s_spi_lock); }
