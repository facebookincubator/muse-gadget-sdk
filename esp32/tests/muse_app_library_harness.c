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

// Host harness for the app library (main/apps): each example's script runs
// in the real sandbox against a fake device that checks every command it
// sends. app.update must name the app's own widgets with properties their
// type takes (as components/muse/muse_apps.c's apply() reads them), led.set
// and audio.beep must be valid, app.show must go somewhere, and the camera
// must aim at an image widget. Its page is shown, every button and clickable
// widget is clicked and long-pressed, sliders dragged, the page tapped and
// hidden; the camera, mic and beeps answer later, as on the device. Usage:
// muse_app_library_harness <main/apps dir> <id>...
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "muse_script_sandbox.h"

static int s_failures;

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    s_failures++;
}

// ---- The app under test: its widgets ------------------------------------------

typedef struct {
    char id[32];
    char type[16];
    int cols;   // a table's columns (its col_w), 0 if it doesn't say
} widget_t;

static char s_app[32];
static widget_t s_widgets[128];
static int s_nwidgets;
static const char *s_all_ids[32];   // every example, for app.list and app.show
static int s_nall;

static void collect(const cJSON *node) {
    const cJSON *kids = cJSON_GetObjectItem(node, "children"), *k;
    cJSON_ArrayForEach(k, kids) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(k, "id"));
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(k, "type"));
        if (id && type && s_nwidgets < 128) {
            snprintf(s_widgets[s_nwidgets].id, sizeof(s_widgets[0].id), "%s", id);
            snprintf(s_widgets[s_nwidgets].type, sizeof(s_widgets[0].type), "%s", type);
            s_widgets[s_nwidgets].cols = cJSON_GetArraySize(cJSON_GetObjectItem(k, "col_w"));
            s_nwidgets++;
        }
        collect(k);
    }
}

static const widget_t *widget(const char *id) {
    for (int i = 0; i < s_nwidgets; i++) {
        if (!strcmp(s_widgets[i].id, id)) return &s_widgets[i];
    }
    return NULL;
}

static bool in_list(const char *s, const char *const *list) {
    for (; *list; list++) {
        if (!strcmp(s, *list)) return true;
    }
    return false;
}

// What apply() reads, for every widget and by type.
static const char *const COMMON[] = {
    "w", "h", "x", "y", "align", "bg", "bg_opa", "opa", "radius", "border", "border_color", "pad", "gap",
    "font", "text_align", "hidden", "scroll", "clickable", "live", "justify", "items", "wrap", NULL,
};

static const char *const *own_props(const char *type) {
    static const char *const text[] = { "text", "icon", "color", "long", "on", NULL };
    static const char *const sw[] = { "on", "color", NULL };
    static const char *const range[] = { "min", "max", "value", "color", NULL };
    static const char *const arc[] = { "min", "max", "value", "start", "end", "rotation", "thickness", "knob", "color", NULL };
    static const char *const color[] = { "color", NULL };
    static const char *const image[] = { "src", "zoom", "angle", NULL };
    static const char *const chart[] = { "kind", "min", "max", "points", "series", "push", NULL };
    static const char *const led[] = { "color", "brightness", "on", NULL };
    static const char *const pick[] = { "options", "selected", "rows", NULL };
    static const char *const line[] = { "points", "color", "width", NULL };
    static const char *const canvas[] = { "fill", "draw", "clear", "add", NULL };
    static const char *const qr[] = { "text", NULL };
    static const char *const table[] = { "rows", "col_w", NULL };
    static const char *const input[] = { "text", "placeholder", NULL };
    static const char *const none[] = { NULL };
    if (!strcmp(type, "label") || !strcmp(type, "button") || !strcmp(type, "checkbox")) return text;
    if (!strcmp(type, "switch")) return sw;
    if (!strcmp(type, "slider") || !strcmp(type, "bar")) return range;
    if (!strcmp(type, "arc")) return arc;
    if (!strcmp(type, "spinner")) return color;
    if (!strcmp(type, "image")) return image;
    if (!strcmp(type, "chart")) return chart;
    if (!strcmp(type, "led")) return led;
    if (!strcmp(type, "roller") || !strcmp(type, "dropdown")) return pick;
    if (!strcmp(type, "line")) return line;
    if (!strcmp(type, "canvas")) return canvas;
    if (!strcmp(type, "qr")) return qr;
    if (!strcmp(type, "table")) return table;
    if (!strcmp(type, "input")) return input;
    return none;
}

