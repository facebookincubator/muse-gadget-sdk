/*
 * bsp_imu — QMI8658A 加速度采样（BSP）+ 水平校准
 *
 * 本文件内容：
 *   硬件层
 *     bsp_imu_init     — 驱动初始化、±2G / 250Hz、采样任务、EMA 低通
 *     bsp_imu_get_accel — 滤波后加速度（板载轴，m/s²）
 *     bsp_imu_get_tilt  — 屏幕平面倾斜（减校准零点后映射到屏坐标）
 *   水平校准（可选，不调 start 则 tilt 不减零点）
 *     bsp_imu_calib_start / poll / get_info
 *     2s 窗口内：三轴 std ≤ 0.05g 且相对水平 ≤ 8° → PASSED；30s 超时 FAILED
 *
 * 硬件注意（勿轻易改配置顺序）：
 *   - 地址 0x6B（SA0 高），WHO_AM_I=0x05，复用 bsp_i2c_get_bus()
 *   - 改量程/ODR 前须 DISABLE_ALL，否则引擎停转、读数恒 0
 *   - accel 与 gyro 须同时使能；使能后丢弃首帧假数据，再等 20ms
 *   - 轴→屏映射见 IMU_MAP_SCREEN_X/Y
 */
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/smartring_plus.h"

#undef M_PI   /* waveshare/qmi8658 头文件会重复定义 M_PI，先取消避免告警 */
#include "qmi8658.h"

static const char *TAG = "bsp_imu";

/* ---------------------------------------------------------------- 硬件层参数 */

#define IMU_SAMPLE_PERIOD_MS    4       /* 250Hz 采样任务（与 ODR 匹配） */
#define IMU_EMA_ALPHA           0.3f    /* EMA 低通系数；250Hz 下滞后 ~9ms */

/* ---------------------------------------------------------------- 校准参数（参考值，应用可按手感改） */

#define IMU_CALIB_WINDOW_MS     2000    /* 判定窗口时长 */
#define IMU_CALIB_WINDOW        (IMU_CALIB_WINDOW_MS / IMU_SAMPLE_PERIOD_MS)
#define IMU_CALIB_TIMEOUT_MS    30000   /* 总时限，超时 FAILED */
#define IMU_CALIB_STD_MAX_G     0.05f   /* 稳定性：三轴标准差上限（g） */
#define IMU_CALIB_TILT_MAX_DEG  8.0f    /* 水平度：允许的最大桌面倾斜角 */

#define GRAVITY_MPS2            9.807f

/* ---------------------------------------------------------------- 轴映射（实机对轴定案）
 * 板载 IMU 轴 → 屏幕坐标（+x 右，+y 下，与 LVGL 一致）。本板直通。
 */
#define IMU_MAP_SCREEN_X(ax, ay)  (ax)
#define IMU_MAP_SCREEN_Y(ax, ay)  (ay)

/* ---------------------------------------------------------------- 内部状态 */

static qmi8658_dev_t  s_dev;
static SemaphoreHandle_t s_lock;
static bool s_ready;

/* 滤波后读数（板载轴，m/s²） */
static float s_ax, s_ay, s_az;

/* 校准零点（板载轴，m/s²）与状态 */
static float s_zero_x, s_zero_y, s_zero_z;
static bool  s_has_zero;
static volatile bsp_imu_calib_status_t s_calib_status = BSP_IMU_CALIB_IDLE;

/* 校准窗口累计 */
static double  s_sum_x, s_sum_y, s_sum_z;
static double  s_sumsq_x, s_sumsq_y, s_sumsq_z;
static int     s_win_n;
static int64_t s_calib_start_us;

/* 最近一次窗口判定结果（观测用） */
static float s_last_std_g;
static float s_last_tilt_deg;

/* ---------------------------------------------------------------- 采样任务 */

