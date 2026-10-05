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

#ifndef MUSE_TEST_HOST_COMPAT_H
#define MUSE_TEST_HOST_COMPAT_H

#include <stddef.h>
#include <string.h>

/* ESP-IDF provides these, but some host C libraries do not. */
#ifndef strlcpy
static inline size_t muse_test_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t len = strlen(src);
    if (cap) {
        size_t count = len < cap - 1 ? len : cap - 1;
        memcpy(dst, src, count);
        dst[count] = '\0';
    }
    return len;
}

#define strlcpy muse_test_strlcpy
#endif

#ifndef strlcat
static inline size_t muse_test_strlcat(char *dst, const char *src, size_t cap)
{
    size_t dst_len = 0;
    while (dst_len < cap && dst[dst_len]) {
        dst_len++;
    }

    size_t src_len = strlen(src);
    if (dst_len == cap) {
        return cap + src_len;
    }

    size_t available = cap - dst_len - 1;
    size_t count = src_len < available ? src_len : available;
    memcpy(dst + dst_len, src, count);
    dst[dst_len + count] = '\0';
    return dst_len + src_len;
}

#define strlcat muse_test_strlcat
#endif

/* Production UI code looks its strings up in the Chinese table
 * (components/muse/i18n.h). Host harnesses compile that code without LVGL, so
 * they get the English key back: the tests pin the untranslated text. */
static inline const char *tr(const char *en)
{
    return en;
}

#endif
