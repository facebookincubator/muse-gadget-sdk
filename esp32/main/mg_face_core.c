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

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1   /* strnlen: glibc declares it only with this (macOS and newlib do anyway) */
#endif
#include "mg_face_core.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

/* ---- The probe ---------------------------------------------------------- */

void face_head_reset(face_head_t *h)
{
    memset(h, 0, sizeof(*h));
}

static bool name_is(const char *name, size_t len, const char *want)
{
    return strlen(want) == len && strncasecmp(name, want, len) == 0;
}

/* A decimal uint32 at *p (advanced); false if none or it overflows. */
static bool parse_u32(const char **p, const char *end, uint32_t *out)
{
    const char *s = *p;
    uint64_t v = 0;
    if (s >= end || !isdigit((unsigned char)*s)) {
        return false;
    }
    while (s < end && isdigit((unsigned char)*s)) {
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > 0xFFFFFFFFu) {
            return false;
        }
        s++;
    }
    *p = s;
    *out = (uint32_t)v;
    return true;
}

void face_head_header(face_head_t *h, const char *name, size_t name_len, const char *value,
                      size_t value_len)
{
    const char *v = value, *end = value + value_len;
    while (v < end && (*v == ' ' || *v == '\t')) {
        v++;
    }
    if (name_is(name, name_len, "Content-Range")) {
        /* "bytes 0-0/1234", or "bytes (star)/1234" on a 416. */
        if ((size_t)(end - v) < 6 || strncasecmp(v, "bytes", 5) != 0) {
            return;
        }
        v += 5;
        while (v < end && *v == ' ') {
            v++;
        }
        uint32_t first = 0, last = 0, total = 0;
        if (v < end && *v == '*') {
            v++;
        } else {
            if (!parse_u32(&v, end, &first) || v >= end || *v != '-') {
                return;
            }
            v++;
            if (!parse_u32(&v, end, &last) || last < first) {
                return;
            }
        }
        bool ranged = v > value && v[-1] != '*';
        if (v >= end || *v != '/') {
            return;
        }
        v++;
        if (parse_u32(&v, end, &total) && v == end && (!ranged || last < total)) {
            h->have_total = true;
            h->total = total;
            if (ranged) {
                h->range_start_ok = true;
                h->range_start = first;
            }
        }
    } else if (name_is(name, name_len, "Content-Length")) {
        uint32_t n;
        if (parse_u32(&v, end, &n) && v == end) {
            h->have_length = true;
            h->content_length = n;
        }
    } else if (name_is(name, name_len, "ETag")) {
        size_t n = (size_t)(end - v);
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) {
            n--;
        }
        /* A weak ETag can't tell two pictures apart byte for byte. */
        if (n >= 2 && (v[0] == 'W' || v[0] == 'w') && v[1] == '/') {
            h->etag[0] = '\0';
            return;
        }
        if (n == 0 || n >= sizeof(h->etag)) {
            h->etag[0] = '\0';
            return;
        }
        memcpy(h->etag, v, n);
        h->etag[n] = '\0';
    }
}

face_probe_t face_probe_decide(const face_head_t *h, const char *stored_etag, uint32_t stored_len,
                               uint32_t cap)
{
    uint32_t total;
    if (h->status == 404 || h->status == 410) {
        return FACE_PROBE_ABSENT;
    }
    if (h->status == 416) {
        return FACE_PROBE_ABSENT;   /* an empty file */
    }
    if (h->status == 206) {
        if (!h->have_total) {
            return FACE_PROBE_FAILED;
        }
        total = h->total;
    } else if (h->status == 200) {
        /* The daemon ignored Range: the length is the file's. */
        if (!h->have_length) {
            return FACE_PROBE_FAILED;
        }
        total = h->content_length;
    } else {
        return FACE_PROBE_FAILED;
    }
    if (total == 0) {
        return FACE_PROBE_ABSENT;
    }
    if (total > cap) {
        return FACE_PROBE_TOO_LARGE;
    }
    bool stored = stored_etag && stored_etag[0];
    if (h->etag[0]) {
        return stored && strcmp(h->etag, stored_etag) == 0 ? FACE_PROBE_SAME : FACE_PROBE_CHANGED;
    }
    if (stored_len && total == stored_len) {
        return FACE_PROBE_SAME;
    }
    return FACE_PROBE_CHANGED;
}

