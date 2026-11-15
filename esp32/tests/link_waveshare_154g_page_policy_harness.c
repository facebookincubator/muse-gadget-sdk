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

// Host harness for the 1.54G's reply page against display.draw_url: the
// page task draws nothing while an image downloads but keeps the request for
// when it ends, holds the download slot while it draws, and leaves an image
// drawn this turn up until the next turn. A PWR tap during a download is
// dropped, and the first tap after a kept image shows the page it covered.
// The runner extracts the production rendering and page task code into
// page_render.inc and page_task.inc; FreeRTOS, the display, the download
// slot and Muse's captions are fakes here.
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "image_fetch.h"
#include "led_status.h"
#include "muse_chat.h"
#include "muse_text.h"
#include "pixel_font.h"

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
static int xSemaphoreTake(SemaphoreHandle_t lock, int ticks) {
    (void)lock, (void)ticks;
    return 1;
}
static void xSemaphoreGive(SemaphoreHandle_t lock) { (void)lock; }
static void xTaskNotifyGive(TaskHandle_t task) { (void)task; }
static unsigned ulTaskNotifyTake(int clear, int ticks) {
    (void)clear, (void)ticks;
    return 1;
}
#define VOICE_MIC_RATE 16000

// ---- Fakes -------------------------------------------------------------------

static bool s_fetching;        // a download holds the slot
static bool s_claimed;         // the page task holds it
static unsigned s_image_bands; // bands downloads have drawn
static int s_page_bands, s_draw_done, s_animation;
static const char *s_reply;    // Muse's reply text, NULL for none yet
static const char *s_reply_more; // its second page, from a tenth of a second in

bool image_fetch_claim(void) {
    if (s_fetching || s_claimed) return false;
    s_claimed = true;
    return true;
}

void image_fetch_release(void) {
    assert(s_claimed);
    s_claimed = false;
}

unsigned image_fetch_drawn(void) { return s_image_bands; }

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    (void)x, (void)y, (void)w, (void)h, (void)pixels;
    // No download can start halfway through the page.
    assert(s_claimed && !s_fetching);
    s_page_bands++;
    return true;
}

void led_status_draw_done(void) { s_draw_done++; }

void led_status_show_animation(void) {
    assert(s_claimed && !s_fetching);
    s_animation++;
}

bool muse_hatch_turn_caption(size_t played, char *out, size_t cap) {
    if (!s_reply) return false;
    bool more = s_reply_more && played >= VOICE_MIC_RATE / 10;
    snprintf(out, cap, "%s", more ? s_reply_more : s_reply);
    return true;
}

void muse_text_to_ascii(char *s, size_t cap) { (void)s, (void)cap; }

#include "page_render.inc"
#include "page_task.inc"

// ---- Steps -------------------------------------------------------------------

static char s_cache_store[CACHE_PAGES * PAGE_BYTES];
static uint8_t s_strip_store[PAGE_W * CELL_H * 2];

static void reset_counts(void) { s_page_bands = s_draw_done = s_animation = 0; }

// voice.c's states for a voice note whose reply is `reply`, up to Muse
// speaking it; the page task runs after each.
static void turn_until_speaking(const char *reply) {
    s_reply = s_reply_more = NULL;
    page_follow_voice(LED_VOICE_LISTENING);
    (void)page_serve();
    page_follow_voice(LED_VOICE_TRANSCRIBING);
    page_follow_voice(LED_VOICE_THINKING);
    s_reply = reply;
}

static void speak_and_end(void) {
    page_follow_voice(LED_VOICE_SPEAKING);
    (void)page_serve();
    page_follow_voice(LED_VOICE_IDLE);
    (void)page_serve();
}

// A download that draws a whole image and finishes.
static void image_drawn(void) {
    s_fetching = true;
    s_image_bands += 13;
    s_fetching = false;
}

