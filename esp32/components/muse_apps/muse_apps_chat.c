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
 * The API's chat_* group: following Muse's chat and sending to it over Link's
 * session. chat_open subscribes (POST /chat/subscribe, NDJSON) and fetches the
 * latest message (GET /chat/history?limit=1, one message so the reply fits a
 * frame); the gadget parses the VM's formats, keeps the last 8 messages and
 * tells the app with EV_CHAT. It reconnects by itself, with backoff.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "muse_apps_priv.h"
#include "muse_apps_vm.h"
#include "muse_link.h"

/* ---- The chat's formats and messages: plain C from here to "The chat for apps" ----
 * tests/test_apps_vm.py compiles this part on the host. The VM, not the
 * gadget, defines the formats: parse defensively, skip what isn't known.
 *
 * A subscription line: {"event": NAME, "payload": {...}, "seq": N, "type": "event"}.
 * History: {"ok": true, "result": {"chat_events": [{"role", "display_text",
 * "event_name", "message_id"}, ...]}}. The parser streams: it keeps only the
 * fields the app needs, so a line longer than a frame (the subscription's
 * opening approvals.snapshot, 17 KB or more) passes through unbuffered. */

#define CHAT_MESSAGES 8         /* limits.chat_messages */
#define CHAT_TEXT_BYTES 4096    /* limits.chat_text_bytes */

/* What one event or history entry says, as far as the app cares. */
typedef struct {
    char event[32];             /* "event", or "event_name" */
    char type[16];
    char role[16];
    char id[72];                /* "message_id", else "id" */
    char activity[24];          /* "activity_code" */
    char status[24];
    uint64_t seq;
    int ready;                  /* "display_text_ready": -1 when absent */
    bool id_is_message_id;
    int text_rank;              /* 0 none, 1 "text", 2 "content", 3 "display_text" */
    bool text_cut;
    size_t text_len;
    char text[CHAT_TEXT_BYTES + 1];
} chat_row_t;

typedef void (*chat_row_fn)(void *ctx, const chat_row_t *row);

enum {
    K_NONE, K_EVENT, K_TYPE, K_SEQ, K_PAYLOAD, K_ROLE, K_MESSAGE_ID, K_ID,
    K_TEXT, K_CONTENT, K_DISPLAY_TEXT,   /* in rank order */
    K_ACTIVITY, K_STATUS, K_READY, K_CHAT_EVENTS,
};

typedef struct {
    bool lines;                 /* NDJSON: each line a record; else one document with chat_events */
    chat_row_t *row;
    chat_row_fn fn;
    void *ctx;
    int depth;                  /* containers open */
    uint64_t arrays;            /* bit d: the container at depth d (< 64) is an array */
    int record, payload, list;  /* depths of the record, its payload, and chat_events; 0 none */
    bool want_key;              /* the next string is a key */
    int key;                    /* the key whose value comes next */
    bool in_str, str_key;
    int esc;                    /* 0; 1 after a backslash; 2 to 5 in \u's digits */
    uint32_t u, hi;             /* \u's value; a high surrogate waiting for its pair */
    char keybuf[24];
    size_t key_len;
    bool key_long;
    char *dst;                  /* where a wanted string goes, or NULL */
    size_t dst_cap, dst_len;
    bool dst_cut;
    bool in_scalar;
    int scalar_key;
    char scalar[24];
    size_t scalar_len;
} chat_parser_t;

static void chat_parser_init(chat_parser_t *p, bool lines, chat_row_t *row, chat_row_fn fn, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->lines = lines;
    p->row = row;
    p->fn = fn;
    p->ctx = ctx;
}

/* n, less a last character cut short. */
static size_t utf8_whole(const char *s, size_t n)
{
    if (!n) {
        return 0;
    }
    size_t start = n - 1;
    while (start > 0 && n - start < 4 && ((unsigned char)s[start] & 0xC0) == 0x80) {
        start--;
    }
    unsigned char lead = (unsigned char)s[start];
    size_t need = lead < 0xC0 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    return n - start < need ? start : n;
}

static bool top_is_object(const chat_parser_t *p)
{
    return p->depth > 0 && p->depth < 64 && !(p->arrays >> p->depth & 1);
}

static int key_id(const chat_parser_t *p)
{
    static const struct {
        const char *name;
        int key;
    } fields[] = {
        {"role", K_ROLE}, {"message_id", K_MESSAGE_ID}, {"id", K_ID}, {"text", K_TEXT},
        {"content", K_CONTENT}, {"display_text", K_DISPLAY_TEXT}, {"activity_code", K_ACTIVITY},
        {"status", K_STATUS}, {"display_text_ready", K_READY},
    };
    const char *k = p->keybuf;
    if (!p->record) {
        return !p->lines && !strcmp(k, "chat_events") ? K_CHAT_EVENTS : K_NONE;
    }
    bool rec = p->depth == p->record;
    if (rec) {
        if (!strcmp(k, "event") || !strcmp(k, "event_name")) return K_EVENT;
        if (!strcmp(k, "type")) return K_TYPE;
        if (!strcmp(k, "seq")) return K_SEQ;
        if (!strcmp(k, "payload")) return K_PAYLOAD;
    }
    if (!rec && !(p->payload && p->depth == p->payload)) {
        return K_NONE;
    }
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (!strcmp(k, fields[i].name)) {
            return fields[i].key;
        }
    }
    return K_NONE;
}