const char *face_probe_name(face_probe_t p)
{
    switch (p) {
    case FACE_PROBE_ABSENT: return "absent";
    case FACE_PROBE_SAME: return "unchanged";
    case FACE_PROBE_CHANGED: return "changed";
    case FACE_PROBE_TOO_LARGE: return "too large";
    default: return "failed";
    }
}

/* ---- The ranges --------------------------------------------------------- */

bool face_asm_next(const face_asm_t *a, uint32_t chunk, uint32_t *first, uint32_t *last)
{
    if (!chunk || a->have >= a->total) {
        return false;
    }
    uint32_t left = a->total - a->have;
    uint32_t n = left < chunk ? left : chunk;
    *first = a->have;
    *last = a->have + n - 1;
    return true;
}

bool face_asm_put(face_asm_t *a, uint32_t start, const uint8_t *data, size_t len)
{
    if (start != a->have || len > a->total - a->have) {
        return false;
    }
    memcpy(a->buf + a->have, data, len);
    a->have += (uint32_t)len;
    return true;
}

void face_range_header(char *out, size_t cap, uint32_t first, uint32_t last)
{
    snprintf(out, cap, "bytes=%u-%u", (unsigned)first, (unsigned)last);
}

/* ---- The bytes ---------------------------------------------------------- */

face_fmt_t face_sniff(const uint8_t *p, size_t len)
{
    if (len >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) {
        return FACE_FMT_JPEG;
    }
    if (len >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) {
        return FACE_FMT_PNG;
    }
    if (len >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) {
        return FACE_FMT_WEBP;
    }
    if (len >= 6 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))) {
        return FACE_FMT_GIF;
    }
    return FACE_FMT_UNKNOWN;
}

const char *face_fmt_name(face_fmt_t f)
{
    switch (f) {
    case FACE_FMT_JPEG: return "JPEG";
    case FACE_FMT_PNG: return "PNG";
    case FACE_FMT_WEBP: return "WebP";
    case FACE_FMT_GIF: return "GIF";
    default: return "unknown";
    }
}

bool face_jpeg_frame(const uint8_t *p, size_t len, int *width, int *height, bool *progressive)
{
    *progressive = false;
    if (face_sniff(p, len) != FACE_FMT_JPEG) {
        return false;
    }
    size_t i = 2;
    while (i + 4 <= len) {
        if (p[i] != 0xFF) {
            return false;
        }
        uint8_t m = p[i + 1];
        if (m == 0xFF) {
            i++;   /* fill byte */
            continue;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            i += 2;
            continue;
        }
        size_t seg = ((size_t)p[i + 2] << 8) | p[i + 3];
        if (seg < 2 || i + 2 + seg > len) {
            return false;
        }
        if (m == 0xDA || m == 0xD9) {
            return false;   /* a scan (or the end) before any frame */
        }
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (m == 0xC2) {
                *progressive = true;
            }
            if (m != 0xC0 && m != 0xC1) {
                return false;   /* progressive, lossless or arithmetic */
            }
            if (seg < 8) {
                return false;
            }
            int h = (p[i + 5] << 8) | p[i + 6];
            int w = (p[i + 7] << 8) | p[i + 8];
            if (w <= 0 || h <= 0) {
                return false;
            }
            *width = w;
            *height = h;
            return true;
        }
        i += 2 + seg;
    }
    return false;
}

