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

#include "muse_review.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "muse_chat_priv.h"
#include "muse_text.h"

#define REVIEW_CAP (32 * 1024)

static SemaphoreHandle_t s_lock;
static char *s_text;
static uint32_t s_turn, s_next_turn, s_version;
static bool s_have_turn, s_active;
static int s_page, s_pages;   /* s_pages: 0 until the text is paged */

esp_err_t muse_review_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

uint32_t muse_review_turn(void)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t turn = ++s_next_turn;
    xSemaphoreGive(s_lock);
    return turn;
}

void muse_review_store(uint32_t turn, const char *text)
{
    if (!s_lock || !text || !text[0]) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_text) s_text = heap_caps_malloc(REVIEW_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_text) {
        bool fresh = !s_have_turn || turn != s_turn;
        s_version++;
        if (fresh) {
            s_text[0] = '\0';
            s_turn = turn;
            s_have_turn = true;
            s_active = false;
            s_page = 0;
        } else if (s_text[0]) {
            strlcat(s_text, "\n\n", REVIEW_CAP);
        }
        strlcat(s_text, text, REVIEW_CAP);
        s_pages = 0;   /* steps before the next paging aren't clamped to the old count */
        /* Truncation must not leave half a UTF-8 character at the end. */
        size_t len = strlen(s_text);
        if (len) {
            size_t at = len - 1;
            while (at && ((unsigned char)s_text[at] & 0xc0) == 0x80) at--;
            unsigned char lead = (unsigned char)s_text[at];
            size_t need = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            if (len - at < need) s_text[at] = '\0';
        }
    }
    xSemaphoreGive(s_lock);
}

bool muse_review_step(int direction)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool have = s_text && s_text[0];
    if (have) {
        s_version++;
        if (!s_active) {
            s_active = true;
            s_page = 0;
        } else {
            s_page += direction;
            if (s_page < 0) s_page = 0;
            if (s_pages && s_page >= s_pages) s_page = s_pages - 1;
        }
    }
    xSemaphoreGive(s_lock);
    return have;
}

void muse_review_close(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) s_version++;
    s_active = false;
    xSemaphoreGive(s_lock);
}

bool muse_review_active(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_active;
    xSemaphoreGive(s_lock);
    return active;
}

bool muse_review_page(char *out, size_t cap, int *page, int *pages)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_active && s_text && s_text[0];
    if (active) {
        s_pages = muse_chat_caption_page(s_text, s_page, out, cap);
        if (s_pages) muse_text_to_ascii(out, cap);
        if (!s_pages) {
            s_active = active = false;
            s_version++;
        } else {
            if (s_page >= s_pages) s_page = s_pages - 1;
            *page = s_page + 1;
            *pages = s_pages;
        }
    }
    xSemaphoreGive(s_lock);
    return active;
}

bool muse_review_frame(muse_review_view_t *view, char *out, size_t cap, bool *changed)
{
    *changed = false;
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_active && s_text && s_text[0];
    uint32_t version = s_version;
    xSemaphoreGive(s_lock);
    if (active && (!view->shown || view->stale || view->version != version)) {
        /* Another store or step in between moves the version again: the next frame catches it. */
        view->version = version;
        view->stale = false;
        active = muse_review_page(out, cap, &view->page, &view->pages);
        *changed = active;
    }
    if (active != view->shown) {
        view->shown = active;
        *changed = true;
    }
    return active;
}
