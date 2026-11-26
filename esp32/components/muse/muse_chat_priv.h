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

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Between the status side (muse_chat.c) and the session task (muse_chat_session.cpp),
 * plus voice note and caption helpers shared with muse_chat_link.c. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "muse_chat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Token saved (the VM ID is optional) or Link paired to a Muse account. */
bool muse_hatch_configured(void);
/* Session task -> status shown in settings and over BLE. */
void muse_hatch_report(muse_hatch_state_t state, const char *detail);
/* The VM the session connected to, for the settings page; empty forgets it. */
void muse_hatch_report_vm(const char *vm_id);

/* Asks the session task to (re)connect now. */
void muse_hatch_chat_connect(void);
/* Drops the connection and the cached VM credentials (settings changed). */
void muse_hatch_chat_forget(void);

/* Keep rejected IDs for the turn: later events may omit their parent.
 * On overflow, require a known parent for new messages. */
#define MUSE_CHAT_REJECTED_MAX 8
typedef struct {
    char ids[MUSE_CHAT_REJECTED_MAX][80];
    unsigned count;
    bool overflow;
} muse_chat_rejected_t;

static inline bool muse_chat_is_rejected(const muse_chat_rejected_t *rejected, const char *id)
{
    for (unsigned i = 0; i < rejected->count; i++) {
        if (!strcmp(rejected->ids[i], id)) return true;
    }
    return false;
}

static inline void muse_chat_reject(muse_chat_rejected_t *rejected, const char *id)
{
    if (!id[0] || muse_chat_is_rejected(rejected, id)) return;
    size_t len = strlen(id);
    if (rejected->count == MUSE_CHAT_REJECTED_MAX || len >= sizeof(rejected->ids[0])) {
        rejected->overflow = true;
        return;
    }
    memcpy(rejected->ids[rejected->count++], id, len + 1);
}

/*
 * Every POST /chat/stream body names this gadget's node at its top level
 * (the VM's chat API: a gadget's
 * chat turn carries device_id or node_id, and never chat_context, which the
 * VM refuses with 400). Provided by the app: Link's node id
 * ("gadget-xxxxxx"; a gadget set up by older firmware keeps "homelink-xxxxxx";
 * the one link.register sends), a RAM string, "" if
 * unknown. Called on the session task, whose stack may be in PSRAM: it must
 * not read flash.
 */
const char *muse_chat_node_id(void);

/* The longest node id a body carries; longer ones are cut. */
#define MUSE_CHAT_NODE_MAX 64
/* "node_id":"<id>", for a JSON body built as text: only a node id's own
 * characters (letters, digits, - _ . :) are kept, so nothing needs escaping.
 * Returns the length, 0 if it doesn't fit cap. */
static inline size_t muse_chat_node_field(char *out, size_t cap, const char *node)
{
    static const char head[] = "\"node_id\":\"";
    size_t o = sizeof(head) - 1;
    if (cap < o + MUSE_CHAT_NODE_MAX + 3) return 0;
    memcpy(out, head, o);
    size_t k = 0;
    for (const char *c = node ? node : ""; *c && k < MUSE_CHAT_NODE_MAX; c++) {
        char ch = *c;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '_' || ch == '.' || ch == ':') {
            out[o++] = ch;
            k++;
        }
    }
    out[o++] = '"';
    out[o++] = ',';
    out[o] = '\0';
    return o;
}

/* Identifies spoken turns (voice notes and dictation) as coming from a small
 * microcontroller voice device, so Muse answers in short plain sentences.
 * Typed turns leave it out. */
#define MUSE_CHAT_SPOKEN_DEVICE "mcu-wearable"

/* A voice note is a POST /chat/stream body: the head (muse_note_head:
 * {"node_id":..., then NOTE_FIELDS), a base64 WAV, NOTE_TAIL. */
#define MUSE_NOTE_FIELDS \
    "\"message\":\"\",\"output_modality\":\"text\",\"device\":\"" MUSE_CHAT_SPOKEN_DEVICE "\"," \
    "\"items\":[{\"type\":\"file\"," \
    "\"mime_type\":\"audio/wav\",\"filename\":\"voice_note.wav\",\"data_base64\":\""
#define MUSE_HATCH_NOTE_TAIL "\"}]}"
#define MUSE_NOTE_HEAD_MAX (1 + 16 + MUSE_CHAT_NODE_MAX + sizeof(MUSE_NOTE_FIELDS))

/* The note's head for node into out (cap >= MUSE_NOTE_HEAD_MAX); the
 * length, 0 if it doesn't fit. */
static inline size_t muse_note_head(char *out, size_t cap, const char *node)
{
    if (cap < MUSE_NOTE_HEAD_MAX) return 0;
    out[0] = '{';
    size_t n = muse_chat_node_field(out + 1, cap - 1, node);
    if (!n) return 0;
    n += 1;
    memcpy(out + n, MUSE_NOTE_FIELDS, sizeof(MUSE_NOTE_FIELDS));
    return n + sizeof(MUSE_NOTE_FIELDS) - 1;
}
#define MUSE_HATCH_WAV_HEADER 44

/* A voice note's photo (CONFIG_MUSE_PHOTO_NOTES) follows its audio as an
 * image item: the request ends PHOTO_HEAD, the base64 JPEG, PHOTO_TAIL in
 * place of NOTE_TAIL. */
#define MUSE_CHAT_PHOTO_HEAD \
    "\"},{\"type\":\"image\",\"mime_type\":\"image/jpeg\",\"filename\":\"photo.jpg\",\"data_base64\":\""
#define MUSE_CHAT_PHOTO_TAIL "\"}]}"
/* Whether `data` is `len` bytes of base64 for a JPEG (muse_chat_image.c). */
bool muse_chat_is_jpeg_base64(const char *data, size_t len);

/* Voice note helpers (muse_chat_text.c). The note's length isn't known until
 * the release, so the WAV header gives the streaming "unknown" size. */
void muse_hatch_wav_header(uint8_t h[MUSE_HATCH_WAV_HEADER], uint32_t rate);
/* Writes 4 characters per 3 bytes of `in`, padded; returns the length. */
size_t muse_hatch_base64(const uint8_t *in, size_t n, char *out);

/* Caption text (muse_chat_text.c): the last line or so of `src`, and the page
 * of wrapped lines holding byte `at` of `text` (false if there's no text). */
void muse_hatch_tail_words(const char *src, char *out, size_t cap);
bool muse_hatch_caption_at(const char *text, size_t at, char *out, size_t cap);
/* Manual pages, with the same word/UTF-8 wrapping as automatic captions.
 * Returns the total number of pages; page is clamped to that range. */
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER || CONFIG_MUSE_LATEST_REPLY
int muse_chat_caption_page(const char *text, int page, char *out, size_t cap);
#endif

#ifdef __cplusplus
}
#endif
