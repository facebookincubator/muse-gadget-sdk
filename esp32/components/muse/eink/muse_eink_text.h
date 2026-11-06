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
#include <stddef.h>
#include <string.h>

namespace muse_eink {

/* Invalid/truncated sequences are single bytes; never read beyond their NUL. */
inline size_t utf8_scalar_bytes(const char *s)
{
    unsigned char c = (unsigned char)s[0];
    if (!c) return 0;
    if (c < 0x80) return 1;
    size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 1;
    if (n == 1) return 1;
    for (size_t i = 1; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) return 1;
    }
    unsigned char second = (unsigned char)s[1];
    if ((c == 0xE0 && second < 0xA0) || (c == 0xED && second >= 0xA0) ||
        (c == 0xF0 && second < 0x90) || (c == 0xF4 && second >= 0x90)) return 1;
    return n;
}

/* Non-expanding presentation repair; leave stored SSID/credential bytes alone. */
inline void sanitize_utf8(char *text)
{
    while (*text) {
        size_t n = utf8_scalar_bytes(text);
        if (n == 1 && (unsigned char)*text >= 0x80) *text = '?';
        text += n;
    }
}

/* A byte-limited prefix, ending between valid codepoints. */
inline size_t utf8_prefix_bytes(const char *text, size_t limit)
{
    size_t n = 0;
    while (text[n] && n < limit) {
        size_t step = utf8_scalar_bytes(text + n);
        if (step > limit - n) break;
        n += step;
    }
    return n;
}

inline size_t utf8_pop_bytes(const char *text, size_t n)
{
    if (n) do n--; while (n && ((unsigned char)text[n] & 0xC0) == 0x80);
    return n;
}

}  // namespace muse_eink