static void string_start(chat_parser_t *p)
{
    p->in_str = true;
    p->esc = 0;
    p->hi = 0;
    p->str_key = p->want_key && top_is_object(p);
    p->want_key = false;
    p->dst = NULL;
    p->dst_len = 0;
    p->dst_cut = false;
    if (p->str_key) {
        p->key_len = 0;
        p->key_long = false;
        return;
    }
    chat_row_t *r = p->row;
    switch (p->key) {
    case K_EVENT: p->dst = r->event; p->dst_cap = sizeof(r->event) - 1; break;
    case K_TYPE: p->dst = r->type; p->dst_cap = sizeof(r->type) - 1; break;
    case K_ROLE: p->dst = r->role; p->dst_cap = sizeof(r->role) - 1; break;
    case K_ACTIVITY: p->dst = r->activity; p->dst_cap = sizeof(r->activity) - 1; break;
    case K_STATUS: p->dst = r->status; p->dst_cap = sizeof(r->status) - 1; break;
    case K_MESSAGE_ID:
        r->id_is_message_id = true;
        p->dst = r->id;
        p->dst_cap = sizeof(r->id) - 1;
        break;
    case K_ID:
        if (!r->id_is_message_id) {
            p->dst = r->id;
            p->dst_cap = sizeof(r->id) - 1;
        }
        break;
    case K_TEXT:
    case K_CONTENT:
    case K_DISPLAY_TEXT:
        if (p->key - K_TEXT + 1 >= r->text_rank) {
            r->text_rank = p->key - K_TEXT + 1;
            r->text_len = 0;
            r->text_cut = false;
            p->dst = r->text;
            p->dst_cap = CHAT_TEXT_BYTES;
        }
        break;
    default: break;
    }
}

static void string_out(chat_parser_t *p, const char *b, size_t n)
{
    if (p->str_key) {
        if (p->key_len + n >= sizeof(p->keybuf)) {
            p->key_long = true;
        } else {
            memcpy(p->keybuf + p->key_len, b, n);
            p->key_len += n;
        }
    } else if (p->dst && !p->dst_cut) {
        if (p->dst_len + n > p->dst_cap) {
            p->dst_cut = true;
        } else {
            memcpy(p->dst + p->dst_len, b, n);
            p->dst_len += n;
        }
    }
}

static void string_code(chat_parser_t *p, uint32_t c)
{
    char b[4];
    size_t n;
    if (c == '\n') {
        string_out(p, "\n", 1);
        return;
    }
    if (c == '\t') {
        c = ' ';
    }
    if (c < 0x20) {
        return;   /* other control characters show as nothing */
    }
    if (c < 0x80) {
        b[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        b[0] = (char)(0xC0 | c >> 6);
        b[1] = (char)(0x80 | (c & 63));
        n = 2;
    } else if (c < 0x10000) {
        b[0] = (char)(0xE0 | c >> 12);
        b[1] = (char)(0x80 | (c >> 6 & 63));
        b[2] = (char)(0x80 | (c & 63));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | c >> 18);
        b[1] = (char)(0x80 | (c >> 12 & 63));
        b[2] = (char)(0x80 | (c >> 6 & 63));
        b[3] = (char)(0x80 | (c & 63));
        n = 4;
    }
    string_out(p, b, n);
}

static void string_end(chat_parser_t *p)
{
    p->in_str = false;
    if (p->str_key) {
        p->keybuf[p->key_len] = '\0';
        p->key = p->key_long ? K_NONE : key_id(p);
        return;
    }
    if (p->dst) {
        if (p->dst_cut) {
            p->dst_len = utf8_whole(p->dst, p->dst_len);
        }
        p->dst[p->dst_len] = '\0';
        if (p->dst == p->row->text) {
            p->row->text_len = p->dst_len;
            p->row->text_cut = p->dst_cut;
        }
        p->dst = NULL;
    }
    p->key = K_NONE;
}

