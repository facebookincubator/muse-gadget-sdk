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
#ifndef MUSE_FAKE_LOG_DEFINED   /* a harness may capture the log itself */
#include <stdarg.h>
#include <stdio.h>
/* No printf format checking: the firmware's own formats assume ESP-IDF's types. */
static inline void muse_fake_log(char level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%c %s: ", level, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
#define ESP_LOGE(tag, ...) muse_fake_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) muse_fake_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) muse_fake_log('I', tag, __VA_ARGS__)
#endif
