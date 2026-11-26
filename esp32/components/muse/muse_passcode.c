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

#include "muse_passcode.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "muse_pass";

static muse_pass_t s_p;
static SemaphoreHandle_t s_mu;
static atomic_bool s_on, s_locked, s_supported;
static atomic_int s_keypad_req;
static bool s_inited;

static int64_t s_store_failed_ms = -1;   /* the last failed store (-1: none), retried every few seconds */

#define STORE_RETRY_MS 5000

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void take(void)
{
    if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY);
}

/* Stores a changed record; under the mutex. A failed store stays dirty. */
static bool store(void)
{
    if (!s_p.dirty) return true;
    if (!muse_passcode_platform_save(&s_p.rec)) {
        if (s_store_failed_ms < 0) ESP_LOGE(TAG, "the passcode record couldn't be stored: retrying");
        s_store_failed_ms = now_ms();
        return false;
    }
    if (s_store_failed_ms >= 0) ESP_LOGI(TAG, "the passcode record is stored");
    s_store_failed_ms = -1;
    s_p.dirty = false;
    return true;
}

/* Publishes the flags and stores a changed record; under the mutex. */
static void give(void)
{
    bool was = atomic_load(&s_locked);
    atomic_store(&s_on, s_p.rec.on != 0);
    atomic_store(&s_locked, s_p.locked && atomic_load(&s_supported));
    if (was != atomic_load(&s_locked)) {
        ESP_LOGI(TAG, "%s", s_p.locked ? "locked" : "unlocked");
        muse_passcode_platform_locked(atomic_load(&s_locked));   /* video and UI streams held while locked */
    }
    if (s_p.dirty && (s_store_failed_ms < 0 || now_ms() - s_store_failed_ms >= STORE_RETRY_MS)) {
        store();
    }
    if (s_mu) xSemaphoreGive(s_mu);
}

void muse_passcode_init(void)
{
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    muse_pass_rec_t rec;
    bool have = muse_passcode_platform_load(&rec);
    take();
    muse_pass_init(&s_p, have ? &rec : NULL, now_ms());
    s_inited = true;
    memset(&rec, 0, sizeof(rec));
    ESP_LOGI(TAG, "passcode %s", s_p.rec.on ? "on: locked at boot" : "off");
    give();
}

void muse_passcode_set_supported(bool supported)
{
    atomic_store(&s_supported, supported);
    take();
    give();
}

bool muse_passcode_supported(void) { return atomic_load(&s_supported); }
bool muse_passcode_on(void) { return atomic_load(&s_on); }
bool muse_passcode_locked(void) { return atomic_load(&s_locked); }

void muse_passcode_request_keypad(muse_pass_why_t why)
{
    atomic_store(&s_keypad_req, (int)why);
}

int muse_passcode_take_keypad_request(void)
{
    return atomic_exchange(&s_keypad_req, 0);
}

void muse_passcode_screen_off(bool busy)
{
    take();
    muse_pass_screen_off(&s_p, now_ms(), busy);
    give();
}

void muse_passcode_screen_on(void)
{
    take();
    muse_pass_screen_on(&s_p, now_ms());
    give();
}

void muse_passcode_tick(uint32_t idle_ms, bool stays_on, bool busy)
{
    take();
    if (muse_pass_tick(&s_p, now_ms(), idle_ms, stays_on, busy)) {
        ESP_LOGI(TAG, "5 min without input: locking");
    }
    give();
}

/* The write shape: a wrong code's count is stored
 * (give()) before WRONG or WAIT is returned to the keypad; a right code
 * writes nothing unless it resets a count. */
muse_pass_result_t muse_passcode_check(const char digits[MUSE_PASS_DIGITS])
{
    take();
    muse_pass_result_t r = muse_pass_check(&s_p, digits, now_ms(), muse_passcode_platform_sha256);
    if (r == MUSE_PASS_WRONG || r == MUSE_PASS_WAIT) {
        ESP_LOGI(TAG, "wrong passcode (%u in a row)%s", (unsigned)s_p.rec.tries, r == MUSE_PASS_WAIT ? ": waiting" : "");
        store();   /* tried now, even within a failed store's retry delay */
    }
    give();
    return r;
}

int64_t muse_passcode_wait_left_ms(void)
{
    take();
    int64_t left = muse_pass_wait_left_ms(&s_p, now_ms());
    give();
    return left;
}

bool muse_passcode_set(const char digits[MUSE_PASS_DIGITS])
{
    uint8_t salt[MUSE_PASS_SALT_LEN];
    if (!muse_passcode_platform_salt(salt)) {
        ESP_LOGE(TAG, "no salt (no radio on): the passcode wasn't set");
        return false;
    }
    take();
    muse_pass_t before = s_p;
    bool ok = muse_pass_set(&s_p, digits, salt, muse_passcode_platform_sha256);
    bool stored = ok && store();
    if (ok && !stored) {
        s_p = before;   /* never "on" or a new code in RAM that a restart wouldn't keep */
    }
    ESP_LOGI(TAG, "%s", stored ? "passcode set" : ok ? "the passcode couldn't be stored: not set"
                                                       : "the passcode couldn't be hashed: not set");
    give();
    memset(&before, 0, sizeof(before));
    memset(salt, 0, sizeof(salt));
    return stored;
}

void muse_passcode_turn_off(void)
{
    take();
    muse_pass_turn_off(&s_p);
    ESP_LOGI(TAG, "passcode off");
    give();
}

muse_pass_lock_t muse_passcode_lock_mode(void)
{
    take();
    muse_pass_lock_t m = (muse_pass_lock_t)s_p.rec.lock;
    give();
    return m;
}

void muse_passcode_set_lock_mode(muse_pass_lock_t mode)
{
    take();
    muse_pass_set_lock(&s_p, mode);
    give();
}

void muse_passcode_lock_now(void)
{
    take();
    muse_pass_lock_now(&s_p);
    give();
}

void muse_passcode_forget(const char *why)
{
    take();
    /* Before init (SAFE mode never starts Muse) the store is erased anyway. */
    bool had = !s_inited || s_p.rec.on || s_p.rec.tries || s_p.rec.lock;
    muse_pass_forget(&s_p);
    if (!had) {
        s_p.dirty = false;   /* nothing stored to erase */
    }
    ESP_LOGI(TAG, "passcode erased with the pairing (%s)", why ? why : "reset");
    give();
}

bool muse_passcode_store_clean(void)
{
    take();
    bool clean = !s_p.dirty;   /* a failed store stays dirty (store()) */
    give();
    return clean;
}

void muse_passcode_clear(void)
{
    take();
    muse_pass_turn_off(&s_p);   /* the code, the count and the wait; Lock stays */
    ESP_LOGW(TAG, "passcode cleared from the console");
    give();
}

void muse_passcode_status(char *out, size_t n)
{
    if (!atomic_load(&s_supported)) {
        snprintf(out, n, "off (no lock screen on this board)");
        return;
    }
    take();
    muse_pass_status(&s_p, now_ms(), out, n);
    give();
}