static int hex_value(unsigned char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void string_char(chat_parser_t *p, char ch)
{
    unsigned char c = (unsigned char)ch;
    if (p->esc == 1) {
        p->esc = 0;
        if (c == 'u') {
            p->esc = 2;
            p->u = 0;
            return;   /* a high surrogate may still wait for this one */
        }
        p->hi = 0;
        if (c == 'n') {
            string_out(p, "\n", 1);
        } else if (c == 't') {
            string_out(p, " ", 1);
        } else if (c != 'r' && c != 'b' && c != 'f') {
            string_out(p, &ch, 1);   /* \" \\ \/ */
        }
        return;
    }
    if (p->esc >= 2) {
        int v = hex_value(c);
        if (v < 0) {
            p->esc = 0;
            p->hi = 0;
            return;
        }
        p->u = p->u << 4 | (uint32_t)v;
        if (++p->esc < 6) {
            return;
        }
        p->esc = 0;
        uint32_t u = p->u;
        if (u >= 0xD800 && u < 0xDC00) {
            p->hi = u;
        } else if (u >= 0xDC00 && u < 0xE000) {
            if (p->hi) {
                string_code(p, 0x10000 + ((p->hi - 0xD800) << 10) + (u - 0xDC00));
            }
            p->hi = 0;
        } else {
            p->hi = 0;
            string_code(p, u);
        }
        return;
    }
    if (c == '\\') {
        p->esc = 1;
        return;
    }
    p->hi = 0;
    if (c == '"') {
        string_end(p);
    } else if (c >= 0x20) {
        string_out(p, &ch, 1);
    }
}

static void scalar_end(chat_parser_t *p)
{
    p->in_scalar = false;
    p->scalar[p->scalar_len] = '\0';
    if (p->scalar_key == K_SEQ) {
        uint64_t v = 0;
        for (const char *s = p->scalar; *s >= '0' && *s <= '9'; s++) {
            v = v * 10 + (uint64_t)(*s - '0');
        }
        p->row->seq = v;
    } else if (p->scalar_key == K_READY) {
        p->row->ready = p->scalar[0] == 't';
    }
    p->key = K_NONE;
}

static void open_container(chat_parser_t *p, bool array)
{
    int key = p->key;
    p->key = K_NONE;
    p->depth++;
    if (p->depth < 64) {
        if (array) {
            p->arrays |= 1ull << p->depth;
        } else {
            p->arrays &= ~(1ull << p->depth);
        }
    }
    p->want_key = !array;
    if (array) {
        if (key == K_CHAT_EVENTS && !p->list) {
            p->list = p->depth;
        }
    } else if (!p->record && (p->lines ? p->depth == 1 : p->list && p->depth == p->list + 1)) {
        p->record = p->depth;
        chat_row_t *r = p->row;
        memset(r, 0, offsetof(chat_row_t, text));
        r->text[0] = '\0';
        r->ready = -1;
    } else if (p->record && key == K_PAYLOAD && p->depth == p->record + 1) {
        p->payload = p->depth;
    }
}

static void close_container(chat_parser_t *p)
{
    if (p->depth <= 0) {
        return;
    }
    if (p->record && p->depth == p->record) {
        p->record = p->payload = 0;
        if (p->fn) {
            p->fn(p->ctx, p->row);
        }
    } else if (p->payload && p->depth == p->payload) {
        p->payload = 0;
    } else if (p->list && p->depth == p->list) {
        p->list = 0;
    }
    p->depth--;
    p->want_key = false;
    p->key = K_NONE;
}

/* Feeds the next bytes, in any pieces: a line may span frames. */
static void chat_parse(chat_parser_t *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (p->lines && c == '\n') {   /* a line ends: whatever is half read goes */
            chat_parser_init(p, p->lines, p->row, p->fn, p->ctx);
            continue;
        }
        if (p->in_str) {
            string_char(p, c);
            continue;
        }
        if (p->in_scalar) {
            if (c != ',' && c != '}' && c != ']' && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                if (p->scalar_len < sizeof(p->scalar) - 1) {
                    p->scalar[p->scalar_len++] = c;
                }
                continue;
            }
            scalar_end(p);
        }
        switch (c) {
        case '{': open_container(p, false); break;
        case '[': open_container(p, true); break;
        case '}':
        case ']': close_container(p); break;
        case ',':
            p->key = K_NONE;
            p->want_key = top_is_object(p);
            break;
        case '"': string_start(p); break;
        case ':':
        case ' ':
        case '\t':
        case '\r':
        case '\n': break;
        default:
            p->in_scalar = true;
            p->scalar_key = p->key;
            p->scalar[0] = c;
            p->scalar_len = 1;
            break;
        }
    }
}

/* "[<app> on the gadget] ", which chat_send puts in front of an app's
 * messages: its length, or 0. */
static size_t chat_app_prefix(const char *s, size_t n)
{
    static const char tail[] = " on the gadget]";
    const size_t k = sizeof(tail) - 1;
    if (!n || s[0] != '[') {
        return 0;
    }
    for (size_t i = 2; i + k <= n && i < 96 && s[i - 1] != '\n'; i++) {
        if (!memcmp(s + i, tail, k)) {
            size_t end = i + k;
            return end == n ? end : s[end] == ' ' ? end + 1 : 0;
        }
    }
    return 0;
}

/* Anything to show: a line that isn't an attachment and isn't blank. */
static bool chat_visible(const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        size_t end = i;
        while (end < n && s[end] != '\n') {
            end++;
        }
        if (end - i < 6 || memcmp(s + i, "[file:", 6)) {
            for (size_t k = i; k < end; k++) {
                if (s[k] != ' ' && s[k] != '\r') {
                    return true;
                }
            }
        }
        i = end + 1;
    }
    return false;
}

/* One line's text without the Markdown markers a gadget can't draw: ** and
 * ~~, backticks, and a link's (url), keeping its text. */
static size_t clean_inline(const char *s, size_t n, char *out)
{
    size_t o = 0, skip_from = (size_t)-1, skip_to = 0;
    for (size_t i = 0; i < n; i++) {
        if (i == skip_from) {   /* a link's "](url)" */
            i = skip_to;
            skip_from = (size_t)-1;
            continue;
        }
        char c = s[i];
        if ((c == '*' || c == '~') && i + 1 < n && s[i + 1] == c) {
            i++;
            continue;
        }
        if (c == '`') {
            continue;
        }
        if (skip_from == (size_t)-1 && (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '['))) {
            size_t open = c == '!' ? i + 1 : i, close = open + 1;
            while (close < n && s[close] != ']' && s[close] != '[') {
                close++;
            }
            if (close + 1 < n && s[close] == ']' && s[close + 1] == '(') {
                size_t paren = close + 2;
                while (paren < n && s[paren] != ')' && s[paren] != ' ') {
                    paren++;
                }
                if (paren < n && s[paren] == ')') {
                    skip_from = close;
                    skip_to = paren;
                    i = open;
                    continue;
                }
            }
        }
        out[o++] = c;
    }
    return o;
}

