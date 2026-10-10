/*
 * AXP2101 底层驱动（SmartRing-Plus V2 PMIC，组件内部使用）
 *
 * 依据 X-Powers AXP2101 Datasheet；不对外作为应用层主 API。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 探测并初始化 AXP2101（ALDO3 / 充电 / fuel gauge / VBAT ADC）
 *
 * @param bus 共享 I2C 总线（须已由 bsp_display_init 创建）
 * @return ESP_OK 探测成功并完成配置；ESP_ERR_NOT_FOUND 无芯片；其它为 I2C/配置错误
 */
esp_err_t bsp_axp2101_init(i2c_master_bus_handle_t bus);

/** 是否已成功初始化 */
bool bsp_axp2101_is_ready(void);

/**
 * @brief 读取电量与充电/外供电状态（以及可选电池电压）
 *
 * @param percent   0~100（REG 0xA4；无电池时为 0），可为 NULL
 * @param charging  充电中或 VBUS 在位（与 V1“外供电”语义对齐），可为 NULL
 * @param voltage_v 电池电压（V，ADC 1mV/LSB），失败/无电池时为 0；可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化；ESP_FAIL I2C 失败
 */
esp_err_t bsp_axp2101_get_battery(int *percent, bool *charging, float *voltage_v);

#ifdef __cplusplus
}
#endif