// ---- The fake device ------------------------------------------------------------

static int64_t s_now = 1000000;
static int64_t now_us(void) { return s_now; }
static void *mem_realloc(void *p, size_t n) { return realloc(p, n); }

static char s_log[64 * 1024];
static void log_cb(const char *script, const char *line) {
    char l[300];
    snprintf(l, sizeof(l), "%s| %s\n", script, line);
    strncat(s_log, l, sizeof(s_log) - strlen(s_log) - 1);
}

typedef struct {
    int64_t token;
    cJSON *result;
} pending_t;

static pending_t s_pending[64];
static int s_npending;
static int64_t s_tokens;
static int s_updates, s_shows, s_leds, s_beeps, s_previews, s_preview_offs, s_captures, s_detects, s_listens;
static const char *s_event;   // the ui event being delivered, if any
static char s_last_show[32];
static char s_state_text[32];         // the last text of a widget called "state"
static const char *s_mode = "idle";   // device.status's: Muse talking or not

static cJSON *ok(cJSON *payload) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddItemToObject(r, "payload", payload ? payload : cJSON_CreateObject());
    return r;
}

static cJSON *error(const char *code, const char *message) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return r;
}

// Answers later, as the voice task and the camera do.
static cJSON *later(cJSON *result) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "_async", true);
    cJSON_AddNumberToObject(r, "_token", (double)++s_tokens);
    assert(s_npending < 64);
    s_pending[s_npending++] = (pending_t){ s_tokens, result };
    return r;
}

static bool valid_color(const char *c) {
    static const char *const names[] = { "black", "white", "red", "green", "blue", "yellow", "orange", "purple",
                                         "pink", "cyan", "teal", "gray", "grey", "muse", "dark", NULL };
    if (!c) return false;
    if (in_list(c, names)) return true;
    if (*c == '#') c++;
    size_t n = strlen(c);
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)c[i])) return false;
    }
    return n == 3 || n == 6;
}

static void check_update(const cJSON *params) {
    const char *app = cJSON_GetStringValue(cJSON_GetObjectItem(params, "app"));
    if (!app || strcmp(app, s_app)) fail("%s: app.update of '%s'", s_app, app ? app : "(none)");
    const cJSON *set = cJSON_GetObjectItem(params, "set"), *w;
    if (!cJSON_IsObject(set)) {
        fail("%s: app.update without a set object", s_app);
        return;
    }
    cJSON_ArrayForEach(w, set) {
        const widget_t *wd = widget(w->string);
        if (!wd) {
            fail("%s: app.update of widget '%s', which the page doesn't have", s_app, w->string);
            continue;
        }
        if (!cJSON_IsObject(w)) {
            fail("%s: app.update of %s isn't an object", s_app, w->string);
            continue;
        }
        const cJSON *p;
        bool has_min = false, has_max = false;
        cJSON_ArrayForEach(p, w) {
            if (!in_list(p->string, COMMON) && !in_list(p->string, own_props(wd->type))) {
                fail("%s: %s (%s) has no property '%s'", s_app, wd->id, wd->type, p->string);
            }
            has_min |= !strcmp(p->string, "min");
            has_max |= !strcmp(p->string, "max");
            if (!strcmp(p->string, "color") && !valid_color(cJSON_GetStringValue(p))) {
                fail("%s: %s: bad colour", s_app, wd->id);
            }
            if (!strcmp(p->string, "text") && !cJSON_IsString(p)) fail("%s: %s: text isn't a string", s_app, wd->id);
            if (!strcmp(p->string, "text") && !strcmp(wd->id, "state") && cJSON_IsString(p)) {
                snprintf(s_state_text, sizeof(s_state_text), "%s", p->valuestring);
            }
            if (!strcmp(p->string, "rows") && wd->cols) {   // a stray cell makes a column the page has no room for
                const cJSON *row;
                cJSON_ArrayForEach(row, p) {
                    if (cJSON_GetArraySize(row) != wd->cols) {
                        fail("%s: %s: a row of %d cells in a table of %d columns", s_app, wd->id,
                             cJSON_GetArraySize(row), wd->cols);
                    }
                }
            }
        }
        if (has_min != has_max) fail("%s: %s sets only one of min and max, which apply() ignores", s_app, wd->id);
    }
}

