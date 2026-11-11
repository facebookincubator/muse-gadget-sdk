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

/* The apps' chat parser and message store (muse_apps_chat.c's host-tested
 * part, extracted by tests/test_apps_vm.py into chat_section.inc), fed
 * synthetic subscription lines and history in pieces of every size. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_apps_api.h"
#include "chat_section.inc"

static int failures;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
        }                                                             \
    } while (0)

/* ---- What the app is told ---- */

typedef struct {
    int what, value;
} told_t;

static told_t s_told[256];
static int s_ntold;
static bool s_queue_full;

static bool record(void *ctx, int what, int value)
{
    (void)ctx;
    if (s_queue_full || s_ntold >= 256) {
        return false;
    }
    s_told[s_ntold++] = (told_t){what, value};
    return true;
}

static void told_reset(void)
{
    s_ntold = 0;
}

static bool told_is(const told_t *want, int n)
{
    if (n != s_ntold) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (want[i].what != s_told[i].what || want[i].value != s_told[i].value) {
            return false;
        }
    }
    return true;
}

static void dump_told(void)
{
    for (int i = 0; i < s_ntold; i++) {
        fprintf(stderr, "  told %d %d\n", s_told[i].what, s_told[i].value);
    }
}

/* ---- Feeding ---- */

static chat_store_t *new_store(void)
{
    chat_store_t *s = calloc(1, sizeof(*s));
    chat_store_init(s, 1, record, NULL);
    return s;
}

static void apply_line(void *ctx, const chat_row_t *row)
{
    chat_apply(ctx, row, false);
}

static void apply_history(void *ctx, const chat_row_t *row)
{
    chat_apply(ctx, row, true);
}

/* Feeds text to a fresh line parser in pieces of chunk bytes (0: whole). */
static void feed_lines(chat_store_t *s, chat_row_t *row, const char *text, size_t chunk)
{
    chat_parser_t p;
    chat_parser_init(&p, true, row, apply_line, s);
    size_t n = strlen(text);
    if (!chunk) {
        chunk = n ? n : 1;
    }
    for (size_t at = 0; at < n; at += chunk) {
        /* A copy of just this piece, so reading past it shows up under ASan. */
        size_t k = n - at < chunk ? n - at : chunk;
        char *piece = malloc(k);
        memcpy(piece, text + at, k);
        chat_parse(&p, piece, k);
        free(piece);
    }
}

static void feed_history(chat_store_t *s, chat_row_t *row, const char *text)
{
    chat_parser_t p;
    chat_parser_init(&p, false, row, apply_history, s);
    chat_parse(&p, text, strlen(text));
}

static const char *text_of(chat_store_t *s, int32_t id)
{
    chat_msg_t *m = chat_by_id(s, id);
    size_t len;
    return m ? chat_text_of(m, &len) : "(none)";
}

/* ---- The subscription, end to end ---- */

/* An approvals.snapshot line of about 20 KB: keys the parser watches for
 * (text, status, id) nested at every depth, escaped quotes and braces in
 * strings, never buffered whole. */
static char *snapshot_line(void)
{
    size_t cap = 32 * 1024;
    char *s = malloc(cap);
    size_t n = (size_t)snprintf(s, cap, "{\"event\":\"approvals.snapshot\",\"payload\":{\"approvals\":[");
    for (int i = 0; n < 20000; i++) {
        n += (size_t)snprintf(s + n, cap - n,
                              "%s{\"id\":\"a%d\",\"text\":\"run \\\"rm\\\" {x} [y]\",\"status\":\"pending\","
                              "\"detail\":{\"text\":\"deep\",\"list\":[1,2,{\"role\":\"user\"}]}}",
                              i ? "," : "", i);
    }
    snprintf(s + n, cap - n, "]},\"seq\":1,\"ts_ms\":1,\"type\":\"event\"}\n");
    return s;
}

