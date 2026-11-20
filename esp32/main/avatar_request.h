/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef void (*avatar_request_cb)(void *, int, const uint8_t *, size_t, bool);
typedef struct {
    int64_t (*open)(const char *, const char *, const char *const *, bool, avatar_request_cb, void *);
    void (*cancel)(int64_t);
    void *(*alloc)(size_t);
    void (*free)(void *);
    int64_t (*now_us)(void);
    void (*yield)(void);
    bool (*stop)(uintptr_t);
} avatar_request_ops_t;
typedef struct { uint8_t *body; size_t len; int status; bool overflow; } avatar_response_t;

/* open returning zero must not retain/call its callback. Otherwise exactly
 * one terminal callback releases transport ownership, even after cancel.
 * The caller can stop waiting without keeping its stack/context alive. */
avatar_response_t *avatar_request_get(const char *path, const char *const *headers,
                                     size_t cap, uintptr_t generation, const avatar_request_ops_t *ops);
/* Diagnostic/export callers need explicit HTTP statuses (for example 416 at
 * exact-range EOF). Errors and oversized bodies still fail closed. */
avatar_response_t *avatar_request_get_status(const char *path, const char *const *headers,
                                            size_t cap, uintptr_t generation, const avatar_request_ops_t *ops);
void avatar_request_release(avatar_response_t *response);
