// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#include "muse_options.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#ifdef ESP_PLATFORM
#include "muse_mem.h"
#endif

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_options_t *s_question;
static uint32_t s_generation, s_revision;

bool muse_options_console(const char *line)
{
    bool select = !strncmp(line, "options.select=", 15);
    if (strcmp(line, "options") && !select) return false;
    bool accepted = false;
    if (select) {
        char *end;
        const char *p = line + 15;
        unsigned long generation = strtoul(p, &end, 10);
        if (*p >= '0' && *p <= '9' && end != p && *end == ',' && generation <= UINT32_MAX) {
            p = end + 1;
            unsigned long index = strtoul(p, &end, 10);
            if (*p >= '0' && *p <= '9' && end != p && !*end && index < MUSE_OPTIONS_MAX)
                accepted = muse_options_request((uint32_t)generation, (int)index);
        }
    }
    /* The USB task has a small stack; copy only status, not the label buffers. */
    uint32_t generation = 0;
    int count = 0, selected = -1, status = MUSE_OPTIONS_EMPTY;
    bool saved = false;
    portENTER_CRITICAL(&s_lock);
    if (s_question) {
        generation = s_question->generation; count = s_question->count;
        selected = s_question->selected; status = s_question->status; saved = s_question->saved;
    }
    portEXIT_CRITICAL(&s_lock);
    printf("@options {\"generation\":%lu,\"count\":%d,\"selected\":%d,\"status\":%d,\"saved\":%s,\"accepted\":%s}\n",
           (unsigned long)generation, count, selected, status,
           saved ? "true" : "false", accepted ? "true" : "false");
    fflush(stdout);
    return true;
}

bool muse_options_snapshot(muse_options_t *out)
{
    portENTER_CRITICAL(&s_lock);
    if (s_question) *out = *s_question;
    else { memset(out, 0, sizeof(*out)); out->selected = -1; }
    portEXIT_CRITICAL(&s_lock);
    return out->status != MUSE_OPTIONS_EMPTY;
}

void muse_options_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_question) memset(s_question, 0, sizeof(*s_question));
    ++s_generation; ++s_revision;
    if (s_question) { s_question->generation = s_generation; s_question->revision = s_revision; }
    portEXIT_CRITICAL(&s_lock);
}

void muse_options_disconnected(void)
{
    portENTER_CRITICAL(&s_lock);
    if (!s_question) { portEXIT_CRITICAL(&s_lock); return; }
    if (s_question->status == MUSE_OPTIONS_SENDING) {
        s_question->status = MUSE_OPTIONS_UNCERTAIN;
        snprintf(s_question->detail, sizeof(s_question->detail), "Check the Muse app before answering again.");
    } else if (s_question->status == MUSE_OPTIONS_READY || s_question->status == MUSE_OPTIONS_QUEUED) {
        s_question->status = MUSE_OPTIONS_STALE;
        snprintf(s_question->detail, sizeof(s_question->detail), "Connection lost. Continue in the Muse app.");
    } else if (s_question->status == MUSE_OPTIONS_SELECTED && !s_question->saved) {
        snprintf(s_question->detail, sizeof(s_question->detail), "Answer sent. Check selection in the Muse app.");
    }
    s_question->revision = ++s_revision;
    portEXIT_CRITICAL(&s_lock);
}

bool muse_options_publish(const muse_options_t *q)
{
    if (!q->widget_id[0] || !q->message_id[0] || q->count < 1 || q->count > MUSE_OPTIONS_MAX) return false;
    if (!s_question) {
#ifdef ESP_PLATFORM
        muse_options_t *allocated = heap_caps_calloc(1, sizeof(*allocated), MUSE_BIG_CAPS);
#else
        muse_options_t *allocated = calloc(1, sizeof(*allocated));
#endif
        if (!allocated) return false;
        portENTER_CRITICAL(&s_lock);
        if (!s_question) { s_question = allocated; allocated = NULL; }
        portEXIT_CRITICAL(&s_lock);
        free(allocated);
    }
    portENTER_CRITICAL(&s_lock);
    bool same = !strcmp(s_question->widget_id, q->widget_id) && !strcmp(s_question->message_id, q->message_id);
    /* A duplicate must not undo a queued/in-flight/accepted/uncertain choice. */
    if (same && s_question->count == q->count && !memcmp(s_question->labels, q->labels, sizeof(q->labels))) {
        if (q->state_version > s_question->state_version) s_question->state_version = q->state_version;
        if (q->selected >= 0 && q->state_version >= s_question->state_version) {
            s_question->selected = q->selected;
            s_question->state_version = q->state_version;
            s_question->status = MUSE_OPTIONS_SELECTED;
            s_question->saved = true;
            s_question->detail[0] = '\0';
            s_question->revision = ++s_revision;
        }
        portEXIT_CRITICAL(&s_lock);
        return true;
    }
    *s_question = *q;
    s_question->generation = ++s_generation;
    s_question->revision = ++s_revision;
    s_question->status = q->selected >= 0 ? MUSE_OPTIONS_SELECTED : MUSE_OPTIONS_READY;
    s_question->saved = q->selected >= 0;
    s_question->detail[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool muse_options_request(uint32_t generation, int index)
{
    portENTER_CRITICAL(&s_lock);
    bool ok = s_question && s_question->generation == generation && s_question->status == MUSE_OPTIONS_READY
              && index >= 0 && index < s_question->count;
    if (ok) {
        s_question->selected = index;
        s_question->status = MUSE_OPTIONS_QUEUED;
        s_question->revision = ++s_revision;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

bool muse_options_pending(void)
{
    portENTER_CRITICAL(&s_lock);
    bool pending = s_question && s_question->status == MUSE_OPTIONS_QUEUED;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

bool muse_options_waiting(void)
{
    portENTER_CRITICAL(&s_lock);
    bool waiting = s_question && (s_question->status == MUSE_OPTIONS_READY ||
                    s_question->status == MUSE_OPTIONS_QUEUED || s_question->status == MUSE_OPTIONS_SENDING);
    portEXIT_CRITICAL(&s_lock);
    return waiting;
}

bool muse_options_take(muse_options_t *out)
{
    portENTER_CRITICAL(&s_lock);
    bool ok = s_question && s_question->status == MUSE_OPTIONS_QUEUED;
    if (ok) {
        s_question->status = MUSE_OPTIONS_SENDING;
        s_question->revision = ++s_revision;
        *out = *s_question;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

void muse_options_result(uint32_t generation, muse_options_status_t status, bool saved, const char *detail)
{
    portENTER_CRITICAL(&s_lock);
    if (s_question && s_question->generation == generation) {
        /* A newer server selection wins over an in-flight local request. */
        if (s_question->saved && s_question->status == MUSE_OPTIONS_SELECTED) {
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        s_question->status = status;
        s_question->saved = saved;
        if (status == MUSE_OPTIONS_READY) s_question->selected = -1;
        snprintf(s_question->detail, sizeof(s_question->detail), "%s", detail ? detail : "");
        s_question->revision = ++s_revision;
    }
    portEXIT_CRITICAL(&s_lock);
}
