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
 * A BMI270 as the devices accel, gyro and steps (bmi270_device.h).
 *
 * Espressif's bmi270_sensor 0.4.1 owns the low-level Bosch driver and Base
 * configuration image. This adapter owns only registry lifecycle, polling,
 * units, screen axes and software events. The caller keeps the I2C bus.
 * At boot we verify the chip ID without initializing it. First use creates
 * the vendor sensor on the worker task; idle disables all sensors/features
 * and restores advanced power save, retaining the initialized handle.
 * The package uses synchronous 400 kHz transfers with a 200 ms timeout.
 * Its prebuilt archive must match the ESP-IDF major/minor and target.
 *
 * Bosch's base configuration counts steps but has no tap or orientation
 * detector, so the accel device's events come from its readings, while it's
 * open (imu_events.c).
 */

#include "bmi270_device.h"
#include "bmi270_api.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "capabilities_registry.h"
#include "imu_events.h"

static const char *TAG = "bmi270";

/* The pinned package exports Bosch's disable API but omits its prototype
 * from bmi270_api.h. Same declaration as BMI270_SensorAPI v2.113.0 bmi270.h;
 * needed to disable the step feature, not just physical accel/gyro sensors. */
int8_t bmi270_sensor_disable(const uint8_t *sens_list, uint8_t n_sens, bmi270_handle_t dev);

#define ACC_G_LSB (8.0f / 32768.0f)
#define GYR_DPS_LSB (2000.0f / 32768.0f)
#define ODR_HZ 100

#define I2C_HZ 400000
#define I2C_MS 50
#define TASK_STACK 4096
#define TASK_PRIORITY 4             /* above the app task (2), below the UI (5) */
#define ACC_POLL_US 9000            /* a bit faster than the chip's 100 Hz... */
#define RETRY_US 2000               /* ...and again this soon when nothing was new */
#define IDLE_POLL_US 100000         /* the gyro open at rate 0: dev_read stays current */
#define STEPS_US 1000000            /* the step count, once a second */
#define FRESH_US 200000             /* older than this, a sample isn't the latest */

enum { DEV_ACCEL, DEV_GYRO, DEV_STEPS, DEV_N };

typedef struct {
    bool on[DEV_N];
    int rate[DEV_N];
    uint8_t epoch[DEV_N];   /* counts starts from stopped: steps count from the latest */
} want_t;

typedef struct {
    bmi270_device_cfg_t cfg;
    bmi270_handle_t sensor;
    TaskHandle_t task;
    /* Under s_lock. */
    want_t want;
    bool failed;                /* didn't start: no more tries until a restart */
    float acc[3], gyr[3];       /* the latest, in the screen's frame */
    int64_t acc_us, gyr_us;
    int32_t steps;              /* since the steps device started; -1 none yet */
    /* The task's own. */
    want_t applied;
    bool loaded, step_feature, have_base;
    uint32_t step_base;
    int64_t next_us, next_steps_us;
    int phase[DEV_N];           /* thinning the 100 Hz samples to each device's rate */
    unsigned io_errors;
    imu_events_t events;
} imu_t;

/* In PSRAM where the build allows it: only the task and the device calls
 * touch it, never with the cache off. */
static EXT_RAM_BSS_ATTR imu_t s;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static int imu_read(const capability_t *dev, float *values);
static int imu_start(const capability_t *dev, int rate_hz);
static void imu_stop(const capability_t *dev);

/* The well-known names, exactly as components/muse_apps/sdk/api/api.json has them. */
static const capability_field_t s_accel_fields[] = {
    {"x", "g", -8, 8},
    {"y", "g", -8, 8},
    {"z", "g", -8, 8},
};
static const char *const s_accel_events[] = {"tap", "double_tap", "shake", "face_up", "face_down"};
static const capability_field_t s_gyro_fields[] = {
    {"x", "deg/s", -2000, 2000},
    {"y", "deg/s", -2000, 2000},
    {"z", "deg/s", -2000, 2000},
};
static const capability_field_t s_steps_fields[] = {
    {"count", "", 0, 0},
};

