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
#include <stdint.h>

/* Distinguish a sleep click from push-to-talk before posting any mic event. */
#define MUSE_WHEEL_HOLD_MS 300

typedef struct {
    bool down, recording, swallow, woke;
    uint32_t pressed_ms;
} muse_wheel_t;

typedef enum {
    MUSE_WHEEL_NONE,
    MUSE_WHEEL_WAKE,
    MUSE_WHEEL_SLEEP,
    MUSE_WHEEL_TALK_DOWN,
    MUSE_WHEEL_TALK_UP,
} muse_wheel_action_t;

static inline muse_wheel_action_t muse_wheel_update(muse_wheel_t *s, bool down,
                                                    uint32_t now, bool asleep,
                                                    bool confirmed)
{
    if (down && !s->down) {
        s->pressed_ms = now;
        s->recording = false;
        s->swallow = confirmed;
        s->woke = asleep;
        s->down = true;
        if (asleep) return MUSE_WHEEL_WAKE;
    }
    if (!down && s->down) {
        s->down = false;
        if (s->recording) {
            s->recording = false;
            return MUSE_WHEEL_TALK_UP;
        }
        if (!s->swallow && !s->woke) return MUSE_WHEEL_SLEEP;
    }
    if (down && !s->swallow && !s->recording && now - s->pressed_ms >= MUSE_WHEEL_HOLD_MS) {
        s->recording = true;
        return MUSE_WHEEL_TALK_DOWN;
    }
    return MUSE_WHEEL_NONE;
}
