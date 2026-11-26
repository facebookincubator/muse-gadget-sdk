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

#include "session_image_core.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

bool simg_parse(const cJSON *params, char *path, size_t path_cap, int *duration_ms,
                const char **code, const char **message)
{
    const cJSON *p = cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "path") : NULL;
    if (!cJSON_IsString(p) || !p->valuestring || !p->valuestring[0]) {
        *code = "invalid_params";
        *message = "path is required: the JPEG's path on this VM, e.g. workspace/picture.jpg";
        return false;
    }
    const char *s = p->valuestring;
    size_t n = strlen(s);
    if (n > SIMG_PATH_MAX || n + 1 > path_cap) {
        *code = "invalid_params";
        *message = "path is too long (at most 200 characters)";
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) {
            *code = "invalid_params";
            *message = "path has a control character";
            return false;
        }
    }
    if (!s[strspn(s, "/")]) {
        *code = "invalid_params";
        *message = "path names no file";
        return false;
    }
    int ms = SIMG_DEFAULT_MS;
    const cJSON *d = cJSON_GetObjectItem(params, "duration_ms");
    if (d && !cJSON_IsNull(d)) {
        if (!cJSON_IsNumber(d)) {
            *code = "invalid_params";
            *message = "duration_ms must be a number of milliseconds";
            return false;
        }
        double v = d->valuedouble;
        if (v < 0) {
            *code = "invalid_params";
            *message = "duration_ms must be 0 (until dismissed) or 1000 to 60000";
            return false;
        }
        ms = v == 0 ? 0 : v < SIMG_MIN_MS ? SIMG_MIN_MS : v > SIMG_MAX_MS ? SIMG_MAX_MS : (int)v;
    }
    memcpy(path, s, n + 1);
    *duration_ms = ms;
    return true;
}

size_t simg_chat_body(char *out, size_t cap, const char *gadget_name, const char *node_id)
{
    /* The name as the gadget registers it (its BLE name, link.register's
     * display_name), so Muse can tell which node to use; anything outside
     * the name's own characters is left out rather than escaped. */
    char name[48];
    size_t k = 0;
    for (const char *c = gadget_name ? gadget_name : ""; *c && k + 1 < sizeof(name); c++) {
        if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-'
            || *c == '_' || *c == ' ') {
            name[k++] = *c;
        }
    }
    name[k] = '\0';
    /* The node at the top level, never chat_context (a gadget's chat turn
     * carries device_id or node_id); only a node
     * id's own characters, as the name's. */
    char node[65];
    k = 0;
    for (const char *c = node_id ? node_id : ""; *c && k + 1 < sizeof(node); c++) {
        if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-'
            || *c == '_' || *c == '.' || *c == ':') {
            node[k++] = *c;
        }
    }
    node[k] = '\0';
    int n = snprintf(out, cap,
                     "{\"node_id\":\"%s\",\"message\":\"Please make a random picture (any subject you like) and "
                     "show it on my gadget %s for 5 seconds.\",\"output_modality\":\"text\"}",
                     node, name[0] ? name : "(this one)");
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static void reset(simg_run_t *r)
{
    memset(r, 0, sizeof(*r));
}

static void fail(simg_run_t *r, const char *why)
{
    r->state = SIMG_FAILED;
    snprintf(r->error, sizeof(r->error), "%s", why ? why : "failed");
}

void simg_on_ask(simg_run_t *r, int64_t now)
{
    reset(r);
    r->state = SIMG_ASKING;
    r->asked = true;
    r->press_us = now;
}

void simg_on_ask_failed(simg_run_t *r, const char *why, int64_t now)
{
    reset(r);
    r->asked = true;
    r->press_us = now;
    fail(r, why);
}

void simg_on_chat_status(simg_run_t *r, int status, int64_t now)
{
    if (r->state != SIMG_ASKING || r->chat_status) {
        return;   /* a later frame of the same answer, or the picture came first */
    }
    r->chat_status = status;
    r->chat_us = now;
    if (status == SIMG_CHAT_NO_SESSION) {
        /* It waited 10 s for the session (asked for at once) and none came. */
        fail(r, SIMG_NOT_CONNECTED);
    } else if (status < 0) {
        fail(r, "the connection to Muse dropped (is the Muse app open on the phone?)");
    } else if (status < 200 || status > 299) {
        char why[64];
        snprintf(why, sizeof(why), "Muse didn't take the request (HTTP %d)", status);
        fail(r, why);
    }
}