/* What the gadget shows of a message: no attachment lines ("[file:audio/wav
 * ...]", a voice note's recording), no code fences, headings' #, quotes' >
 * or the inline markers above; bullets as "- "; at most one blank line in a
 * row, none at either end. Never longer than the text: out has room for n. */
static size_t chat_clean(const char *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        size_t end = i;
        while (end < n && in[end] != '\n') {
            end++;
        }
        bool nl = end < n;
        size_t a = i;
        while (a < end && in[a] == ' ') {
            a++;
        }
        bool attachment = end - i >= 6 && !memcmp(in + i, "[file:", 6);
        bool fence = end - a >= 3 && !memcmp(in + a, "```", 3);
        if (!attachment && !fence) {
            memcpy(out + o, in + i, a - i);   /* its indent */
            o += a - i;
            size_t h = a;
            while (h < end && h - a < 6 && in[h] == '#') {
                h++;
            }
            if (h > a && h < end && in[h] == ' ') {
                a = h + 1;
            } else if (end - a >= 2 && in[a] == '>' && in[a + 1] == ' ') {
                a += 2;
            } else if (end - a >= 2 && (in[a] == '*' || in[a] == '+') && in[a + 1] == ' ') {
                out[o++] = '-';
                out[o++] = ' ';
                a += 2;
            }
            o += clean_inline(in + a, end - a, out + o);
            if (nl) {
                out[o++] = '\n';
            }
        }
        i = end + (nl ? 1 : 0);
    }
    size_t w = 0;
    for (size_t r = 0; r < o; r++) {
        char c = out[r];
        if (c == '\r') {
            continue;
        }
        if (c == '\n' && (w == 0 || (w >= 2 && out[w - 1] == '\n' && out[w - 2] == '\n'))) {
            continue;
        }
        out[w++] = c;
    }
    while (w && (out[w - 1] == '\n' || out[w - 1] == ' ')) {
        w--;
    }
    out[w] = '\0';
    return w;
}

/* The messages kept for the app, and what it's told about them. */
typedef struct {
    int32_t id;                 /* 0: free */
    char key[72];               /* the VM's message_id, or "" */
    int role;                   /* GADGET_ROLE_ */
    bool done, cut;
    bool told_text;             /* a CHAT_TEXT is out that the app hasn't read the text since */
    bool dirty;                 /* clean is out of date */
    bool history;               /* from GET /chat/history */
    uint32_t conn;              /* the subscription it came with */
    size_t raw_len, clean_len;
    char raw[CHAT_TEXT_BYTES + 1];
    char clean[CHAT_TEXT_BYTES + 1];
} chat_msg_t;

/* Tells the app (EV_CHAT): false if the event was dropped. */
typedef bool (*chat_post_fn)(void *ctx, int what, int value);

typedef struct {
    chat_msg_t msgs[CHAT_MESSAGES];
    int order[CHAT_MESSAGES];   /* slots, oldest first */
    int count;
    int32_t next_id;
    int busy;                   /* -1 until a status says */
    uint64_t last_seq;
    uint32_t conn;
    chat_post_fn post;
    void *post_ctx;
} chat_store_t;

static void chat_store_init(chat_store_t *s, int32_t next_id, chat_post_fn post, void *ctx)
{
    s->count = 0;
    for (int i = 0; i < CHAT_MESSAGES; i++) {
        s->msgs[i].id = 0;
    }
    s->next_id = next_id > 0 ? next_id : 1;
    s->busy = -1;
    s->last_seq = 0;
    s->conn = 0;
    s->post = post;
    s->post_ctx = ctx;
}

/* A new subscription: its seq starts again. */
static void chat_store_connected(chat_store_t *s)
{
    s->conn++;
    s->last_seq = 0;
}

static bool tell(chat_store_t *s, int what, int value)
{
    return s->post && s->post(s->post_ctx, what, value);
}

static void tell_text(chat_store_t *s, chat_msg_t *m)
{
    if (!m->told_text) {
        m->told_text = tell(s, GADGET_CHAT_TEXT, m->id);
    }
}

static chat_msg_t *chat_by_id(chat_store_t *s, int32_t id)
{
    for (int i = 0; id > 0 && i < CHAT_MESSAGES; i++) {
        if (s->msgs[i].id == id) {
            return &s->msgs[i];
        }
    }
    return NULL;
}

static chat_msg_t *chat_by_key(chat_store_t *s, const char *key)
{
    for (int i = 0; i < CHAT_MESSAGES; i++) {
        if (s->msgs[i].id && !strcmp(s->msgs[i].key, key)) {
            return &s->msgs[i];
        }
    }
    return NULL;
}

/* The newest message of Muse's still being written. */
static chat_msg_t *chat_last_open(chat_store_t *s)
{
    for (int i = s->count - 1; i >= 0; i--) {
        chat_msg_t *m = &s->msgs[s->order[i]];
        if (m->role == GADGET_ROLE_MUSE && !m->done) {
            return m;
        }
    }
    return NULL;
}

static int32_t chat_latest_id(const chat_store_t *s, int n)
{
    return n >= 0 && n < s->count ? s->msgs[s->order[s->count - 1 - n]].id : 0;
}

/* A history message goes before what this subscription brought, being older. */
static int history_pos(const chat_store_t *s)
{
    for (int i = 0; i < s->count; i++) {
        const chat_msg_t *m = &s->msgs[s->order[i]];
        if (m->conn == s->conn && !m->history) {
            return i;
        }
    }
    return s->count;
}

/* A message at pos (0 oldest, count newest); the oldest goes when all eight
 * are taken. NULL if it would be older than all of them. */
