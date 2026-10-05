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
 * Muse chat turns on boards without PSRAM (CONFIG_MUSE_HATCH=n). There's no room
 * for a second TLS and Noise connection or for MP3 decoding, so turns ride
 * Home Link's own session (muse_link_req_*) and replies are text only:
 * A live POST /chat/subscribe receives NDJSON replies while POST /chat/stream
 * uploads the note. Both use the existing device token and Link connection.
 * Lines are parsed without cJSON; bounded buffers keep internal RAM use small.
 */
#include "muse_chat.h"
#include "i18n.h"
#include "muse_chat_priv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_link.h"
#include "muse_wifi.h"

static const char *TAG = "muse_chat_link";

#define MIC_RATE 16000
#define STAGE_BYTES 1536                /* PCM per body chunk: 2 KB of base64, 48 ms */
#define CHUNK_BYTES (STAGE_BYTES / 3 * 4)
#define ACK_MAX 2048
#define ROW_MAX 12288                   /* maximum subscription line gathered across frames */
#define TEXT_MAX 1024                   /* reply text kept for the captions */
#define EV_TEXT 72
#define SEND_WAIT_MS 200               /* the press queues the pre-roll all at once */
#define SETTLE_US 3000000               /* quiet after a reply before the turn ends */
#define REPLY_TIMEOUT_US 60000000
#define READ_CHARS_PER_S 14             /* caption scroll, about speaking pace */


/* A reply being received on Link's session task, read by the voice task once done. */
typedef struct {
    uint32_t gen;       /* bumped per request; frames of older ones are dropped */
    int status;         /* HTTP status, -1 if the request died */
    bool done;
    bool overflow;
    size_t len;
    size_t cap;         /* fixed note ACK buffer; 0: subscription line, gathered only if split */
    char *body;
} rx_t;

/* A subscription event, decoded without allocating a JSON tree. */
typedef struct {
    char type[16];
    bool ready;         /* display_text_ready */
    uint64_t seq;
    char event[24];
    char msg[80];
    char reply_to[80];
    char text[TEXT_MAX];
} row_t;

enum { RX_NOTE, RX_SUB, RX_COUNT };

typedef enum { T_IDLE, T_TALKING, T_ACK, T_REPLY } phase_t;

typedef struct {
    char text[EV_TEXT];
    muse_hatch_ev_t type;
} ev_t;

static SemaphoreHandle_t s_rx_lock;
static rx_t s_rx[RX_COUNT];
static int64_t s_stream[RX_COUNT];      /* open request per slot, 0 none */
static QueueHandle_t s_events;
/* Subscription callback state, always under s_rx_lock. Keep the two newest
 * completed messages until the voice task drains them, including before ACK. */
static row_t s_row, s_delta, s_pending[2];
static unsigned s_pending_count;
static uint64_t s_last_seq;
static bool s_early_evicted, s_skipped_big;
static char s_note_id[80], s_parent_id[80]; /* ACK IDs shared under s_rx_lock */
static muse_chat_rejected_t s_rejected;   /* under s_rx_lock */

/* Voice task only. */
static struct {
    phase_t phase;
    uint8_t *stage;                     /* PCM waiting for the next chunk */
    size_t stage_len;
    char *chunk;
    char note_id[80], parent_id[80], seen[2][80];
    unsigned seen_count;
    int64_t t_end, t_reply, t_show;
    bool heard, replied;
    bool after_note;
    char error[EV_TEXT];                /* why the turn failed, repeated at the release */
    char text[TEXT_MAX];
    char shown[EV_TEXT];
} s_turn;

/* ---- Subscription rows, read in place ---- */

typedef struct {
    const char *p, *end;
} scan_t;

static void skip_ws(scan_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t')) {
        s->p++;
    }
}