static void check_tones(const char *tones) {
    int n = 0, ms = 0;
    const char *p = tones;
    while (p && *p) {
        int hz, len, used;
        if (sscanf(p, "%d:%d%n", &hz, &len, &used) != 2 || hz < 0 || len <= 0) {
            fail("%s: bad tones '%s'", s_app, tones);
            return;
        }
        n++;
        ms += len;
        p += used;
        if (*p == ',') p++;
        else if (*p) {
            fail("%s: bad tones '%s'", s_app, tones);
            return;
        }
    }
    if (!tones || n == 0 || n > 32 || ms > 15000) fail("%s: tones '%s': %d tones, %d ms", s_app, tones, n, ms);
}

static cJSON *device_call(const char *script, const char *command, const cJSON *params) {
    (void)script;
    const char *s;
    if (!strcmp(command, "app.update")) {
        s_updates++;
        check_update(params);
        return ok(NULL);
    }
    if (!strcmp(command, "app.list")) {
        cJSON *pl = cJSON_CreateObject(), *apps = cJSON_AddArrayToObject(pl, "apps");
        static const char *const builtin[][2] = { { "face", "Muse" }, { "pet", "Pet" }, { "settings", "Settings" } };
        for (int i = 0; i < 3; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", builtin[i][0]);
            cJSON_AddStringToObject(o, "title", builtin[i][1]);
            cJSON_AddBoolToObject(o, "builtin", true);
            cJSON_AddBoolToObject(o, "shown", i == 0);
            cJSON_AddItemToArray(apps, o);
        }
        for (int i = 0; i < s_nall; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", s_all_ids[i]);
            cJSON_AddStringToObject(o, "title", s_all_ids[i]);
            cJSON_AddNumberToObject(o, "order", i * 10);
            cJSON_AddBoolToObject(o, "shown", false);
            cJSON_AddItemToArray(apps, o);
        }
        cJSON_AddNumberToObject(pl, "max", 16);
        return ok(pl);
    }
    if (!strcmp(command, "app.show")) {
        s_shows++;
        // The screen changes page when someone asks, never by itself.
        if (!s_event || strcmp(s_event, "click")) fail("%s: app.show outside a click (the page jumps by itself)", s_app);
        s = cJSON_GetStringValue(cJSON_GetObjectItem(params, "app"));
        bool known = s && (!strcmp(s, "face") || !strcmp(s, "pet") || !strcmp(s, "settings"));
        for (int i = 0; s && i < s_nall; i++) known |= !strcmp(s, s_all_ids[i]);
        if (!known) fail("%s: app.show of '%s'", s_app, s ? s : "(none)");
        snprintf(s_last_show, sizeof(s_last_show), "%s", s ? s : "");
        return ok(NULL);
    }
    if (!strcmp(command, "device.time")) {
        cJSON *pl = cJSON_CreateObject();
        int secs = (int)(s_now / 1000000) % 60;
        char iso[32];
        snprintf(iso, sizeof(iso), "2026-09-29T23:45:%02d", secs);
        cJSON_AddBoolToObject(pl, "valid", true);
        cJSON_AddNumberToObject(pl, "unix", 1790750700 + (double)(s_now / 1000000));
        cJSON_AddStringToObject(pl, "local_time", iso);
        cJSON_AddStringToObject(pl, "utc_time", iso);
        cJSON_AddNumberToObject(pl, "hour", 23);
        cJSON_AddNumberToObject(pl, "minute", 45);
        cJSON_AddNumberToObject(pl, "weekday", 2);
        cJSON_AddStringToObject(pl, "tz", "PST8PDT");
        return ok(pl);
    }
    if (!strcmp(command, "device.status")) {
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddNumberToObject(pl, "battery_percent", 87);
        cJSON_AddBoolToObject(pl, "charging", false);
        cJSON_AddBoolToObject(pl, "usb_power", true);
        cJSON_AddBoolToObject(pl, "wifi_connected", true);
        cJSON_AddStringToObject(pl, "wifi_ssid", "Home");
        cJSON_AddNumberToObject(pl, "wifi_rssi", -52);
        cJSON_AddNumberToObject(pl, "chip_temp_c", 48.6);
        cJSON_AddNumberToObject(pl, "uptime_s", 3725);
        cJSON_AddStringToObject(pl, "boot_reason", "power_on");
        cJSON_AddStringToObject(pl, "mode", s_mode);
        return ok(pl);
    }
    if (!strcmp(command, "led.set")) {
        s_leds++;
        static const char *const effects[] = { "solid", "blink", "breathe", "off", NULL };
        const cJSON *c = cJSON_GetObjectItem(params, "color"), *e = cJSON_GetObjectItem(params, "effect");
        const cJSON *b = cJSON_GetObjectItem(params, "brightness");
        if (c && !valid_color(cJSON_GetStringValue(c))) fail("%s: led.set colour", s_app);
        if (e && (!cJSON_IsString(e) || !in_list(e->valuestring, effects))) fail("%s: led.set effect", s_app);
        if (b && (!cJSON_IsNumber(b) || b->valuedouble < 0 || b->valuedouble > 100)) fail("%s: led.set brightness", s_app);
        return ok(NULL);
    }
    if (!strcmp(command, "audio.beep")) {
        s_beeps++;
        check_tones(cJSON_GetStringValue(cJSON_GetObjectItem(params, "tones")));
        return later(ok(NULL));
    }
    if (!strcmp(command, "audio.listen")) {
        s_listens++;
        const cJSON *sec = cJSON_GetObjectItem(params, "seconds");
        if (sec && (!cJSON_IsNumber(sec) || sec->valuedouble < 1 || sec->valuedouble > 10)) fail("%s: listen seconds", s_app);
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddNumberToObject(pl, "ms", 1000);
        cJSON_AddNumberToObject(pl, "average_dbfs", -41.5 + (s_listens % 5) * 6);
        cJSON_AddNumberToObject(pl, "peak_dbfs", -20.2);
        cJSON_AddNumberToObject(pl, "loud_percent", 30);
        return later(ok(pl));
    }
    if (!strcmp(command, "camera.preview")) {
        const cJSON *on = cJSON_GetObjectItem(params, "on");
        if (cJSON_IsFalse(on)) {
            s_preview_offs++;
            return ok(NULL);
        }
        s_previews++;
        s = cJSON_GetStringValue(cJSON_GetObjectItem(params, "widget"));
        if (s) {
            char app[32] = "";
            const char *dot = strchr(s, '.');
            snprintf(app, sizeof(app), "%.*s", dot ? (int)(dot - s) : 0, s);
            const widget_t *w = dot ? widget(dot + 1) : NULL;
            if (strcmp(app, s_app) || !w || strcmp(w->type, "image")) fail("%s: camera.preview into '%s'", s_app, s);
        }
        return later(ok(NULL));
    }
    if (!strcmp(command, "camera.capture")) {
        s_captures++;
        cJSON *pl = cJSON_CreateObject();
        s = cJSON_GetStringValue(cJSON_GetObjectItem(params, "save"));
        if (s) cJSON_AddStringToObject(pl, "saved", s);
        else cJSON_AddStringToObject(pl, "data_base64", "/9j/4AAQSkZJRgABAQ==");
        return later(ok(pl));
    }
    if (!strcmp(command, "storage.info")) {   // no card in the slot, like the Watcher here
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddBoolToObject(pl, "card", false);
        cJSON_AddBoolToObject(pl, "mounted", false);
        return ok(pl);
    }
    if (!strcmp(command, "camera.detect")) {
        s_detects++;
        static const char *const models[] = { "person", "pet", "gesture", "custom", NULL };
        s = cJSON_GetStringValue(cJSON_GetObjectItem(params, "model"));
        if (s && !in_list(s, models)) fail("%s: camera.detect model '%s'", s_app, s);
        cJSON *pl = cJSON_CreateObject(), *d = cJSON_AddArrayToObject(pl, "detections");
        for (int i = 0; i < s_detects % 3; i++) {   // none, one, then two
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "label", "person");
            cJSON_AddNumberToObject(o, "score", 80 - i);
            cJSON_AddItemToArray(d, o);
        }
        return later(ok(pl));
    }
    fail("%s: calls %s, which the fake device doesn't know", s_app, command);
    return error("unsupported", "unknown");
}