static chat_msg_t *chat_add(chat_store_t *s, int pos, int role, const char *key, bool history)
{
    int slot = -1;
    if (s->count == CHAT_MESSAGES) {
        if (pos == 0) {
            return NULL;
        }
        slot = s->order[0];
        memmove(s->order, s->order + 1, (CHAT_MESSAGES - 1) * sizeof(s->order[0]));
        s->count--;
        pos--;
    } else {
        for (int i = 0; i < CHAT_MESSAGES && slot < 0; i++) {
            if (!s->msgs[i].id) {
                slot = i;
            }
        }
    }
    memmove(s->order + pos + 1, s->order + pos, (size_t)(s->count - pos) * sizeof(s->order[0]));
    s->order[pos] = slot;
    s->count++;
    chat_msg_t *m = &s->msgs[slot];
    m->id = s->next_id;
    s->next_id = s->next_id >= 0x7FFFFFF0 ? 1 : s->next_id + 1;
    snprintf(m->key, sizeof(m->key), "%s", key);
    m->role = role;
    m->done = m->cut = m->told_text = false;
    m->dirty = true;
    m->history = history;
    m->conn = s->conn;
    m->raw_len = m->clean_len = 0;
    m->raw[0] = m->clean[0] = '\0';
    return m;
}

/* Sets a message's whole text (a person's loses an app's prefix, and becomes
 * the app's): false if nothing changed. */
static bool chat_set_text(chat_msg_t *m, const char *text, size_t n, bool cut)
{
    int role = m->role;
    if (role != GADGET_ROLE_MUSE) {
        size_t k = chat_app_prefix(text, n);
        role = k ? GADGET_ROLE_APP : GADGET_ROLE_USER;
        text += k;
        n -= k;
    }
    if (n > CHAT_TEXT_BYTES) {
        n = utf8_whole(text, CHAT_TEXT_BYTES);
        cut = true;
    }
    if (role == m->role && n == m->raw_len && cut == m->cut && !memcmp(m->raw, text, n)) {
        return false;
    }
    m->role = role;
    memcpy(m->raw, text, n);
    m->raw[n] = '\0';
    m->raw_len = n;
    m->cut = cut;
    m->dirty = true;
    return true;
}

static bool chat_append(chat_msg_t *m, const char *text, size_t n)
{
    if (!n || m->cut) {
        return false;
    }
    size_t room = CHAT_TEXT_BYTES - m->raw_len;
    if (n > room) {
        n = utf8_whole(text, room);
        m->cut = true;
    }
    memcpy(m->raw + m->raw_len, text, n);
    m->raw_len += n;
    m->raw[m->raw_len] = '\0';
    m->dirty = true;
    return true;
}

static const char *chat_text_of(chat_msg_t *m, size_t *len)
{
    if (m->dirty) {
        m->clean_len = chat_clean(m->raw, m->raw_len, m->clean);
        m->dirty = false;
    }
    *len = m->clean_len;
    return m->clean;
}

static bool word_in(const char *w, const char *const *list)
{
    for (; *list; list++) {
        if (!strcmp(w, *list)) {
            return true;
        }
    }
    return false;
}

/* The link is gone: whether Muse is working is unknown until it's back. */
static void chat_store_lost(chat_store_t *s)
{
    if (s->busy == 1) {
        tell(s, GADGET_CHAT_BUSY, 0);
    }
    s->busy = -1;
}

/* One subscription line, or one history entry. */
static void chat_apply(chat_store_t *s, const chat_row_t *r, bool history)
{
    static const char *const idle[] = {"online", "idle", NULL};
    static const char *const ended[] = {"completed", "failed", "cancelled", "canceled", "idle", "done", "error", NULL};
    if (!history) {
        if (strcmp(r->type, "event")) {
            return;
        }
        if (r->seq) {
            if (r->seq <= s->last_seq) {
                return;   /* seen already */
            }
            s->last_seq = r->seq;
        }
    }
    const char *ev = r->event;
    if (!strcmp(ev, "agent.status") || !strcmp(ev, "task.status")) {
        int busy = r->activity[0] ? !word_in(r->activity, idle) : r->status[0] ? !word_in(r->status, ended) : -1;
        if (busy >= 0 && busy != s->busy) {
            s->busy = busy;
            tell(s, GADGET_CHAT_BUSY, busy);
        }
        return;
    }
    bool start = !strcmp(ev, "delta.message_start");
    bool append = !strcmp(ev, "delta.text_append");
    bool user = !start && !append && (!strcmp(r->role, "user") || !strcmp(ev, "message.user"));
    bool done_ev = !strcmp(ev, "delta.message_done");
    bool final = !user && (done_ev || !strcmp(ev, "message.assistant") || (history && !strcmp(r->role, "assistant")));
    if (!user && !start && !append && !final) {
        return;
    }
    if (history && !user && !final) {
        return;
    }
    chat_msg_t *m = r->id[0] ? chat_by_key(s, r->id) : user ? NULL : chat_last_open(s);
    int pos = history ? history_pos(s) : s->count;
    if (user) {
        if (!m) {
            if (!chat_visible(r->text, r->text_len)) {
                return;   /* nothing a gadget can show (a bare attachment) */
            }
            m = chat_add(s, pos, GADGET_ROLE_USER, r->id, history);
            if (!m) {
                return;
            }
            chat_set_text(m, r->text, r->text_len, r->text_cut);
            m->done = true;
            tell(s, GADGET_CHAT_MESSAGE, m->id);
            tell(s, GADGET_CHAT_DONE, m->id);
        } else if (r->text_len && chat_set_text(m, r->text, r->text_len, r->text_cut)) {
            tell(s, GADGET_CHAT_DONE, m->id);   /* new text: a voice note's transcript */
        }
        return;
    }
    if (!m) {
        if ((final || append) && !r->text_len) {
            return;
        }
        m = chat_add(s, pos, GADGET_ROLE_MUSE, r->id, history);
        if (!m) {
            return;
        }
        tell(s, GADGET_CHAT_MESSAGE, m->id);
    }
    if (start) {
        return;
    }
    if (append) {
        if (!m->done && chat_append(m, r->text, r->text_len)) {
            tell_text(s, m);
        }
        return;
    }
    bool changed = r->text_len && chat_set_text(m, r->text, r->text_len, r->text_cut);
    bool ready = done_ev || r->ready != 0;   /* message.assistant before its text is ready isn't final */
    if (ready && (!m->done || changed)) {
        m->done = true;
        tell(s, GADGET_CHAT_DONE, m->id);
    } else if (!ready && changed && !m->done) {
        tell_text(s, m);
    }
}

