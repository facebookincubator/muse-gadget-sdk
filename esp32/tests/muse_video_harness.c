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
/* The video receive ring (muse_vstream.c) and the Wi-Fi messages
 * (muse_video_wire.c) on the host: tests/test_muse_video.py. */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "muse_video_wire.h"
#include "muse_vstream.h"

static int fails;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            fails++;                                                \
        }                                                           \
    } while (0)

/* A frame: header, then an access unit of len bytes starting with a start code. */
static size_t make_frame(uint8_t *out, uint16_t number, uint8_t flags, uint32_t len, uint8_t fill)
{
    out[0] = MUSE_VIDEO_FRAME_MAGIC;
    out[1] = flags;
    out[2] = (uint8_t)number;
    out[3] = (uint8_t)(number >> 8);
    out[4] = (uint8_t)len;
    out[5] = (uint8_t)(len >> 8);
    out[6] = (uint8_t)(len >> 16);
    out[7] = (uint8_t)(len >> 24);
    uint8_t *au = out + MUSE_VIDEO_FRAME_HEADER_LEN;
    memset(au, fill, len);
    au[0] = 0;
    au[1] = 0;
    au[2] = 0;
    au[3] = 1;
    return MUSE_VIDEO_FRAME_HEADER_LEN + len;
}

static void test_ring(void)
{
    static uint8_t ring[4096], f[3000], out[3000];
    muse_vstream_t s;
    muse_vframe_t fr;
    muse_vstream_init(&s, ring, sizeof(ring), 2048);
    muse_vstream_write(&s, f, 10);   /* not started: ignored */
    CHECK(muse_vstream_received(&s) == 0);
    muse_vstream_start(&s);

    /* Whole frames, and one split across writes. */
    size_t n = make_frame(f, 0, MUSE_VIDEO_FRAME_KEY, 1500, 0x11);
    muse_vstream_write(&s, f, 700);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 0);   /* not all there */
    muse_vstream_write(&s, f + 700, n - 700);
    CHECK(muse_vstream_received(&s) == n && muse_vstream_available(&s) == sizeof(ring) - n);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 1500 && fr.number == 0 &&
          fr.flags == MUSE_VIDEO_FRAME_KEY && !fr.after_gap && out[4] == 0x11);
    CHECK(muse_vstream_available(&s) == sizeof(ring));

    /* The zero-copy producer, across the ring's end. */
    n = make_frame(f, 1, 0, 2000, 0x22);
    size_t done = 0;
    while (done < n) {
        uint8_t *dst;
        uint32_t room = muse_vstream_space(&s, &dst);
        CHECK(room > 0);
        uint32_t k = room < n - done ? room : (uint32_t)(n - done);
        memcpy(dst, f + done, k);
        muse_vstream_commit(&s, k);
        done += k;
    }
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 2000 && fr.number == 1 && fr.flags == 0 &&
          out[1999] == 0x22);

    /* Garbage before a frame: skipped, and the frame says so. */
    uint8_t junk[5] = { 1, 2, 3, 4, 5 };
    muse_vstream_write(&s, junk, sizeof(junk));
    n = make_frame(f, 2, MUSE_VIDEO_FRAME_KEY, 100, 0x33);
    muse_vstream_write(&s, f, n);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 100 && fr.number == 2 && fr.after_gap);
    CHECK(s.resyncs == 1);

    /* Too big for the caller's buffer: dropped whole. */
    n = make_frame(f, 3, 0, 1000, 0x44);
    muse_vstream_write(&s, f, n);
    CHECK(muse_vstream_pop(&s, out, 500, &fr) == -1);
    CHECK(muse_vstream_available(&s) == sizeof(ring));

    /* Two bands: the top band's length after the header. */
    n = make_frame(f, 4, MUSE_VIDEO_FRAME_KEY, 200, 0x55);
    uint8_t *au = f + MUSE_VIDEO_FRAME_HEADER_LEN;
    au[0] = 96;   /* top band: 96 bytes from au + 4 */
    au[1] = au[2] = au[3] = 0;
    au[4] = au[5] = au[6] = 0;
    au[7] = 1;
    au[100] = au[101] = au[102] = 0;
    au[103] = 1;
    muse_vstream_write(&s, f, n);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 200 && fr.number == 4 && !fr.after_gap);

    /* A client ignoring credits: the excess is counted, then the next frame resyncs. */
    n = make_frame(f, 5, 0, 1500, 0x66);
    muse_vstream_write(&s, f, n);
    muse_vstream_write(&s, f, n);
    muse_vstream_write(&s, f, n);   /* only part of the third fits */
    CHECK(s.overruns == 3 * n - sizeof(ring));
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 1500 && fr.number == 5);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 1500 && fr.number == 5);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 0);   /* the cut one waits for bytes that never come */

    /* Stopped: the producer has no space. */
    muse_vstream_stop(&s);
    uint8_t *dst;
    CHECK(muse_vstream_space(&s, &dst) == 0);
}