bool face_place(int img_w, int img_h, int panel_w, int panel_h, int *scale, int *x0, int *y0)
{
    if (img_w <= 0 || img_h <= 0 || panel_w <= 0 || panel_h <= 0) {
        return false;
    }
    /* The largest picture that still covers the panel's short side: no
     * scale-up (TJpgDec only shrinks), the middle kept. */
    int s = 1;
    while (s < 8 && img_w / (s * 2) >= panel_w && img_h / (s * 2) >= panel_h) {
        s *= 2;
    }
    int w = img_w / s, h = img_h / s;
    if (w > panel_w * 2 || h > panel_h * 2 || w * 4 < panel_w || h * 4 < panel_h) {
        return false;
    }
    *scale = s;
    *x0 = (panel_w - w) / 2;
    *y0 = (panel_h - h) / 2;
    return true;
}

uint32_t face_crc32(uint32_t crc, const uint8_t *p, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t face_etag_tag(const char *etag)
{
    return etag && etag[0] ? face_crc32(0, (const uint8_t *)etag, strlen(etag)) : 0;
}

/* ---- The stored record -------------------------------------------------- */

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strnlen(src, cap - 1) : 0;
    if (n) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

size_t face_rec_build(uint8_t *out, size_t out_cap, const uint8_t *jpeg, uint32_t jpeg_len,
                      int width, int height, const char *etag, const char *node, const char *name)
{
    if (!out || !jpeg || jpeg_len == 0 || jpeg_len > FACE_JPEG_MAX ||
        out_cap < sizeof(face_rec_t) + jpeg_len) {
        return 0;
    }
    face_rec_t r;
    memset(&r, 0, sizeof(r));
    r.magic = FACE_REC_MAGIC;
    r.version = FACE_REC_VERSION;
    r.jpeg_len = jpeg_len;
    r.crc = face_crc32(0, jpeg, jpeg_len);
    r.width = (uint16_t)width;
    r.height = (uint16_t)height;
    copy_str(r.etag, sizeof(r.etag), etag);
    copy_str(r.node, sizeof(r.node), node);
    copy_str(r.name, sizeof(r.name), name);
    memcpy(out, &r, sizeof(r));
    memcpy(out + sizeof(r), jpeg, jpeg_len);
    return sizeof(r) + jpeg_len;
}

face_rec_check_t face_rec_check(const uint8_t *blob, size_t len, const char *node,
                                face_rec_t *hdr, const uint8_t **jpeg)
{
    face_rec_t r;
    if (!blob || len < sizeof(r)) {
        return FACE_REC_SHORT;
    }
    memcpy(&r, blob, sizeof(r));
    if (r.magic != FACE_REC_MAGIC || r.version != FACE_REC_VERSION) {
        return FACE_REC_BAD_MAGIC;
    }
    if (r.jpeg_len == 0 || r.jpeg_len > FACE_JPEG_MAX || len != sizeof(r) + r.jpeg_len) {
        return FACE_REC_BAD_LEN;
    }
    const uint8_t *j = blob + sizeof(r);
    if (face_crc32(0, j, r.jpeg_len) != r.crc) {
        return FACE_REC_BAD_CRC;
    }
    int w, h;
    bool prog;
    if (!face_jpeg_frame(j, r.jpeg_len, &w, &h, &prog)) {
        return FACE_REC_BAD_JPEG;
    }
    r.etag[sizeof(r.etag) - 1] = '\0';
    r.node[sizeof(r.node) - 1] = '\0';
    r.name[sizeof(r.name) - 1] = '\0';
    if (!node || !node[0] || strcmp(node, r.node) != 0) {
        return FACE_REC_OTHER_NODE;
    }
    if (hdr) {
        *hdr = r;
    }
    if (jpeg) {
        *jpeg = j;
    }
    return FACE_REC_OK;
}

const char *face_rec_check_name(face_rec_check_t c)
{
    switch (c) {
    case FACE_REC_OK: return "ok";
    case FACE_REC_SHORT: return "short";
    case FACE_REC_BAD_MAGIC: return "bad header";
    case FACE_REC_BAD_LEN: return "bad length";
    case FACE_REC_BAD_CRC: return "bad checksum";
    case FACE_REC_BAD_JPEG: return "not a baseline JPEG";
    case FACE_REC_OTHER_NODE: return "another pairing's";
    default: return "?";
    }
}

/* ---- NVS headroom ------------------------------------------------------- */

uint32_t face_nvs_entries(uint32_t len)
{
    /* A blob is split into chunks of at most one page's data (126 - 1 entries
     * for the page header's share): each chunk costs a header entry, and the
     * blob an index entry. */
    uint32_t data = (len + 31) / 32;
    uint32_t chunks = (data + 124) / 125;
    return data + chunks + 1;
}

static uint32_t reserve_entries(uint32_t total)
{
    uint32_t r = total * FACE_NVS_RESERVE_PCT / 100;
    return r < FACE_NVS_RESERVE_MIN ? FACE_NVS_RESERVE_MIN : r;
}

uint32_t face_nvs_cap(uint32_t total_entries)
{
    /* A third of the partition for one record: room beside the old one on a
     * roomy partition, in place on a small one, never into the reserve. */
    uint32_t r = reserve_entries(total_entries);
    if (total_entries <= r + 32) {
        return 0;
    }
    uint32_t entries = total_entries / 3;
    if (entries > total_entries - r) {
        entries = total_entries - r;
    }
    uint32_t chunks = (entries + 125) / 126;
    uint32_t bytes = entries > chunks + 1 ? (entries - chunks - 1) * 32 : 0;
    bytes = bytes > sizeof(face_rec_t) ? bytes - (uint32_t)sizeof(face_rec_t) : 0;
    return bytes > FACE_JPEG_MAX ? FACE_JPEG_MAX : bytes;
}

face_nvs_plan_t face_nvs_plan(uint32_t total_entries, uint32_t free_entries, uint32_t old_len, uint32_t new_len)
{
    uint32_t r = reserve_entries(total_entries);
    uint32_t need = face_nvs_entries(new_len);
    uint32_t old = old_len ? face_nvs_entries(old_len) : 0;
    if (free_entries >= need && free_entries - need >= r) {
        return FACE_NVS_BESIDE;
    }
    if (old && free_entries + old >= need && free_entries + old - need >= r) {
        return FACE_NVS_IN_PLACE;
    }
    return FACE_NVS_REFUSE;
}

/* ---- When to look ------------------------------------------------------- */

void face_sched_init(face_sched_t *s)
{
    memset(s, 0, sizeof(*s));
    s->retry_ms = FACE_RETRY_FIRST_MS;
}

void face_sched_on_session(face_sched_t *s, uint64_t generation, int64_t now_us)
{
    if (generation != s->session) {
        s->session = generation;
        if (s->owed) {
            s->next_us = now_us ? now_us : 1;
            s->retry_ms = FACE_RETRY_FIRST_MS;
        }
    }
}

void face_sched_on_look(face_sched_t *s, bool ok, int64_t now_us)
{
    if (ok) {
        s->owed = false;
        s->retry_ms = FACE_RETRY_FIRST_MS;
        s->next_us = 0;   /* nothing more until the next boot, selection or face.set */
        return;
    }
    s->next_us = now_us + (int64_t)s->retry_ms * 1000;
    s->retry_ms = s->retry_ms * 2 > FACE_RETRY_MAX_MS ? FACE_RETRY_MAX_MS : s->retry_ms * 2;
    if (s->next_us == 0) {
        s->next_us = 1;
    }
}

bool face_sched_wants(const face_sched_t *s, int64_t now_us, int64_t *until_us)
{
    int64_t until = 0;
    if (s->owed && now_us - s->owed_us < (int64_t)FACE_OWED_HOLD_MS * 1000) {
        until = s->owed_us + (int64_t)FACE_OWED_HOLD_MS * 1000;
    }
    if (until_us) {
        *until_us = until;
    }
    return until != 0;
}

bool face_sched_due(const face_sched_t *s, bool session_open, int64_t now_us)
{
    return session_open && s->next_us != 0 && now_us >= s->next_us;
}

void face_sched_now(face_sched_t *s, int64_t now_us)
{
    s->owed = true;
    s->owed_us = now_us;
    s->retry_ms = FACE_RETRY_FIRST_MS;
    s->next_us = now_us ? now_us : 1;
}

/* ---- The face choice ---------------------------------------------------- */

face_pick_t face_pick_after_store(face_pick_t pick)
{
    return pick == FACE_PICK_UNSET ? FACE_PICK_AVATAR : pick;
}

bool face_avatar_shown(face_pick_t pick, bool have_picture)
{
    return pick == FACE_PICK_AVATAR && have_picture;
}

/* ---- The Muse's name ---------------------------------------------------- */

static void name_trim(const char *in, char *out, size_t cap)
{
    /* Control characters dropped, runs of whitespace one space, the ends
     * trimmed, cut at a UTF-8 boundary to fit. */
    size_t o = 0;
    bool space = false;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        if (c < 0x20 || c == 0x7F) {
            if (c == '\t' || c == '\n' || c == '\r') {
                space = o > 0;
            }
            continue;
        }
        if (c == ' ') {
            space = o > 0;
            continue;
        }
        if (space) {
            if (o + 1 >= cap) {
                break;
            }
            out[o++] = ' ';
            space = false;
        }
        if (o + 1 >= cap) {
            break;
        }
        out[o++] = (char)c;
    }
    /* A cut in the middle of a character: back to its start. */
    if (o) {
        size_t start = o;
        while (start > 0 && ((unsigned char)out[start - 1] & 0xC0) == 0x80) {
            start--;
        }
        if (start > 0) {
            unsigned char lead = (unsigned char)out[start - 1];
            size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (lead >= 0xC0 && o - (start - 1) < need) {
                o = start - 1;
            }
        }
    }
    out[o] = '\0';
}