static void notify_cb(const char *script, const char *text) {
    (void)script;
    (void)text;
}

// The commands muse_script.c lets scripts call.
static const char *const COMMANDS[] = {
    "device.status", "device.time", "display.show_text", "display.show_ui", "display.draw_url",
    "display.show_animation", "display.power", "led.set", "audio.beep", "audio.play_url", "audio.listen",
    "audio.record", "wifi.scan", "camera.capture", "camera.detect", "camera.models",
    "camera.preview", "camera.watch", "camera.stop", "grove.power", "i2c.scan", "i2c.read",
    "i2c.write", "uart.write", "uart.read", "storage.info", "storage.list", "storage.read", "storage.write",
    "storage.delete", "app.define", "app.update", "app.remove", "app.show", "app.list", "pet.status",
    "pet.care", "pet.name", NULL,
};

// ---- Driving ----------------------------------------------------------------------

static void answer_all(sb_runtime_t *rt) {
    for (int round = 0; round < 8 && s_npending; round++) {
        int n = s_npending;
        pending_t batch[64];
        memcpy(batch, s_pending, n * sizeof(batch[0]));
        s_npending = 0;
        for (int i = 0; i < n; i++) {
            sb_async_result(rt, batch[i].token, batch[i].result);
            cJSON_Delete(batch[i].result);
        }
    }
}

