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

/* Between the status side (muse_chat.c) and the session task (muse_chat_session.cpp),
 * plus voice note and caption helpers shared with muse_chat_link.c. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "muse_chat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Token saved (the VM ID is optional) or Link paired to a Hatch account. */
bool muse_hatch_configured(void);
/* Session task -> status shown in settings and over BLE. */
void muse_hatch_report(muse_hatch_state_t state, const char *detail);
/* The VM the session connected to, for the settings page; empty forgets it. */
void muse_hatch_report_vm(const char *vm_id);

/* Asks the session task to (re)connect now. */
void muse_hatch_chat_connect(void);
/* Drops the connection and the cached VM credentials (settings changed). */
void muse_hatch_chat_forget(void);

/* True only for a backend whose current turn needs the voice task to keep
 * calling muse_hatch_turn_event(). The full session has its own task. */
bool muse_chat_needs_voice_poll(void);

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

/* A voice note is a POST /chat/stream body: NOTE_HEAD, a base64 WAV, NOTE_TAIL. */
#define MUSE_HATCH_NOTE_HEAD \
    "{\"message\":\"\",\"output_modality\":\"text\",\"items\":[{\"type\":\"file\"," \
    "\"mime_type\":\"audio/wav\",\"filename\":\"voice_note.wav\",\"data_base64\":\""
#define MUSE_HATCH_NOTE_TAIL "\"}]}"
#define MUSE_HATCH_WAV_HEADER 44

/* /chat/stream JPEG body: the caption in `message` and as the first item (Muse
 * reads only the items when there are any), then an image item. */
#define MUSE_CHAT_IMAGE_HEAD_1 "{\"message\":\""
#define MUSE_CHAT_IMAGE_HEAD_2 "\",\"output_modality\":\"text\",\"items\":[{\"type\":\"text\",\"text\":\""
#define MUSE_CHAT_IMAGE_HEAD_3 \
    "\"},{\"type\":\"image\",\"mime_type\":\"image/jpeg\",\"filename\":\"camera.jpg\",\"data_base64\":\""
#define MUSE_CHAT_IMAGE_TAIL "\"}]}"
#define MUSE_CHAT_IMAGE_BODY_MAX (256u * 1024u)

/* Exact request sizing/escaping shared by both transports (muse_chat_image.c).
 * UTF-8 is kept as UTF-8; JSON syntax and control bytes are escaped. */
bool muse_chat_image_body_size(const char *caption, size_t jpeg_base64_len,
                               size_t *escaped_caption_len, size_t *body_len);
bool muse_chat_json_escape(const char *text, char *out, size_t cap);
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

#ifdef __cplusplus
}
#endif
