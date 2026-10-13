/*
 * SmartRing-Plus 板级支持包（组件名：smartring_plus）
 *
 * Author : Ayang
 * Company: SHENZHEN VIEWE TECHNOLOGY CO.,LTD
 * License: Apache-2.0
 *
 * 封装本板已验证的固定硬件层，应用层只面对 bsp_* API：
 *   - 显示：ST77916 QSPI 初始化（官方 B 版初始化表）+ esp_lvgl_adapter + 背光 LEDC
 *   - 触摸：CST816（esp_lcd_touch，注册为 LVGL 输入设备）
 *   - 共享 I2C 总线句柄（音频 codec / IMU 复用）
 *   - 电池：运行时探测 AXP2101（V2）或 GPIO1 ADC（V1）；电芯均为 3.7V/600mAh
 *   - 电源：软关机（GPIO47，V1/V2 相同；AXP 不参与关机）
 *   - 音频：ES8311 codec + I2S（硬件层）+ 阻塞读写原语（句柄也导出，可绕过原语）
 *   - IMU：QMI8658A 采样/滤波（硬件层）+ 水平校准（参考实现，不用可不调）
 *   - SD 卡：SDMMC 4-bit + VFS FAT 挂载（硬件层）+ 列目录（参考实现，可弃可换）
 *   - V2 PMIC：AXP2101 ALDO3=3.3V / 充电 300mA≈0.5C（I2C 0x34）
 *
 * 推荐：#include "bsp/smartring_plus.h"
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_lcd_touch.h"
#include "esp_codec_dev.h"
#include "sdmmc_cmd.h"
#include "lvgl.h"

#include "bsp/board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 显示/触摸 */

/**
 * @brief 显示子系统初始化（I2C 总线 + 触摸 + ST77916 + esp_lvgl_adapter + 背光 100%）
 *
 * 成功后 LVGL 任务已运行，应用层 esp_lv_adapter_lock(-1) 后即可建 UI。
 */
esp_err_t bsp_display_init(void);

/**
 * @brief bsp_display_init() with the LVGL task pinned to task_core_id (-1: no
 *        affinity) at task_priority. Added for muse-gadget-sdk, which runs LVGL
 *        on the core that set up the panel's SPI bus.
 */
esp_err_t bsp_display_init_with_task(int task_core_id, uint32_t task_priority);

/** 背光亮度 0~100（LEDC PWM，GPIO46 高电平点亮） */
void bsp_display_backlight_set(uint8_t percent);

/**
 * @brief 运行时切换 RGB565 字节序（调试/换屏用）
 *
 * 通过 lv_display_set_color_format(RGB565_SWAPPED/RGB565) 实现，切后全屏重绘。
 */
void bsp_display_set_swap_bytes(bool enable);
bool bsp_display_get_swap_bytes(void);

/** LVGL 显示句柄（lv_display_set_color_format 等场景用） */
lv_display_t *bsp_display_get_handle(void);

/** 触摸句柄（直读坐标用），未初始化返回 NULL */
esp_lcd_touch_handle_t bsp_touch_get_handle(void);

/** 共享 I2C 总线句柄（SDA=8/SCL=9/400kHz，音频/IMU 复用），未初始化返回 NULL */
i2c_master_bus_handle_t bsp_i2c_get_bus(void);

/* ---------------------------------------------------------------- 电池 */

/** 运行时探测到的电源管理路径 */
typedef enum {
    BSP_PMIC_UNKNOWN = 0,
    BSP_PMIC_V1_ADC,       /**< GPIO1 ADC 分压估电量（无 AXP2101） */
    BSP_PMIC_V2_AXP2101,   /**< AXP2101 fuel gauge / 充电 / ALDO3 */
} bsp_pmic_type_t;

/** 电池采样数据快照 */
typedef struct {
    float voltage_v;    /**< 电压（V）；V1 为分压轨，V2 为 AXP VBAT ADC */
    int   percent;      /**< 电量 0~100；V1 查表估算，V2 为 E-Gauge */
    bool  charging;     /**< true = 充电中或外供电（V1 电压滞回 / V2 VBUS|充电方向） */
} bsp_battery_data_t;