int main(void) {
    s_cache = s_cache_store;
    s_strip = s_strip_store;
    static int page_task_handle;
    s_page_task = &page_task_handle;
    (void)page_task;

    // A reply with no image: its page goes up, and the slot is free after.
    turn_until_speaking("Sure, here it is");
    reset_counts();
    speak_and_end();
    assert(s_page_bands > 0 && s_draw_done == 1);
    assert(!s_claimed);

    // The next turn takes the page down for the status screen.
    reset_counts();
    turn_until_speaking("Second reply");
    assert(s_animation == 1 && s_page_bands == 0);

    // A reply that arrives while an image downloads draws nothing: not a row
    // into the image, and no refresh. The request waits for the download.
    s_fetching = true;
    reset_counts();
    speak_and_end();
    assert(s_page_bands == 0 && s_draw_done == 0);
    assert(page_serve());
    // The image that download draws stays up for the rest of the turn.
    s_image_bands += 13;
    s_fetching = false;
    assert(!page_serve());
    assert(s_page_bands == 0 && s_draw_done == 0);

    // A download that fails before drawing anything: once it ends, the
    // reply's page goes up after all.
    reset_counts();
    turn_until_speaking("Here is the map");
    s_fetching = true;
    speak_and_end();
    assert(s_page_bands == 0 && s_draw_done == 0);
    s_fetching = false;
    assert(!page_serve());
    assert(s_page_bands > 0 && s_draw_done == 1);
    assert(!page_serve());  // and only once
    assert(s_draw_done == 1);

    // An image Muse drew earlier in this turn, before the reply: kept.
    reset_counts();
    turn_until_speaking("Here is the radar map");
    image_drawn();
    speak_and_end();
    assert(s_page_bands == 0 && s_draw_done == 0);

    // Only until the next turn: that one goes back to the status screen,
    // and its reply's page goes up.
    reset_counts();
    turn_until_speaking("Anything else?");
    assert(s_animation == 1);
    speak_and_end();
    assert(s_page_bands > 0 && s_draw_done == 1);

    // A new turn that begins while an image downloads leaves the screen to
    // it, status screen included, and the image it draws stays up.
    reset_counts();
    s_fetching = true;
    turn_until_speaking("Third reply");
    assert(s_animation == 0 && s_page_bands == 0);
    s_image_bands += 13;
    s_fetching = false;
    assert(!page_serve());
    assert(s_animation == 0 && s_page_bands == 0);

    // A two-page reply, its first page up.
    reset_counts();
    turn_until_speaking("Here is the radar map");
    s_reply_more = "and the forecast";
    speak_and_end();
    assert(s_cache_pages == 2 && s_cache_shown == 0 && s_draw_done == 1);

    // A PWR tap while an image downloads draws nothing, and nothing waits
    // for the download: the image would cover the page the tap asks for.
    reset_counts();
    s_fetching = true;
    page_tap();
    assert(!page_serve());
    assert(s_page_bands == 0 && s_draw_done == 0 && s_cache_shown == 0);
    s_image_bands += 13;
    s_fetching = false;
    assert(!page_serve());
    assert(s_page_bands == 0 && s_draw_done == 0);

    // The first tap after the image shows the page it covered, not the next.
    page_tap();
    assert(!page_serve());
    assert(s_page_bands > 0 && s_draw_done == 1 && s_cache_shown == 0);
    assert(!s_claimed);

    // The next taps turn the page, round to the first after the last.
    page_tap();
    assert(!page_serve());
    assert(s_draw_done == 2 && s_cache_shown == 1);
    page_tap();
    assert(!page_serve());
    assert(s_draw_done == 3 && s_cache_shown == 0);

    // A one-page reply has nothing to turn.
    reset_counts();
    turn_until_speaking("Done");
    speak_and_end();
    reset_counts();
    page_tap();
    assert(!page_serve());
    assert(s_page_bands == 0 && s_draw_done == 0);

    puts("PASS waveshare 1.54G page vs display.draw_url: no drawing during a download, "
         "the request kept for when it ends, slot held while drawing, "
         "a same-turn image kept until the next turn, a tap during a download "
         "dropped, the first tap after an image shows the page it covered");
    return 0;
}
