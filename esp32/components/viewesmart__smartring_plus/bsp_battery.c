/*
 * SmartRing-Plus BSP 电池采样
 *
 * 电芯（V1/V2 相同）：单节锂电 3.7V / 600mAh，满充 4.2V
 * 运行时探测 AXP2101（CHIP ID 0x03 == 0x4A）：
 *   V2：芯片 E-Gauge + 0.5C(300mA) 充电（无 REGA1 厂参；靠自学习）
 *   V1：GPIO1 ADC + 同规格 OCV 曲线查表（无电量计；仅有 ESP ADC 曲线校准）
 *
 * 本模块不做“用户电池校准”流程：
 *   - V1：adc_cali 校准 ADC 读数；SoC 为查表估算
 *   - V2：使用 AXP 内置 fuel gauge；完整厂校需电池型号参数（REGA1），未实现
 */
#include "bsp/smartring_plus.h"
#include "bsp/board_config.h"
#include "bsp/bsp_axp2101.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "battery";

#define SAMPLE_PERIOD_MS      1000
#define SAMPLES_PER_READING   32
#define AVG_WINDOW            8

/* V1：充电检测（测的是 VCC_OUT 轨，插 USB 后电压会抬高） */
#define CHG_ENTER_V           4.25f
#define CHG_EXIT_V            4.15f
#define CHG_CONFIRM_COUNT     2
#define V_VALID_MIN           2.80f

typedef struct {
    float voltage_v;
    int   percent;
} bat_curve_point_t;

/*
 * 3.7V / 600mAh 单节锂电典型放电 OCV→SoC（轻载）。
 * 截止 BOARD_BAT_EMPTY_V，满充 BOARD_BAT_FULL_V；标称 3.7V 约 40%。
 */
static const bat_curve_point_t s_curve[] = {
    { 3.30f,   0 },
    { 3.40f,   5 },
    { 3.50f,  10 },
    { 3.55f,  15 },
    { 3.60f,  20 },
    { 3.65f,  28 },
    { 3.70f,  40 },
    { 3.75f,  48 },
    { 3.80f,  55 },
    { 3.85f,  62 },
    { 3.90f,  70 },
    { 3.95f,  76 },
    { 4.00f,  82 },
    { 4.05f,  88 },
    { 4.10f,  93 },
    { 4.15f,  97 },
    { 4.20f, 100 },
};
#define CURVE_POINTS  (sizeof(s_curve) / sizeof(s_curve[0]))

static bsp_pmic_type_t s_pmic = BSP_PMIC_UNKNOWN;
static SemaphoreHandle_t s_lock;
static bsp_battery_data_t s_data = { .voltage_v = 0.0f, .percent = 0, .charging = false };

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;
static float s_avg_buf[AVG_WINDOW];
static int s_avg_count;
static int s_avg_idx;

static int percent_from_voltage(float v)
{
    if (v <= s_curve[0].voltage_v) {
        return s_curve[0].percent;
    }
    if (v >= s_curve[CURVE_POINTS - 1].voltage_v) {
        return s_curve[CURVE_POINTS - 1].percent;
    }
    for (int i = 1; i < CURVE_POINTS; i++) {
        if (v <= s_curve[i].voltage_v) {
            float t = (v - s_curve[i - 1].voltage_v)
                    / (s_curve[i].voltage_v - s_curve[i - 1].voltage_v);
            return s_curve[i - 1].percent
                 + (int)(t * (s_curve[i].percent - s_curve[i - 1].percent) + 0.5f);
        }
    }
    return 0;
}