static const char *const LINES =
    "{\"event\":\"agent.status\",\"payload\":{\"activity_code\":\"working\"},\"seq\":2,\"ts_ms\":5,\"type\":\"event\"}\n"
    "{\"event\":\"message.user\",\"payload\":{\"display_text\":\"[clock on the gadget] What time is it?\","
    "\"role\":\"user\",\"message_id\":\"u1\"},\"seq\":3,\"type\":\"event\"}\r\n"
    "{\"event\":\"message.user\",\"payload\":{\"display_te\n"   /* cut short: skipped */
    "{\"event\":\"delta.message_start\",\"payload\":{\"message_id\":\"m1\",\"role\":\"assistant\"},\"seq\":4,\"type\":\"event\"}\n"
    "{\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"m1\",\"text\":\"It's **noon** \\ud83d\\ude00 caf\\u00e9\"},"
    "\"seq\":5,\"type\":\"event\"}\n"
    "{\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"m1\",\"text\":\"DUPLICATE\"},\"seq\":5,\"type\":\"event\"}\n"
    "{\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"m1\",\"text\":\" \\u2014 see [docs](https://x.y/z).\"},"
    "\"seq\":6,\"type\":\"event\"}\n"
    "{\"event\":\"delta.message_done\",\"payload\":{\"message_id\":\"m1\",\"display_text\":"
    "\"It's **noon** \xf0\x9f\x98\x80 caf\xc3\xa9 \xe2\x80\x94 see [docs](https://x.y/z).\"},\"seq\":7,\"type\":\"event\"}\n"
    "{\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"m1\",\"role\":\"assistant\",\"display_text_ready\":true,"
    "\"display_text\":\"It's **noon** \xf0\x9f\x98\x80 caf\xc3\xa9 \xe2\x80\x94 see [docs](https://x.y/z).\"},\"seq\":8,\"type\":\"event\"}\n"
    "{\"event\":\"agent.status\",\"payload\":{\"activity_code\":\"idle\"},\"seq\":9,\"type\":\"event\"}\n"
    "{\"payload\":{\"role\":\"user\",\"message_id\":\"u2\",\"display_text\":\"Turn on the lights\\n[file:audio/wav workspace/n.wav]\"},"
    "\"type\":\"event\",\"event\":\"message.user\",\"seq\":10}\n"
    "{\"event\":\"message.user\",\"payload\":{\"display_text\":\"not an event\",\"message_id\":\"u3\"},\"seq\":11,\"type\":\"other\"}\n";

static void test_subscription(void)
{
    char *snap = snapshot_line();
    size_t n = strlen(snap) + strlen(LINES) + 1;
    char *all = malloc(n);
    snprintf(all, n, "%s%s", snap, LINES);
    static const size_t chunks[] = {0, 1, 2, 3, 7, 64, 1000, 4096, 12288};
    chat_row_t *row = malloc(sizeof(*row));
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        chat_store_t *s = new_store();
        chat_store_connected(s);
        told_reset();
        feed_lines(s, row, all, chunks[c]);
        const told_t want[] = {
            {GADGET_CHAT_BUSY, 1},
            {GADGET_CHAT_MESSAGE, 1}, {GADGET_CHAT_DONE, 1},
            {GADGET_CHAT_MESSAGE, 2}, {GADGET_CHAT_TEXT, 2},   /* the second append doesn't tell again: unread */
            {GADGET_CHAT_DONE, 2},                             /* message.assistant after it: nothing new */
            {GADGET_CHAT_BUSY, 0},
            {GADGET_CHAT_MESSAGE, 3}, {GADGET_CHAT_DONE, 3},
        };
        CHECK(told_is(want, sizeof(want) / sizeof(want[0])), "events, in pieces of %zu", chunks[c]);
        if (!told_is(want, sizeof(want) / sizeof(want[0]))) dump_told();
        CHECK(s->count == 3, "%d messages", s->count);
        chat_msg_t *u1 = chat_by_id(s, 1), *m1 = chat_by_id(s, 2), *u2 = chat_by_id(s, 3);
        CHECK(u1 && u1->role == GADGET_ROLE_APP && u1->done, "an app's message");
        CHECK(!strcmp(text_of(s, 1), "What time is it?"), "prefix removed: %s", text_of(s, 1));
        CHECK(m1 && m1->role == GADGET_ROLE_MUSE && m1->done && !m1->cut, "Muse's reply");
        CHECK(!strcmp(text_of(s, 2), "It's noon \xf0\x9f\x98\x80 caf\xc3\xa9 \xe2\x80\x94 see docs."),
              "reply cleaned, pieces of %zu: %s", chunks[c], text_of(s, 2));
        CHECK(!strstr(m1->raw, "DUPLICATE"), "a repeated seq is dropped");
        CHECK(u2 && u2->role == GADGET_ROLE_USER && !strcmp(text_of(s, 3), "Turn on the lights"),
              "attachment line dropped: %s", text_of(s, 3));
        CHECK(chat_latest_id(s, 0) == 3 && chat_latest_id(s, 2) == 1 && chat_latest_id(s, 3) == 0, "latest");
        free(s);
    }
    free(row);
    free(all);
    free(snap);
}

/* The parser reads a line longer than any buffer it has: a 4 KB text cut on a
 * character, the rest of the line still read. */