static void run_for(sb_runtime_t *rt, int ms) {
    for (int t = 0; t < ms; t += 20) {
        s_now += 20000;
        sb_run_due(rt);
        if (t % 200 == 0) answer_all(rt);
    }
    answer_all(rt);
}

static void send(sb_runtime_t *rt, const char *id, const char *event, int value) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "app", s_app);
    cJSON_AddStringToObject(e, "id", id);
    cJSON_AddStringToObject(e, "event", event);
    cJSON_AddNumberToObject(e, "value", value);
    s_event = event;
    sb_event(rt, "ui", e);
    s_event = NULL;
    cJSON_Delete(e);
}

static void ui(sb_runtime_t *rt, const char *id, const char *event, int value) {
    send(rt, id, event, value);
    run_for(rt, 1500);
}

// A touch or the wheel, as muse_hw reports it.
static void input(sb_runtime_t *rt, const char *type) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "type", type);
    cJSON_AddNumberToObject(e, "x", 200);
    cJSON_AddNumberToObject(e, "y", 200);
    sb_event(rt, type, e);
    cJSON_Delete(e);
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static const char *state_of(sb_runtime_t *rt, const char *name, char *err, size_t errlen) {
    static char st[32];
    st[0] = '\0';
    err[0] = '\0';
    cJSON *l = sb_list(rt), *s;
    cJSON_ArrayForEach(s, l) {
        if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(s, "name")), name)) continue;
        snprintf(st, sizeof(st), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(s, "state")));
        const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(s, "error"));
        if (e) snprintf(err, errlen, "%s", e);
    }
    cJSON_Delete(l);
    return st;
}

// Its timers and the callbacks waiting: a ticker or a loop each.
static int busy(sb_runtime_t *rt, const char *name) {
    int n = 0;
    cJSON *l = sb_list(rt), *s;
    cJSON_ArrayForEach(s, l) {
        if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(s, "name")), name)) continue;
        n = (int)(cJSON_GetNumberValue(cJSON_GetObjectItem(s, "timers"))
                  + cJSON_GetNumberValue(cJSON_GetObjectItem(s, "waiting")));
    }
    cJSON_Delete(l);
    return n;
}