static bool expect(scan_t *s, char c)
{
    skip_ws(s);
    if (s->p < s->end && *s->p == c) {
        s->p++;
        return true;
    }
    return false;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v = v << 4 | (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

/* Reads the string at s->p into out (may be NULL), cut to fit on a character boundary. */
static bool read_string(scan_t *s, char *out, size_t cap)
{
    if (s->p >= s->end || *s->p != '"') {
        return false;
    }
    size_t n = 0;
    bool clipped = false;
    for (s->p++; s->p < s->end && *s->p != '"'; s->p++) {
        char buf[4];
        size_t len = 1;
        buf[0] = *s->p;
        if (*s->p == '\\' && ++s->p < s->end) {
            switch (*s->p) {
            case 'n': buf[0] = '\n'; break;
            case 't': buf[0] = ' '; break;
            case 'r': buf[0] = ' '; break;
            case 'b': case 'f': buf[0] = ' '; break;
            case 'u': {
                if (s->end - s->p < 5) {
                    return false;
                }
                uint32_t c = hex4(s->p + 1);
                s->p += 4;
                if (c >= 0xD800 && c < 0xDC00) {
                    if (s->end - s->p < 7 || s->p[1] != '\\' || s->p[2] != 'u') return false;
                    uint32_t low = hex4(s->p + 3);
                    if (low < 0xDC00 || low > 0xDFFF) return false;
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    s->p += 6;
                } else if (c >= 0xDC00 && c <= 0xDFFF) {
                    return false;
                }
                if (c < 0x80) {
                    buf[0] = (char)c;
                } else if (c < 0x800) {
                    buf[0] = (char)(0xC0 | c >> 6);
                    buf[1] = (char)(0x80 | (c & 63));
                    len = 2;
                } else if (c < 0x10000) {
                    buf[0] = (char)(0xE0 | c >> 12);
                    buf[1] = (char)(0x80 | (c >> 6 & 63));
                    buf[2] = (char)(0x80 | (c & 63));
                    len = 3;
                } else {
                    buf[0] = (char)(0xF0 | c >> 18);
                    buf[1] = (char)(0x80 | (c >> 12 & 63));
                    buf[2] = (char)(0x80 | (c >> 6 & 63));
                    buf[3] = (char)(0x80 | (c & 63));
                    len = 4;
                }
                break;
            }
            default: buf[0] = *s->p; break;   /* \" \\ \/ */
            }
        }
        if (out && !clipped && n + len < cap) {
            memcpy(out + n, buf, len);
            n += len;
        } else if (out) {
            clipped = true;
        }
    }
    if (out && cap) {
        if (clipped && n) {
            size_t start = n - 1;
            while (start && ((unsigned char)out[start] & 0xC0) == 0x80) start--;
            unsigned char lead = (unsigned char)out[start];
            size_t need = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            if (n - start < need) n = start;
        }
        out[n] = '\0';
    }
    if (s->p >= s->end) {
        return false;
    }
    s->p++;
    return true;
}

/* Steps over one value: a string, number, literal, or a whole object or array. */
static bool skip_value(scan_t *s)
{
    int depth = 0;
    skip_ws(s);
    while (s->p < s->end) {
        char c = *s->p;
        if (c == '"') {
            if (!read_string(s, NULL, 0)) {
                return false;
            }
        } else if (c == '{' || c == '[') {
            depth++;
            s->p++;
        } else if (c == '}' || c == ']') {
            if (!depth) {
                return true;
            }
            depth--;
            s->p++;
        } else if (c == ',' && !depth) {
            return true;
        } else {
            s->p++;
        }
        if (!depth && (c == '"' || c == '}' || c == ']')) {
            return true;
        }
    }
    return false;
}

/* Reads a string member's value into out; anything else (null) is skipped. */
static bool read_field(scan_t *s, char *out, size_t cap)
{
    skip_ws(s);
    return s->p < s->end && *s->p == '"' ? read_string(s, out, cap) : skip_value(s);
}

static bool parse_row(scan_t *s, row_t *r, scan_t *payload)
{
    if (!expect(s, '{')) {
        return false;
    }
    if (expect(s, '}')) {
        return true;
    }
    unsigned text_rank = 0;
    bool have_id = false, have_parent = false;
    do {
        char key[24];
        skip_ws(s);
        if (!read_string(s, key, sizeof(key)) || !expect(s, ':')) {
            return false;
        }
        skip_ws(s);
        bool ok;
        bool string = s->p < s->end && *s->p == '"';
        if (payload && !strcmp(key, "payload")) {
            *payload = *s;
            ok = skip_value(s);
        } else if (payload && !strcmp(key, "type")) {
            ok = read_field(s, r->type, sizeof(r->type));
        } else if (payload && !strcmp(key, "seq")) {
            for (r->seq = 0; s->p < s->end && *s->p >= '0' && *s->p <= '9'; s->p++) {
                r->seq = r->seq * 10 + (*s->p - '0');
            }
            ok = skip_value(s);
        } else if (!strcmp(key, "display_text_ready")) {
            r->ready = s->p < s->end && *s->p == 't';
            ok = skip_value(s);
        } else if (payload && (!strcmp(key, "event_name") || !strcmp(key, "event"))) {
            ok = read_field(s, r->event, sizeof(r->event));
        } else if (!strcmp(key, "message_id") || (!have_id && !strcmp(key, "id"))) {
            ok = read_field(s, r->msg, sizeof(r->msg));
            have_id |= string && !strcmp(key, "message_id");
        } else if (!strcmp(key, "reply_to_message_id") || (!have_parent && !strcmp(key, "parent_message_id"))) {
            ok = read_field(s, r->reply_to, sizeof(r->reply_to));
            have_parent |= string && !strcmp(key, "reply_to_message_id");
        } else if (!strcmp(key, "display_text") || !strcmp(key, "content") || !strcmp(key, "text")) {
            unsigned rank = !strcmp(key, "display_text") ? 3 : !strcmp(key, "content") ? 2 : 1;
            ok = string && rank >= text_rank ? read_field(s, r->text, sizeof(r->text)) : skip_value(s);
            if (string && rank > text_rank) text_rank = rank;
        } else {
            ok = skip_value(s);
        }
        if (!ok) {
            return false;
        }
    } while (expect(s, ','));
    return expect(s, '}');
}

static bool parse_event(const char *data, size_t len)
{
    scan_t scan = { data, data + len }, payload = {0};
    memset(&s_row, 0, sizeof(s_row));
    s_row.ready = true;
    if (!parse_row(&scan, &s_row, &payload)) return false;
    skip_ws(&scan);
    if (scan.p != scan.end || strcmp(s_row.type, "event")) return false;
    return !payload.p || parse_row(&payload, &s_row, NULL);
}

static bool related(const row_t *row)
{
    if (!strcmp(row->event, "message.user")) {
        return !s_note_id[0] || !strcmp(row->msg, s_note_id) || !strcmp(row->msg, s_parent_id);
    }
    if (muse_chat_is_rejected(&s_rejected, row->msg)) return false;
    if (!s_note_id[0]) return true;
    if (!row->reply_to[0]) {
        /* After overflow, parentless events must match a correlated delta. */
        return !s_rejected.overflow || (!strcmp(row->msg, s_delta.msg) && s_delta.reply_to[0]
            && (!strcmp(s_delta.reply_to, s_note_id) || !strcmp(s_delta.reply_to, s_parent_id)));
    }
    if (!strcmp(row->reply_to, s_note_id) || !strcmp(row->reply_to, s_parent_id)) return true;
    muse_chat_reject(&s_rejected, row->msg);
    return false;
}

/* Append whole UTF-8 characters even when the caption is already nearly full. */
static void text_append(char *out, size_t cap, const char *text)
{
    size_t used = strlen(out), n = strlen(text);
    if (n >= cap - used) {
        n = cap - used - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
    }
    memcpy(out + used, text, n);
    out[used + n] = 0;
}

/* Called with the receive lock held; never waits on the voice task. */
static void subscription_event(void)
{
    if (s_row.seq && s_row.seq <= s_last_seq) return;
    if (s_row.seq) s_last_seq = s_row.seq;
    bool append = !strcmp(s_row.event, "delta.text_append");
    bool start = !strcmp(s_row.event, "delta.message_start");
    bool done = !strcmp(s_row.event, "delta.message_done");
    bool full = !strcmp(s_row.event, "message.assistant");
    bool user = !strcmp(s_row.event, "message.user");
    if ((!append && !start && !done && !full && !user) || !s_row.msg[0]) return;
    if (!related(&s_row)) return;
    if (start || append) {
        if (strcmp(s_delta.msg, s_row.msg)) {
            memset(&s_delta, 0, sizeof(s_delta));
            strlcpy(s_delta.msg, s_row.msg, sizeof(s_delta.msg));
        }
        if (s_row.reply_to[0]) strlcpy(s_delta.reply_to, s_row.reply_to, sizeof(s_delta.reply_to));
        if (append) {
            text_append(s_delta.text, sizeof(s_delta.text), s_row.text);
        }
        return;
    }
    if (!done && !s_row.ready) return;
    if (!strcmp(s_row.msg, s_delta.msg)) {
        if (!s_row.text[0]) strlcpy(s_row.text, s_delta.text, sizeof(s_row.text));
        if (!s_row.reply_to[0]) strlcpy(s_row.reply_to, s_delta.reply_to, sizeof(s_row.reply_to));
    }
    if (!related(&s_row) || !s_row.text[0]) return;
    if (!user) strlcpy(s_row.event, "message.assistant", sizeof(s_row.event));
    s_row.ready = true;
    /* Coalesce the done event and persisted full message, including before ACK. */
    for (unsigned i = 0; i < s_pending_count; i++) {
        if (!strcmp(s_pending[i].msg, s_row.msg)) { s_pending[i] = s_row; return; }
    }
    /* Keep the two newest finals if the voice task has not drained the queue.
     * Before ACK they are provisional: unrelated traffic must not fail a turn
     * or grow memory without bound. Remember eviction for a useful timeout. */
    if (s_pending_count == 2) {
        if (!s_note_id[0]) s_early_evicted = true;
        s_pending[0] = s_pending[1];
        s_pending_count--;
    }
    s_pending[s_pending_count++] = s_row;
}

/* Parse complete lines in Link's buffer; gather only a line split across frames.
 * An oversized line is skipped through its newline, so later events still work. */
static void row_data(rx_t *rx, const uint8_t *data, size_t len, bool end)
{
    if (rx->status != 200) return;
    while (len) {
        const uint8_t *newline = memchr(data, '\n', len);
        size_t n = newline ? (size_t)(newline - data) : len;
        if (!rx->overflow) {
            if (n > ROW_MAX - rx->len) {
                rx->overflow = s_skipped_big = true;
            } else if (!rx->body && newline) {
                if (parse_event((const char *)data, n)) subscription_event();
            } else if (n) {
                char *grown = realloc(rx->body, rx->len + n);
                if (!grown) {
                    rx->overflow = s_skipped_big = true;
                } else {
                    rx->body = grown;
                    memcpy(rx->body + rx->len, data, n);
                    rx->len += n;
                }
            }
        }
        if (rx->overflow) {
            free(rx->body);
            rx->body = NULL;
            rx->len = 0;
        }
        if (newline) {
            if (rx->body && !rx->overflow && parse_event(rx->body, rx->len)) subscription_event();
            free(rx->body);
            rx->body = NULL;
            rx->len = 0;
            rx->overflow = false;
            n++;
        }
        data += n;
        len -= n;
    }
    if (end) {
        if (rx->len && !rx->overflow && parse_event(rx->body, rx->len)) subscription_event();
        free(rx->body);
        rx->body = NULL;
        rx->len = 0;
    }
}

static void rx_clear(rx_t *rx)
{
    rx->status = 0;
    rx->done = rx->overflow = false;
    rx->len = 0;
    if (!rx->cap) {
        free(rx->body);
        rx->body = NULL;
        s_skipped_big = false;
        s_pending_count = 0;
        s_last_seq = 0;
        s_early_evicted = false;
        s_note_id[0] = s_parent_id[0] = 0;
        memset(&s_rejected, 0, sizeof(s_rejected));
        memset(&s_delta, 0, sizeof(s_delta));
    }
}

static void on_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    uintptr_t token = (uintptr_t)ctx;
    rx_t *rx = &s_rx[token & 1];
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    if ((uint32_t)(token >> 1) == rx->gen && !rx->done) {
        if (status) {
            rx->status = status;
        }
        if (!rx->cap) {
            row_data(rx, data, len, end && status >= 0);
        } else if (len && rx->len + len >= rx->cap) {
            rx->overflow = true;
        } else if (len) {
            memcpy(rx->body + rx->len, data, len);
            rx->len += len;
            rx->body[rx->len] = '\0';
        }
        rx->done = end || status < 0;
    }
    xSemaphoreGive(s_rx_lock);
}

/* Starts a request whose reply lands in slot `slot`. False if Link's session is down. */
static bool request(int slot, const char *verb, const char *path, bool json, bool end_body)
{
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08" PRIx32 "-%08" PRIx32, esp_random(), esp_random());
    const char *headers[] = { "x-request-id", req_id, "x-app-id", "hatch-web",
                              json ? "Content-Type" : NULL, "application/json",
                              "Accept", slot == RX_SUB ? "application/x-ndjson" : "application/json", NULL };
    rx_t *rx = &s_rx[slot];
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    rx->gen++;
    rx_clear(rx);
    if (rx->cap) {
        rx->body[0] = '\0';
    }
    uintptr_t token = (uintptr_t)rx->gen << 1 | slot;
    xSemaphoreGive(s_rx_lock);
    s_stream[slot] = muse_link_req_open(verb, path, headers, end_body, on_frame, (void *)token);
    return s_stream[slot] != 0;
}

/* Drops slot `slot`'s request, if any, and whatever of its reply is still coming. */
static void drop(int slot)
{
    if (s_stream[slot]) {
        muse_link_req_cancel(s_stream[slot]);
        s_stream[slot] = 0;
    }
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    s_rx[slot].gen++;
    rx_clear(&s_rx[slot]);
    xSemaphoreGive(s_rx_lock);
}

/* True once slot `slot`'s reply is complete (or the request died). */
static bool received(int slot)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    bool done = s_stream[slot] && s_rx[slot].done;
    xSemaphoreGive(s_rx_lock);
    if (done) {
        s_stream[slot] = 0;
    }
    return done;
}