bool face_identity_name(const char *json, size_t len, char *out, size_t cap)
{
    if (!json || !len || !out || cap < 2) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return false;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *name = cJSON_GetObjectItem(cJSON_IsObject(result) ? result : root, "name");
    bool ok = false;
    if (cJSON_IsString(name) && name->valuestring) {
        name_trim(name->valuestring, out, cap);
        ok = out[0] != '\0';
    }
    cJSON_Delete(root);
    if (!ok) {
        out[0] = '\0';
    }
    return ok;
}

const char *face_label(const char *stored_name)
{
    return stored_name && stored_name[0] ? stored_name : FACE_NAME_DEFAULT;
}

/* ---- face.set ----------------------------------------------------------- */

bool face_set_parse(const cJSON *params, const char *own_path, uint32_t cap, uint32_t *byte_len,
                    char *etag, size_t etag_cap, const char **code, const char **message)
{
    *code = "invalid_params";
    if (etag && etag_cap) {
        etag[0] = '\0';
    }
    const cJSON *path = cJSON_GetObjectItem(params, "path");
    if (!cJSON_IsString(path) || !path->valuestring) {
        *message = "path is required";
        return false;
    }
    const char *p = path->valuestring;
    if (p[0] == '~' && p[1] == '/') {
        p += 2;
    }
    if (strcmp(p, own_path) != 0) {
        *message = "path must be this gadget's own face file, workspace/devices/<node_id>/face.jpg";
        return false;
    }
    const cJSON *len = cJSON_GetObjectItem(params, "byte_len");
    if (!cJSON_IsNumber(len) || len->valuedouble < 1 || len->valuedouble != (double)(int64_t)len->valuedouble) {
        *message = "byte_len is required: the file's size in bytes";
        return false;
    }
    if (len->valuedouble > (double)cap) {
        *code = "too_large";
        *message = "the JPEG is too large: make it smaller (lower quality) and call again";
        return false;
    }
    *byte_len = (uint32_t)len->valuedouble;
    const cJSON *e = cJSON_GetObjectItem(params, "etag");
    if (cJSON_IsString(e) && e->valuestring && etag && etag_cap) {
        copy_str(etag, etag_cap, e->valuestring);
    }
    return true;
}

