// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#include "muse_options.h"
#include "cJSON.h"
#include <ctype.h>
#include <math.h>
#include <string.h>

static const cJSON *member(const cJSON *j, const char *key) { return cJSON_GetObjectItemCaseSensitive(j, key); }
static const char *string(const cJSON *j, const char *key) { return cJSON_GetStringValue(member(j, key)); }
static bool copy(char *out, size_t cap, const char *text)
{
    if (!text || !*text || strlen(text) >= cap) return false;
    memcpy(out, text, strlen(text) + 1);
    return true;
}

/* Wire data/state may be objects or JSON-encoded strings. */
static const cJSON *object(const cJSON *j, cJSON **owned)
{
    *owned = NULL;
    if (cJSON_IsString(j)) j = *owned = cJSON_Parse(cJSON_GetStringValue(j));
    return cJSON_IsObject(j) ? j : NULL;
}

static void append_question(char *out, size_t cap, const char *text)
{
    if (!text) return;
    size_t used = strlen(out);
    size_t n = strlen(text);
    if (n >= cap - used) {
        n = cap - used - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
    }
    memcpy(out + used, text, n);
    out[used + n] = '\0';
}

/* Display text may contain widget placeholders or only whitespace. Treat those
 * as missing so an empty title/display_text cannot hide the actual question. */
static bool clean_question(char *out)
{
    char *marker;
    while ((marker = strstr(out, "[[hatch_widget:"))) {
        char *end = strstr(marker, "]]");
        if (!end) { *marker = '\0'; break; }
        memmove(marker, end + 2, strlen(end + 2) + 1);
    }
    char *start = out;
    while (isspace((unsigned char)*start)) ++start;
    size_t n = strlen(start);
    while (n && isspace((unsigned char)start[n - 1])) --n;
    memmove(out, start, n);
    out[n] = '\0';
    return n > 0;
}

static bool question_text(char *out, size_t cap, const char *text)
{
    out[0] = '\0';
    append_question(out, cap, text);
    return clean_question(out);
}

static bool transcript_question(char *out, size_t cap, const cJSON *payload)
{
    const cJSON *rows = member(member(payload, "transcript"), "messages"), *row;
    out[0] = '\0';
    cJSON_ArrayForEach(row, rows) {
        const char *role = string(row, "role");
        if (!role || strcmp(role, "assistant")) continue;
        const cJSON *blocks = member(row, "content"), *block;
        cJSON_ArrayForEach(block, blocks) {
            const char *type = string(block, "type"), *text = string(block, "text");
            if (!type || strcmp(type, "text") || !text || !text[0]) continue;
            if (out[0]) append_question(out, cap, "\n");
            append_question(out, cap, text);
        }
    }
    return clean_question(out);
}

static bool receive_widget(const cJSON *widget, const char *message, const cJSON *payload,
                           const char *streamed_text)
{
    const char *kind = string(widget, "kind");
    if (!kind || strcmp(kind, "option")) return false;
    muse_options_t q = { .selected = -1 };
    const char *id = string(widget, "widget_id");
    if (!id) id = string(widget, "id");
    if (!copy(q.widget_id, sizeof(q.widget_id), id) || !copy(q.message_id, sizeof(q.message_id), message)) return false;
    cJSON *data_owned, *state_owned;
    const cJSON *data = object(member(widget, "data"), &data_owned);
    const cJSON *state = object(member(widget, "state"), &state_owned);
    const cJSON *version = member(state, "version");
    if (cJSON_IsNumber(version) && isfinite(version->valuedouble) && version->valuedouble >= 0) {
        q.state_version = version->valuedouble;
        state = member(state, "data");
    }
    const cJSON *options = member(data, "options");
    bool ok = cJSON_IsArray(options);
    q.count = cJSON_GetArraySize(options);
    ok = ok && q.count > 0 && q.count <= MUSE_OPTIONS_MAX;
    /* URL actions require a different interaction; do not mis-submit them as text. */
    const cJSON *actions = member(data, "option_actions");
    if (actions && (!cJSON_IsArray(actions) || cJSON_GetArraySize(actions))) ok = false;
    const char *selected = string(state, "selectedText");
    for (int i = 0; ok && i < q.count; ++i) {
        ok = copy(q.labels[i], sizeof(q.labels[i]), cJSON_GetStringValue(cJSON_GetArrayItem(options, i)));
        for (int k = 0; ok && k < i; ++k) if (!strcmp(q.labels[k], q.labels[i])) ok = false;
        if (selected && !strcmp(selected, q.labels[i])) q.selected = i;
    }
    /* A question is display-only and can be clipped; IDs and choices cannot. */
    const char *candidates[] = {string(widget, "title"), string(data, "title"),
        string(payload, "display_text"), string(member(payload, "payload"), "content"),
        string(payload, "content")};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (question_text(q.question, sizeof(q.question), candidates[i])) break;
    }
    if (!q.question[0] && !transcript_question(q.question, sizeof(q.question), payload)) {
        question_text(q.question, sizeof(q.question), streamed_text);
    }
    if (ok) ok = muse_options_publish(&q);
    cJSON_Delete(data_owned);
    cJSON_Delete(state_owned);
    return ok;
}

