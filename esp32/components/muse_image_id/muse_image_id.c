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

#include "muse_image_id.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#ifdef ESP_PLATFORM
#include <stddef.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
_Static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) == MUSE_IMAGE_APP_DESC_OFFSET,
               "the app description follows the image and segment headers");
_Static_assert(offsetof(esp_image_header_t, chip_id) == MUSE_IMAGE_CHIP_ID_OFFSET, "esp_image_header_t.chip_id");
_Static_assert(MUSE_IMAGE_APP_DESC_OFFSET + sizeof(esp_app_desc_t) == MUSE_IMAGE_BOARD_DESC_OFFSET,
               "the custom app description follows esp_app_desc_t");
_Static_assert(offsetof(esp_app_desc_t, version) == 16, "esp_app_desc_t.version");
#define BOARD_DESC_SECTION __attribute__((section(".rodata_custom_desc"), used))
#else
#define BOARD_DESC_SECTION
#endif

#ifndef CONFIG_HOMEHUB_BLE_PRODUCT_CODE
#define CONFIG_HOMEHUB_BLE_PRODUCT_CODE ""
#endif
#ifndef CONFIG_MUSE_BOARD_ID
#define CONFIG_MUSE_BOARD_ID "none"
#endif
#ifndef CONFIG_IDF_FIRMWARE_CHIP_ID
#error "CONFIG_IDF_FIRMWARE_CHIP_ID: the chip this build is for"
#endif
_Static_assert(sizeof(CONFIG_HOMEHUB_BLE_PRODUCT_CODE) <= 16, "product code fits muse_board_desc_t");
_Static_assert(sizeof(CONFIG_MUSE_BOARD_ID) <= 32, "board id fits muse_board_desc_t");

#if CONFIG_MUSE_GADGET_BLE_AUDIO
#define FLAG_GADGET MUSE_BOARD_DESC_F_GADGET
#else
#define FLAG_GADGET 0
#endif
#if CONFIG_MUSE_GADGET_BLE_AUDIO && CONFIG_MUSE_GADGET_BLE_SMP
#define FLAG_BLE_SMP MUSE_BOARD_DESC_F_BLE_SMP
#else
#define FLAG_BLE_SMP 0
#endif
/* The device model's hash, from CMakeLists.txt (SHA-256 of CONFIG_MUSE_DEVICE_MODEL_ID, its first 8 bytes). */
#ifdef MUSE_DEVICE_MODEL_HASH
#define FLAG_MODEL MUSE_BOARD_DESC_F_MODEL
#define MODEL_HASH { MUSE_DEVICE_MODEL_HASH }
#else
#define FLAG_MODEL 0
#define MODEL_HASH { 0 }
#endif