static void emit(muse_hatch_ev_t type, const char *text)
{
    ev_t ev = { .type = type };
    strlcpy(ev.text, text ? text : "", sizeof(ev.text));
    xQueueSend(s_events, &ev, 0);
}

static void end_turn(void)
{
    drop(RX_NOTE);
    drop(RX_SUB);
    free(s_turn.stage);
    free(s_turn.chunk);
    s_turn.stage = NULL;
    s_turn.chunk = NULL;
    s_turn.phase = T_IDLE;
}

static void fail(const char *why)
{
    ESP_LOGW(TAG, "turn failed: %s", why);
    end_turn();
    strlcpy(s_turn.error, why, sizeof(s_turn.error));
    emit(MUSE_HATCH_EV_ERROR, why);
}

/* ---- The note ---- */

/* Sends the staged PCM as one body chunk; whole chunks are a multiple of 3 bytes, so only the last pads. */
static bool send_stage(bool last)
{
    size_t n = muse_hatch_base64(s_turn.stage, s_turn.stage_len, (char *)s_turn.chunk);
    if (last) {
        memcpy(s_turn.chunk + n, MUSE_HATCH_NOTE_TAIL, sizeof(MUSE_HATCH_NOTE_TAIL) - 1);
        n += sizeof(MUSE_HATCH_NOTE_TAIL) - 1;
    }
    s_turn.stage_len = 0;
    return muse_link_req_send(s_stream[RX_NOTE], s_turn.chunk, n, last, SEND_WAIT_MS);
}

