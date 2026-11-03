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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Watcher reply review: a bounded RAM copy survives the chat turn ending. */
esp_err_t muse_review_init(void);
/* A new identity for each chat turn, typed or spoken. Replies stored under
 * one identity are joined; the next identity's first reply replaces them. */
uint32_t muse_review_turn(void);
void muse_review_store(uint32_t turn, const char *text);
bool muse_review_step(int direction);
void muse_review_close(void);
bool muse_review_active(void);
/* Copies the selected page and returns its one-based position and total. */
bool muse_review_page(char *out, size_t cap, int *page, int *pages);

/* The screen's copy of the review page, apart from the live caption. */
typedef struct {
    uint32_t version;
    bool shown;
    bool stale;   /* set when the page layout changes: the page is wrapped again */
    int page, pages;
} muse_review_view_t;

/* While reviewing, true, with the page in out. *changed says whether the
 * label needs redrawing: a new page, or review just ended and the live
 * caption has to come back. out is only written when the page changes. */
bool muse_review_frame(muse_review_view_t *view, char *out, size_t cap, bool *changed);

#ifdef __cplusplus
}
#endif