/**
 * @brief 电池子系统初始化（须先 bsp_display_init，以便 V2 复用 I2C）
 *
 * 运行时探测 AXP2101（CHIP ID 0x03 == 0x4A）；电芯规格见 board_config（3.7V/600mAh）：
 *   - 命中 → V2：ALDO3/0.5C 充电 + 芯片 E-Gauge（无 REGA1 厂校写入）
 *   - 未命中 → V1：GPIO1 ADC + 同规格 OCV 查表；充电时 SoC 做轨压补偿
 *
 * 不做用户“电池校准”流程；V1 仅 ADC 硬件校准，V2 依赖 E-Gauge 自学习。
 */
esp_err_t bsp_battery_init(void);

/** 当前 PMIC 路径（bsp_battery_init 之后有效） */
bsp_pmic_type_t bsp_battery_get_pmic_type(void);

/** 读取最新采样快照（线程安全） */
void bsp_battery_get_data(bsp_battery_data_t *out);

/** 便捷查询（线程安全） */
float bsp_battery_get_voltage(void);
int   bsp_battery_get_percent(void);
bool  bsp_battery_is_charging(void);

/* ---------------------------------------------------------------- 电源 */

/**
 * @brief 电源管理初始化（配置 PW_OFF GPIO，默认不触发）
 *
 * V1/V2 均使用 GPIO47 软关机；AXP2101 不参与关机。
 */
esp_err_t bsp_power_init(void);

/**
 * @brief 软关机，正常情况不返回（整机断电）
 *
 * GPIO47 输出高电平并保持 BOARD_PW_OFF_HOLD_MS（3.5s，实测）。
 * 调用方负责 UI 提示（如"正在关机…"）。
 */
void bsp_power_shutdown(void);

/* ---------------------------------------------------------------- 音频（ES8311 + I2S） */

/**
 * @brief 音频初始化（I2S 48kHz/16bit/立体声 + ES8311 codec，复用共享 I2C 总线）
 *
 * 须先 bsp_display_init()（I2C 总线在其中初始化）；重复调用跳过。
 * PA（NS4150B）由 ES8311 驱动按 pa_pin 自动控制（播放时拉高）。
 * 初始化后默认输出静音，只做硬件初始化，**不设增益/音量**——由应用层
 * 用 bsp_audio_get_codec() 自行设置。
 *
 * 本板实测推荐值（录音机工程调校，非硬件事实，仅供应用参考）：
 *   - 录音增益 36dB：esp_codec_dev_set_in_gain()，驱动按 6dB 整档
 *     向下取整（0/6/.../42，设 34 会落回 30）；36dB 底噪可接受
 *   - 回放音量 75：esp_codec_dev_set_out_vol()，范围 0~100
 */
esp_err_t bsp_audio_init(void);

/**
 * @brief codec 句柄（未初始化返回 NULL）
 *
 * 需要增益/音量/静音等细粒度控制，或做流式连续播放时，用此句柄直调
 * esp_codec_dev API（绕过下面的读写原语）。注意两个坑：
 *   - esp_codec_dev_write 返回 0 才是成功（与 esp_err_t 约定相反）
 *   - 回放结束务必 esp_codec_dev_set_out_mute(true)，否则爆音
 */
esp_codec_dev_handle_t bsp_audio_get_codec(void);

/** I2S RX 通道句柄（直读原始 PCM 用，未初始化返回 NULL） */
i2s_chan_handle_t bsp_audio_get_rx(void);

/**
 * @brief 阻塞读 PCM（录音），填满 len 字节才返回
 *
 * 数据格式固定：48kHz/16bit/立体声交错（约 192KB/s）。
 * 单次读失败/超时（每次最长 timeout_ms）会打日志并重试；
 * 超时为 0 表示用驱动默认等待。
 */
esp_err_t bsp_audio_read(void *buf, size_t len, uint32_t timeout_ms);

/**
 * @brief 阻塞写 PCM（回放），写完 len 字节才返回
 *
 * **不做静音管理**：一段回放的完整时序由调用方保证——
 * bsp_audio_set_mute(false) → bsp_audio_write(...)（可分块，便于更新进度）
 * → bsp_audio_set_mute(true)。结尾静音漏掉会爆音（实测坑）。
 */
esp_err_t bsp_audio_write(const void *buf, size_t len);

/** 输出静音开关（回放时序见 bsp_audio_write 注释） */
esp_err_t bsp_audio_set_mute(bool mute);