/* ---- The reply ---- */

static void on_ack(void)
{
    rx_t *rx = &s_rx[RX_NOTE];
    if (rx->status != 200 || rx->overflow) {
        ESP_LOGW(TAG, "chat/stream: %d", rx->status);
        fail(rx->status < 0 ? tr("LOST CONNECTION TO MUSE") : tr("MUSE DIDN'T TAKE IT"));
        return;
    }
    cJSON *root = cJSON_Parse(rx->body);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_IsObject(result) ? result : root, "message_id"));
    strlcpy(s_turn.note_id, id ? id : "", sizeof(s_turn.note_id));
    const char *parent = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_IsObject(result) ? result : root, "reply_to_message_id"));
    strlcpy(s_turn.parent_id, parent ? parent : "", sizeof(s_turn.parent_id));
    cJSON_Delete(root);
    if (!s_turn.note_id[0]) {
        ESP_LOGW(TAG, "chat/stream ack without a message id");
        fail(tr("MUSE DIDN'T TAKE IT"));
        return;
    }
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    strlcpy(s_note_id, s_turn.note_id, sizeof(s_note_id));
    strlcpy(s_parent_id, s_turn.parent_id, sizeof(s_parent_id));
    unsigned kept = 0;
    for (unsigned i = 0; i < s_pending_count; i++) {
        if (related(&s_pending[i])) s_pending[kept++] = s_pending[i];
    }
    s_pending_count = kept;
    if (!related(&s_delta)) memset(&s_delta, 0, sizeof(s_delta));
    xSemaphoreGive(s_rx_lock);
    s_turn.phase = T_REPLY;
    emit(MUSE_HATCH_EV_SENT, NULL);
}

