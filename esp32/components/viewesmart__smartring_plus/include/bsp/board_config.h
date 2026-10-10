/*
 * SmartRing-Plus 板级定义（smartring_plus）
 *
 * Author : Ayang
 * Company: SHENZHEN VIEWE TECHNOLOGY CO.,LTD
 *
 * 全工程引脚/地址的单一事实来源，新增外设定义请加在这里。
 */
#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "hal/i2s_types.h"   /* I2S_NUM_0（音频引脚宏用） */

/* ------------------------------------------------------------------------
 * 共用 I2C 总线（SDA/SCL 上拉 R8/R9 = 2.2kΩ → 3V3）
 * 设备：CST816 触摸、ES8311 codec、QMI8658A IMU；V2 另有 AXP2101（0x34）
 * ------------------------------------------------------------------------ */
#define BOARD_I2C_SDA_IO          GPIO_NUM_8
#define BOARD_I2C_SCL_IO          GPIO_NUM_9
#define BOARD_I2C_PORT            I2C_NUM_0
#define BOARD_I2C_FREQ_HZ         400000

/* 触摸 CST816（7 位地址 0x15，实测芯片 ID = 0xB6，单点变体） */
#define BOARD_ADDR_TOUCH_CST816S  0x15
#define BOARD_TP_RST_IO           GPIO_NUM_40
#define BOARD_TP_INT_IO           GPIO_NUM_41

/* 屏幕 ST77916 QSPI（无 DC 脚，TE=GPIO38 已接但本工程不用） */
#define BOARD_LCD_QSPI_SCL_IO     GPIO_NUM_10
#define BOARD_LCD_QSPI_CS_IO      GPIO_NUM_11
#define BOARD_LCD_QSPI_D0_IO      GPIO_NUM_12
#define BOARD_LCD_QSPI_D1_IO      GPIO_NUM_13
#define BOARD_LCD_QSPI_D2_IO      GPIO_NUM_15
#define BOARD_LCD_QSPI_D3_IO      GPIO_NUM_14
#define BOARD_LCD_RST_IO          GPIO_NUM_39
#define BOARD_LCD_TE_IO           GPIO_NUM_38
#define BOARD_LCD_SPI_HOST        SPI2_HOST

#define BOARD_LCD_H_RES           360
#define BOARD_LCD_V_RES           360

/* 背光：GPIO46 经 Q1(AO3400A) 驱动 LEDK，高电平点亮，LEDC PWM 调光 */
#define BOARD_LCD_BK_IO           GPIO_NUM_46
#define BOARD_LCD_BK_LEDC_TIMER   LEDC_TIMER_0
#define BOARD_LCD_BK_LEDC_CHANNEL LEDC_CHANNEL_0
#define BOARD_LCD_BK_LEDC_FREQ_HZ 4000

/* ------------------------------------------------------------------------
 * SD 卡 SDMMC 4-bit
 * 板载 10kΩ 上拉，无独立卡检测脚（软件无法区分"没插卡"与"挂载失败"）
 * ------------------------------------------------------------------------ */
#define BOARD_SD_CLK_IO           GPIO_NUM_4
#define BOARD_SD_CMD_IO           GPIO_NUM_5
#define BOARD_SD_D0_IO            GPIO_NUM_3
#define BOARD_SD_D1_IO            GPIO_NUM_2
#define BOARD_SD_D2_IO            GPIO_NUM_7
#define BOARD_SD_D3_IO            GPIO_NUM_6
#define BOARD_SD_MOUNT_POINT      "/sdcard"

/* ------------------------------------------------------------------------
 * 电池与电源
 * V1/V2 共用同一电芯：单节 LiPo 3.7V 标称 / 4.2V 满电 / 600mAh
 * V1：GPIO1 ADC 分压估电量 + GPIO47 软关机
 * V2：AXP2101（I2C 0x34，与触摸共用总线）电量/充电/ALDO3；软关机仍用 GPIO47
 * ------------------------------------------------------------------------ */
