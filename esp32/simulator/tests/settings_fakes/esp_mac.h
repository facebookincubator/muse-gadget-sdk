/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include "esp_err.h"
#define ESP_MAC_WIFI_STA 0
esp_err_t esp_read_mac(uint8_t *mac, int type);
