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

/*
 * Which board an app image is for, readable from its first bytes.
 *
 * Every image carries a muse_board_desc_t in ESP-IDF's custom app description
 * section (`.rodata_custom_desc`, "Adding a Custom Structure to an
 * Application" in ESP-IDF's App Image Format): the linker puts it right after
 * esp_app_desc_t, so in the .bin it sits at a fixed offset, 288 bytes from
 * the start (the 24-byte image header, the first segment's 8-byte header,
 * the 256-byte esp_app_desc_t). An updater holding the first
 * MUSE_IMAGE_HEAD_BYTES of a download can tell the chip (the image header's
 * chip_id), the version, the board (product code and board profile) and the
 * device model before it writes a byte to flash: muse_image_check().
 *
 * Images built before the description existed carry other bytes there and
 * read as "no board description".
 *
 * The device model (CONFIG_MUSE_DEVICE_MODEL_ID) came later, in bytes the
 * first layout left reserved (zeros) and that firmware before it never reads:
 * an image with it still passes an older updater's check, and an updater
 * compares models only when both its own build and the image carry one
 * (MUSE_BOARD_DESC_F_MODEL). It tells apart builds the product code and the
 * board profile don't, such as the Home Link boards of one chip (no product
 * code, board "none").
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_BOARD_DESC_MAGIC 0x4442474Du   /* "MGBD" */
#define MUSE_BOARD_DESC_LAYOUT 1

#define MUSE_BOARD_DESC_F_GADGET 0x01u    /* built with musegadgets BLE audio */
#define MUSE_BOARD_DESC_F_BLE_SMP 0x02u   /* takes updates over BLE (MCUmgr SMP, the BLE recovery tier) */
#define MUSE_BOARD_DESC_F_MODEL 0x04u     /* model_hash holds this build's device model */

typedef struct {
    uint32_t magic;          /* MUSE_BOARD_DESC_MAGIC */
    uint8_t layout;          /* MUSE_BOARD_DESC_LAYOUT */
    uint8_t flags;           /* MUSE_BOARD_DESC_F_* */
    uint16_t chip_id;        /* CONFIG_IDF_FIRMWARE_CHIP_ID */
    char product_code[16];   /* CONFIG_HOMEHUB_BLE_PRODUCT_CODE, NUL-padded ("" without one) */
    char board_id[32];       /* CONFIG_MUSE_BOARD_ID, NUL-padded ("none" on Link-only builds) */
    uint8_t model_hash[8];   /* with MUSE_BOARD_DESC_F_MODEL: SHA-256(CONFIG_MUSE_DEVICE_MODEL_ID)'s first 8 bytes;
                                zeros in images from before it (the first layout's reserved bytes) */
} muse_board_desc_t;

#ifndef __cplusplus
_Static_assert(sizeof(muse_board_desc_t) == 64, "muse_board_desc_t is 64 bytes in every image");
_Static_assert(offsetof(muse_board_desc_t, model_hash) == 56, "the first layout's reserved bytes");
#endif

/* Offsets in the .bin (ESP-IDF's layout; muse_image_id.c checks them against
 * ESP-IDF's types on the target). */
#define MUSE_IMAGE_CHIP_ID_OFFSET 12u            /* esp_image_header_t.chip_id */
#define MUSE_IMAGE_APP_DESC_OFFSET 32u           /* esp_app_desc_t */
#define MUSE_IMAGE_BOARD_DESC_OFFSET (32u + 256u)
#define MUSE_IMAGE_HEAD_BYTES (MUSE_IMAGE_BOARD_DESC_OFFSET + 64u)   /* what muse_image_check() reads */

/* This build's own description (in .rodata_custom_desc). */
extern const muse_board_desc_t muse_board_desc;

typedef enum {
    MUSE_IMAGE_OK = 0,
    MUSE_IMAGE_SHORT,           /* fewer than MUSE_IMAGE_HEAD_BYTES */
    MUSE_IMAGE_NOT_APP,         /* no image magic or no app description */
    MUSE_IMAGE_WRONG_CHIP,      /* built for another chip */
    MUSE_IMAGE_NO_BOARD,        /* no board description (an older build, or not this firmware) */
    MUSE_IMAGE_WRONG_BOARD,     /* another product code or board profile */
    MUSE_IMAGE_NO_BLE_UPDATE,   /* this build takes updates over BLE and the image doesn't */
    MUSE_IMAGE_WRONG_MODEL,     /* another device model (both carry one) */
} muse_image_verdict_t;

/* What the image says about itself, printable (anything else becomes '?'). */
typedef struct {
    uint16_t chip_id;
    bool has_board;
    uint8_t flags;
    char version[33];
    char product_code[17];
    char board_id[33];
} muse_image_info_t;

/* Checks the first `n` bytes of an app image (n >= MUSE_IMAGE_HEAD_BYTES)
 * against this build: the chip always; with `board`, also the board
 * description: the same product code and board profile, the same device
 * model when both carry one, and BLE updates if this build takes them. Fills
 * `info` (may be NULL) as far as it got. */
muse_image_verdict_t muse_image_check(const uint8_t *head, size_t n, bool board, muse_image_info_t *info);

/* Checks the first `n` bytes against this build where the board profile isn't
 * compared (a Home Link build's Wi-Fi OTA): the chip, then the device model when
 * both carry one. MUSE_IMAGE_NO_BOARD for an image without a board
 * description (its caller decides what that means). */
muse_image_verdict_t muse_image_check_model(const uint8_t *head, size_t n, muse_image_info_t *info);

/* A short phrase for logs ("built for another chip"). */
const char *muse_image_verdict_text(muse_image_verdict_t v);

/* "chip 9, W175 waveshare_s3_175c, version 1.4.2" (or "no board description")
 * for logs; returns snprintf's count. */
int muse_image_describe(const muse_image_info_t *info, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