/* 电芯规格（V1/V2 相同：3.7V / 600mAh 单节锂电） */
#define BOARD_BAT_CAPACITY_MAH    600
#define BOARD_BAT_NOMINAL_V       3.7f
#define BOARD_BAT_FULL_V          4.2f
#define BOARD_BAT_EMPTY_V         3.30f
#define BOARD_BAT_CHARGE_MA       300   /* 恒流 ≈0.5C */
#define BOARD_BAT_PRECHARGE_MA    75    /* 预充 ≈0.1C 档 */
#define BOARD_BAT_ITERM_MA        25    /* 终止电流 ≈C/24 */
/* V1 充电时 VCC_OUT 相对电芯抬升（本板 USB 约 4.35V，用于还原近似电芯电压） */
#define BOARD_BAT_CHG_IR_DROP_V   0.30f

/* V1 电池电压检测：GPIO1 = ADC1_CH0，R18/R21 各 10kΩ = 1:2 分压（测 VCC_OUT 轨） */
#define BOARD_BAT_ADC_IO          GPIO_NUM_1
#define BOARD_BAT_ADC_UNIT        ADC_UNIT_1
#define BOARD_BAT_ADC_CHAN        ADC_CHANNEL_0
#define BOARD_BAT_DIVIDER_RATIO   2.0f

/* V2 AXP2101（运行时探测 CHIP ID；地址与触摸共总线） */
#define BOARD_ADDR_AXP2101        0x34
#define BOARD_AXP2101_CHIP_ID     0x4A   /* REG 0x03，社区/M5 常用判定值 */
#define BOARD_AXP2101_ALDO3_MV    3300   /* ALDO3 输出 3.3V */
#define BOARD_AXP2101_CHG_MA      BOARD_BAT_CHARGE_MA

/* 软关机：GPIO47 经 Q4(SI1308EDL) 拉低 EC190707 KEY = 模拟长按电源键（实测 3.5s 断电）
 * V1/V2 均用此路径；AXP2101 不参与关机 */
#define BOARD_PW_OFF_IO           GPIO_NUM_47
#define BOARD_PW_OFF_HOLD_MS      3500   /* 保持高电平时长（长按需 ~3 秒） */

/* ------------------------------------------------------------------------
 * 音频：I2S 至 ES8311（播放 DOUT / 录音 DIN），PA 高电平使能；控制口走共享 I2C
 * ------------------------------------------------------------------------ */
#define BOARD_ADDR_CODEC_ES8311   0x18   /* 7 位地址，CE 脚经 R30 10kΩ 拉低 */
#define BOARD_I2S_PORT            I2S_NUM_0
#define BOARD_I2S_MCLK_IO         GPIO_NUM_48
#define BOARD_I2S_BCLK_IO         GPIO_NUM_21
#define BOARD_I2S_WS_IO           GPIO_NUM_17
#define BOARD_I2S_DOUT_IO         GPIO_NUM_16
#define BOARD_I2S_DIN_IO          GPIO_NUM_18
#define BOARD_PA_CTRL_IO          GPIO_NUM_45
/* 音频采样率（可通过编译选项覆盖以适配不同应用需求）*/
#ifndef BOARD_AUDIO_SAMPLE_RATE
#define BOARD_AUDIO_SAMPLE_RATE   48000
#endif
#define BOARD_AUDIO_MCLK_MULT     384   /* MCLK = 384 × Fs，与 ES8311 coeff 匹配 */

/* IMU 中断 1（QMI8658A INT1；INT2 未连接，原理图为空网络）。
 * 当前 bsp_imu 用 250Hz 任务轮询未使用此脚，预留给运动唤醒/敲击检测 */
#define BOARD_IMU_INT1_IO         GPIO_NUM_42

/* ------------------------------------------------------------------------
 * 系统脚登记（芯片功能占用，勿当普通 IO 使用；只作完整性登记，代码勿引用）
 *   GPIO0       BOOT（strapping，上电按住进下载模式）
 *   GPIO19/20   USB_N / USB_P（原生 USB，控制台/烧录走此口）
 *   GPIO43/44   UART0_RX / TX（备用调试串口）
 *   GPIO33~37   PSRAM 专用（MSPI 存储器总线 D4~D7/DQS，IOMUX 固定路径，
 *               SiP 封装内部键合未引出；PSRAM 全靠 Kconfig 配置，无引脚参数）
 * ------------------------------------------------------------------------ */