/* ---- The scrim ---------------------------------------------------------- */

void face_scrim_rows(int height, const face_band_t *bands, int n, int feather, uint8_t *dark)
{
    int edge = height / 4;
    for (int y = 0; y < height; y++) {
        int d = 0;
        /* The edge gradients, top and bottom. */
        if (edge > 0) {
            int from_edge = y < height - 1 - y ? y : height - 1 - y;
            if (from_edge < edge) {
                d = FACE_SCRIM_EDGE * (edge - from_edge) / edge;
            }
        }
        for (int i = 0; i < n; i++) {
            int b;
            if (y >= bands[i].y0 && y < bands[i].y1) {
                b = FACE_SCRIM_BAND;
            } else {
                int dist = y < bands[i].y0 ? bands[i].y0 - y : y - (bands[i].y1 - 1);
                b = feather > 0 && dist < feather ? FACE_SCRIM_BAND * (feather - dist) / feather : 0;
            }
            if (b > d) {
                d = b;
            }
        }
        dark[y] = (uint8_t)(d > 255 ? 255 : d);
    }
}

static inline uint16_t swap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

void face_bake(uint16_t *px, int width, int height, const uint8_t *dark, bool round, int ring_px,
               bool byte_swapped)
{
    float cx = (width - 1) / 2.0f, cy = (height - 1) / 2.0f;
    float r_out = (width < height ? width : height) / 2.0f;
    float r_in = r_out - (float)ring_px;
    for (int y = 0; y < height; y++) {
        uint32_t keep = 255u - dark[y];
        uint16_t *row = px + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            bool in_ring;
            if (round) {
                float dx = x - cx, dy = y - cy;
                in_ring = dx * dx + dy * dy >= r_in * r_in;
            } else {
                in_ring = x < ring_px || y < ring_px || x >= width - ring_px || y >= height - ring_px;
            }
            if (in_ring) {
                row[x] = 0;
                continue;
            }
            if (keep == 255u) {
                continue;
            }
            uint16_t v = byte_swapped ? swap16(row[x]) : row[x];
            uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            r = (r * keep + 127) / 255;
            g = (g * keep + 127) / 255;
            b = (b * keep + 127) / 255;
            v = (uint16_t)((r << 11) | (g << 5) | b);
            row[x] = byte_swapped ? swap16(v) : v;
        }
    }
}