/* ---------------------------------------------------------------- IMU（QMI8658A） */

/**
 * @brief IMU 初始化（±2G/250Hz 加速度 + 250Hz 采样任务 + EMA 低通）
 *
 * 复用 bsp_i2c_get_bus()，须先 bsp_display_init()。
 * gyro 随 accel 一并使能（硬件事实，数据不用管）。
 */
esp_err_t bsp_imu_init(void);

/** 最新滤波后加速度（板载轴，m/s²），线程安全 */
void bsp_imu_get_accel(float *ax, float *ay, float *az);

/**
 * @brief 屏幕平面倾斜向量（m/s²，已减校准零点），线程安全
 *
 * gx 指向屏幕 +x（右）、gy 指向屏幕 +y（下）方向的"下滑力"。
 * 未校准通过时返回原始屏幕平面分量（未减零点）。
 */
void bsp_imu_get_tilt(float *gx, float *gy);

/* ----- 水平校准（参考实现：不用可不调用，想换思路在 get_accel 之上自写） ----- */

typedef enum {
    BSP_IMU_CALIB_IDLE = 0,   /**< 未开始（初始状态） */
    BSP_IMU_CALIB_RUNNING,    /**< 采样窗口累计/判定中 */
    BSP_IMU_CALIB_PASSED,     /**< 已通过，零点已记录 */
    BSP_IMU_CALIB_FAILED,     /**< 30s 内持续不满足条件 */
} bsp_imu_calib_status_t;

/** 开始/重新开始校准（重置判定窗口与 30s 截止） */
void bsp_imu_calib_start(void);

/** 查询校准状态（线程安全，建议 UI 每 100~200ms 轮询） */
bsp_imu_calib_status_t bsp_imu_calib_poll(void);

/**
 * @brief 校准过程观测值（调试用，线程安全）
 * @param std_g      最近窗口三轴标准差最大值（g），NULL 跳过
 * @param tilt_deg   最近窗口平均姿态相对水平面夹角（度），NULL 跳过
 * @param elapsed_ms 距 bsp_imu_calib_start 的毫秒数，NULL 跳过
 */
void bsp_imu_calib_get_info(float *std_g, float *tilt_deg, uint32_t *elapsed_ms);

/* ---------------------------------------------------------------- SD 卡 */

/**
 * @brief 挂载 SD 卡（SDMMC 4-bit + FAT，挂到 BOARD_SD_MOUNT_POINT）
 *
 * 失败（含没插卡）返回错误并打日志，不重启不 assert；重复调用返回 ESP_ERR_INVALID_STATE。
 * 本板无卡检测脚，软件无法区分"没插卡"与"挂载失败"。
 */
esp_err_t bsp_sd_init(void);

/** 是否已挂载 */
bool bsp_sd_is_mounted(void);

/** 卸载（未挂载时调用安全无操作） */
void bsp_sd_deinit(void);

/** 卡句柄（sdmmc_card_print_info 调试用），未挂载返回 NULL */
const sdmmc_card_t *bsp_sd_get_card(void);

/* ----- 列目录（参考实现：纯 POSIX FAT 操作，与硬件无关，可弃可换） ----- */

/** 目录条目类型 */
typedef enum {
    BSP_SD_ENTRY_FILE = 0,
    BSP_SD_ENTRY_DIR,
} bsp_sd_entry_type_t;

/** 目录条目（name 为条目名，不含路径） */
typedef struct {
    char *name;
    bsp_sd_entry_type_t type;
} bsp_sd_entry_t;

/**
 * @brief 列目录（排序：文件夹在前、文件在后，各自 strcasecmp 升序）
 *
 * @param path     绝对路径（如 "/sdcard" 或 "/sdcard/DCIM"）
 * @param entries  输出条目数组（calloc 分配，调用方用完须 bsp_sd_free_list 释放）
 * @param count    输出条目数
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 路径打不开；ESP_ERR_INVALID_STATE 未挂载
 *
 * 如实返回目录内容（含 FAT 隐藏系统项），过滤由调用方决定。
 */
esp_err_t bsp_sd_list(const char *path, bsp_sd_entry_t **entries, size_t *count);

/** 释放 bsp_sd_list 返回的数组 */
void bsp_sd_free_list(bsp_sd_entry_t *entries, size_t count);

#ifdef __cplusplus
}
#endif