/* Handles a completed subscription row on the voice task. */
static void on_row(row_t *r)
{
    if (!strcmp(r->event, "message.user")) {
        s_turn.after_note = !strcmp(r->msg, s_turn.note_id) || !strcmp(r->msg, s_turn.parent_id);
    }
    if (s_turn.after_note && !strcmp(r->event, "message.user")) {
        /* The row reads "<transcript>\n[file:audio/wav ...]", or tr("[Voice note]") before transcription. */
        char *heard = r->text;   /* the voice task owns this dequeued row */
        char *att = strstr(heard, "\n[file:");
        if (att) {
            *att = '\0';
        }
        if (heard[0] && strcmp(heard, tr("[Voice note]")) && !s_turn.heard) {
            char line[EV_TEXT];
            muse_hatch_tail_words(heard, line, sizeof(line));
            emit(MUSE_HATCH_EV_HEARD, line);
            s_turn.heard = true;
        }
        return;
    }
    /* Explicit parents must match the ACK. Unparented live replies follow the note. */
    if (strcmp(r->event, "message.assistant")
        || (r->reply_to[0] && strcmp(r->reply_to, s_turn.note_id) && strcmp(r->reply_to, s_turn.parent_id))) {
        return;
    }
    if (!r->ready) {
        return;
    }
    for (unsigned i = 0; i < s_turn.seen_count; i++) {
        if (!strcmp(r->msg, s_turn.seen[i])) return;
    }
    if (r->text[0]) {
        if (s_turn.seen_count == 2) {
            memcpy(s_turn.seen[0], s_turn.seen[1], sizeof(s_turn.seen[0]));
            s_turn.seen_count = 1;
        }
        strlcpy(s_turn.seen[s_turn.seen_count++], r->msg, sizeof(s_turn.seen[0]));
        if (s_turn.text[0]) text_append(s_turn.text, sizeof(s_turn.text), " ");
        text_append(s_turn.text, sizeof(s_turn.text), r->text);
        ESP_LOGI(TAG, "reply after %.2fs: %.80s", (esp_timer_get_time() - s_turn.t_end) / 1e6, r->text);
        if (!s_turn.replied) {
            s_turn.t_show = esp_timer_get_time();
        }
        s_turn.replied = true;
        s_turn.t_reply = esp_timer_get_time();
    }
    return;
}

