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

/*
 * protocols/test-vectors/mg-subtitle-v1.json
 * through the firmware: mg_proto's parsing (ops.subtitle) and the model
 * (muse_subs_core) as muse_subs.c joins them. Reads a script (test_muse_subs.py
 * writes it from the vectors), one step a line:
 *   M <hex>   a message on Control        -> "OK <fields>" or "ERR"
 *   R <n>     a frame header received (16-bit number): the only step that extends the count
 *   F <n>     a frame received earlier shown, with the number its receipt gave it (as muse_video.c's
 *             number32: showing it is no receipt)
 *   S         a stream start (frame numbers start again)
 *   O         a new reveal case: a fresh model, subtitle mode off (its state message turns it on)
 * and after each step "T <revealed text as hex>".
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_proto.h"
#include "mgcommands.h"
#include "muse_subs_core.h"

static muse_subs_t S;
static uint32_t ext;
static bool ext_reset = true;
static uint32_t ext_of[65536];   /* each 16-bit number's extended number, as its latest receipt gave it */
static bool got[65536];
static char last_err;
/* One message's fields: room for a whole words message (480 bytes of words at two hex digits each, and a show_at per
 * word); add() never writes past it, and a line that didn't fit ends "CUT" (no vector's line matches it). */
static char fields[4096];
static size_t fields_len;

static void add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void add(const char *fmt, ...)
{
    if (fields_len >= sizeof(fields) - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(fields + fields_len, sizeof(fields) - fields_len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(fields) - fields_len) {
        static const char cut[] = " CUT";
        fields_len = sizeof(fields) - sizeof(cut);
        memcpy(fields + fields_len, cut, sizeof(cut));
        fields_len = sizeof(fields) - 1;
    } else {
        fields_len += (size_t)n;
    }
}

static bool f_notify(void *ctx, mg_ch_t ch, const uint8_t *d, size_t n)
{
    (void)ctx;
    (void)ch;
    if (n >= 2 && d[0] == mg_command_error && d[1] == mg_command_subtitle) {
        last_err = 1;
    }
    return true;
}
static void f_disconnect(void *ctx) { (void)ctx; }
static void f_capture(void *ctx, bool start, const mg_audio_format_t *fmt) { (void)ctx; (void)start; (void)fmt; }
static void f_mic_gain(void *ctx, uint8_t g) { (void)ctx; (void)g; }
static void f_settings(void *ctx) { (void)ctx; }
static uint32_t f_now(void *ctx) { (void)ctx; return 0; }

static void hexs(const uint8_t *b, int n, char *out)
{
    for (int i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", b[i]);
    }
    out[2 * n] = 0;
}

/* As muse_subs_event does. */
static void f_subtitle(void *ctx, const mg_subs_event_t *ev)
{
    (void)ctx;
    fields_len = 0;
    fields[0] = 0;
    add("%u", ev->sub);
    switch (ev->sub) {
    case mg_subtitle_state:
        add(" active=%u timing=%u", ev->active, ev->timing);
        muse_subs_state(&S, ev->active, ev->timing ? MUSE_SUBS_MS : MUSE_SUBS_FRAME);
        break;
    case mg_subtitle_words: {
        add(" reply=%u flags=%u offset=%u", ev->reply_id, ev->flags, ev->char_offset);
        muse_subs_in_word_t w[MG_SUBS_MAX_WORDS];
        for (int i = 0; i < ev->count; i++) {
            char h[600];
            hexs(ev->words[i].utf8, ev->words[i].len, h);
            add(" %u:%s", ev->words[i].show_at, h);
            w[i].show_at = ev->words[i].show_at;
            w[i].utf8 = ev->words[i].utf8;
            w[i].len = ev->words[i].len;
        }
        muse_subs_words(&S, ev->reply_id, ev->flags, ev->char_offset, w, ev->count, 0);
        break;
    }
    case mg_subtitle_reply_end:
        add(" reply=%u show_at=%u", ev->reply_id, ev->show_at);
        muse_subs_reply_end(&S, ev->reply_id, ev->show_at, 0);
        break;
    case mg_subtitle_clear:
        add(" reply=%u reason=%u", ev->reply_id, ev->reason);
        muse_subs_clear(&S, ev->reply_id);
        break;
    case mg_subtitle_clock:
        add(" reply=%u playout=%u", ev->reply_id, ev->playout_ms);
        muse_subs_clock(&S, ev->reply_id, ev->playout_ms, 0);
        break;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        return 2;
    }
    static const mg_subs_caps_t caps = { .flags = 0x0c, .lines = 2, .chars_per_line = 26, .max_words_bytes = 480 };
    mg_proto_config_t cfg = { .subtitles = &caps };
    mg_proto_ops_t ops = { .notify = f_notify, .disconnect = f_disconnect, .capture = f_capture,
                           .mic_gain = f_mic_gain, .settings_changed = f_settings, .now_ms = f_now,
                           .subtitle = f_subtitle };
    static mg_proto_t p;
    mg_proto_init(&p, &cfg, &ops, NULL);
    mg_proto_connect(&p);
    mg_proto_subscribe(&p, MG_CH_CONTROL, true);
    mg_proto_subscribe(&p, MG_CH_DATA, true);
    muse_subs_init(&S);
    char line[4096];
    bool locked = false;   /* the passcode lock screen is up: muse_subs.c draws nothing over it (render_locked) */
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'O') {   /* a new case with subtitle mode off: only a state message turns it on */
            muse_subs_init(&S);
            locked = false;
            ext_reset = true;
            memset(got, 0, sizeof(got));
            continue;
        }
        if (line[0] == 'M') {
            uint8_t b[2048];
            size_t n = 0;
            for (const char *h = line + 2; h[0] && h[1]; h += 2) {
                unsigned v;
                sscanf(h, "%2x", &v);
                b[n++] = (uint8_t)v;
            }
            last_err = 0;
            fields[0] = 0;
            fields_len = 0;
            mg_proto_write(&p, MG_CH_CONTROL, b, n);
            printf("%s %s\n", last_err ? "ERR" : "OK", fields);
        } else if (line[0] == 'R') {
            uint16_t n16 = (uint16_t)atoi(line + 2);
            ext_of[n16] = muse_subs_extend_frame(&ext, &ext_reset, n16);
            got[n16] = true;
        } else if (line[0] == 'F') {
            uint16_t n16 = (uint16_t)atoi(line + 2);
            if (!got[n16]) {
                printf("BAD frame %u shown, never received\n", n16);
                return 3;
            }
            muse_subs_frame(&S, ext_of[n16]);
        } else if (line[0] == 'S') {
            ext_reset = true;
            memset(got, 0, sizeof(got));
        } else if (line[0] == 'L' || line[0] == 'U') {
            locked = line[0] == 'L';   /* the user's own lock and unlock: no subtitle message changes it */
            muse_subs_lock(&S, locked);
        } else {
            printf("BAD step %s\n", line);
            return 3;
        }
        muse_subs_update(&S, 0);
        char t[2048], h[4100];
        int n = locked ? 0 : muse_subs_revealed_text(&S, t, sizeof(t));
        hexs((const uint8_t *)t, n, h);
        printf("T %s\n", h);
    }
    fclose(f);
    return 0;
}