/* ---- The chat for apps ---- */

static const char *TAG = "muse_apps_chat";

#define BACKOFF_FIRST_MS 1000
#define BACKOFF_MAX_MS 30000
#define OFFLINE_RETRY_MS 2000
#define STEADY_US (30 * 1000000LL)   /* a subscription up this long starts the backoff again */
#define HISTORY_TIMEOUT_MS 15000
#define SEND_TIMEOUT_MS 30000
#define SEND_TEXT_MAX 4096           /* limits.text_bytes */

typedef struct {
    chat_store_t store;
    chat_row_t row;                  /* the subscription's line being read */
} chat_mem_t;

/* Under lock: the app task, the worker, the session task (frames) and the
 * timer all touch it. */
static struct {
    SemaphoreHandle_t lock;
    esp_timer_handle_t timer;
    chat_mem_t *mem;                 /* while open; counted in the app's budget */
    chat_parser_t parser;
    uint32_t gen;                    /* the subscription's: frames of another are dropped */
    bool open, live, connected, queued;
    int64_t stream;
    int status;
    int64_t up_us;
    int backoff_ms;
    char err[96];
    size_t err_len;
    int32_t next_id;                 /* message ids stay new for the app's whole run */
} s_chat;

static void lock(void)
{
    xSemaphoreTake(s_chat.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_chat.lock);
}

static bool post_chat(void *ctx, int what, int value)
{
    (void)ctx;
    return muse_apps_post(GADGET_EV_CHAT, what, value);
}

static void set_connected_locked(bool on)
{
    if (on != s_chat.connected) {
        s_chat.connected = on;
        muse_apps_post(GADGET_EV_CHAT, GADGET_CHAT_CONNECTED, on);
    }
    if (!on && s_chat.mem) {
        chat_store_lost(&s_chat.mem->store);
    }
}

static void schedule_locked(int ms)
{
    if (s_chat.timer) {
        esp_timer_stop(s_chat.timer);   /* not running: fine */
        esp_timer_start_once(s_chat.timer, (uint64_t)ms * 1000);
    }
}

static void retry_locked(bool steady)
{
    if (steady) {
        s_chat.backoff_ms = BACKOFF_FIRST_MS;
    }
    schedule_locked(s_chat.backoff_ms);
    s_chat.backoff_ms = s_chat.backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_chat.backoff_ms * 2;
}

static void apply_line(void *ctx, const chat_row_t *row)
{
    chat_apply(ctx, row, false);
}

static void apply_history(void *ctx, const chat_row_t *row)
{
    chat_apply(ctx, row, true);
}

/* The subscription's frames, on the session task: parsed in place. */
static void chat_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    lock();
    if (!s_chat.open || (uint32_t)(uintptr_t)ctx != s_chat.gen || !s_chat.live) {
        unlock();
        return;
    }
    if (status > 0) {
        s_chat.status = status;
        if (status == 200) {
            s_chat.up_us = esp_timer_get_time();
            set_connected_locked(true);
            ESP_LOGI(TAG, "following the chat");
        }
    }
    if (len && s_chat.status == 200) {
        chat_parse(&s_chat.parser, (const char *)data, len);
    } else if (len && s_chat.status > 0) {
        size_t n = sizeof(s_chat.err) - 1 - s_chat.err_len;
        n = len < n ? len : n;
        memcpy(s_chat.err + s_chat.err_len, data, n);
        s_chat.err_len += n;
    }
    if (end || status < 0) {
        bool steady = s_chat.status == 200 && esp_timer_get_time() - s_chat.up_us > STEADY_US;
        s_chat.err[s_chat.err_len] = '\0';
        for (size_t i = 0; i < s_chat.err_len; i++) {
            if ((unsigned char)s_chat.err[i] < 0x20) s_chat.err[i] = ' ';
        }
        if (s_chat.status == 200) {
            ESP_LOGI(TAG, "the chat's subscription ended (%d); again in %d ms", status, steady ? BACKOFF_FIRST_MS : s_chat.backoff_ms);
        } else {
            ESP_LOGW(TAG, "POST /chat/subscribe: %s%d%s%s", s_chat.status > 0 ? "HTTP " : "status ",
                     s_chat.status > 0 ? s_chat.status : status, s_chat.err_len ? ": " : "", s_chat.err);
        }
        s_chat.live = false;
        s_chat.stream = 0;
        set_connected_locked(false);
        retry_locked(steady);
    }
    unlock();
}