static void test_long_text(void)
{
    size_t cap = 16 * 1024;
    char *line = malloc(cap);
    size_t n = (size_t)snprintf(line, cap, "{\"event\":\"message.assistant\",\"payload\":{\"display_text\":\"");
    for (int i = 0; i < 4095; i++) line[n++] = 'a';
    n += (size_t)snprintf(line + n, cap - n, "\\u00e9");   /* 2 bytes from 4095: past 4096 */
    for (int i = 0; i < 3000; i++) line[n++] = 'b';
    snprintf(line + n, cap - n, "\",\"message_id\":\"m7\"},\"seq\":3,\"type\":\"event\"}\n");
    chat_row_t *row = malloc(sizeof(*row));
    chat_store_t *s = new_store();
    told_reset();
    feed_lines(s, row, line, 1000);
    chat_msg_t *m = chat_by_id(s, 1);
    CHECK(m && m->cut && m->raw_len == 4095 && m->done, "cut at 4095 on a character: %zu", m ? m->raw_len : 0);
    CHECK(m && !strcmp(m->key, "m7"), "the id after the long text is read: %s", m ? m->key : "");
    /* Appends past the limit cut too, and say so once. */
    chat_store_t *t = new_store();
    char piece[512];
    snprintf(piece, sizeof(piece), "{\"event\":\"delta.message_start\",\"payload\":{\"message_id\":\"x\"},\"type\":\"event\"}\n");
    feed_lines(t, row, piece, 0);
    for (int i = 0; i < 50; i++) {
        char text[200];
        memset(text, 'c', 99);
        text[99] = '\0';
        snprintf(piece, sizeof(piece), "{\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"x\",\"text\":\"%s\"},"
                 "\"type\":\"event\"}\n", text);
        feed_lines(t, row, piece, 0);
    }
    chat_msg_t *x = chat_by_id(t, 1);
    CHECK(x && x->cut && x->raw_len == 4096 && !x->done, "appends cut at 4096: %zu", x ? x->raw_len : 0);
    free(t);
    free(s);
    free(row);
    free(line);
}

/* Eight kept: the ninth pushes out the oldest. */
static void test_eviction(void)
{
    chat_row_t *row = malloc(sizeof(*row));
    chat_store_t *s = new_store();
    for (int i = 0; i < 10; i++) {
        char line[200];
        snprintf(line, sizeof(line), "{\"event\":\"message.user\",\"payload\":{\"display_text\":\"hi %d\",\"message_id\":\"k%d\"},"
                 "\"type\":\"event\"}\n", i, i);
        feed_lines(s, row, line, 5);
    }
    CHECK(s->count == 8, "%d kept", s->count);
    CHECK(!chat_by_id(s, 1) && !chat_by_id(s, 2) && chat_by_id(s, 3), "oldest gone");
    CHECK(chat_latest_id(s, 0) == 10 && chat_latest_id(s, 7) == 3, "order");
    CHECK(!strcmp(text_of(s, 10), "hi 9"), "%s", text_of(s, 10));
    free(s);
    free(row);
}

/* History (one document, pretty-printed or not): its message is older than
 * what this subscription brought, and it finishes a reply cut off by a drop. */
static void test_history(void)
{
    chat_row_t *row = malloc(sizeof(*row));
    chat_store_t *s = new_store();
    chat_store_connected(s);
    told_reset();
    feed_lines(s, row, "{\"event\":\"message.user\",\"payload\":{\"display_text\":\"new\",\"message_id\":\"n1\"},\"type\":\"event\"}\n", 0);
    feed_history(s, row,
                 "{\n  \"ok\": true,\n  \"result\": {\n    \"chat_events\": [\n      {\"display_text\": \"Hello \\\"there\\\"\",\n"
                 "       \"event_name\": \"message.assistant\", \"message_id\": \"h1\", \"role\": \"assistant\",\n"
                 "       \"meta\": {\"text\": \"not this\", \"role\": \"user\"}}\n    ],\n    \"has_more\": true\n  }\n}\n");
    CHECK(s->count == 2 && chat_latest_id(s, 0) == 1 && chat_latest_id(s, 1) == 2, "history goes first");
    chat_msg_t *h = chat_by_id(s, 2);
    CHECK(h && h->role == GADGET_ROLE_MUSE && h->done && !strcmp(text_of(s, 2), "Hello \"there\""), "history: %s", text_of(s, 2));
    /* Again (a reconnect): the same message isn't added twice. */
    told_reset();
    chat_store_connected(s);
    feed_history(s, row, "{\"ok\":true,\"result\":{\"chat_events\":[{\"display_text\":\"Hello \\\"there\\\"\",\"message_id\":\"h1\","
                 "\"role\":\"assistant\"}]}}");
    CHECK(s->count == 2 && s_ntold == 0, "no duplicate");
    /* A reply cut off by a drop is finished by the history after it. */
    feed_lines(s, row, "{\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"r1\",\"text\":\"Half a\"},\"type\":\"event\"}\n", 0);
    chat_store_lost(s);
    chat_store_connected(s);
    told_reset();
    feed_history(s, row, "{\"ok\":true,\"result\":{\"chat_events\":[{\"display_text\":\"Half a reply, now whole.\",\"message_id\":\"r1\","
                 "\"role\":\"assistant\"}]}}");
    chat_msg_t *r = chat_by_id(s, 3);
    const told_t want[] = {{GADGET_CHAT_DONE, 3}};
    CHECK(r && r->done && told_is(want, 1) && !strcmp(text_of(s, 3), "Half a reply, now whole."), "finished: %s", text_of(s, 3));
    /* Missed while the link was down: newer than everything kept. */
    feed_history(s, row, "{\"ok\":true,\"result\":{\"chat_events\":[{\"display_text\":\"while away\",\"message_id\":\"w1\",\"role\":\"user\"}]}}");
    CHECK(chat_latest_id(s, 0) == 4, "newest");
    free(s);
    free(row);
}

