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
 * The passcode's platform on the gadget (muse_passcode.h): the record as one
 * blob in Muse's settings namespace (kept across reboots and updates; the
 * default record is no key at all), SHA-256 from PSA Crypto, and the salt
 * from secure_random() (refused without a radio, never a weaker source).
 * Opens NVS itself, so the pairing's erase works before Muse starts (SAFE
 * mode, a reset from Link).
 */

#include <string.h>

#include "esp_log.h"
#include "muse_passcode.h"
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif
#include "nvs.h"
#include "psa/crypto.h"
#include "secure_random.h"

#define NS "muse"          /* muse_settings.c's namespace */
#define KEY "passcode"

static const char *TAG = "muse_pass";

/* Only the store saying there's no record (no namespace yet, no key) is "no passcode". Any other error, or a blob of
 * no known size or format, is a record that couldn't be read: the passcode starts locked, never off because of an
 * error. The pairing's erase (Reset pairing, Forgot passcode's way out) erases the key whatever it holds. */
muse_pass_load_t muse_passcode_platform_load(muse_pass_rec_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t n = sizeof(*rec);
        err = nvs_get_blob(h, KEY, rec, &n);
        nvs_close(h);
        /* Format 1 (an earlier layout) is one byte shorter: the core migrates it. */
        bool known = (n == sizeof(*rec) && (rec->format == MUSE_PASS_FORMAT || rec->format == 1)) ||
                     (n == MUSE_PASS_FORMAT_1_SIZE && rec->format == 1);
        if (err == ESP_OK && known) {
            return MUSE_PASS_LOAD_OK;
        }
        if (err == ESP_OK) {
            ESP_LOGE(TAG, "load: a record of no known size or format (%u bytes): locked", (unsigned)n);
            memset(rec, 0, sizeof(*rec));
            return MUSE_PASS_LOAD_UNREADABLE;
        }
    }
    memset(rec, 0, sizeof(*rec));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return MUSE_PASS_LOAD_NONE;   /* no namespace yet, or no record: no passcode */
    }
    ESP_LOGE(TAG, "load: %s: locked", esp_err_to_name(err));
    return MUSE_PASS_LOAD_UNREADABLE;
}

static bool is_default(const muse_pass_rec_t *rec)
{
    muse_pass_rec_t d = { .format = MUSE_PASS_FORMAT, .lock = MUSE_PASS_LOCK_SCREEN_OFF };
    return memcmp(rec, &d, sizeof(d)) == 0;
}

bool muse_passcode_platform_save(const muse_pass_rec_t *rec)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err;
    if (is_default(rec)) {
        err = nvs_erase_key(h, KEY);   /* off and nothing counted: no record */
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_blob(h, KEY, rec, sizeof(*rec));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "store: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool muse_passcode_platform_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    size_t got = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &got) != PSA_SUCCESS || got != 32) {
        ESP_LOGE(TAG, "SHA-256 failed");   /* nothing is set, and no code is right */
        return false;
    }
    return true;
}

bool muse_passcode_platform_salt(uint8_t salt[MUSE_PASS_SALT_LEN])
{
    return secure_random(salt, MUSE_PASS_SALT_LEN);
}

void muse_passcode_platform_locked(bool locked)
{
#if CONFIG_MUSE_VIDEO
    muse_video_set_hold(locked);   /* a UI stream or camera view waits for the unlock */
#else
    (void)locked;
#endif
}