static void subscription_error(int status)
{
    if (status == 403) fail(tr("MUSE REPLY ACCESS DENIED (403)"));
    else if (status == 401) fail(tr("MUSE REPLY AUTH REQUIRED (401)"));
    else if (status <= 0 || status == 200) fail(tr("LOST CONNECTION TO MUSE"));
    else {
        char why[EV_TEXT];
        snprintf(why, sizeof(why), "MUSE REPLY ERROR (HTTP %d)", status);
        fail(why);
    }
}

/* How far through the reply reading has got. */
static size_t read_at(int64_t now)
{
    return (size_t)((now - s_turn.t_show) * READ_CHARS_PER_S / 1000000);
}

/* Scrolls the reply through the caption at reading pace; true once it's all been shown. */
static bool scroll(int64_t now)
{
    size_t len = strlen(s_turn.text);
    size_t at = read_at(now);
    char line[EV_TEXT];
    if (muse_hatch_caption_at(s_turn.text, at < len ? at : len - 1, line, sizeof(line))
        && strcmp(line, s_turn.shown)) {
        strlcpy(s_turn.shown, line, sizeof(s_turn.shown));
        emit(MUSE_HATCH_EV_REPLY, line);
    }
    return at >= len;
}

/* Moves the turn along; runs on the voice task each time it asks for events. */
static void pump(void)
{
    int64_t now = esp_timer_get_time();
    if (s_turn.phase == T_ACK && received(RX_NOTE)) {
        on_ack();
    }
    if (s_turn.phase == T_IDLE) return;
    /* Preserve early events until the POST ACK supplies the note IDs. */
    static row_t pending; /* voice task only; avoid a large stack frame */
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    int status = s_rx[RX_SUB].status;
    bool closed = s_rx[RX_SUB].done;
    bool early_evicted = s_early_evicted;
    bool skipped_big = s_skipped_big;
    xSemaphoreGive(s_rx_lock);
    if (status < 0 || status >= 400) {
        subscription_error(status);
        return;
    }
    if (s_turn.phase == T_REPLY) {
        for (;;) {
            xSemaphoreTake(s_rx_lock, portMAX_DELAY);
            bool have = s_pending_count > 0;
            if (have) {
                pending = s_pending[0];
                if (--s_pending_count) s_pending[0] = s_pending[1];
            }
            xSemaphoreGive(s_rx_lock);
            if (!have) break;
            on_row(&pending);
        }
        if (closed && !s_turn.replied) {
            subscription_error(status);
            return;
        }
    }
    if (s_turn.phase != T_REPLY) {
        if (s_turn.phase == T_ACK && now - s_turn.t_end > REPLY_TIMEOUT_US) {
            fail(tr("NO REPLY FROM MUSE"));
        }
        return;
    }
    if (s_turn.replied) {
        if (scroll(now) && now - s_turn.t_reply > SETTLE_US) {
            end_turn();
            emit(MUSE_HATCH_EV_DONE, NULL);
        }
    } else if (now - s_turn.t_end > REPLY_TIMEOUT_US) {
        fail(skipped_big ? tr("REPLY TOO LONG") : early_evicted ? tr("REPLY BUFFER LIMIT - TRY AGAIN") : tr("NO REPLY FROM MUSE"));
    }
}