/* A VP8 frame: header, then a frame tag (keyframes: the sync code). */
static size_t make_vp8(uint8_t *out, uint16_t number, bool key, uint32_t len, uint8_t fill)
{
    size_t n = make_frame(out, number, key ? MUSE_VIDEO_FRAME_KEY : 0, len, fill);
    uint8_t *au = out + MUSE_VIDEO_FRAME_HEADER_LEN;
    uint32_t first = len / 2;   /* the first partition's size */
    uint32_t tag = (key ? 0 : 1) | 1 << 1 /* version 1 */ | 1 << 4 /* shown */ | first << 5;
    au[0] = (uint8_t)tag;
    au[1] = (uint8_t)(tag >> 8);
    au[2] = (uint8_t)(tag >> 16);
    if (key) {
        au[3] = 0x9d;
        au[4] = 0x01;
        au[5] = 0x2a;
    }
    return n;
}

static void test_ring_vp8(void)
{
    static uint8_t ring[4096], f[3000], out[3000];
    muse_vstream_t s;
    muse_vframe_t fr;
    muse_vstream_init(&s, ring, sizeof(ring), 2048);
    s.codec = MUSE_VIDEO_CODEC_VP8;
    muse_vstream_start(&s);
    size_t n = make_vp8(f, 0, true, 900, 0x11);
    muse_vstream_write(&s, f, n);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 900 && fr.flags == MUSE_VIDEO_FRAME_KEY && !fr.after_gap);
    n = make_vp8(f, 1, false, 300, 0x22);
    muse_vstream_write(&s, f, n);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 300 && fr.number == 1 && !fr.after_gap);

    /* Garbage with a header-shaped run in it: skipped to the real frame. */
    uint8_t junk[12] = { MUSE_VIDEO_FRAME_MAGIC, MUSE_VIDEO_FRAME_KEY, 0, 0, 20, 0, 0, 0, 0xff, 0xff, 0xff, 7 };
    muse_vstream_write(&s, junk, sizeof(junk));
    n = make_vp8(f, 2, true, 200, 0x33);
    muse_vstream_write(&s, f, n);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 200 && fr.number == 2 && fr.after_gap);

    /* A key flag on an inter frame's tag (or the reverse) isn't a frame. */
    n = make_vp8(f, 3, false, 200, 0x44);
    f[1] = MUSE_VIDEO_FRAME_KEY;
    muse_vstream_write(&s, f, n);
    n = make_vp8(f, 4, false, 100, 0x55);
    muse_vstream_write(&s, f, n);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 100 && fr.number == 4 && fr.after_gap);

    /* Two bands: the top band's length, then two frames. */
    memset(f, 0, sizeof(f));
    make_frame(f, 5, MUSE_VIDEO_FRAME_KEY, 4 + 100 + 80, 0x66);
    uint8_t *au = f + MUSE_VIDEO_FRAME_HEADER_LEN;
    uint8_t band[200];
    au[0] = 100;
    au[1] = au[2] = au[3] = 0;
    make_vp8(band, 0, true, 100, 0x66);
    memcpy(au + 4, band + MUSE_VIDEO_FRAME_HEADER_LEN, 100);
    make_vp8(band, 0, true, 80, 0x67);
    memcpy(au + 104, band + MUSE_VIDEO_FRAME_HEADER_LEN, 80);
    muse_vstream_write(&s, f, MUSE_VIDEO_FRAME_HEADER_LEN + 184);
    CHECK(muse_vstream_pop(&s, out, sizeof(out), &fr) == 184 && fr.number == 5 && !fr.after_gap);
}

