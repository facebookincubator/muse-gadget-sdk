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

/*
 * The video decode task's keyframe gate (muse_video.c's decode_task), plain C
 * so the host replays run it (tests/test_video_route.py): which frames from
 * the ring go to the decoder, which wait for a keyframe, and when to ask the
 * client for one.
 *
 * - A frame the ring had to drop or resync before (n < 0, or after_gap and
 *   not a keyframe) makes the gate wait for a keyframe; so does a frame
 *   whose header number isn't the last one's + 1 (a frame lost upstream or
 *   dropped before the ring, its bytes intact: P frames would decode against
 *   the missing reference), a decode error, and a keyframe whose decode made
 *   no picture (it didn't repair anything: an error, not a recovery). A
 *   stream's first frame and every keyframe start the count again; the
 *   number wraps at 16 bits, and UI still patches take numbers too
 *   (muse_vgate_seen).
 * - While it waits, frames that aren't keyframes are dropped, and
 *   request_keyframe goes to the client at once on the first trouble after a
 *   frame went to the decoder, then at most every MUSE_VGATE_ASK_US while it
 *   keeps waiting (a keyframe repairs the decoder, whatever came
 *   before; mgcommands.h: a device that drops or can't decode
 *   video sends request_keyframe at once).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_VGATE_ASK_US (300 * 1000)   /* between requests while still waiting */

typedef struct {
    bool need_key;     /* frames that aren't keyframes are dropped */
    bool good;         /* a frame decoded since the last request: the next trouble asks at once */
    int64_t asked_us;  /* when the last request_keyframe went */
    uint16_t last;     /* the last frame header's number this stream */
    bool have_last;
} muse_vgate_t;

/* A new stream: it starts at a keyframe. */
static inline void muse_vgate_start(muse_vgate_t *g)
{
    g->need_key = true;
    g->good = true;
    g->have_last = false;
}

/* A frame header that doesn't go through take() (a UI still patch): its
 * number counts. */
static inline void muse_vgate_seen(muse_vgate_t *g, uint16_t number)
{
    g->last = number;
    g->have_last = true;
}

static inline bool muse_vgate_ask(muse_vgate_t *g, int64_t now_us)
{
    if (g->good || now_us - g->asked_us > MUSE_VGATE_ASK_US) {
        g->good = false;
        g->asked_us = now_us;
        return true;
    }
    return false;
}

/* A UI still patch's header: its number counts, and a gap before it (a frame number skipped, or bytes lost) is one
 * the next picture's skip check would no longer see: the gate waits for a keyframe here. True: send
 * request_keyframe. */
static inline bool muse_vgate_still(muse_vgate_t *g, bool after_gap, uint16_t number, int64_t now_us)
{
    bool skipped = g->have_last && (uint16_t)(number - g->last) != 1;
    muse_vgate_seen(g, number);
    if (!after_gap && !skipped) {
        return false;
    }
    g->need_key = true;
    return muse_vgate_ask(g, now_us);
}

/* Too far behind to catch up (a board without PSRAM decoding H.264 in a reply):
 * lag_ms of the stream waits in the ring, over bound_ms, and this frame isn't a keyframe. Decoding the
 * backlog only makes pictures the show task skips as late (an H.264 P picture can't be skipped alone:
 * the next ones predict from it), so the gate stops here: this frame and the ones after it drop until a
 * keyframe, the last good picture stays up, and request_keyframe goes to the client (the existing ask,
 * at once, then at most every MUSE_VGATE_ASK_US while waiting). True: drop it (and ask if *ask). */
static inline bool muse_vgate_behind(muse_vgate_t *g, bool key, uint16_t number, uint32_t lag_ms, uint32_t bound_ms,
                                     int64_t now_us, bool *ask)
{
    *ask = false;
    if (key || g->need_key || lag_ms <= bound_ms) {
        return false;
    }
    g->need_key = true;
    g->last = number;   /* its number counts (the keyframe restarts the count anyway) */
    g->have_last = true;
    *ask = muse_vgate_ask(g, now_us);
    return true;
}

/* A frame popped from the ring: n its length, or < 0 if the ring dropped
 * one or was damaged; key its keyframe flag; after_gap: bytes before it were
 * lost; number its header's frame number. True: decode it. False: drop it,
 * and send request_keyframe if *ask. */
static inline bool muse_vgate_take(muse_vgate_t *g, int32_t n, bool key, bool after_gap, uint16_t number,
                                   int64_t now_us, bool *ask)
{
    *ask = false;
    key = key && n > 0;
    bool skipped = false;
    if (n > 0) {
        skipped = g->have_last && (uint16_t)(number - g->last) != 1 && !key;
        muse_vgate_seen(g, number);
    }
    if (n < 0 || ((after_gap || skipped) && !key)) {
        g->need_key = true;
    }
    if (n < 0 || (g->need_key && !key)) {
        *ask = muse_vgate_ask(g, now_us);
        return false;
    }
    g->need_key = false;
    g->good = true;   /* a frame for the decoder: its error, if any, asks at once (mgcommands.h) */
    return true;
}

/* The decoder's verdict on a frame take() passed: ok (no error) and got (a
 * picture). True: a decode error (count it, drop the frame); then send
 * request_keyframe if *ask. */
static inline bool muse_vgate_result(muse_vgate_t *g, bool ok, bool got, bool key, int64_t now_us, bool *ask)
{
    *ask = false;
    if (!ok || (key && !got)) {
        g->need_key = true;
        *ask = muse_vgate_ask(g, now_us);
        return true;
    }
    if (got) {
        g->good = true;
    }
    return false;
}

/* A frame lost on the device's side after take() passed it (no picture slot,
 * a bad band length): wait for a keyframe, as after a gap. */
static inline void muse_vgate_lost(muse_vgate_t *g)
{
    g->need_key = true;
}

#ifdef __cplusplus
}
#endif