void simg_on_invoke(simg_run_t *r, int duration_ms, int64_t now)
{
    /* A picture after the test's press (or after its timeout) belongs to
     * that run; otherwise Muse called on its own: a new run. */
    if (!r->asked || r->state == SIMG_SHOWN || (r->state == SIMG_FAILED && r->invoke_us)) {
        reset(r);
    } else if (r->state == SIMG_FAILED) {
        r->error[0] = '\0';
    }
    r->state = SIMG_FETCHING;
    r->invoke_us = now;
    r->drawn_us = 0;
    r->duration_ms = duration_ms;
}

void simg_on_drawn(simg_run_t *r, bool ok, unsigned bytes, int width, int height, int scale,
                   int fetch_ms, const char *error, int64_t now)
{
    if (r->state != SIMG_FETCHING) {
        return;
    }
    r->bytes = bytes;
    r->width = width;
    r->height = height;
    r->scale = scale;
    r->fetch_ms = fetch_ms;
    if (ok) {
        r->state = SIMG_SHOWING;
        r->drawn_us = now;
    } else {
        fail(r, error);
    }
}

void simg_tick(simg_run_t *r, int64_t now)
{
    if (r->state == SIMG_ASKING && now - r->press_us >= (int64_t)SIMG_ASK_TIMEOUT_MS * 1000) {
        char why[64];
        snprintf(why, sizeof(why), "Muse sent no picture in %d s", SIMG_ASK_TIMEOUT_MS / 1000);
        fail(r, why);
    } else if (r->state == SIMG_SHOWING && r->duration_ms
               && now - r->drawn_us >= (int64_t)r->duration_ms * 1000) {
        r->state = SIMG_SHOWN;
    }
}

bool simg_busy(const simg_run_t *r)
{
    return r->state == SIMG_ASKING || r->state == SIMG_FETCHING;
}

static int secs10(int64_t us)
{
    return (int)((us + 50000) / 100000);   /* tenths of a second */
}

void simg_status_text(const simg_run_t *r, int64_t now, char *out, size_t cap)
{
    switch (r->state) {
    case SIMG_IDLE:
        snprintf(out, cap, "Asks Muse to make a picture and show it here for 5 s.");
        break;
    case SIMG_ASKING:
        snprintf(out, cap, "Asking Muse... %d s", (int)((now - r->press_us) / 1000000));
        break;
    case SIMG_FETCHING:
        snprintf(out, cap, "Muse sent a picture. Loading...");
        break;
    case SIMG_SHOWING:
        if (r->duration_ms) {
            snprintf(out, cap, "Showing it for %d s", r->duration_ms / 1000);
        } else {
            snprintf(out, cap, "Showing it until a tap");
        }
        break;
    case SIMG_SHOWN: {
        int kb = (int)((r->bytes + 512) / 1024);
        if (r->asked) {
            int t = secs10(r->drawn_us - r->press_us);
            snprintf(out, cap, "Shown %d s: %dx%d, %d KB, on screen %d.%d s after the press.", r->duration_ms / 1000,
                     r->width, r->height, kb, t / 10, t % 10);
        } else {
            snprintf(out, cap, "Muse showed a picture: %dx%d, %d KB.", r->width, r->height, kb);
        }
        break;
    }
    case SIMG_FAILED:
        snprintf(out, cap, "No picture: %s.", r->error);
        break;
    }
}

static const char *state_name(simg_state_t s)
{
    switch (s) {
    case SIMG_IDLE: return "idle";
    case SIMG_ASKING: return "asking";
    case SIMG_FETCHING: return "fetching";
    case SIMG_SHOWING: return "showing";
    case SIMG_SHOWN: return "shown";
    case SIMG_FAILED: return "failed";
    }
    return "?";
}

static int ms_since(int64_t t0, int64_t t)
{
    return t ? (int)((t - t0) / 1000) : -1;
}

void simg_report(const simg_run_t *r, int64_t now, char *out, size_t cap)
{
    int64_t t0 = r->asked ? r->press_us : r->invoke_us;
    int n = snprintf(out, cap,
                     "img %s asked=%d chat=%d@%dms invoke=%dms drawn=%dms fetch=%dms bytes=%u size=%dx%d scale=1/%d "
                     "show=%dms age=%dms",
                     state_name(r->state), r->asked ? 1 : 0, r->chat_status, ms_since(t0, r->chat_us),
                     ms_since(t0, r->invoke_us), ms_since(t0, r->drawn_us), r->fetch_ms, r->bytes, r->width,
                     r->height, r->scale ? r->scale : 1, r->duration_ms, ms_since(t0, now));
    if (r->state == SIMG_FAILED && n > 0 && (size_t)n < cap) {
        snprintf(out + n, cap - (size_t)n, " error=\"%s\"", r->error);
    }
}
