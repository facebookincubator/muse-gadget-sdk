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

#include "muse_ha_tts_text.h"

#include <stdio.h>
#include <string.h>

/* "[label](target)" at s: the label's length, and the whole link's in *whole. 0 if not a link. */
static size_t link_at(const char *s, size_t *whole)
{
    const char *close = strchr(s + 1, ']');
    if (!close || close[1] != '(' || memchr(s + 1, '\n', close - s - 1)) {
        return 0;
    }
    const char *end = strchr(close + 2, ')');
    if (!end || memchr(close + 2, ' ', end - close - 2) || memchr(close + 2, '\n', end - close - 2)) {
        return 0;
    }
    *whole = end + 1 - s;
    return close - s - 1;
}

void muse_ha_tts_clean(char *text)
{
    char *out = text;
    for (char *in = text; *in;) {
        size_t whole, label = *in == '[' ? link_at(in, &whole) : 0;
        if (label) {
            /* The label stays where it was, so moving it down never overruns. */
            memmove(out, in + 1, label);
            char *stop = out + label;
            for (char *p = out; p < stop; p++) {
                if (*p != '*' && *p != '`') {
                    *out++ = *p;
                }
            }
            in += whole;
            continue;
        }
        char c = *in++;
        if (c == '*' || c == '#' || c == '`') {
            continue;
        }
        *out++ = c == '\n' || c == '\r' ? ' ' : c;
    }
    *out = '\0';
}

bool muse_ha_tts_speech_append(muse_ha_tts_speech_t *s, const char *text, size_t max,
                               void *(*grow)(void *, size_t))
{
    size_t add = strlen(text);
    if (s->cut || !max) {
        s->cut = s->cut || add;
        return !add;
    }
    size_t want = s->len + add + 1;
    if (want > s->cap) {
        size_t cap = s->cap ? s->cap : 256;
        while (cap < want && cap < max) {
            cap *= 2;
        }
        if (cap > max) {
            cap = max;
        }
        char *buf = grow(s->buf, cap);
        if (buf) {
            s->buf = buf;
            s->cap = cap;
        } else if (!s->buf) {
            s->cut = true;
            return false;
        }
    }
    size_t fit = s->cap - 1 - s->len;
    if (add > fit) {
        /* Back off to a character boundary: a continuation byte is 10xxxxxx. */
        while (fit && ((unsigned char)text[fit] & 0xc0) == 0x80) {
            fit--;
        }
        add = fit;
        s->cut = true;
    }
    memcpy(s->buf + s->len, text, add);
    s->len += add;
    s->buf[s->len] = '\0';
    return !s->cut;
}

void muse_ha_tts_speech_free(muse_ha_tts_speech_t *s, void (*release)(void *))
{
    if (s->buf) {
        release(s->buf);
    }
    *s = (muse_ha_tts_speech_t){ 0 };
}

size_t muse_ha_tts_id3_size(const uint8_t *data, size_t len)
{
    if (len < 10 || memcmp(data, "ID3", 3)) {
        return 0;
    }
    /* The size is syncsafe: seven bits a byte, and it leaves out the header
     * and, if the flags say there is one, the footer. */
    size_t size = (size_t)(data[6] & 0x7f) << 21 | (size_t)(data[7] & 0x7f) << 14 |
                  (size_t)(data[8] & 0x7f) << 7 | (data[9] & 0x7f);
    return 10 + size + (data[5] & 0x10 ? 10 : 0);
}

/* Passes data on, dropping what's left of the ID3 tag first, and checking that
 * the audio after it opens with an MP3 frame sync: 11 set bits. */
static muse_ha_tts_mp3_result_t pass(muse_ha_tts_mp3_t *m, const uint8_t *data, size_t len,
                                     muse_ha_tts_emit_t emit, void *ctx)
{
    size_t drop = m->skip < len ? m->skip : len;
    m->skip -= drop;
    data += drop;
    len -= drop;
    if (len && !m->synced) {
        if (!m->held) {
            m->first = *data++;
            m->held = true;
            if (!--len) {
                return MUSE_HA_TTS_MP3_OK;   /* the second byte decides */
            }
        }
        if (m->first != 0xff || (*data & 0xe0) != 0xe0) {
            return MUSE_HA_TTS_MP3_NOT_MP3;
        }
        m->synced = true;
        if (!emit(ctx, &m->first, 1)) {
            return MUSE_HA_TTS_MP3_STOPPED;
        }
    }
    if (len && !emit(ctx, data, len)) {
        return MUSE_HA_TTS_MP3_STOPPED;
    }
    return MUSE_HA_TTS_MP3_OK;
}

/* The first 10 bytes are in: decides what the audio is, then passes them on. */
static muse_ha_tts_mp3_result_t start(muse_ha_tts_mp3_t *m, muse_ha_tts_emit_t emit, void *ctx)
{
    m->started = true;
    if (m->have >= 4 && !memcmp(m->head, "RIFF", 4)) {
        return MUSE_HA_TTS_MP3_WAV;
    }
    m->skip = muse_ha_tts_id3_size(m->head, m->have);
    return pass(m, m->head, m->have, emit, ctx);
}

muse_ha_tts_mp3_result_t muse_ha_tts_mp3_feed(muse_ha_tts_mp3_t *m, const uint8_t *data, size_t len,
                                              muse_ha_tts_emit_t emit, void *ctx)
{
    if (!m->started) {
        size_t take = sizeof(m->head) - m->have < len ? sizeof(m->head) - m->have : len;
        memcpy(m->head + m->have, data, take);
        m->have += take;
        data += take;
        len -= take;
        if (m->have < sizeof(m->head)) {
            return MUSE_HA_TTS_MP3_OK;
        }
        muse_ha_tts_mp3_result_t r = start(m, emit, ctx);
        if (r != MUSE_HA_TTS_MP3_OK) {
            return r;
        }
    }
    return len ? pass(m, data, len, emit, ctx) : MUSE_HA_TTS_MP3_OK;
}

muse_ha_tts_mp3_result_t muse_ha_tts_mp3_end(muse_ha_tts_mp3_t *m, muse_ha_tts_emit_t emit, void *ctx)
{
    if (!m->started && m->have) {
        muse_ha_tts_mp3_result_t r = start(m, emit, ctx);
        if (r != MUSE_HA_TTS_MP3_OK) {
            return r;
        }
    }
    return m->synced ? MUSE_HA_TTS_MP3_OK : MUSE_HA_TTS_MP3_NOT_MP3;   /* no audio came */
}

void muse_ha_tts_base(char *out, size_t cap, const char *base)
{
    if (!cap) {
        return;
    }
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') {
        n--;
    }
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(out, base, n);
    out[n] = '\0';
}

int muse_ha_tts_audio_url(char *out, size_t cap, const char *base, const char *path, const char *url)
{
    int n;
    if (path && path[0] == '/') {
        n = snprintf(out, cap, "%s%s", base, path);
    } else if (url && url[0]) {
        n = snprintf(out, cap, "%s", url);
    } else {
        return 0;
    }
    return n > 0 && (size_t)n < cap;
}
