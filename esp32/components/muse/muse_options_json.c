// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#include "muse_options.h"
#include "cJSON.h"
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

static bool receive_widget(const cJSON *widget, const char *message, const char *question)
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
    const char *title = string(data, "title");
    if (!title) title = question;
    /* A question is display-only and can be clipped; IDs and choices cannot. */
    if (title) {
        size_t n = strlen(title);
        if (n >= sizeof(q.question)) {
            n = sizeof(q.question) - 1;
            while (n && ((unsigned char)title[n] & 0xc0) == 0x80) --n;
        }
        memcpy(q.question, title, n);
        char *marker;
        while ((marker = strstr(q.question, "[[hatch_widget:"))) {
            char *end = strstr(marker, "]]");
            if (!end) { *marker = '\0'; break; }
            memmove(marker, end + 2, strlen(end + 2) + 1);
        }
    }
    if (ok) ok = muse_options_publish(&q);
    cJSON_Delete(data_owned);
    cJSON_Delete(state_owned);
    return ok;
}

bool muse_options_receive(const cJSON *payload)
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
    const char *question = string(payload, "display_text");
    if (!question) question = string(payload, "content");
    const cJSON *widgets = member(payload, "widgets"), *widget;
    bool found = false;
    cJSON_ArrayForEach(widget, widgets) {
        if (receive_widget(widget, id, question)) found = true;
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