/* ---- Public ---- */

void muse_hatch_start(void)
{
    s_rx_lock = xSemaphoreCreateMutex();
    s_events = xQueueCreate(8, sizeof(ev_t));
    static char ack[ACK_MAX];
    s_rx[RX_NOTE].body = ack;
    s_rx[RX_NOTE].cap = sizeof(ack);
}

void muse_hatch_status(muse_hatch_status_t *out)
{
    if (!muse_link_hatch_linked()) {
        out->state = MUSE_HATCH_NOT_SET;
        strlcpy(out->detail, tr("Pair in the Muse app"), sizeof(out->detail));
    } else if (!muse_wifi_connected()) {
        out->state = MUSE_HATCH_OFFLINE;
        strlcpy(out->detail, tr("Waiting for Wi-Fi"), sizeof(out->detail));
    } else if (muse_link_req_ready()) {
        out->state = MUSE_HATCH_REACHABLE;
        strlcpy(out->detail, tr("Through Home Link, text replies"), sizeof(out->detail));
    } else {
        out->state = MUSE_HATCH_TESTING;
        strlcpy(out->detail, tr("Connecting..."), sizeof(out->detail));
    }
}

void muse_hatch_test(void)
{
}

void muse_hatch_config_changed(void)
{
}

void muse_hatch_set_resting(bool resting)
{
    (void)resting;   /* Link's session does the polling */
}

