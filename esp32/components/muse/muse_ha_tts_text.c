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

/* Decides what the header held in m->head is: WAV (only at the very start),
 * an ID3 tag to skip, or the audio, which has to open with an MP3 frame sync
 * (11 set bits). At the end of the download (`final`) the header may be short. */
static muse_ha_tts_mp3_result_t decide(muse_ha_tts_mp3_t *m, bool final, muse_ha_tts_emit_t emit, void *ctx)
{
    bool at_start = !m->started;
    m->started = true;
    if (at_start && m->have >= 4 && !memcmp(m->head, "RIFF", 4)) {
        return MUSE_HA_TTS_MP3_WAV;
    }
    if (m->have >= 3 && !memcmp(m->head, "ID3", 3)) {
        if (final || m->have < sizeof(m->head)) {
            return MUSE_HA_TTS_MP3_NOT_MP3;   /* a tag cut short: no audio after it */
        }
        m->skip = muse_ha_tts_id3_size(m->head, m->have) - m->have;   /* its header is in */
        m->have = 0;
        return MUSE_HA_TTS_MP3_OK;
    }
    if (m->have < 2 || m->head[0] != 0xff || (m->head[1] & 0xe0) != 0xe0) {
        return MUSE_HA_TTS_MP3_NOT_MP3;
    }
    m->synced = true;
    size_t n = m->have;
    m->have = 0;
    return emit(ctx, m->head, n) ? MUSE_HA_TTS_MP3_OK : MUSE_HA_TTS_MP3_STOPPED;
}

muse_ha_tts_mp3_result_t muse_ha_tts_mp3_feed(muse_ha_tts_mp3_t *m, const uint8_t *data, size_t len,
                                              muse_ha_tts_emit_t emit, void *ctx)
{
    while (len) {
        if (m->synced) {
            return emit(ctx, data, len) ? MUSE_HA_TTS_MP3_OK : MUSE_HA_TTS_MP3_STOPPED;
        }
        if (m->skip) {
            size_t drop = m->skip < len ? m->skip : len;
            m->skip -= drop;
            data += drop;
            len -= drop;
            continue;
        }
        size_t take = sizeof(m->head) - m->have < len ? sizeof(m->head) - m->have : len;
        memcpy(m->head + m->have, data, take);
        m->have += take;
        data += take;
        len -= take;
        if (m->have == sizeof(m->head)) {
            muse_ha_tts_mp3_result_t r = decide(m, false, emit, ctx);
            if (r != MUSE_HA_TTS_MP3_OK) {
                return r;
            }
        }
    }
    return MUSE_HA_TTS_MP3_OK;
}

muse_ha_tts_mp3_result_t muse_ha_tts_mp3_end(muse_ha_tts_mp3_t *m, muse_ha_tts_emit_t emit, void *ctx)
{
    if (!m->synced && !m->skip && m->have) {
        muse_ha_tts_mp3_result_t r = decide(m, true, emit, ctx);
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