static const capability_t s_devs[DEV_N] = {
    [DEV_ACCEL] = {
        .name = "accel",
        .desc = "Accelerometer (BMI270), in g: x right, y up, z out of the screen. Taps, shakes and lying flat "
                "are told from its readings while it's open.",
        .fields = s_accel_fields,
        .n_fields = 3,
        .rate = ODR_HZ,
        .events = s_accel_events,
        .n_events = 5,
        .read = imu_read,
        .start = imu_start,
        .stop = imu_stop,
    },
    [DEV_GYRO] = {
        .name = "gyro",
        .desc = "Gyroscope (BMI270), in degrees a second about accel's axes.",
        .fields = s_gyro_fields,
        .n_fields = 3,
        .rate = ODR_HZ,
        .read = imu_read,
        .start = imu_start,
        .stop = imu_stop,
    },
    [DEV_STEPS] = {
        .name = "steps",
        .desc = "Steps, from the BMI270's step counter, since the device was opened.",
        .fields = s_steps_fields,
        .n_fields = 1,
        .rate = 1,
        .read = imu_read,
        .start = imu_start,
        .stop = imu_stop,
    },
};

/* ---- The bus (the task, and registration) ------------------------------------------------- */

static void sleep_ms(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);   /* at least ms, whatever the tick's phase */
}

/* A transfer that failed while running: said once, then every 100th. */
static void note_io(esp_err_t err, const char *what)
{
    if (err == ESP_OK) {
        s.io_errors = 0;
    } else if (s.io_errors++ % 100 == 0) {
        ESP_LOGW(TAG, "%s: %s (%u in a row)", what, esp_err_to_name(err), s.io_errors);
    }
}

/* ---- Starting and stopping the chip (the task) ------------------------------------------------ */

static bool cleanup_sensor(void)
{
    if (s.sensor && bmi270_sensor_del(&s.sensor) != ESP_OK) {
        ESP_LOGW(TAG, "vendor device cleanup failed; retrying later");
        return false;
    }
    return true;
}

static bool load(void)
{
    esp_err_t err = bmi270_sensor_create_from_master_bus_with_address(
        s.cfg.bus, s.cfg.addr, &s.sensor, NULL,
        BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "vendor sensor initialization failed: %s", esp_err_to_name(err));
        /* Creation can leave a cleanup-only handle if device removal failed. */
        cleanup_sensor();
        return false;
    }
    /* Vendor I2C failures otherwise log for every sample; our throttled
     * note_io reports them without flooding the console. One IMU per gadget. */
    esp_log_level_set("BMI2_ESP32", ESP_LOG_NONE);
    return true;
}

static esp_err_t set_steps(bool on)
{
    const uint8_t feature = BMI2_STEP_COUNTER;
    int8_t result = on ? bmi270_sensor_enable(&feature, 1, s.sensor)
                       : bmi270_sensor_disable(&feature, 1, s.sensor);
    if (result == BMI2_OK) s.step_feature = on;
    return result == BMI2_OK ? ESP_OK : ESP_FAIL;
}

/* Sensors/features off and advanced power save on. Never invent the
 * vendor handle layout or modify its private I2C callback context. */
static bool suspend(void)
{
    if (!s.sensor || !s.loaded) return false;
    const uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO, BMI2_AUX, BMI2_TEMP, BMI2_STEP_COUNTER};
    int8_t result = bmi270_sensor_disable(sensors, sizeof(sensors), s.sensor);
    if (result == BMI2_OK) result = bmi2_set_adv_power_save(BMI2_ENABLE, s.sensor);
    if (result != BMI2_OK) {
        ESP_LOGW(TAG, "suspending failed (%d)", result);
        return false;
    }
    s.step_feature = false;
    memset(&s.applied, 0, sizeof(s.applied));
    ESP_LOGI(TAG, "suspended");
    return true;
}

static void give_up(void)
{
    if (s.loaded) {
        if (!suspend()) {
            /* Bosch soft_reset reloads Base firmware with power save off. */
            int8_t reset = bmi2_soft_reset(s.sensor);
            int8_t power_save = bmi2_set_adv_power_save(BMI2_ENABLE, s.sensor);
            if (reset != BMI2_OK || power_save != BMI2_OK) {
                ESP_LOGE(TAG, "could not restore the IMU to rest");
            }
        }
        s.loaded = false;
    }
    cleanup_sensor();
    taskENTER_CRITICAL(&s_lock);
    s.failed = true;
    memset(&s.want, 0, sizeof(s.want));
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGE(TAG, "the IMU failed: accel, gyro and steps stay off until a restart");
    memset(&s.applied, 0, sizeof(s.applied));
}

