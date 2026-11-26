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

/* Host fakes of the ESP-IDF pieces mg_smp_esp.c uses (tests/mg_smp_esp_server.c implements them). */
#pragma once
#define CONFIG_BOOTLOADER_OFFSET_IN_FLASH 0x0
#define CONFIG_PARTITION_TABLE_OFFSET 0x10000
/* The build the server stands for (muse_image_id.c): a Waveshare S3 1.75C
 * Muse Gadget (ESP32-S3, chip id 9) with BLE updates. */
#define CONFIG_IDF_FIRMWARE_CHIP_ID 9
#define CONFIG_HOMEHUB_BLE_PRODUCT_CODE "W175"
#define CONFIG_MUSE_BOARD_ID "waveshare_s3_175c"
#define CONFIG_MUSE_GADGET_BLE_AUDIO 1
#define CONFIG_MUSE_GADGET_BLE_SMP 1