bool muse_options_receive(const cJSON *payload)
{
    return muse_options_receive_message(payload, NULL);
}

bool muse_options_receive_message(const cJSON *payload, const char *streamed_text)
{
    const char *host = string(payload, "host_message_id"), *widget_id = string(payload, "widget_id");
    if (host && widget_id) {
        muse_options_t q;
        if (!muse_options_snapshot(&q) || strcmp(host, q.message_id) || strcmp(widget_id, q.widget_id)) return false;
        cJSON *owned;
        const cJSON *state = object(member(payload, "state"), &owned);
        const cJSON *version = member(state, "version");
        const char *selected = string(member(state, "data"), "selectedText");
        bool valid = cJSON_IsNumber(version) && isfinite(version->valuedouble) && version->valuedouble >= q.state_version;
        if (valid) {
            q.state_version = version->valuedouble;
            q.selected = -1;
            for (int i = 0; selected && i < q.count; ++i) if (!strcmp(selected, q.labels[i])) q.selected = i;
            if (selected && q.selected < 0) {
                muse_options_result(q.generation, MUSE_OPTIONS_STALE, false, "Answered in the Muse app.");
            } else {
                muse_options_publish(&q);
            }
        }
        cJSON_Delete(owned);
        return valid;
    }
    const char *id = string(payload, "message_id");
    if (!id) id = string(payload, "id");
    const cJSON *widgets = member(payload, "widgets"), *widget;
    bool found = false;
    cJSON_ArrayForEach(widget, widgets) {
        if (receive_widget(widget, id, payload, streamed_text)) found = true;
    }
    return found;
}

cJSON *muse_options_chat_body(const muse_options_t *q, const char *interaction_id)
{
    if (q->selected < 0 || q->selected >= q->count) return NULL;
    cJSON *body = cJSON_CreateObject(), *metadata = NULL, *origin = NULL;
    if (!body || !cJSON_AddStringToObject(body, "message", q->labels[q->selected]) ||
        !cJSON_AddStringToObject(body, "output_modality", "text") ||
        !(metadata = cJSON_AddObjectToObject(body, "metadata")) ||
        !(origin = cJSON_AddObjectToObject(metadata, "message_origin")) ||
        !cJSON_AddStringToObject(origin, "kind", "options_widget") ||
        !cJSON_AddStringToObject(origin, "widget_id", q->widget_id) ||
        !cJSON_AddStringToObject(origin, "message_id", q->message_id) ||
        !cJSON_AddNumberToObject(origin, "option_index", q->selected) ||
        !cJSON_AddStringToObject(origin, "interaction_id", interaction_id)) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

cJSON *muse_options_state_body(const muse_options_t *q, const char *mutation_id)
{
    if (q->selected < 0 || q->selected >= q->count) return NULL;
    cJSON *body = cJSON_CreateObject(), *state;
    if (!body || !cJSON_AddStringToObject(body, "widget_id", q->widget_id) ||
        !cJSON_AddNumberToObject(body, "base_version", q->state_version) ||
        !cJSON_AddStringToObject(body, "mutation_id", mutation_id) ||
        !(state = cJSON_AddObjectToObject(body, "state")) ||
        !cJSON_AddStringToObject(state, "selectedText", q->labels[q->selected])) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

bool muse_options_state_response(const muse_options_t *q, const cJSON *response)
{
    const cJSON *result = member(response, "result");
    if (cJSON_IsObject(result)) response = result;
    const char *selected = string(member(member(response, "state"), "data"), "selectedText");
    return cJSON_IsTrue(member(response, "applied")) ||
           (selected && q->selected >= 0 && q->selected < q->count && !strcmp(selected, q->labels[q->selected]));
}
