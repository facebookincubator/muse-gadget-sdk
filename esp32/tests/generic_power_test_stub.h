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
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#define STUB(type, name, args, value) static inline type name args { return value; }
typedef void *esp_codec_dev_handle_t;
typedef struct { uint32_t sample_rate; uint8_t channel, bits_per_sample; } esp_codec_dev_sample_info_t;
#define ESP_CODEC_DEV_OK 0
int esp_codec_dev_open(esp_codec_dev_handle_t d, esp_codec_dev_sample_info_t *s);
int esp_codec_dev_close(esp_codec_dev_handle_t d);
bool test_capture(void);
STUB(int, esp_codec_dev_set_out_vol, (esp_codec_dev_handle_t d, int v), (assert(!test_capture()), ESP_OK))
STUB(int, esp_codec_dev_set_in_gain, (esp_codec_dev_handle_t d, float v), (assert(!test_capture()), ESP_OK))
static inline int esp_codec_dev_read(esp_codec_dev_handle_t d, void *p, int n) { memset(p, 0, n); return ESP_OK; }
STUB(int, esp_codec_dev_write, (esp_codec_dev_handle_t d, void *p, int n), ESP_OK)
typedef int lv_align_t, lv_display_t, lv_indev_t, lv_obj_t, lv_timer_t;
typedef uint32_t lv_color_t;
#define LV_OPA_COVER 255
lv_obj_t *lv_display_get_screen_active(lv_display_t *);
#define lv_obj_set_style_bg_opa(...) ((void)0)
#define lv_obj_set_style_bg_color(...) ((void)0)
lv_timer_t *lv_timer_create(void (*)(lv_timer_t *), uint32_t, void *);
#define lv_timer_pause(...) ((void)0)
#define lv_timer_resume(...) ((void)0)
#define lv_timer_reset(...) ((void)0)
#define lv_refr_now(...) ((void)0)
typedef int gpio_num_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *dev);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tx_size, uint8_t *rx, size_t rx_size, int timeout);
esp_err_t i2c_master_get_bus_handle(int port, i2c_master_bus_handle_t *out);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int ms);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t n, int ms);
typedef struct { size_t rx_buffer_size, tx_buffer_size; } usb_serial_jtag_driver_config_t;
#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() { .rx_buffer_size = 256, .tx_buffer_size = 256 }
STUB(esp_err_t, usb_serial_jtag_driver_install, (const usb_serial_jtag_driver_config_t *c), ESP_OK)
int usb_serial_jtag_write_bytes(const void *buf, size_t n, uint32_t ticks);
int usb_serial_jtag_read_bytes(void *p, size_t n, uint32_t ticks);
STUB(bool, usb_serial_jtag_is_connected, (void), true)
STUB(esp_err_t, usb_serial_jtag_wait_tx_done, (uint32_t ticks), ESP_OK)
void vTaskDelay(unsigned ticks);
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct fake_task *TaskHandle_t;
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack_bytes, void *arg, unsigned priority, TaskHandle_t *out);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(int clear, TickType_t wait);