static double lin(double c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(double r, double g, double b)
{
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

int face_contrast_x100(uint32_t text_rgb, uint16_t bg565)
{
    double lt = luminance(((text_rgb >> 16) & 0xFF) / 255.0, ((text_rgb >> 8) & 0xFF) / 255.0,
                          (text_rgb & 0xFF) / 255.0);
    double lb = luminance(((bg565 >> 11) & 0x1F) / 31.0, ((bg565 >> 5) & 0x3F) / 63.0, (bg565 & 0x1F) / 31.0);
    double hi = lt > lb ? lt : lb, lo = lt > lb ? lb : lt;
    return (int)((hi + 0.05) / (lo + 0.05) * 100.0);
}

/* ---- Size: Large / Small ------------------------------------------------ */

const char *face_size_name(face_size_t size)
{
    return size == FACE_SIZE_SMALL ? "small" : "large";
}

/* a x num / den, rounded half up (a >= 0) */
static int scale_round(int a, int num, int den)
{
    return (int)(((int64_t)a * num + den / 2) / den);
}

void face_small_geom(int panel_w, int panel_h, face_small_geom_t *g)
{
    g->w = scale_round(panel_w, FACE_SMALL_NUM, FACE_SMALL_DEN);
    g->h = scale_round(panel_h, FACE_SMALL_NUM, FACE_SMALL_DEN);
    if (g->w < 1) g->w = 1;
    if (g->h < 1) g->h = 1;
    g->x0 = (panel_w - g->w) / 2;
    g->ear_y = scale_round(panel_h, FACE_SMALL_REF_EAR, FACE_SMALL_REF);
    g->ear_max = scale_round(g->h, FACE_SMALL_REF_MAX, FACE_SMALL_REF_W);
    g->ear_px = scale_round(g->w, FACE_SMALL_REF_PX, FACE_SMALL_REF_W);
    if (g->ear_px < 1) g->ear_px = 1;
}

size_t face_small_work_words(int panel_w)
{
    int sw = scale_round(panel_w, FACE_SMALL_NUM, FACE_SMALL_DEN);
    return (size_t)9 * (size_t)(sw > 0 ? sw : 1) + 128u;   /* two rows of sums, a row across, 32+64+32 bins */
}

static int hist_median(const uint32_t *hist, int bins, uint32_t n)
{
    uint32_t half = (n + 1) / 2, seen = 0;
    for (int i = 0; i < bins && n; i++) {
        seen += hist[i];
        if (seen >= half) {
            return i;
        }
    }
    return 0;
}

/* The picture's background, as the reference design finds it: the per-channel median of
 * its outer ring (r 166 .. 172.5 of 180; a rectangle's frame at the same
 * depth in from its nearest edge). */
static uint16_t ring_field(const uint16_t *px, int w, int h, bool round, bool byte_swapped, uint32_t *hist)
{
    memset(hist, 0, 128 * sizeof(uint32_t));
    int s = w < h ? w : h;
    int64_t in2 = (int64_t)332 * s * 332 * s, out2 = (int64_t)345 * s * 345 * s;   /* (2r x 360)^2 */
    int lo = scale_round(s, 15, 720), hi = scale_round(s, 14, 360);                  /* 7.5 .. 14 of 360 */
    uint32_t n = 0;
    for (int y = 0; y < h; y++) {
        const uint16_t *row = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            bool in;
            if (round) {
                int64_t dx = 2 * x + 1 - w, dy = 2 * y + 1 - h;
                int64_t d2 = (dx * dx + dy * dy) * 360 * 360;
                in = d2 >= in2 && d2 < out2;
            } else {
                int e = x < y ? x : y;
                if (w - 1 - x < e) e = w - 1 - x;
                if (h - 1 - y < e) e = h - 1 - y;
                in = e >= lo && e < hi;
            }
            if (!in) {
                continue;
            }
            uint16_t v = byte_swapped ? swap16(row[x]) : row[x];
            hist[v >> 11]++;
            hist[32 + ((v >> 5) & 63)]++;
            hist[96 + (v & 31)]++;
            n++;
        }
    }
    return (uint16_t)(hist_median(hist, 32, n) << 11 | hist_median(hist + 32, 64, n) << 5 |
                      hist_median(hist + 96, 32, n));
}

/* The reference design's step off the background: about 48 / 765 in
 * 8-bit terms. */
static bool off_field(uint16_t v, uint16_t field)
{
    int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
    int fr = field >> 11, fg = (field >> 5) & 63, fb = field & 31;
    return 2 * abs(r - fr) + abs(g - fg) + 2 * abs(b - fb) > 12;
}

void face_small_draw(uint16_t *px, int w, int h, bool round, bool byte_swapped, uint32_t *work,
                     face_small_t *out)
{
    face_small_geom_t g;
    face_small_geom(w, h, &g);
    const int sw = g.w, sh = g.h;
    uint32_t *acc[2] = { work, work + 3 * sw }, *across = work + 6 * sw, *hist = work + 9 * sw;
    uint16_t field = ring_field(px, w, h, round, byte_swapped, hist);
    memset(work, 0, (size_t)9 * sw * sizeof(uint32_t));
    /* The area average, in place. Across, input pixel i spans [i sw, (i+1) sw)
     * and output column j [j w, (j+1) w); down, input row y spans
     * [y sh, (y+1) sh) and output row k [k h, (k+1) h): an input pixel or row
     * meets at most two outputs, and an output pixel's weights add up to
     * w x h. Input rows are read in order; output row k, complete once its
     * last input row is in, is written to px's row-major small picture at
     * k x sw, which never reaches an input row still unread (sw sh <= w h). */
    const uint64_t total = (uint64_t)w * (uint64_t)h;
    for (int y = 0; y < h; y++) {
        const uint16_t *row = px + (size_t)y * w;
        memset(across, 0, (size_t)3 * sw * sizeof(uint32_t));
        for (int i = 0; i < w; i++) {
            uint16_t v = byte_swapped ? swap16(row[i]) : row[i];
            uint32_t r = v >> 11, gr = (v >> 5) & 63, b = v & 31;
            int a = i * sw, z = a + sw, j0 = a / w, j1 = (z - 1) / w;
            uint32_t w0 = (uint32_t)((j0 == j1 ? z : (j0 + 1) * w) - a);
            uint32_t *q = across + 3 * j0;
            q[0] += w0 * r;
            q[1] += w0 * gr;
            q[2] += w0 * b;
            if (j1 != j0) {
                uint32_t w1 = (uint32_t)sw - w0;
                q[3] += w1 * r;
                q[4] += w1 * gr;
                q[5] += w1 * b;
            }
        }
        int a = y * sh, z = a + sh, k0 = a / h, k1 = (z - 1) / h;
        uint32_t v0 = (uint32_t)((k0 == k1 ? z : (k0 + 1) * h) - a), v1 = (uint32_t)sh - v0;
        uint32_t *p0 = acc[k0 & 1], *p1 = acc[k1 & 1];
        for (int c = 0; c < 3 * sw; c++) {
            p0[c] += v0 * across[c];
            if (k1 != k0) {
                p1[c] += v1 * across[c];
            }
        }
        if (z >= (k0 + 1) * h && k0 < sh) {
            uint16_t *o = px + (size_t)k0 * sw;
            for (int j = 0; j < sw; j++) {
                uint32_t r = (uint32_t)((p0[3 * j] + total / 2) / total);
                uint32_t gr = (uint32_t)((p0[3 * j + 1] + total / 2) / total);
                uint32_t b = (uint32_t)((p0[3 * j + 2] + total / 2) / total);
                uint16_t v = (uint16_t)(r << 11 | gr << 5 | b);
                o[j] = byte_swapped ? swap16(v) : v;
            }
            memset(p0, 0, (size_t)3 * sw * sizeof(uint32_t));
        }
    }
    /* The ear row: the first small row with more than ear_px pixels off the
     * background, at most ear_max down. */
    int ear = g.ear_max;
    for (int k = 0; k < g.ear_max && k < sh; k++) {
        const uint16_t *r = px + (size_t)k * sw;
        int off = 0;
        for (int j = 0; j < sw; j++) {
            off += off_field(byte_swapped ? swap16(r[j]) : r[j], field);
        }
        if (off > g.ear_px) {
            ear = k;
            break;
        }
    }
    int top = g.ear_y - ear;
    if (top < 0) {
        top = 0;   /* (never at the reference proportions: ear_y - ear_max is 0.30 of the height) */
    }
    /* Into place, last row first: row k lands at (top + k) w + x0, at or past
     * the end of every small row still to move (k sw). */
    for (int k = sh - 1; k >= 0; k--) {
        int y = top + k;
        if (y < h) {
            memmove(px + (size_t)y * w + g.x0, px + (size_t)k * sw, (size_t)sw * sizeof(uint16_t));
        }
    }
    for (int y = 0; y < h; y++) {
        uint16_t *row = px + (size_t)y * w;
        if (y < top || y >= top + sh) {
            memset(row, 0, (size_t)w * sizeof(uint16_t));
        } else {
            memset(row, 0, (size_t)g.x0 * sizeof(uint16_t));
            memset(row + g.x0 + sw, 0, (size_t)(w - g.x0 - sw) * sizeof(uint16_t));
        }
    }
    if (out) {
        out->top = top;
        out->ear = ear;
        out->field = field;
    }
}

int face_small_bands(face_band_t *bands, int n, int top)
{
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (bands[i].y1 > top) {
            bands[kept++] = bands[i];
        }
    }
    return kept;
}