static void test_app(const char *dir, const char *id) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", dir, id);
    char *json = slurp(path);
    snprintf(path, sizeof(path), "%s/%s.lua", dir, id);
    char *lua = slurp(path);
    if (!json || !lua) {
        fail("%s: missing its .json or .lua", id);
        return;
    }
    cJSON *def = cJSON_Parse(json);
    if (!def) {
        fail("%s: the page isn't JSON", id);
        return;
    }
    snprintf(s_app, sizeof(s_app), "%s", id);
    s_nwidgets = 0;
    collect(def);
    s_log[0] = '\0';
    s_state_text[0] = '\0';
    s_updates = s_shows = s_leds = s_beeps = s_previews = s_preview_offs = s_captures = s_detects = s_listens = 0;

    sb_platform_t pf = {
        .now_us = now_us, .mem_realloc = mem_realloc, .mem_free = free,
        .log = log_cb, .device_call = device_call, .notify = notify_cb,
    };
    sb_limits_t lim;
    sb_default_limits(&lim);
    lim.commands = COMMANDS;
    lim.slice_instructions = 500000;   // the firmware's
    lim.slice_ms = 100000;             // the clock only moves between slices here
    sb_runtime_t *rt = sb_new(&pf, &lim);
    char err[200];
    if (!sb_check_syntax(rt, lua, strlen(lua), err, sizeof(err))) fail("%s: %s", id, err);
    if (!sb_start(rt, id, lua, strlen(lua), err, sizeof(err))) fail("%s: doesn't start: %s", id, err);
    run_for(rt, 2000);
    int at_start = s_updates;

    ui(rt, "", "show", 0);
    run_for(rt, 3500);
    int shown = s_updates;
    // Shown again (a relayout does that), then hidden and shown at once: no
    // second ticker, loop or video.
    int running = busy(rt, id), previews = s_previews;
    ui(rt, "", "show", 0);
    if (busy(rt, id) != running || s_previews != previews) fail("%s: a second show started things again", id);
    send(rt, "", "hide", 0);
    send(rt, "", "show", 0);
    run_for(rt, 3500);
    if (busy(rt, id) != running) fail("%s: hidden and shown at once, it runs %d things, not %d", id, busy(rt, id), running);
    if (!strcmp(id, "camera")) {   // the full-screen video ends on any touch, the wheel or a talk
        const char *ends[] = { "swipe", "wheel_turn", NULL };
        for (int i = 0; i < 3; i++) {
            ui(rt, "full", "click", 0);
            previews = s_previews;
            if (ends[i]) input(rt, ends[i]);
            else s_mode = "speaking";
            run_for(rt, 1500);
            s_mode = "idle";
            if (s_previews != previews + 1 || busy(rt, id) != running) {
                fail("camera: the page's video didn't come back after %s", ends[i] ? ends[i] : "a talk");
            }
        }
    }
    for (int i = 0; i < s_nwidgets; i++) {
        const widget_t *w = &s_widgets[i];
        bool clickable = !strcmp(w->type, "button") || strstr(json, "\"clickable\": true") != NULL;
        if (clickable) {
            ui(rt, w->id, "click", 0);
            ui(rt, w->id, "long_press", 0);
            ui(rt, w->id, "click", 0);
        }
        if (!strcmp(w->type, "slider") || !strcmp(w->type, "arc")) ui(rt, w->id, "change", 55);
        if (!strcmp(w->type, "switch") || !strcmp(w->type, "checkbox")) ui(rt, w->id, "change", 1);
    }
    ui(rt, "", "click", 0);   // the page's background
    input(rt, "tap");   // anywhere
    run_for(rt, 3000);
    ui(rt, "", "hide", 0);
    run_for(rt, 3000);
    int hidden = s_updates;
    run_for(rt, 10000);
    int later_updates = s_updates - hidden;

    // Left alone for 20 minutes: a running timer finishes, off its page.
    int shows = s_shows, leds = s_leds, beeps = s_beeps;
    run_for(rt, 20 * 60 * 1000);
    if (!strcmp(id, "timer")) {
        if (s_shows != shows) fail("timer: switched pages when done");
        if (s_leds < leds + 2 || s_beeps == beeps) fail("timer: no light or chime when done");
        ui(rt, "", "show", 0);
        if (strcmp(s_state_text, "DONE!")) fail("timer: back on its page when done, it says %s", s_state_text);
    }

    if (strstr(s_log, "error")) fail("%s: the script logged an error:\n%s", id, s_log);
    const char *st = state_of(rt, id, err, sizeof(err));
    if (strcmp(st, "running") && strcmp(st, "finished")) fail("%s: ended %s: %s", id, st, err);
    if (s_updates == 0) fail("%s: never changed its page", id);
    // One that stops drawing while hidden must catch up when it's back.
    if (strstr(lua, "\"hide\"") && shown == at_start) fail("%s: nothing changed when its page showed", id);
    // What it did in the ten seconds after it was hidden: a clock or meter
    // that keeps going off screen wastes the battery.
    if (later_updates > 2) fail("%s: %d page updates while hidden", id, later_updates);
    printf("%-8s ok: %d updates (%d after show), %d app.show, %d led, %d beeps, %d listens, camera %d/%d/%d/%d, "
           "%d widgets; hidden: %d\n",
           id, s_updates, shown - at_start, s_shows, s_leds, s_beeps, s_listens, s_previews, s_preview_offs,
           s_captures, s_detects, s_nwidgets, later_updates);
    cJSON *l = sb_list(rt), *s;
    cJSON_ArrayForEach(s, l) {
        double mem = cJSON_GetNumberValue(cJSON_GetObjectItem(s, "mem_peak"));
        if (mem > 128 * 1024) fail("%s: peaks at %.0f KB of Lua memory", id, mem / 1024);
    }
    cJSON_Delete(l);
    sb_free(rt);
    cJSON_Delete(def);
    free(json);
    free(lua);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <apps dir> <id>...\n", argv[0]);
        return 2;
    }
    for (int i = 2; i < argc && s_nall < 32; i++) s_all_ids[s_nall++] = argv[i];
    for (int i = 2; i < argc; i++) test_app(argv[1], argv[i]);
    if (s_failures) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("all %d examples ok\n", argc - 2);
    return 0;
}