/* The latest message: on the worker, after each (re)subscription. */
static void chat_history(void)
{
    size_t cap = muse_link_req_max_body();
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    chat_row_t *row = heap_caps_malloc(sizeof(*row), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf && row) {
        muse_apps_fetch_t f = {.verb = "GET", .path = "/chat/history?limit=1", .json = true,
                               .reply = buf, .reply_cap = cap, .timeout_ms = HISTORY_TIMEOUT_MS};
        int status = muse_apps_vm_fetch(&f);
        if (status == 200 && !f.reply_cut) {
            lock();
            if (s_chat.open) {
                chat_parser_t p;
                chat_parser_init(&p, false, row, apply_history, &s_chat.mem->store);
                chat_parse(&p, (const char *)buf, f.reply_len);
            }
            unlock();
        } else if (status == 200) {
            ESP_LOGW(TAG, "the chat's latest message is too long for one frame");
        }
    }
    heap_caps_free(buf);
    heap_caps_free(row);
}

/* Subscribes, then fetches the latest message: on the worker. */
static void chat_connect(void *arg)
{
    (void)arg;
    lock();
    s_chat.queued = false;
    if (!s_chat.open || s_chat.live) {
        unlock();
        return;
    }
    if (!muse_link_req_ready()) {
        set_connected_locked(false);
        schedule_locked(OFFLINE_RETRY_MS);
        unlock();
        return;
    }
    uint32_t token = ++s_chat.gen;
    s_chat.live = true;
    s_chat.status = 0;
    s_chat.err_len = 0;
    chat_parser_init(&s_chat.parser, true, &s_chat.mem->row, apply_line, &s_chat.mem->store);
    chat_store_connected(&s_chat.mem->store);
    unlock();
    int64_t id = muse_apps_vm_open("POST", "/chat/subscribe", "application/x-ndjson", "{}", chat_frame,
                                   (void *)(uintptr_t)token);
    lock();
    bool mine = s_chat.open && s_chat.gen == token;
    if (mine && id) {
        s_chat.stream = id;
    } else if (mine && s_chat.live) {
        ESP_LOGW(TAG, "couldn't open the chat's subscription");
        s_chat.live = false;
        retry_locked(false);
    }
    unlock();
    if (!mine && id) {
        muse_link_req_cancel(id);
    }
    if (mine && id) {
        chat_history();
    }
}

static void chat_connect_dropped(void *arg)
{
    (void)arg;
    lock();
    s_chat.queued = false;
    unlock();
}

static void queue_connect(void)
{
    lock();
    bool go = s_chat.open && !s_chat.live && !s_chat.queued;
    s_chat.queued |= go;
    unlock();
    if (go && !muse_apps_vm_job(chat_connect, chat_connect_dropped, NULL)) {
        lock();
        s_chat.queued = false;
        schedule_locked(OFFLINE_RETRY_MS);
        unlock();
    }
}

static void chat_timer(void *arg)
{
    (void)arg;
    queue_connect();
}

/* Stops following and frees the messages. */
static void chat_end(void)
{
    if (!s_chat.lock) {
        return;
    }
    lock();
    if (s_chat.mem) {
        s_chat.next_id = s_chat.mem->store.next_id;
    }
    chat_mem_t *mem = s_chat.mem;
    int64_t id = s_chat.stream;
    s_chat.mem = NULL;
    s_chat.open = s_chat.live = s_chat.connected = false;
    s_chat.gen++;
    s_chat.stream = 0;
    if (s_chat.timer) {
        esp_timer_stop(s_chat.timer);
    }
    unlock();
    if (id) {
        muse_link_req_cancel(id);
    }
    muse_apps_free(mem);
}

void muse_apps_chat_start(void)
{
    if (!s_chat.lock) {
        s_chat.lock = xSemaphoreCreateMutex();
    }
    s_chat.next_id = 1;
}

void muse_apps_chat_stop(void)
{
    chat_end();
}

int32_t gadget_chat_open(wasm_exec_env_t env)
{
    (void)env;
    if (!muse_apps_permitted("chat.read")) {
        return GADGET_ERR_DENIED;
    }
    if (!s_chat.lock) {
        return GADGET_ERR_NO_MEMORY;
    }
    if (s_chat.open) {
        return 0;
    }
    if (!muse_link_req_ready()) {
        return GADGET_ERR_OFFLINE;
    }
    if (!s_chat.timer) {
        const esp_timer_create_args_t args = {.callback = chat_timer, .name = "muse_apps_chat"};
        if (esp_timer_create(&args, &s_chat.timer) != ESP_OK) {
            s_chat.timer = NULL;
            return GADGET_ERR_NO_MEMORY;
        }
    }
    chat_mem_t *mem = muse_apps_alloc(sizeof(*mem));
    if (!mem) {
        return GADGET_ERR_NO_MEMORY;
    }
    lock();
    chat_store_init(&mem->store, s_chat.next_id, post_chat, NULL);
    s_chat.mem = mem;
    s_chat.open = true;
    s_chat.live = s_chat.queued = s_chat.connected = false;
    s_chat.backoff_ms = BACKOFF_FIRST_MS;
    unlock();
    queue_connect();
    return 0;
}

void gadget_chat_close(wasm_exec_env_t env)
{
    (void)env;
    if (muse_apps_permitted("chat.read")) {
        chat_end();
    }
}