static bool any(const want_t *w)
{
    return w->on[DEV_ACCEL] || w->on[DEV_GYRO] || w->on[DEV_STEPS];
}

/* Brings the chip to what the started devices need. */
static void apply(const want_t *w)
{
    if (!any(w)) {
        if (any(&s.applied)) {
            if (!suspend()) give_up();
        }
        return;
    }
    if (!s.loaded) {
        if (!load()) {
            give_up();
            return;
        }
        s.loaded = true;
    }
    int8_t result = bmi2_set_adv_power_save(BMI2_DISABLE, s.sensor);
    if (result == BMI2_OK && !any(&s.applied)) {
        struct bmi2_sens_config configs[2] = {{.type = BMI2_ACCEL}, {.type = BMI2_GYRO}};
        result = bmi270_get_sensor_config(configs, 2, s.sensor);
        if (result == BMI2_OK) {
            configs[0].cfg.acc.odr = BMI2_ACC_ODR_100HZ;
            configs[0].cfg.acc.range = BMI2_ACC_RANGE_8G;
            configs[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
            configs[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
            configs[1].cfg.gyr.odr = BMI2_GYR_ODR_100HZ;
            configs[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;
            configs[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
            configs[1].cfg.gyr.noise_perf = BMI2_POWER_OPT_MODE;
            configs[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
            result = bmi270_set_sensor_config(configs, 2, s.sensor);
        }
    }
    if (result == BMI2_OK && w->on[DEV_STEPS] != s.step_feature) {
        if (set_steps(w->on[DEV_STEPS]) != ESP_OK) result = BMI2_E_COM_FAIL;
    }
    const uint8_t accel = BMI2_ACCEL, gyro = BMI2_GYRO;
    bool acc = w->on[DEV_ACCEL] || w->on[DEV_STEPS];
    if (result == BMI2_OK) result = acc ? bmi2_sensor_enable(&accel, 1, s.sensor)
                                     : bmi2_sensor_disable(&accel, 1, s.sensor);
    if (result == BMI2_OK) result = w->on[DEV_GYRO] ? bmi2_sensor_enable(&gyro, 1, s.sensor)
                                                 : bmi2_sensor_disable(&gyro, 1, s.sensor);
    if (result != BMI2_OK) {
        ESP_LOGW(TAG, "configuring the sensor failed (%d)", result);
        give_up();
        return;
    }
    if (w->on[DEV_ACCEL] && (!s.applied.on[DEV_ACCEL] || w->epoch[DEV_ACCEL] != s.applied.epoch[DEV_ACCEL])) {
        imu_events_init(&s.events);
    }
    if (w->on[DEV_STEPS] && (!s.applied.on[DEV_STEPS] || w->epoch[DEV_STEPS] != s.applied.epoch[DEV_STEPS])) {
        s.have_base = false;   /* the next count read is the start */
        s.next_steps_us = 0;
    }
    memset(s.phase, 0, sizeof(s.phase));
    s.next_us = 0;
    s.applied = *w;
    ESP_LOGI(TAG, "running: accel %s, gyro %s, steps %s", w->on[DEV_ACCEL] ? "on" : "off",
             w->on[DEV_GYRO] ? "on" : "off", w->on[DEV_STEPS] ? "on" : "off");
}

/* ---- Reading it (the task) ------------------------------------------------------------------------ */

static void to_screen(const struct bmi2_sens_axes_data *raw, float scale, float out[3])
{
    const int16_t v[3] = {raw->x, raw->y, raw->z};
    for (int i = 0; i < 3; i++) out[i] = s.cfg.sign[i] * v[s.cfg.axis[i]] * scale;
}

/* Each sample of the 100 Hz stream pushed at about the device's rate. */
static bool due(int dev, bool every)
{
    int rate = s.applied.rate[dev];
    if (rate <= 0) {
        return false;
    }
    if (every || rate >= ODR_HZ) {
        return true;
    }
    s.phase[dev] += rate;
    if (s.phase[dev] < ODR_HZ) {
        return false;
    }
    s.phase[dev] -= ODR_HZ;
    return true;
}

static void sample(int64_t now)
{
    bool acc = s.applied.on[DEV_ACCEL], gyr = s.applied.on[DEV_GYRO];
    struct bmi2_sens_data data = {0};
    esp_err_t err = bmi2_get_sensor_data(&data, s.sensor) == BMI2_OK ? ESP_OK : ESP_FAIL;
    note_io(err, "reading the IMU");
    int64_t period = acc ? ACC_POLL_US : s.applied.rate[DEV_GYRO] > 0 ? 1000000 / s.applied.rate[DEV_GYRO] : IDLE_POLL_US;
    if (period < ACC_POLL_US) {
        period = ACC_POLL_US;
    }
    if (err != ESP_OK) {
        s.next_us = now + period;
        return;
    }
    bool new_acc = acc && (data.status & BMI2_DRDY_ACC), new_gyr = gyr && (data.status & BMI2_DRDY_GYR);
    float a[3], g[3];
    to_screen(&data.acc, ACC_G_LSB, a);
    to_screen(&data.gyr, GYR_DPS_LSB, g);
    taskENTER_CRITICAL(&s_lock);
    if (new_acc) {
        memcpy(s.acc, a, sizeof(a));
        s.acc_us = now;
    }
    if (new_gyr) {
        memcpy(s.gyr, g, sizeof(g));
        s.gyr_us = now;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (new_acc) {
        unsigned ev = imu_events_feed(&s.events, a);
        for (int i = 0; ev && i < s_devs[DEV_ACCEL].n_events; i++) {
            if (ev & (1u << i)) {
                capabilities_registry_event(&s_devs[DEV_ACCEL], i);
            }
        }
        if (due(DEV_ACCEL, false)) {
            capabilities_registry_push(&s_devs[DEV_ACCEL], a, 3);
        }
    }
    if (new_gyr && due(DEV_GYRO, !acc)) {   /* polled at its own rate without accel */
        capabilities_registry_push(&s_devs[DEV_GYRO], g, 3);
    }
    /* At the chip's rate, look again soon when the sample wasn't new yet. */
    s.next_us = now + ((acc && !new_acc) ? RETRY_US : period);
}

static void read_steps(int64_t now)
{
    struct bmi2_feat_sensor_data data = {.type = BMI2_STEP_COUNTER};
    esp_err_t err = bmi270_get_feature_data(&data, 1, s.sensor) == BMI2_OK ? ESP_OK : ESP_FAIL;
    note_io(err, "reading the step count");
    s.next_steps_us = now + STEPS_US;
    if (err != ESP_OK) return;
    uint32_t count = data.sens_data.step_counter_output;
    if (!s.have_base || count < s.step_base) {   /* started, or the chip's count began again */
        s.step_base = count;
        s.have_base = true;
    }
    int32_t n = (int32_t)(count - s.step_base);
    taskENTER_CRITICAL(&s_lock);
    int32_t was = s.steps;
    s.steps = n;
    taskEXIT_CRITICAL(&s_lock);
    if (n != was && s.applied.rate[DEV_STEPS] > 0) {
        float v = (float)n;
        capabilities_registry_push(&s_devs[DEV_STEPS], &v, 1);
    }
}

static void imu_task(void *arg)
{
    (void)arg;
    for (;;) {
        want_t w;
        taskENTER_CRITICAL(&s_lock);
        w = s.want;
        taskEXIT_CRITICAL(&s_lock);
        if (memcmp(&w, &s.applied, sizeof(w))) {
            apply(&w);
        }
        if (!any(&s.applied)) {
            /* A failed create/delete can retain a cleanup-only handle. Do
             * not operate on it; retry deletion without losing ownership. */
            if (s.sensor && !s.loaded && !cleanup_sensor()) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            } else {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            continue;
        }
        bool motion = s.applied.on[DEV_ACCEL] || s.applied.on[DEV_GYRO];
        int64_t now = esp_timer_get_time();
        if (motion && now >= s.next_us) {
            sample(now);
        }
        if (s.applied.on[DEV_STEPS] && now >= s.next_steps_us) {
            read_steps(now);
        }
        int64_t next = s.applied.on[DEV_STEPS] ? s.next_steps_us : INT64_MAX;
        if (motion && s.next_us < next) {
            next = s.next_us;
        }
        int64_t wait_us = next - esp_timer_get_time();
        if (wait_us > 0) {
            TickType_t ticks = pdMS_TO_TICKS((wait_us + 999) / 1000);
            ulTaskNotifyTake(pdTRUE, ticks ? ticks : 1);   /* a start or stop wakes it sooner */
        }
    }
}

/* ---- The devices (the caller's task: an app's) ------------------------------------------------------ */

static int imu_read(const capability_t *dev, float *values)
{
    int i = (int)(dev - s_devs), n = -1;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    if (i == DEV_ACCEL && s.want.on[i] && s.acc_us && now - s.acc_us < FRESH_US) {
        memcpy(values, s.acc, sizeof(s.acc));
        n = 3;
    } else if (i == DEV_GYRO && s.want.on[i] && s.gyr_us && now - s.gyr_us < FRESH_US) {
        memcpy(values, s.gyr, sizeof(s.gyr));
        n = 3;
    } else if (i == DEV_STEPS && s.want.on[i] && s.steps >= 0) {
        values[0] = (float)s.steps;
        n = 1;
    }
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

static int imu_start(const capability_t *dev, int rate_hz)
{
    int i = (int)(dev - s_devs);
    if (i < 0 || i >= DEV_N) {
        return -1;
    }
    /* Starts and stops come from one task at a time (the app task), so the
     * task is made once. */
    if (!s.task && xTaskCreatePinnedToCoreWithCaps(imu_task, "bmi270", TASK_STACK, NULL, TASK_PRIORITY, &s.task,
                                                   tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s.task = NULL;
        ESP_LOGE(TAG, "no memory for the IMU's task");
        return -1;
    }
    taskENTER_CRITICAL(&s_lock);
    bool failed = s.failed;
    if (!failed) {
        if (!s.want.on[i]) {
            s.want.epoch[i]++;
            if (i == DEV_STEPS) {
                s.steps = -1;
            }
        }
        s.want.on[i] = true;
        s.want.rate[i] = rate_hz;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (failed) {
        return -1;
    }
    xTaskNotifyGive(s.task);
    return 0;
}

static void imu_stop(const capability_t *dev)
{
    int i = (int)(dev - s_devs);
    if (i < 0 || i >= DEV_N) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    s.want.on[i] = false;
    s.want.rate[i] = 0;
    taskEXIT_CRITICAL(&s_lock);
    if (s.task) {
        xTaskNotifyGive(s.task);
    }
}

bool bmi270_device_register(const bmi270_device_cfg_t *cfg)
{
    if (!cfg || !cfg->bus) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (cfg->axis[i] > 2 || (cfg->sign[i] != 1 && cfg->sign[i] != -1)) {
            ESP_LOGE(TAG, "bad axis map");
            return false;
        }
    }
    s.cfg = *cfg;
    s.steps = -1;
    sleep_ms(2);   /* a soft reset just before: the chip is back 2 ms later */
    if (i2c_master_probe(cfg->bus, cfg->addr, I2C_MS) != ESP_OK) {
        ESP_LOGW(TAG, "nothing at 0x%02x: no IMU devices", cfg->addr);
        return false;
    }
    /* A short-lived ID-only probe leaves power/configuration untouched.
     * The library's create API performs initialization, so don't call it yet. */
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->addr,
        .scl_speed_hz = I2C_HZ,
    };
    i2c_master_dev_handle_t probe = NULL;
    if (i2c_master_bus_add_device(cfg->bus, &dc, &probe) != ESP_OK) return false;
    uint8_t reg = BMI2_CHIP_ID_ADDR, id = 0;
    esp_err_t err = i2c_master_transmit_receive(probe, &reg, 1, &id, 1, I2C_MS);
    esp_err_t removed = i2c_master_bus_rm_device(probe);
    if (removed != ESP_OK || err != ESP_OK || id != BMI270_CHIP_ID) {
        ESP_LOGW(TAG, "0x%02x is unavailable or not a BMI270: no IMU devices", cfg->addr);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < DEV_N; i++) {
        ok = capabilities_registry_register(&s_devs[i]) && ok;
    }
    return ok;
}
