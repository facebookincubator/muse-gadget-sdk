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
#include "muse_video_wire.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static int get_int(const cJSON *o, const char *key, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

muse_video_msg_t muse_video_wire_parse(const char *json, size_t n, muse_video_params_t *p)
{
    cJSON *o = cJSON_ParseWithLength(json, n);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return MUSE_VIDEO_MSG_BAD;
    }
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "type");
    muse_video_msg_t m = MUSE_VIDEO_MSG_BAD;
    if (cJSON_IsString(t)) {
        m = MUSE_VIDEO_MSG_UNKNOWN;
        if (!strcmp(t->valuestring, "start")) {
            m = MUSE_VIDEO_MSG_START;
            if (p) {
                const cJSON *codec = cJSON_GetObjectItemCaseSensitive(o, "codec");
                memset(p, 0, sizeof(*p));
                bool h264 = !codec || (cJSON_IsString(codec) && !strcmp(codec->valuestring, "h264"));
                bool vp8 = cJSON_IsString(codec) && !strcmp(codec->valuestring, "vp8");
                int w = get_int(o, "width", 0), h = get_int(o, "height", 0);
                int fps = get_int(o, "fps", 0), kbps = get_int(o, "kbps", 0);
                int playout = get_int(o, "playout_ms", 0), tiles = get_int(o, "bands", 1);
                p->codec = h264 ? MUSE_VIDEO_CODEC_H264 : vp8 ? MUSE_VIDEO_CODEC_VP8 : 0;
                p->width = w > 0 && w <= 0xffff ? (uint16_t)w : 0;
                p->height = h > 0 && h <= 0xffff ? (uint16_t)h : 0;
                p->fps = fps > 0 && fps <= 255 ? (uint8_t)fps : 0;
                p->kbps = kbps > 0 && kbps <= 0xffff ? (uint16_t)kbps : 0;
                p->playout_ms = playout > 0 && playout <= 0xffff ? (uint16_t)playout : 0;
                p->tiles = tiles >= 1 && tiles <= 255 ? (uint8_t)tiles : 0;
            }
        } else if (!strcmp(t->valuestring, "stop")) {
            m = MUSE_VIDEO_MSG_STOP;
        } else if (!strcmp(t->valuestring, "status")) {
            m = MUSE_VIDEO_MSG_STATUS;
        }
    }
    cJSON_Delete(o);
    return m;
}

muse_video_start_check_t muse_video_wire_check(const muse_video_params_t *p, const muse_video_caps_t *c)
{
    if (p->codec != MUSE_VIDEO_CODEC_H264 && !(p->codec == MUSE_VIDEO_CODEC_VP8 && c->vp8)) {
        return MUSE_VIDEO_START_UNSUPPORTED;
    }
    uint8_t bands = p->tiles ? p->tiles : 1;
    uint8_t max_bands = c->max_tiles ? c->max_tiles : 1;
    if (!p->width || !p->height || p->width > c->max_w || p->height > c->max_h || !p->tiles ||
        bands > max_bands || p->playout_ms > c->max_playout_ms || (p->fps && p->fps > c->max_fps)) {
        return MUSE_VIDEO_START_INVALID;
    }
    return MUSE_VIDEO_START_OK;
}

/* Frames the JSON printed by fmt: 'J', its length, the text. */
static size_t frame(uint8_t *out, size_t cap, const char *fmt, ...)
{
    if (cap < 4) {
        return 0;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf((char *)out + 3, cap - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - 3 || n > 0xffff) {
        return 0;
    }
    out[0] = MUSE_VIDEO_WIRE_JSON;
    out[1] = (uint8_t)n;
    out[2] = (uint8_t)(n >> 8);
    return (size_t)n + 3;
}

size_t muse_video_wire_hello(uint8_t *out, size_t cap, const char *name, const muse_video_caps_t *c,
                             uint32_t buffer)
{
    /* The name goes in as is, less anything that would need escaping. */
    char safe[48];
    size_t k = 0;
    for (const char *s = name ? name : ""; *s && k < sizeof(safe) - 1; s++) {
        if (*s >= 0x20 && *s != '"' && *s != '\\') {
            safe[k++] = *s;
        }
    }
    safe[k] = 0;
    return frame(out, cap,
                 "{\"type\":\"hello\",\"version\":%d,\"name\":\"%s\",\"codecs\":[\"h264\"%s],"
                 "\"display\":{\"width\":%u,\"height\":%u,\"round\":%s},"
                 "\"max\":{\"width\":%u,\"height\":%u,\"fps\":%u},"
                 "\"suggested\":{\"width\":%u,\"height\":%u,\"fps\":%u,\"kbps\":%u},"
                 "\"buffer\":%u,\"max_playout_ms\":%u,\"max_bands\":%u,\"band_split_pct\":%u}",
                 MUSE_VIDEO_WIRE_VERSION, safe, c->vp8 ? ",\"vp8\"" : "", c->display_w, c->display_h, c->round ? "true" : "false", c->max_w,
                 c->max_h, c->max_fps, c->sug_w, c->sug_h, c->sug_fps, c->sug_kbps, (unsigned)buffer,
                 c->max_playout_ms, c->max_tiles ? c->max_tiles : 1, c->tile_split_pct);
}

size_t muse_video_wire_buffer(uint8_t *out, size_t cap, uint32_t free_bytes, uint32_t received,
                              uint16_t shown, uint16_t dropped, uint8_t decode_ms, uint16_t last_frame,
                              uint16_t last_age_ms)
{
    if (last_age_ms == 0xffff) {
        return frame(out, cap,
                     "{\"type\":\"buffer\",\"free\":%u,\"received\":%u,\"shown\":%u,\"dropped\":%u,"
                     "\"decode_ms\":%u}",
                     (unsigned)free_bytes, (unsigned)received, shown, dropped, decode_ms);
    }
    return frame(out, cap,
                 "{\"type\":\"buffer\",\"free\":%u,\"received\":%u,\"shown\":%u,\"dropped\":%u,"
                 "\"decode_ms\":%u,\"last_frame\":%u,\"last_age_ms\":%u}",
                 (unsigned)free_bytes, (unsigned)received, shown, dropped, decode_ms, last_frame, last_age_ms);
}

size_t muse_video_wire_simple(uint8_t *out, size_t cap, const char *type)
{
    return frame(out, cap, "{\"type\":\"%s\"}", type);
}

size_t muse_video_wire_want(uint8_t *out, size_t cap, bool on)
{
    return frame(out, cap, "{\"type\":\"want\",\"on\":%s}", on ? "true" : "false");
}

size_t muse_video_wire_error(uint8_t *out, size_t cap, const char *for_type, const char *reason)
{
    return frame(out, cap, "{\"type\":\"error\",\"for\":\"%s\",\"reason\":\"%s\"}", for_type, reason);
}
