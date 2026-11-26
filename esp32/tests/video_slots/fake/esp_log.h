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

/* Host fake for tests/video_slots/harness.c (tests/test_video_untrusted.py). */
#pragma once
#include <stdio.h>
void fake_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) fake_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ((void)0)
