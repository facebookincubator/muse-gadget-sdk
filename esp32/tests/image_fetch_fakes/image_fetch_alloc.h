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

#include <stdlib.h>

void *image_test_malloc(size_t size);
void *image_test_calloc(size_t count, size_t size);
char *image_test_strdup(const char *str);
void image_test_free(void *ptr);

// Only included by the image_fetch.c wrapper, so the harness can use the real
// allocator to implement these hooks and to construct scripted replies.
#define malloc image_test_malloc
#define calloc image_test_calloc
#define strdup image_test_strdup
#define free image_test_free
