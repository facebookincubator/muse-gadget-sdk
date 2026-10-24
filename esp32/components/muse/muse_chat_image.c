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

/* Exact, allocation-free framing helpers for /chat/stream image attachments. */
#include "muse_chat_priv.h"

#include <stdio.h>
#include <string.h>

static bool escaped_len(const char *text, size_t *out)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        size_t add = (*p == '"' || *p == '\\' || *p == '\b' || *p == '\f' || *p == '\n'
                      || *p == '\r' || *p == '\t')
                         ? 2
                         : *p < 0x20 ? 6 : 1;
        if (n > MUSE_HATCH_CHAT_BODY_MAX || add > MUSE_HATCH_CHAT_BODY_MAX - n) {
            return false;
        }
        n += add;
        p++;
    }
    *out = n;
    return true;
}

bool muse_hatch_image_body_size(const char *caption, size_t jpeg_base64_len,
                                size_t *escaped_caption_len, size_t *body_len)
{
    size_t escaped;
    const size_t fixed = sizeof(MUSE_HATCH_IMAGE_HEAD_1) - 1 + sizeof(MUSE_HATCH_IMAGE_HEAD_2) - 1
                         + sizeof(MUSE_HATCH_IMAGE_HEAD_3) - 1 + sizeof(MUSE_HATCH_IMAGE_TAIL) - 1;
    if (!escaped_len(caption, &escaped) || fixed > MUSE_HATCH_CHAT_BODY_MAX
        || jpeg_base64_len > MUSE_HATCH_CHAT_BODY_MAX - fixed
        || escaped > (MUSE_HATCH_CHAT_BODY_MAX - fixed - jpeg_base64_len) / 2) {
        return false;
    }
    if (escaped_caption_len) {
        *escaped_caption_len = escaped;
    }
    if (body_len) {
        *body_len = fixed + jpeg_base64_len + 2 * escaped;
    }
    return true;
}

bool muse_hatch_json_escape(const char *text, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    char *o = out;
    if (!out || !cap) {
        return false;
    }
    while (*p) {
        const char *escape = NULL;
        switch (*p) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        size_t n = escape ? 2 : *p < 0x20 ? 6 : 1;
        if ((size_t)(o - out) >= cap || n >= cap - (size_t)(o - out)) {
            return false;
        }
        if (escape) {
            memcpy(o, escape, 2);
        } else if (*p < 0x20) {
            snprintf(o, 7, "\\u%04x", *p);
        } else {
            *o = (char)*p;
        }
        o += n;
        p++;
    }
    *o = '\0';
    return true;
}

bool muse_hatch_is_jpeg_base64(const char *data, size_t len)
{
    /* Every JPEG begins FF D8 FF, whose first four base64 bytes are /9j/. */
    if (!data || len < 4 || len % 4 || memcmp(data, "/9j/", 4)) {
        return false;
    }
    bool padding = false;
    for (size_t i = 4; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '=') {
            if (i < len - 2) {
                return false;
            }
            padding = true;
        } else if (padding || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                                || (c >= '0' && c <= '9') || c == '+' || c == '/')) {
            return false;
        }
    }
    return true;
}