int32_t gadget_chat_latest(wasm_exec_env_t env, int32_t n)
{
    (void)env;
    if (!muse_apps_permitted("chat.read")) {
        return GADGET_ERR_DENIED;
    }
    if (!s_chat.lock) {
        return 0;
    }
    lock();
    int32_t id = s_chat.mem ? chat_latest_id(&s_chat.mem->store, n) : 0;
    unlock();
    return id;
}

int32_t gadget_chat_info(wasm_exec_env_t env, int32_t id, int32_t key)
{
    (void)env;
    if (!muse_apps_permitted("chat.read")) {
        return GADGET_ERR_DENIED;
    }
    if (key < GADGET_CHAT_INFO_ROLE || key > GADGET_CHAT_INFO_TRUNCATED) {
        return GADGET_ERR_ARG;
    }
    if (!s_chat.lock) {
        return GADGET_ERR_NOT_FOUND;
    }
    lock();
    chat_msg_t *m = s_chat.mem ? chat_by_id(&s_chat.mem->store, id) : NULL;
    int32_t r = GADGET_ERR_NOT_FOUND;
    if (m) {
        size_t len;
        switch (key) {
        case GADGET_CHAT_INFO_ROLE: r = m->role; break;
        case GADGET_CHAT_INFO_LENGTH: chat_text_of(m, &len); r = (int32_t)len; break;
        case GADGET_CHAT_INFO_DONE: r = m->done; break;
        default: r = m->cut; break;
        }
    }
    unlock();
    return r;
}

int32_t gadget_chat_text(wasm_exec_env_t env, int32_t id, int32_t offset, void *buf, uint32_t buf_size)
{
    (void)env;
    if (!muse_apps_permitted("chat.read")) {
        return GADGET_ERR_DENIED;
    }
    if (offset < 0 || !buf || !buf_size) {
        return GADGET_ERR_ARG;
    }
    if (!s_chat.lock) {
        return GADGET_ERR_NOT_FOUND;
    }
    lock();
    chat_msg_t *m = s_chat.mem ? chat_by_id(&s_chat.mem->store, id) : NULL;
    int32_t r = GADGET_ERR_NOT_FOUND;
    if (m) {
        size_t len;
        const char *text = chat_text_of(m, &len);
        if ((size_t)offset > len) {
            r = GADGET_ERR_ARG;
        } else {
            size_t n = len - (size_t)offset;
            if (n > buf_size - 1) {
                size_t whole = utf8_whole(text + offset, buf_size - 1);
                n = whole ? whole : buf_size - 1;   /* a character larger than the buffer: its bytes */
            }
            memcpy(buf, text + offset, n);
            ((char *)buf)[n] = '\0';
            m->told_text = false;
            r = (int32_t)n;
        }
    }
    unlock();
    return r;
}

/* ---- chat_send: POST /chat/stream, as the Muse app sends a typed message ---- */

typedef struct {
    int32_t op;
    size_t len;
    char body[];
} send_job_t;

static void send_run(void *arg)
{
    send_job_t *j = arg;
    muse_apps_fetch_t f = {.verb = "POST", .path = "/chat/stream", .json = true, .body = j->body,
                           .body_len = j->len, .timeout_ms = SEND_TIMEOUT_MS};
    int status = muse_apps_vm_fetch(&f);
    bool ok = status >= 200 && status < 300;
    if (!ok && status != MUSE_APPS_VM_CANCELLED) {
        muse_apps_log("chat_send: %s %d%s%s", status > 0 ? "HTTP" : "status", status, f.error[0] ? ": " : "",
                      f.error);
    }
    muse_apps_op_finish(j->op, ok ? 0 : muse_apps_vm_error(status), NULL, 0);
    heap_caps_free(j);
}

static void send_drop(void *arg)
{
    send_job_t *j = arg;
    muse_apps_op_finish(j->op, GADGET_ERR_FAILED, NULL, 0);
    heap_caps_free(j);
}

int32_t gadget_chat_send(wasm_exec_env_t env, const char *text)
{
    if (!muse_apps_permitted("chat.send")) {
        return GADGET_ERR_DENIED;
    }
    size_t n = text ? strnlen(text, SEND_TEXT_MAX + 1) : 0;
    if (!n || n > SEND_TEXT_MAX) {
        return GADGET_ERR_ARG;
    }
    if (!muse_link_req_ready()) {
        return GADGET_ERR_OFFLINE;
    }
    /* Led by the app's name, so Muse can tell it from the person's own words. */
    const char *name = muse_apps_name();
    size_t msg_cap = strlen(name) + n + 32;
    char *message = heap_caps_malloc(msg_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    send_job_t *j = message ? heap_caps_malloc(sizeof(*j) + 6 * msg_cap + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!j) {
        heap_caps_free(message);
        return GADGET_ERR_NO_MEMORY;
    }
    int len = snprintf(message, msg_cap, "[%s on the gadget] %s", name, text);
    char *o = j->body;
    o += sprintf(o, "{\"message\":\"");
    o += muse_apps_vm_json_escape(o, message, (size_t)len);
    o += sprintf(o, "\",\"output_modality\":\"text\"}");
    j->len = (size_t)(o - j->body);
    heap_caps_free(message);
    int32_t op = muse_apps_op_begin(env, NULL, 0);
    if (op < 0) {
        heap_caps_free(j);
        return op;
    }
    j->op = op;
    if (!muse_apps_vm_job(send_run, send_drop, j)) {
        heap_caps_free(j);
        muse_apps_op_finish(op, GADGET_ERR_NO_MEMORY, NULL, 0);
    }
    return op;
}