/* The framed message's JSON, parsed back. */
static cJSON *unframe(const uint8_t *m, size_t n)
{
    if (n < 3 || m[0] != MUSE_VIDEO_WIRE_JSON || (size_t)(m[1] | m[2] << 8) != n - 3) {
        return NULL;
    }
    return cJSON_ParseWithLength((const char *)m + 3, n - 3);
}

static int num(const cJSON *o, const char *a, const char *b)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, a);
    if (b) {
        v = cJSON_GetObjectItemCaseSensitive(v, b);
    }
    return cJSON_IsNumber(v) ? v->valueint : -1;
}

static void test_wire(void)
{
    muse_video_params_t p;
    const char *start = "{\"type\":\"start\",\"codec\":\"h264\",\"width\":232,\"height\":232,\"fps\":30,"
                        "\"kbps\":1000,\"playout_ms\":200,\"bands\":2,\"future\":true}";
    CHECK(muse_video_wire_parse(start, strlen(start), &p) == MUSE_VIDEO_MSG_START);
    CHECK(p.codec == MUSE_VIDEO_CODEC_H264 && p.width == 232 && p.height == 232 && p.fps == 30 &&
          p.kbps == 1000 && p.playout_ms == 200 && p.tiles == 2);
    const char *minimal = "{\"type\":\"start\",\"width\":240,\"height\":240}";
    CHECK(muse_video_wire_parse(minimal, strlen(minimal), &p) == MUSE_VIDEO_MSG_START);
    CHECK(p.codec == MUSE_VIDEO_CODEC_H264 && p.tiles == 1 && p.playout_ms == 0 && p.fps == 0);

    muse_video_caps_t c = { .display_w = 466, .display_h = 466, .round = true, .max_w = 240, .max_h = 240,
                            .max_fps = 30, .sug_w = 232, .sug_h = 232, .sug_fps = 30, .sug_kbps = 1000,
                            .max_playout_ms = 500, .max_tiles = 1 };
    CHECK(muse_video_wire_check(&p, &c) == MUSE_VIDEO_START_OK);
    muse_video_wire_parse(start, strlen(start), &p);
    CHECK(muse_video_wire_check(&p, &c) == MUSE_VIDEO_START_INVALID);   /* two bands on a one-band board */
    c.max_tiles = 2;
    CHECK(muse_video_wire_check(&p, &c) == MUSE_VIDEO_START_OK);
    const char *big = "{\"type\":\"start\",\"width\":480,\"height\":480}";
    muse_video_wire_parse(big, strlen(big), &p);
    CHECK(muse_video_wire_check(&p, &c) == MUSE_VIDEO_START_INVALID);
    const char *vp9 = "{\"type\":\"start\",\"codec\":\"vp9\",\"width\":240,\"height\":240}";
    muse_video_wire_parse(vp9, strlen(vp9), &p);
    CHECK(muse_video_wire_check(&p, &c) == MUSE_VIDEO_START_UNSUPPORTED);
    /* VP8: only on a board that lists it. */
    const char *vp8 = "{\"type\":\"start\",\"codec\":\"vp8\",\"width\":232,\"height\":232}";
    CHECK(muse_video_wire_parse(vp8, strlen(vp8), &p) == MUSE_VIDEO_MSG_START && p.codec == MUSE_VIDEO_CODEC_VP8);
    CHECK(muse_video_wire_check(&p, &c) == MUSE_VIDEO_START_UNSUPPORTED);
    c.vp8 = true;
    CHECK(muse_video_wire_check(&p, &c) == MUSE_VIDEO_START_OK);
    const char *late = "{\"type\":\"start\",\"width\":240,\"height\":240,\"playout_ms\":900}";
    muse_video_wire_parse(late, strlen(late), &p);
    CHECK(muse_video_wire_check(&p, &c) == MUSE_VIDEO_START_INVALID);

    CHECK(muse_video_wire_parse("{\"type\":\"stop\"}", 15, NULL) == MUSE_VIDEO_MSG_STOP);
    CHECK(muse_video_wire_parse("{\"type\":\"status\"}", 17, NULL) == MUSE_VIDEO_MSG_STATUS);
    CHECK(muse_video_wire_parse("{\"type\":\"dance\"}", 16, NULL) == MUSE_VIDEO_MSG_UNKNOWN);
    CHECK(muse_video_wire_parse("{\"kind\":1}", 10, NULL) == MUSE_VIDEO_MSG_BAD);
    CHECK(muse_video_wire_parse("not json", 8, NULL) == MUSE_VIDEO_MSG_BAD);
    /* Only n bytes are read: the closing brace past them doesn't count. */
    CHECK(muse_video_wire_parse("{\"type\":\"stop\"}", 14, NULL) == MUSE_VIDEO_MSG_BAD);

    uint8_t m[512];
    size_t n = muse_video_wire_hello(m, sizeof(m), "MuseGadget-\"A1B2C3\"", &c, 131072);
    cJSON *o = unframe(m, n);
    CHECK(o != NULL);
    CHECK(!strcmp(cJSON_GetObjectItem(o, "type")->valuestring, "hello"));
    CHECK(!strcmp(cJSON_GetObjectItem(o, "name")->valuestring, "MuseGadget-A1B2C3"));
    CHECK(num(o, "version", NULL) == MUSE_VIDEO_WIRE_VERSION && num(o, "buffer", NULL) == 131072);
    CHECK(num(o, "display", "width") == 466 && cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "display"), "round")));
    CHECK(num(o, "max", "width") == 240 && num(o, "max", "fps") == 30);
    CHECK(num(o, "suggested", "kbps") == 1000 && num(o, "max_bands", NULL) == 2 && num(o, "max_playout_ms", NULL) == 500);
    const cJSON *codecs = cJSON_GetObjectItem(o, "codecs");
    CHECK(cJSON_GetArraySize(codecs) == 2 && !strcmp(cJSON_GetArrayItem(codecs, 0)->valuestring, "h264") &&
          !strcmp(cJSON_GetArrayItem(codecs, 1)->valuestring, "vp8"));
    cJSON_Delete(o);
    c.vp8 = false;
    n = muse_video_wire_hello(m, sizeof(m), "x", &c, 1);
    o = unframe(m, n);
    CHECK(o && cJSON_GetArraySize(cJSON_GetObjectItem(o, "codecs")) == 1);
    cJSON_Delete(o);
    c.vp8 = true;
    CHECK(muse_video_wire_hello(m, 40, "x", &c, 1) == 0);   /* doesn't fit */

    n = muse_video_wire_buffer(m, sizeof(m), 100000, 31072, 10, 2, 21, 9, 15);
    o = unframe(m, n);
    CHECK(o && num(o, "free", NULL) == 100000 && num(o, "received", NULL) == 31072 && num(o, "shown", NULL) == 10 &&
          num(o, "dropped", NULL) == 2 && num(o, "decode_ms", NULL) == 21 && num(o, "last_frame", NULL) == 9 &&
          num(o, "last_age_ms", NULL) == 15);
    cJSON_Delete(o);
    n = muse_video_wire_buffer(m, sizeof(m), 1, 2, 0, 0, 0, 0, 0xffff);
    o = unframe(m, n);
    CHECK(o && num(o, "last_frame", NULL) == -1);
    cJSON_Delete(o);

    n = muse_video_wire_want(m, sizeof(m), true);
    o = unframe(m, n);
    CHECK(o && cJSON_IsTrue(cJSON_GetObjectItem(o, "on")));
    cJSON_Delete(o);
    n = muse_video_wire_error(m, sizeof(m), "start", "busy");
    o = unframe(m, n);
    CHECK(o && !strcmp(cJSON_GetObjectItem(o, "reason")->valuestring, "busy"));
    cJSON_Delete(o);
    n = muse_video_wire_simple(m, sizeof(m), "keyframe");
    o = unframe(m, n);
    CHECK(o && !strcmp(cJSON_GetObjectItem(o, "type")->valuestring, "keyframe"));
    cJSON_Delete(o);
}

int main(void)
{
    test_ring();
    test_ring_vp8();
    test_wire();
    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("PASS muse_video\n");
    return 0;
}