const char *muse_hatch_state_name(muse_hatch_state_t state)
{
    switch (state) {
    case MUSE_HATCH_NOT_SET: return tr("Not set up");
    case MUSE_HATCH_OFFLINE: return tr("Offline");
    case MUSE_HATCH_UNTESTED: return tr("Saved");
    case MUSE_HATCH_TESTING: return tr("Connecting");
    case MUSE_HATCH_REACHABLE: return tr("Connected");
    case MUSE_HATCH_UNREACHABLE: return tr("Can't connect");
    }
    return "";
}

bool muse_hatch_ready(void)
{
    return s_events && muse_link_hatch_linked() && muse_wifi_connected() && muse_link_req_ready();
}

void muse_hatch_turn_begin(void)
{
    end_turn();
    xQueueReset(s_events);
    memset(&s_turn, 0, sizeof(s_turn));
    s_turn.stage = malloc(STAGE_BYTES);
    s_turn.chunk = malloc(CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL));
    if (!s_turn.stage || !s_turn.chunk) {
        fail(tr("OUT OF MEMORY"));
        return;
    }
    if (!request(RX_NOTE, "POST", "/chat/stream", true, false)
        || !muse_link_req_send(s_stream[RX_NOTE], MUSE_HATCH_NOTE_HEAD, sizeof(MUSE_HATCH_NOTE_HEAD) - 1, false, SEND_WAIT_MS)) {
        fail(tr("CAN'T REACH MUSE"));
        return;
    }
    muse_hatch_wav_header(s_turn.stage, MIC_RATE);
    s_turn.stage_len = MUSE_HATCH_WAV_HEADER;
    s_turn.phase = T_TALKING;
}

void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    const uint8_t *p = (const uint8_t *)pcm;
    size_t n = frames * 2;
    while (s_turn.phase == T_TALKING && n) {
        size_t take = STAGE_BYTES - s_turn.stage_len < n ? STAGE_BYTES - s_turn.stage_len : n;
        memcpy(s_turn.stage + s_turn.stage_len, p, take);
        s_turn.stage_len += take;
        p += take;
        n -= take;
        if (s_turn.stage_len == STAGE_BYTES && !send_stage(false)) {
            fail(tr("CAN'T KEEP UP"));
        }
    }
}

void muse_hatch_turn_end(void)
{
    if (s_turn.phase != T_TALKING) {
        /* Failed while recording: that error went to the recording caption. */
        emit(MUSE_HATCH_EV_ERROR, s_turn.error[0] ? s_turn.error : tr("CAN'T REACH MUSE"));
        return;
    }
    /* Subscribe only after recording: keep inbound reply traffic out of the
     * real-time audio upload. Queue it before the note's final body chunk. */
    if (!request(RX_SUB, "POST", "/chat/subscribe", true, false)
        || !muse_link_req_send(s_stream[RX_SUB], "{}", 2, true, SEND_WAIT_MS)) {
        fail(tr("CAN'T SUBSCRIBE TO MUSE"));
        return;
    }
    if (!send_stage(true)) {
        fail(tr("CAN'T KEEP UP"));
        return;
    }
    free(s_turn.stage);
    free(s_turn.chunk);
    s_turn.stage = NULL;
    s_turn.chunk = NULL;
    s_turn.t_end = esp_timer_get_time();
    s_turn.phase = T_ACK;
}

void muse_hatch_turn_cancel(void)
{
    end_turn();
    xQueueReset(s_events);
}

muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    pump();
    ev_t ev;
    if (xQueueReceive(s_events, &ev, 0) != pdTRUE) {
        return MUSE_HATCH_EV_NONE;
    }
    strlcpy(text, ev.text, cap);
    return ev.type;
}

bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    (void)played;   /* no speech: the reply's page follows the reading pace */
    size_t len = strlen(s_turn.text);
    if (s_turn.phase != T_REPLY || !s_turn.replied || !len) {
        return false;
    }
    size_t at = read_at(esp_timer_get_time());
    return muse_hatch_caption_at(s_turn.text, at < len ? at : len - 1, out, cap);
}

size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    (void)pcm;
    (void)frames;
    if (wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(wait_ms) ? pdMS_TO_TICKS(wait_ms) : 1);
    }
    return 0;
}

size_t muse_hatch_mp3_selftest(int16_t **pcm)
{
    *pcm = NULL;
    return 0;
}
