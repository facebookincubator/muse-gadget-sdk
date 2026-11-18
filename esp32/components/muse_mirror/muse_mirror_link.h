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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A transport other than the console cable. It carries the cable's messages
 * (protocols/MIRROR.md) without the cable's framing, one viewer at a time. */
typedef struct {
    /* Mirror task: one whole message to the viewer within ms; false drops the
     * viewer, and after close it must return false. */
    bool (*send)(const void *msg, size_t len, int ms);
    bool audio;   /* fast enough for sound */
} muse_mirror_link_t;

/* The link's own task, one at a time. open: a viewer arrived, or a new one
 * replaces the last (false while the cable or another link has the viewer).
 * input: one of its messages (false: not taken). close: it left. */
bool muse_mirror_link_open(const muse_mirror_link_t *link);
bool muse_mirror_link_input(const muse_mirror_link_t *link, const uint8_t *msg, size_t len);
void muse_mirror_link_close(const muse_mirror_link_t *link);
/* The 'B' message, once the mirror runs; NULL before. */
const char *muse_mirror_link_board(size_t *len);