static float read_voltage_once(void)
{
    int raw_sum = 0;
    int n_ok = 0;
    for (int i = 0; i < SAMPLES_PER_READING; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BOARD_BAT_ADC_CHAN, &raw) != ESP_OK) {
            continue;
        }
        raw_sum += raw;
        n_ok++;
    }
    if (n_ok <= 0) {
        return 0.0f;
    }
    int raw_avg = raw_sum / n_ok;

    int mv = 0;
    if (s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw_avg, &mv) != ESP_OK) {
            mv = 0;
        }
    }
    if (mv <= 0) {
        /* ADC 曲线校准不可用时的粗估：12dB 衰减满量程约 3100mV @ 12-bit */
        mv = (raw_avg * 3100) / 4095;
    }
    return (mv / 1000.0f) * BOARD_BAT_DIVIDER_RATIO;
}

/**
 * V1：以 ADC 实测电压为准 → 3.7V/600mAh OCV 曲线换算电量。
 * - 放电：直接用测得电压查表
 * - 充电：ADC 采到抬高的 VCC_OUT，先减去 BOARD_BAT_CHG_IR_DROP_V 再查表
 * - 显示值向目标值每秒最多 ±1%，避免插电瞬间跳变
 */
static void battery_task_v1(void *arg)
{
    (void)arg;
    bool charging = false;
    int chg_streak = 0;
    int log_countdown = 0;
    int soc_ui = -1;
    bool primed = false; /* 首帧用电压直接定电量，不开机显示 0 */

    while (1) {
        float v = read_voltage_once();

        s_avg_buf[s_avg_idx] = v;
        s_avg_idx = (s_avg_idx + 1) % AVG_WINDOW;
        if (s_avg_count < AVG_WINDOW) {
            s_avg_count++;
        }
        float sum = 0;
        for (int i = 0; i < s_avg_count; i++) {
            sum += s_avg_buf[i];
        }
        float v_avg = sum / (float)s_avg_count;

        if (v_avg < V_VALID_MIN) {
            /* ADC 尚未有效：保持上次显示，绝不写成 0% */
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
            continue;
        }

        bool want = charging;
        if (!charging && v_avg > CHG_ENTER_V) {
            want = true;
        } else if (charging && v_avg < CHG_EXIT_V) {
            want = false;
        }
        if (want != charging) {
            if (++chg_streak >= CHG_CONFIRM_COUNT) {
                charging = want;
                chg_streak = 0;
                ESP_LOGI(TAG, "charging state -> %s (%.2fV)",
                         charging ? "charging" : "battery", (double)v_avg);
            }
        } else {
            chg_streak = 0;
        }

        /*
         * 电量由实测电压查表得到。
         * 插 USB 后 ADC 看到的是抬高的 VCC_OUT，减去轨压抬升得到近似电芯电压。
         * 电压已高于充电门限时即按充电校准，避免开机插着 USB 时用 4.3V 直接算出 100%。
         */
        const bool use_chg_cal = charging || (v_avg > CHG_ENTER_V);
        float v_cell = use_chg_cal ? (v_avg - BOARD_BAT_CHG_IR_DROP_V) : v_avg;
        if (v_cell < s_curve[0].voltage_v) {
            v_cell = s_curve[0].voltage_v;
        }
        if (v_cell > s_curve[CURVE_POINTS - 1].voltage_v) {
            v_cell = s_curve[CURVE_POINTS - 1].voltage_v;
        }
        int target = percent_from_voltage(v_cell);

        if (!primed || soc_ui < 0) {
            soc_ui = target;
            primed = true;
            ESP_LOGI(TAG, "SoC from voltage: meas=%.2fV cell~%.2fV -> %d%% (3.7V/%dmAh)",
                     (double)v_avg, (double)v_cell, soc_ui, BOARD_BAT_CAPACITY_MAH);
        } else if (target > soc_ui) {
            soc_ui++;
        } else if (target < soc_ui) {
            soc_ui--;
        }

        if (soc_ui < 0) {
            soc_ui = 0;
        }
        if (soc_ui > 100) {
            soc_ui = 100;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_data.voltage_v = v_avg;
        s_data.percent = soc_ui;
        s_data.charging = charging;
        xSemaphoreGive(s_lock);

        if (++log_countdown >= 5) {
            log_countdown = 0;
            ESP_LOGI(TAG, "bat(V1): meas=%.2fV cell~%.2fV %d%% %s",
                     (double)v_avg, (double)v_cell, soc_ui,
                     charging ? "charging" : "on-battery");
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

static void battery_task_v2(void *arg)
{
    (void)arg;
    int log_countdown = 0;
    int fail_streak = 0;

    while (1) {
        int pct = 0;
        bool charging = false;
        float volts = 0.0f;
        if (bsp_axp2101_get_battery(&pct, &charging, &volts) == ESP_OK) {
            fail_streak = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_data.voltage_v = volts;
            s_data.percent = pct;
            s_data.charging = charging;
            xSemaphoreGive(s_lock);

            if (++log_countdown >= 5) {
                log_countdown = 0;
                ESP_LOGI(TAG, "bat(V2): %.2fV %d%% %s", (double)volts, pct,
                         charging ? "ext-power/charging" : "on-battery");
            }
        } else if (++fail_streak == 5) {
            ESP_LOGW(TAG, "V2 AXP read failing repeatedly (kept last snapshot)");
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

static esp_err_t init_v1_adc(void)
{
    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BOARD_BAT_ADC_UNIT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc unit init failed");

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BOARD_BAT_ADC_CHAN, &chan_cfg),
                        TAG, "adc chan config failed");

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BOARD_BAT_ADC_UNIT,
        .chan = BOARD_BAT_ADC_CHAN,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK);
#endif
    if (!s_cali_ok) {
        ESP_LOGW(TAG, "ADC 曲线校准不可用，使用 raw 粗估电压");
    }

    BaseType_t ok = xTaskCreate(battery_task_v1, "battery", 3072, NULL, 3, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    ESP_LOGI(TAG, "PMIC=V1_ADC (3.7V/%dmAh OCV, 1Hz, adc_cali=%d)",
             BOARD_BAT_CAPACITY_MAH, s_cali_ok);
    return ESP_OK;
}

esp_err_t bsp_battery_init(void)
{
    if (s_lock != NULL) {
        ESP_LOGW(TAG, "bsp_battery 已初始化，跳过");
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex create failed");

    i2c_master_bus_handle_t bus = bsp_i2c_get_bus();
    if (bus != NULL) {
        esp_err_t axp_err = bsp_axp2101_init(bus);
        if (axp_err == ESP_OK) {
            s_pmic = BSP_PMIC_V2_AXP2101;
            BaseType_t ok = xTaskCreate(battery_task_v2, "battery", 3072, NULL, 3, NULL);
            if (ok != pdPASS) {
                ESP_LOGE(TAG, "V2 task create failed");
                return ESP_ERR_NO_MEM;
            }
            ESP_LOGI(TAG, "PMIC=V2_AXP2101 (3.7V/%dmAh, E-Gauge, charge=%dmA)",
                     BOARD_BAT_CAPACITY_MAH, BOARD_BAT_CHARGE_MA);
            return ESP_OK;
        }
        if (axp_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "AXP2101 init error %s, fallback V1 ADC", esp_err_to_name(axp_err));
        }
    } else {
        ESP_LOGW(TAG, "I2C bus not ready — V1 ADC only (call bsp_display_init first for V2)");
    }

    s_pmic = BSP_PMIC_V1_ADC;
    return init_v1_adc();
}

bsp_pmic_type_t bsp_battery_get_pmic_type(void)
{
    return s_pmic;
}

void bsp_battery_get_data(bsp_battery_data_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        *out = (bsp_battery_data_t){0};
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_lock);
}

float bsp_battery_get_voltage(void)
{
    bsp_battery_data_t d;
    bsp_battery_get_data(&d);
    return d.voltage_v;
}

int bsp_battery_get_percent(void)
{
    bsp_battery_data_t d;
    bsp_battery_get_data(&d);
    return d.percent;
}

bool bsp_battery_is_charging(void)
{
    bsp_battery_data_t d;
    bsp_battery_get_data(&d);
    return d.charging;
}
