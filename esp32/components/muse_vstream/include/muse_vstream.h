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
 * The receive side of a video stream, whatever carries it: the bytes a
 * transport receives go into a ring, and the decoder takes them out one frame
 * (header + access unit) at a time. One producer (the transport's receive
 * task) and one consumer (the decoder task); no lock between them. Plain C,
 * no ESP-IDF, so the host tests build it.
 *
 * The stream is a sequence of frames, each an 8-byte header followed by one
 * H.264 access unit in Annex B form, or one VP8 frame
 * (../../../protocols/VIDEO.md):
 *   uint8_t  magic (MUSE_VIDEO_FRAME_MAGIC)
 *   uint8_t  flags (MUSE_VIDEO_FRAME_KEY)
 *   uint16_t frame number, little-endian, from 0 at the stream's start, wrapping
 *   uint32_t access unit length in bytes, little-endian (not counting the header)
 * With two bands, the access unit is the top band's uint32_t length and access
 * unit, then the bottom band's access unit.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_VIDEO_FRAME_MAGIC 0x56   /* 'V' */
#define MUSE_VIDEO_FRAME_HEADER_LEN 8
#define MUSE_VIDEO_FRAME_KEY 0x01     /* flags: a keyframe (every band an IDR picture, SPS and PPS first; VP8: a key frame) */

#define MUSE_VIDEO_CODEC_H264 1       /* constrained baseline, CAVLC, Annex B */
#define MUSE_VIDEO_CODEC_VP8 2        /* RFC 6386 frames, as an IVF file carries them (no IVF headers) */

/* A stream's parameters, from the client. */
typedef struct {
    uint8_t codec;            /* MUSE_VIDEO_CODEC_*; 0: one this board doesn't know */
    uint16_t width, height;   /* the picture, after cropping */
    uint8_t fps;
    uint16_t kbps;
    uint16_t playout_ms;      /* 0: show each frame once decoded; else at fps, this long after the first */
    uint8_t tiles;            /* horizontal bands, each its own stream of the codec; 0/1: one */
} muse_video_params_t;

/* What a display board offers. */
typedef struct {
    uint16_t display_w, display_h;
    bool round;
    uint16_t max_w, max_h;       /* largest picture it decodes */
    uint8_t max_fps;
    uint16_t sug_w, sug_h;       /* the stream it suggests */
    uint8_t sug_fps;
    uint16_t sug_kbps;
    uint16_t max_playout_ms;     /* longest playout delay it paces, 0: none */
    uint8_t max_tiles;           /* most bands it decodes in parallel, 0/1: one */
    uint8_t tile_split_pct;      /* two bands: the top band's share of the height, %; 0: equal */
    bool vp8;                    /* decodes VP8 as well as H.264 */
} muse_video_caps_t;

typedef struct {
    uint16_t number;
    uint8_t flags;            /* MUSE_VIDEO_FRAME_KEY */
    uint32_t len;
    bool after_gap;           /* bytes before it were lost or skipped */
} muse_vframe_t;

typedef struct {
    uint8_t *buf;
    uint32_t size;            /* a power of two */
    volatile uint32_t head;   /* written by the producer: bytes ever written */
    volatile uint32_t tail;   /* written by the consumer: bytes ever taken */
    /* A start (producer) leaves tail to the consumer, which may be mid-pop:
     * it notes where the new stream begins and counts itself, and the
     * consumer skips to there when it sees the count change. */
    volatile uint32_t start_at;
    volatile uint32_t starts;
    uint32_t started;         /* the consumer's copy of starts */
    volatile uint32_t received;
    uint32_t max_frame;
    uint32_t resyncs;         /* times a bad header made the consumer search */
    uint32_t overruns;        /* bytes the producer had no room for (a client ignoring credits) */
    volatile bool active;
    /* The stream's codec, for telling a frame from noise when resyncing (set
     * before muse_vstream_start; 0 is H.264): H.264 access units start with
     * a start code, VP8 frames with a frame tag (and keyframes a sync code). */
    uint8_t codec;
} muse_vstream_t;

/* buf of size bytes (a power of two), max_frame the largest access unit. */
void muse_vstream_init(muse_vstream_t *s, uint8_t *buf, uint32_t size, uint32_t max_frame);
/* Empties the ring and starts counting from zero. */
void muse_vstream_start(muse_vstream_t *s);
void muse_vstream_stop(muse_vstream_t *s);

/* Producer. Bytes that don't fit are counted in overruns and dropped. */
void muse_vstream_write(muse_vstream_t *s, const uint8_t *d, size_t n);
/* Producer, without a copy: the contiguous free space at the head (*dst, its
 * length returned; 0 when full or stopped), then commit what was put there. */
uint32_t muse_vstream_space(muse_vstream_t *s, uint8_t **dst);
void muse_vstream_commit(muse_vstream_t *s, uint32_t n);
uint32_t muse_vstream_available(const muse_vstream_t *s);
uint32_t muse_vstream_received(const muse_vstream_t *s);

/*
 * Consumer: copies the next whole frame's access unit into dst (cap bytes)
 * and fills *f. Returns its length, 0 if no whole frame is there yet, or -1
 * if a frame was dropped (too big for cap) or the stream was damaged with no
 * whole frame after it yet. f->after_gap says bytes before this frame were
 * lost: unless it's a keyframe, the caller should drop it and ask for one.
 */
int32_t muse_vstream_pop(muse_vstream_t *s, uint8_t *dst, uint32_t cap, muse_vframe_t *f);

#ifdef __cplusplus
}
#endif
