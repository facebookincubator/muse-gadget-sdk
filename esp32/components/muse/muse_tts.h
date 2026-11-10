/*
 * Copyright (c) 2026 ledienbien-ai
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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Speech for Muse's replies. Muse answers gadgets in text, so the words go to
 * a text-to-speech service and come back as MP3, which the chat session
 * decodes and plays (start_tts in muse_chat_session.cpp).
 *
 * The service is the endpoint behind Google Translate's "listen" button. It
 * needs no key, but it isn't a published API: Google can change or block it,
 * and it takes about 200 characters a request. A reply is cut into pieces of
 * that size and fetched one after another. The reply's text goes to Google.
 */

/* The most characters sent in one request, and the most bytes they come to
 * once percent-encoded: a long URL is refused sooner than a long text. */
#define MUSE_TTS_CHUNK_CHARS 180
#define MUSE_TTS_CHUNK_ENCODED 1400
/* Room for one request's URL. */
#define MUSE_TTS_URL_MAX (96 + MUSE_TTS_CHUNK_ENCODED)

/* ---- Text (muse_tts_text.c; no hardware, tested on the host) ---- */

/*
 * `text` as it should be read out: Markdown marks, link addresses, emoji and
 * line breaks go, curly quotes and dashes become plain ones; letters keep
 * their accents. Up to cap - 1 bytes into out;
 * returns the length.
 */
size_t muse_tts_clean(const char *text, char *out, size_t cap);

/*
 * The next piece of *text, at most MUSE_TTS_CHUNK_CHARS characters, ending at
 * a sentence or, failing that, a word. Moves *text past it. False at the end.
 */
bool muse_tts_next_chunk(const char **text, char *out, size_t cap);

/* True if `text` has letters only Vietnamese writes (a, o, u with a horn or a
 * breve, d with a stroke, any tone mark from U+1EA0 on). */
bool muse_tts_is_vietnamese(const char *text);

/* The request for `chunk` spoken in `lang` ("vi", "en"); returns its length,
 * or 0 if it doesn't fit in cap bytes. */
size_t muse_tts_url(const char *chunk, const char *lang, char *out, size_t cap);

/* ---- Fetching (muse_tts.c) ---- */

typedef enum {
    MUSE_TTS_FETCHING,   /* more MP3 may come */
    MUSE_TTS_DONE,       /* all of it has been read */
    MUSE_TTS_FAILED,     /* nothing came: show the text instead */
} muse_tts_state_t;

/* Starts the task that fetches; false if it couldn't. Call once. */
bool muse_tts_start(void);

/*
 * Asks for `text` in `lang`, dropping any speech still being fetched.
 * Returns a ticket for muse_tts_read() and muse_tts_state(); 0 if muse_tts
 * isn't running or the text has nothing to say.
 */
uint32_t muse_tts_speak(const char *text, const char *lang);

/* Drops the speech being fetched. */
void muse_tts_cancel(void);

/* Up to cap bytes of the ticket's MP3 that have arrived; doesn't wait. */
size_t muse_tts_read(uint32_t ticket, uint8_t *buf, size_t cap);

/* Where the ticket's speech stands, once what has arrived has been read. */
muse_tts_state_t muse_tts_state(uint32_t ticket);

#ifdef __cplusplus
}
#endif
