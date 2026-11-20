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

/* Checks a voice note's photo before it goes to Muse (muse_chat_attach_jpeg). */
#include "muse_chat_priv.h"

#include <string.h>

bool muse_chat_is_jpeg_base64(const char *data, size_t len)
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