static void test_status_and_ready(void)
{
    chat_row_t *row = malloc(sizeof(*row));
    chat_store_t *s = new_store();
    told_reset();
    feed_lines(s, row,
               "{\"event\":\"task.status\",\"payload\":{\"status\":\"running\"},\"type\":\"event\"}\n"
               "{\"event\":\"task.status\",\"payload\":{\"status\":\"running\"},\"type\":\"event\"}\n"
               "{\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"q\",\"display_text\":\"draft\",\"display_text_ready\":false},\"type\":\"event\"}\n"
               "{\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"q\",\"text\":\"low\",\"display_text\":\"final\",\"content\":\"mid\",\"display_text_ready\":true},\"type\":\"event\"}\n",
               3);
    chat_store_lost(s);
    const told_t want[] = {
        {GADGET_CHAT_BUSY, 1}, {GADGET_CHAT_MESSAGE, 1}, {GADGET_CHAT_TEXT, 1}, {GADGET_CHAT_DONE, 1},
        {GADGET_CHAT_BUSY, 0},
    };
    CHECK(told_is(want, 5), "busy once, a draft's TEXT then DONE, busy cleared on loss");
    if (!told_is(want, 5)) dump_told();
    CHECK(!strcmp(text_of(s, 1), "final"), "display_text outranks content and text: %s", text_of(s, 1));
    /* A full queue: TEXT is told again once there's room. */
    s_queue_full = true;
    feed_lines(s, row, "{\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"z\",\"text\":\"a\"},\"type\":\"event\"}\n", 0);
    s_queue_full = false;
    told_reset();
    feed_lines(s, row, "{\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"z\",\"text\":\"b\"},\"type\":\"event\"}\n", 0);
    const told_t again[] = {{GADGET_CHAT_TEXT, 2}};
    CHECK(told_is(again, 1), "TEXT after a dropped one");
    free(s);
    free(row);
}

static void test_clean_and_prefix(void)
{
    static const struct {
        const char *in, *out;
    } cases[] = {
        {"# Title\n\nSome **bold** and `code`.\n\n\n\n* one\n+ two\n- three", "Title\n\nSome bold and code.\n\n- one\n- two\n- three"},
        {"```c\nint x;\n```\n> quoted\n![a cat](http://x/c.png) and [link](http://y)", "int x;\nquoted\na cat and link"},
        {"\n\n  indented ~~gone~~ text  \n\n", "  indented gone text"},
        {"[file:audio/wav workspace/a.wav]", ""},
        {"[not a link] (x) [x](no close", "[not a link] (x) [x](no close"},
        {"2 * 3 * 4 = 24, a_b_c", "2 * 3 * 4 = 24, a_b_c"},
        {"#hashtag, ###### six\n####### seven", "#hashtag, ###### six\n####### seven"},
    };
    char out[256];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t n = chat_clean(cases[i].in, strlen(cases[i].in), out);
        CHECK(n == strlen(out) && !strcmp(out, cases[i].out), "clean %zu: [%s]", i, out);
    }
    CHECK(chat_app_prefix("[clock on the gadget] hi", 24) == 22, "prefix");
    CHECK(chat_app_prefix("[clock on the gadget]", 21) == 21, "prefix alone");
    CHECK(chat_app_prefix("[x] hi", 6) == 0 && chat_app_prefix("[x on the gadgets] hi", 21) == 0, "not a prefix");
    CHECK(chat_app_prefix("hi [x on the gadget] ", 21) == 0, "not at the start");
    CHECK(!chat_visible("[file:a]\n  \n", 12) && chat_visible("[file:a]\nx", 10), "visible");
}

int main(void)
{
    test_subscription();
    test_long_text();
    test_eviction();
    test_history();
    test_status_and_ready();
    test_clean_and_prefix();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