static void imu_sample_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    bool first_frame = true;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(IMU_SAMPLE_PERIOD_MS));

        float x, y, z;
        if (qmi8658_read_accel(&s_dev, &x, &y, &z) != ESP_OK) {
            continue;   /* 偶发 I2C 失败丢帧 */
        }
        if (first_frame) {
            /* 使能后第一帧常为满量程假数据，丢弃 */
            first_frame = false;
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);

        /* EMA 低通 */
        s_ax += IMU_EMA_ALPHA * (x - s_ax);
        s_ay += IMU_EMA_ALPHA * (y - s_ay);
        s_az += IMU_EMA_ALPHA * (z - s_az);

        /* 校准窗口累计（用滤波后数据，更抗噪声） */
        if (s_calib_status == BSP_IMU_CALIB_RUNNING) {
            s_sum_x += s_ax;  s_sumsq_x += (double)s_ax * s_ax;
            s_sum_y += s_ay;  s_sumsq_y += (double)s_ay * s_ay;
            s_sum_z += s_az;  s_sumsq_z += (double)s_az * s_az;
            s_win_n++;

            int64_t elapsed_ms = (esp_timer_get_time() - s_calib_start_us) / 1000;
            if (elapsed_ms >= IMU_CALIB_TIMEOUT_MS) {
                s_calib_status = BSP_IMU_CALIB_FAILED;
                ESP_LOGW(TAG, "calib FAILED (30s timeout, last std=%.3fg tilt=%.1f deg)",
                         (double)s_last_std_g, (double)s_last_tilt_deg);
            } else if (s_win_n >= IMU_CALIB_WINDOW) {
                double n = (double)s_win_n;
                double mx = s_sum_x / n, my = s_sum_y / n, mz = s_sum_z / n;
                double vx = s_sumsq_x / n - mx * mx;
                double vy = s_sumsq_y / n - my * my;
                double vz = s_sumsq_z / n - mz * mz;
                if (vx < 0) vx = 0;
                if (vy < 0) vy = 0;
                if (vz < 0) vz = 0;
                double std_max = sqrt(vx);
                if (sqrt(vy) > std_max) std_max = sqrt(vy);
                if (sqrt(vz) > std_max) std_max = sqrt(vz);

                double horiz = sqrt(mx * mx + my * my);
                double tilt_deg = atan2(horiz, fabs(mz)) * 180.0 / M_PI;

                s_last_std_g = (float)(std_max / GRAVITY_MPS2);
                s_last_tilt_deg = (float)tilt_deg;

                if (std_max / GRAVITY_MPS2 <= IMU_CALIB_STD_MAX_G &&
                    tilt_deg <= IMU_CALIB_TILT_MAX_DEG) {
                    s_zero_x = (float)mx;
                    s_zero_y = (float)my;
                    s_zero_z = (float)mz;
                    s_has_zero = true;
                    s_calib_status = BSP_IMU_CALIB_PASSED;
                    ESP_LOGI(TAG, "calib PASSED: zero=(%.2f %.2f %.2f) m/s2, std=%.3fg, tilt=%.1f deg",
                             (double)s_zero_x, (double)s_zero_y, (double)s_zero_z,
                             (double)s_last_std_g, (double)s_last_tilt_deg);
                } else {
                    ESP_LOGD(TAG, "calib window rejected: std=%.3fg tilt=%.1f deg",
                             (double)s_last_std_g, (double)s_last_tilt_deg);
                }

                /* 无论过不过，窗口重置继续下一窗口（超时由上面兜底） */
                s_sum_x = s_sum_y = s_sum_z = 0;
                s_sumsq_x = s_sumsq_y = s_sumsq_z = 0;
                s_win_n = 0;
            }
        }

        xSemaphoreGive(s_lock);
    }
}

/* ---------------------------------------------------------------- 硬件层 API */

esp_err_t bsp_imu_init(void)
{
    if (s_ready) {
        ESP_LOGW(TAG, "bsp_imu 已初始化，跳过");
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C bus not ready, call bsp_display_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = qmi8658_init(&s_dev, bus, QMI8658_ADDRESS_HIGH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "qmi8658_init failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t id = 0;
    if (qmi8658_get_who_am_i(&s_dev, &id) != ESP_OK || id != 0x05) {
        ESP_LOGE(TAG, "WHO_AM_I = 0x%02X, expect 0x05", id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "QMI8658A found (WHO_AM_I=0x%02X, addr=0x6B)", id);

    /* 配置顺序：先全关 → 改量程/ODR → 使能 accel+gyro → 等 20ms */
    qmi8658_enable_sensors(&s_dev, QMI8658_DISABLE_ALL);
    qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_2G);
    qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_250HZ);
    qmi8658_enable_sensors(&s_dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
    qmi8658_set_accel_unit_mps2(&s_dev, true);
    vTaskDelay(pdMS_TO_TICKS(20));

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex create failed");

    BaseType_t ok = xTaskCreate(imu_sample_task, "bsp_imu", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        ESP_LOGE(TAG, "sample task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "bsp_imu init done (accel 2G/250Hz + gyro on, 250Hz task)");
    return ESP_OK;
}

void bsp_imu_get_accel(float *ax, float *ay, float *az)
{
    if (s_lock == NULL) {
        if (ax) *ax = 0;
        if (ay) *ay = 0;
        if (az) *az = 0;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ax) *ax = s_ax;
    if (ay) *ay = s_ay;
    if (az) *az = s_az;
    xSemaphoreGive(s_lock);
}

void bsp_imu_get_tilt(float *gx, float *gy)
{
    float ax = 0, ay = 0;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ax = s_ax;
        ay = s_ay;
        if (s_has_zero) {
            ax -= s_zero_x;
            ay -= s_zero_y;
        }
        xSemaphoreGive(s_lock);
    }

    if (gx) *gx = IMU_MAP_SCREEN_X(ax, ay);
    if (gy) *gy = IMU_MAP_SCREEN_Y(ax, ay);
}

/* ---------------------------------------------------------------- 校准参考实现 API */

void bsp_imu_calib_start(void)
{
    if (s_lock == NULL) {
        ESP_LOGW(TAG, "calib_start ignored (imu not init)");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sum_x = s_sum_y = s_sum_z = 0;
    s_sumsq_x = s_sumsq_y = s_sumsq_z = 0;
    s_win_n = 0;
    s_has_zero = false;
    s_calib_start_us = esp_timer_get_time();
    s_calib_status = BSP_IMU_CALIB_RUNNING;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "calib start (window=%d samples, timeout=%d ms)",
             IMU_CALIB_WINDOW, IMU_CALIB_TIMEOUT_MS);
}

bsp_imu_calib_status_t bsp_imu_calib_poll(void)
{
    return s_calib_status;
}

void bsp_imu_calib_get_info(float *std_g, float *tilt_deg, uint32_t *elapsed_ms)
{
    if (s_lock == NULL) {
        if (std_g) *std_g = 0;
        if (tilt_deg) *tilt_deg = 0;
        if (elapsed_ms) *elapsed_ms = 0;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (std_g) *std_g = s_last_std_g;
    if (tilt_deg) *tilt_deg = s_last_tilt_deg;
    if (elapsed_ms) {
        *elapsed_ms = (s_calib_status == BSP_IMU_CALIB_RUNNING)
                      ? (uint32_t)((esp_timer_get_time() - s_calib_start_us) / 1000)
                      : 0;
    }
    xSemaphoreGive(s_lock);
}
