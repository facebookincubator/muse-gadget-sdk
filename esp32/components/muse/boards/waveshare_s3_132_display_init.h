/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Panel initialization adapted from Waveshare's official
 * waveshare/esp32_s3_touch_amoled_1_32 BSP v1.0.1:
 * https://github.com/waveshareteam/Waveshare-ESP32-components/blob/c77caf968fa6b11f3b6a416c853c578d012a8cea/bsp/esp32_s3_touch_amoled_1_32/esp32_s3_touch_amoled_1_32.c#L50-L62
 *
 * The upstream component includes an Apache-2.0 LICENSE, reproduced in
 * waveshare_s3_132_display_init.LICENSE. Its source has no file-level
 * copyright notice, and the component archive contains no NOTICE file.
 *
 * Local modifications: use sh8601_lcd_init_cmd_t and the s_lcd_init name;
 * use NULL for the two zero-length payloads; append MADCTL 0xC0 for the
 * orientation tested on this board. The first eleven commands retain the
 * upstream command bytes, payload bytes, lengths, order and delays.
 */
#pragma once

static const sh8601_lcd_init_cmd_t s_lcd_init[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, NULL, 0, 100},
    {0x29, NULL, 0, 0},
    /* Local orientation setting; including it also updates the driver's
     * cached MADCTL instead of sending an untracked command after init. */
    {0x36, (uint8_t[]){0xC0}, 1, 0},
};
