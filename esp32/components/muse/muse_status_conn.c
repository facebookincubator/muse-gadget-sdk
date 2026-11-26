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

#include "muse_status_conn.h"

#include <string.h>

#include "muse_text.h"

/* Appends s to out (n bytes, NUL-terminated), cut at a whole UTF-8 character. */
static void append(char *out, size_t n, const char *s)
{
    size_t at = strlen(out);
    if (at + 1 >= n) {
        return;
    }
    size_t len = strlen(s);
    if (len > n - 1 - at) {
        len = n - 1 - at;
        while (len && ((unsigned char)s[len] & 0xC0) == 0x80) {
            len--;   /* never half a character */
        }
    }
    memcpy(out + at, s, len);
    out[at + len] = 0;
}

/* name with the fonts' stand-ins into buf; false if nothing showable is left. */
static bool showable(const char *name, char *buf, size_t cap)
{
    if (!name || !name[0]) {
        return false;
    }
    buf[0] = 0;
    append(buf, cap, name);
    muse_text_to_ascii(buf, cap);
    for (const char *s = buf; *s; s++) {
        if (*s != ' ') {
            return true;
        }
    }
    return false;
}

void muse_status_conn_text(const muse_conn_info_t *in, char *out, size_t n)
{
    if (!n) {
        return;
    }
    out[0] = 0;
    char name[72];
    switch (in->kind) {
    case MUSE_CONN_APP:
        append(out, n, "Connected: ");
        /* Only the name the gadget reads itself; "phone" while that
         * read is pending or after it failed (client_name is reserved). */
        append(out, n, showable(in->gap_name, name, sizeof(name)) ? name : "phone");
        break;
    case MUSE_CONN_WIFI:
        append(out, n, "Connected: ");
        append(out, n, showable(in->ssid, name, sizeof(name)) ? name : "Wi-Fi");
        break;
    case MUSE_CONN_BT_ONLY:
        append(out, n, "Bluetooth only");
        break;
    default:
        append(out, n, "Not connected");
        break;
    }
}

void muse_status_detail_text(const muse_conn_info_t *in, char *out, size_t n)
{
    if (!n) {
        return;
    }
    out[0] = 0;
    char app[48], ver[48];
    if (in->kind == MUSE_CONN_WIFI) {
        char num[16];
        int r = in->rssi < -999 ? -999 : in->rssi > 999 ? 999 : in->rssi;
        unsigned a = (unsigned)(r < 0 ? -r : r);
        size_t k = sizeof(num) - 1;
        num[k] = 0;
        do {
            num[--k] = (char)('0' + a % 10);
            a /= 10;
        } while (a);
        if (r < 0) {
            num[--k] = '-';
        }
        append(out, n, "Wi-Fi: ");
        append(out, n, in->ip && in->ip[0] ? in->ip : "no address");
        append(out, n, ", ");
        append(out, n, num + k);
        append(out, n, " dBm");
    } else if (in->kind == MUSE_CONN_APP || in->kind == MUSE_CONN_BT_ONLY) {
        bool has_app = showable(in->app_name, app, sizeof(app));
        bool has_ver = showable(in->app_version, ver, sizeof(ver));
        if (!has_app && !has_ver) {
            append(out, n, "App version unknown");
            return;
        }
        append(out, n, has_app ? app : "App");
        append(out, n, ": ");
        append(out, n, has_ver ? ver : "version unknown");
    }
}

/* Byte length of text's longest start of whole characters that fits max_w, at
 * least one character (a line always moves on); scratch holds the trial. */
static size_t fit_prefix(const char *text, int max_w, int (*measure)(const char *s, void *ctx), void *ctx,
                         char *scratch, size_t n)
{
    size_t len = strlen(text), best = 0, at = 0;
    if (len > n - 1) {
        len = n - 1;
    }
    while (at < len) {
        size_t next = at + 1;
        while (next < len && ((unsigned char)text[next] & 0xC0) == 0x80) {
            next++;
        }
        if (next > len) {
            break;
        }
        memcpy(scratch, text, next);
        scratch[next] = 0;
        if (measure(scratch, ctx) > max_w) {
            break;
        }
        best = at = next;
    }
    if (!best && len) {
        best = 1;   /* not even one fits: one anyway */
        while (best < len && ((unsigned char)text[best] & 0xC0) == 0x80) {
            best++;
        }
    }
    return best;
}

/* s[0..len) into out (n bytes) without trailing spaces. */
static void put_line(char *out, size_t n, const char *s, size_t len)
{
    while (len && s[len - 1] == ' ') {
        len--;
    }
    if (len > n - 1) {
        len = n - 1;
        while (len && ((unsigned char)s[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    memcpy(out, s, len);
    out[len] = 0;
}

int muse_status_wrap(const char *text, int w1, int w2, int (*measure)(const char *s, void *ctx), void *ctx,
                     char *line1, char *line2, size_t n)
{
    if (!n) {
        return 0;
    }
    line1[0] = line2[0] = 0;
    append(line1, n, text);
    if (measure(line1, ctx) <= w1) {
        return MUSE_STATUS_WRAP_ONE;
    }
    char rest[MUSE_STATUS_LINE_MAX];
    size_t cut = fit_prefix(text, w1, measure, ctx, rest, sizeof(rest));
    /* Whole words where a space lets line 1 end in one ("Connected:" / "Tester's
     * iPhone"), when line 2 then fits; else as many whole characters as fit. */
    size_t word = cut;
    if (text[cut] != ' ') {
        while (word && text[word - 1] != ' ') {
            word--;
        }
    }
    size_t head = word;
    while (head && text[head - 1] == ' ') {
        head--;
    }
    for (int pass = head ? 0 : 1; pass < 2; pass++) {
        size_t brk = pass == 0 ? word : cut;
        const char *tail = text + brk;
        while (*tail == ' ') {
            tail++;
        }
        put_line(line1, n, text, brk);
        put_line(line2, n, tail, strlen(tail));
        if (measure(line2, ctx) <= w2) {
            return MUSE_STATUS_WRAP_TWO;
        }
        if (pass == 1) {
            /* Still too long: line 2 cut at whole characters (no "..."). */
            size_t keep = fit_prefix(tail, w2, measure, ctx, rest, sizeof(rest));
            put_line(line2, n, tail, keep);
            if (measure(line2, ctx) > w2) {
                line2[0] = 0;
            }
        }
    }
    return MUSE_STATUS_WRAP_CUT;
}

void muse_status_tap(muse_status_tap_t *t, int64_t now_ms, bool has_detail)
{
    bool shown = muse_status_detail_shown(t, now_ms);
    t->detail = has_detail && !shown;
    t->since_ms = now_ms;
}

bool muse_status_detail_shown(muse_status_tap_t *t, int64_t now_ms)
{
    if (t->detail && now_ms - t->since_ms >= MUSE_STATUS_DETAIL_MS) {
        t->detail = false;
    }
    return t->detail;
}