/* Kept in every image by the component's `-u muse_board_desc` (CMakeLists.txt). */
const muse_board_desc_t muse_board_desc BOARD_DESC_SECTION = {
    .magic = MUSE_BOARD_DESC_MAGIC,
    .layout = MUSE_BOARD_DESC_LAYOUT,
    .flags = FLAG_GADGET | FLAG_BLE_SMP | FLAG_MODEL,
    .chip_id = CONFIG_IDF_FIRMWARE_CHIP_ID,
    .product_code = CONFIG_HOMEHUB_BLE_PRODUCT_CODE,
    .board_id = CONFIG_MUSE_BOARD_ID,
    .model_hash = MODEL_HASH,
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A NUL-terminated field of `cap` bytes, copied printable; false if the
 * field has no NUL (not a description this firmware wrote). */
static bool field(const uint8_t *src, size_t cap, char *out, size_t out_cap)
{
    size_t n = 0;
    while (n < cap && src[n]) {
        n++;
    }
    size_t k = n < out_cap - 1 ? n : out_cap - 1;
    for (size_t i = 0; i < k; i++) {
        out[i] = src[i] >= 0x20 && src[i] < 0x7F ? (char)src[i] : '?';
    }
    out[k] = '\0';
    return n < cap;
}

muse_image_verdict_t muse_image_check(const uint8_t *head, size_t n, bool board, muse_image_info_t *info)
{
    muse_image_info_t scratch;
    muse_image_info_t *in = info ? info : &scratch;
    memset(in, 0, sizeof(*in));
    if (!head || n < MUSE_IMAGE_HEAD_BYTES) {
        return MUSE_IMAGE_SHORT;
    }
    const uint8_t *app = head + MUSE_IMAGE_APP_DESC_OFFSET;
    if (head[0] != 0xE9 || rd32(app) != 0xABCD5432u) {   /* ESP_IMAGE_HEADER_MAGIC, ESP_APP_DESC_MAGIC_WORD */
        return MUSE_IMAGE_NOT_APP;
    }
    in->chip_id = rd16(head + MUSE_IMAGE_CHIP_ID_OFFSET);
    field(app + 16, 32, in->version, sizeof(in->version));
    const uint8_t *bd = head + MUSE_IMAGE_BOARD_DESC_OFFSET;
    in->has_board = rd32(bd) == MUSE_BOARD_DESC_MAGIC && bd[4] >= MUSE_BOARD_DESC_LAYOUT
                    && field(bd + 8, 16, in->product_code, sizeof(in->product_code))
                    && field(bd + 24, 32, in->board_id, sizeof(in->board_id));
    if (in->has_board) {
        in->flags = bd[5];
    } else {
        in->product_code[0] = in->board_id[0] = '\0';
    }
    if (in->chip_id != muse_board_desc.chip_id) {
        return MUSE_IMAGE_WRONG_CHIP;
    }
    if (!board) {
        return MUSE_IMAGE_OK;
    }
    if (!in->has_board) {
        return MUSE_IMAGE_NO_BOARD;
    }
    /* Compared as stored (the fields hold a NUL within their size), not as printed. */
    if (strncmp((const char *)bd + 8, muse_board_desc.product_code, 16) != 0
        || strncmp((const char *)bd + 24, muse_board_desc.board_id, 32) != 0) {
        return MUSE_IMAGE_WRONG_BOARD;
    }
    /* The model only when both carry one: an image from before it (or a build without one) is judged as before. */
    if ((muse_board_desc.flags & MUSE_BOARD_DESC_F_MODEL) && (in->flags & MUSE_BOARD_DESC_F_MODEL)
        && memcmp(bd + 56, muse_board_desc.model_hash, sizeof(muse_board_desc.model_hash)) != 0) {
        return MUSE_IMAGE_WRONG_MODEL;
    }
    if ((muse_board_desc.flags & MUSE_BOARD_DESC_F_BLE_SMP) && !(in->flags & MUSE_BOARD_DESC_F_BLE_SMP)) {
        return MUSE_IMAGE_NO_BLE_UPDATE;
    }
    return MUSE_IMAGE_OK;
}

muse_image_verdict_t muse_image_check_model(const uint8_t *head, size_t n, muse_image_info_t *info)
{
    muse_image_info_t scratch;
    muse_image_info_t *in = info ? info : &scratch;
    muse_image_verdict_t v = muse_image_check(head, n, false, in);
    if (v != MUSE_IMAGE_OK) {
        return v;
    }
    if (!in->has_board) {
        return MUSE_IMAGE_NO_BOARD;
    }
    if ((muse_board_desc.flags & MUSE_BOARD_DESC_F_MODEL) && (in->flags & MUSE_BOARD_DESC_F_MODEL)
        && memcmp(head + MUSE_IMAGE_BOARD_DESC_OFFSET + 56, muse_board_desc.model_hash,
                  sizeof(muse_board_desc.model_hash)) != 0) {
        return MUSE_IMAGE_WRONG_MODEL;
    }
    return MUSE_IMAGE_OK;
}

const char *muse_image_verdict_text(muse_image_verdict_t v)
{
    switch (v) {
    case MUSE_IMAGE_OK: return "this board's";
    case MUSE_IMAGE_SHORT: return "too short to be an app image";
    case MUSE_IMAGE_NOT_APP: return "not an app image";
    case MUSE_IMAGE_WRONG_CHIP: return "built for another chip";
    case MUSE_IMAGE_NO_BOARD: return "no board description (an older build, or not this firmware)";
    case MUSE_IMAGE_WRONG_BOARD: return "built for another board";
    case MUSE_IMAGE_NO_BLE_UPDATE: return "it has no Bluetooth update (the BLE recovery tier)";
    case MUSE_IMAGE_WRONG_MODEL: return "built for another device model";
    default: return "unknown";
    }
}

int muse_image_describe(const muse_image_info_t *info, char *out, size_t cap)
{
    if (!info->has_board) {
        return snprintf(out, cap, "chip %u, no board description, version %s", (unsigned)info->chip_id,
                        info->version[0] ? info->version : "?");
    }
    return snprintf(out, cap, "chip %u, %s %s, version %s", (unsigned)info->chip_id,
                    info->product_code[0] ? info->product_code : "(no product code)", info->board_id,
                    info->version[0] ? info->version : "?");
}
