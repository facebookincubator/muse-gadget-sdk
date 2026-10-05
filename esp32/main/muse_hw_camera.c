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

// The AI camera's commands for the agent (the Watcher's Himax, over
// muse_camera.h): photos, detection with the camera's own models, raw SSCMA
// commands, and live modes: video on the screen, and continuous detection as
// input events. One worker runs them all; the
// camera stays powered a while after the last, so the next doesn't wait for
// it to boot.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"

#include "muse_apps.h"
#include "muse_board.h"
#include "muse_camera.h"
#if CONFIG_MUSE_HATCH
#include "muse_chat.h"
#endif
#include "muse_hw.h"
#include "muse_hw_commands_priv.h"
#include "muse_mem.h"
#include "muse_state.h"
#include "muse_ui.h"

static const char *TAG = "link.camera";

#define IDLE_OFF_US (90 * 1000000LL)   // powered this long after the last command
#define REPLY_MS 3000                  // a plain reply
#define MODEL_MS 8000                  // selecting a model loads it
#define FRAME_MS 8000                  // a SAMPLE's or INVOKE's first frame
#define IMAGE_MAX (240 * 1024)         // base64 in a result, under the session's 256 KB
#define LIVE_EVENT_MS 400              // live: one wait for a frame
#define LIVE_MISSES 8                  // ... this many in a row and the live command restarts
#define LIVE_RES 416                   // live frames are 416x416 (AT+SENSOR opt 1)
#define DETECT_REPEAT_US 1000000       // camera.watch: a label present is reported at most this often
#define SEEN_MAX 8
#define JPEG_POOL 3100                 // tjpgd's work area
#define ENDED_WAIT_MS 500              // a script's end waits this long at most for room in the queue

// ---- Host-tested: resolutions, labels, base64 ------------------------------

typedef struct {
    const char *name;
    int opt;           // AT+SENSOR=1,1,<opt>
} resolution_t;

static const resolution_t RESOLUTIONS[] = {
    { "240x240", 0 }, { "416x416", 1 }, { "480x480", 2 }, { "640x480", 3 },
};

static int resolution_opt(const char *name) {
    for (size_t i = 0; i < sizeof(RESOLUTIONS) / sizeof(RESOLUTIONS[0]); i++) {
        if (!strcmp(name, RESOLUTIONS[i].name)) return RESOLUTIONS[i].opt;
    }
    return -1;
}

// The models the Watcher ships with, by slot; slot 4 is for your own.
typedef struct {
    int id;
    const char *name;
    const char *labels[4];
} factory_model_t;

static const factory_model_t FACTORY[] = {
    { 1, "person", { "person" } },
    { 2, "pet", { "cat", "dog" } },
    { 3, "gesture", { "paper", "rock", "scissors" } },
    { 4, "custom", { NULL } },
};

// "person", "pet", "gesture", "custom" or "1".."4"; 0 if none.
static int model_id(const char *name) {
    for (size_t i = 0; i < sizeof(FACTORY) / sizeof(FACTORY[0]); i++) {
        if (!strcmp(name, FACTORY[i].name)) return FACTORY[i].id;
    }
    int id = atoi(name);
    return id >= 1 && id <= 4 ? id : 0;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodes base64 into out (room for len / 4 * 3); stops at '=' or a bad
// character. Returns the bytes written.
static size_t b64_decode(const char *in, size_t len, uint8_t *out) {
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64_value(in[i]);
        if (v < 0) break;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

// ---- Host-tested end

size_t hw_base64_decode(const char *in, size_t len, uint8_t *out) { return b64_decode(in, len, out); }

// ---- The camera, powered while in use --------------------------------------

typedef enum { JOB_AT, JOB_CAPTURE, JOB_DETECT, JOB_MODELS, JOB_LIVE, JOB_ENDED } job_kind_t;

typedef struct {
    job_kind_t kind;
    cJSON *params;       // a copy, freed with the job
    hw_reply_to_t to;
} job_t;

static QueueHandle_t s_jobs;     // set once the worker runs
static bool s_open;
static int64_t s_last_use;
static int s_sensor_opt = -1;    // what AT+SENSOR last set; -1 unknown
static int s_model = 0;          // what AT+MODEL last set; 0 unknown
static cJSON *s_labels;          // the current model's classes, from AT+INFO?

static esp_err_t camera_up(void) {
    if (!s_open) {
        esp_err_t err = muse_camera_open();
        if (err != ESP_OK) return err;
        s_open = true;
        s_sensor_opt = -1;
        s_model = 0;
    }
    s_last_use = esp_timer_get_time();
    return ESP_OK;
}

static void camera_down(void) {
    if (s_open) {
        muse_camera_close();
        s_open = false;
        ESP_LOGI(TAG, "camera idle: powered down");
    }
}

static cJSON *camera_error(esp_err_t err, const char *what) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: %s", what,
             err == ESP_ERR_TIMEOUT ? "the camera didn't answer" : esp_err_to_name(err));
    return hw_error(err == ESP_ERR_NOT_SUPPORTED ? "unsupported" : "camera_failed", msg);
}

static esp_err_t set_sensor(int opt) {
    if (opt == s_sensor_opt) return ESP_OK;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "SENSOR=1,1,%d", opt);
    esp_err_t err = muse_camera_request(cmd, NULL, NULL, MODEL_MS);
    if (err == ESP_OK) s_sensor_opt = opt;
    return err;
}

// The current model's class names, from its metadata (base64 JSON in AT+INFO?).
static void load_labels(void) {
    cJSON_Delete(s_labels);
    s_labels = NULL;
    cJSON *data = NULL;
    if (muse_camera_request("INFO?", &data, NULL, REPLY_MS) == ESP_OK) {
        cJSON *info = cJSON_GetObjectItem(data, "info");
        if (cJSON_IsString(info) && info->valuestring[0]) {
            size_t len = strlen(info->valuestring);
            uint8_t *json = malloc(len / 4 * 3 + 4);
            if (json) {
                json[b64_decode(info->valuestring, len, json)] = '\0';
                cJSON *meta = cJSON_Parse((const char *)json);
                cJSON *classes = cJSON_GetObjectItem(meta, "classes");
                if (cJSON_IsArray(classes)) s_labels = cJSON_Duplicate(classes, true);
                cJSON_Delete(meta);
                free(json);
            }
        }
    }
    cJSON_Delete(data);
}

static esp_err_t set_model(int id) {
    if (id == s_model) return ESP_OK;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "MODEL=%d", id);
    esp_err_t err = muse_camera_request(cmd, NULL, NULL, MODEL_MS);
    if (err == ESP_OK) {
        s_model = id;
        load_labels();
    }
    return err;
}

static const char *label(int target) {
    cJSON *l = cJSON_GetArrayItem(s_labels, target);
    if (cJSON_IsString(l)) return l->valuestring;
    for (size_t i = 0; i < sizeof(FACTORY) / sizeof(FACTORY[0]); i++) {
        if (FACTORY[i].id == s_model && target >= 0 && target < 4 && FACTORY[i].labels[target]) {
            return FACTORY[i].labels[target];
        }
    }
    return "unknown";
}

// ---- camera.at -------------------------------------------------------------

static cJSON *run_at(cJSON *params) {
    const char *cmd = hw_str(params, "cmd");
    if (!cmd || !cmd[0]) return hw_error("missing_param", "cmd is required, e.g. MODELS?");
    if (!strncasecmp(cmd, "AT+", 3)) cmd += 3;
    int timeout = REPLY_MS;
    hw_int(params, "timeout_ms", &timeout);
    cJSON *data = NULL;
    int code = -1;
    esp_err_t err = muse_camera_request(cmd, &data, &code, hw_clamp(timeout, 100, 30000));
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_ARG) {
        cJSON_Delete(data);
        return camera_error(err, cmd);
    }
    // A command can change what's selected behind our back.
    s_sensor_opt = -1;
    s_model = 0;
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddNumberToObject(pl, "code", code);
    cJSON_AddItemToObject(pl, "data", data ? data : cJSON_CreateNull());
    return hw_ok(pl);
}

// ---- camera.capture --------------------------------------------------------

static cJSON *image_payload(cJSON *data) {
    cJSON *image = cJSON_GetObjectItem(data, "image");
    if (!cJSON_IsString(image) || !image->valuestring[0]) return NULL;
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "format", "jpeg-base64");
    cJSON *res = cJSON_GetObjectItem(data, "resolution");
    if (cJSON_GetArraySize(res) == 2) {
        cJSON_AddNumberToObject(pl, "width", cJSON_GetArrayItem(res, 0)->valueint);
        cJSON_AddNumberToObject(pl, "height", cJSON_GetArrayItem(res, 1)->valueint);
    }
    cJSON_AddNumberToObject(pl, "bytes", (double)(strlen(image->valuestring) / 4 * 3));
    cJSON_AddItemToObject(pl, "data_base64", cJSON_DetachItemFromObject(data, "image"));
    return pl;
}

static cJSON *run_capture(cJSON *params) {
    const char *res = hw_str(params, "resolution");
    int opt = resolution_opt(res ? res : "640x480");
    if (opt < 0) return hw_error("invalid_params", "resolution is 240x240, 416x416, 480x480 or 640x480");
    esp_err_t err = set_sensor(opt);
    if (err != ESP_OK) return camera_error(err, "setting the resolution");
    muse_camera_flush_events();
    err = muse_camera_request("SAMPLE=1", NULL, NULL, REPLY_MS);
    if (err != ESP_OK) return camera_error(err, "taking a photo");
    cJSON *data = NULL;
    err = muse_camera_event("SAMPLE", &data, FRAME_MS);
    if (err != ESP_OK) {
        cJSON_Delete(data);
        return camera_error(err, "waiting for the photo");
    }
    cJSON *pl = image_payload(data);
    cJSON_Delete(data);
    if (!pl) return hw_error("camera_failed", "the camera sent no image");
#if CONFIG_MUSE_HATCH
    if (cJSON_IsTrue(cJSON_GetObjectItem(params, "ask"))) {
        // For Muse to see: the next voice note carries it, as the camera sent it.
        const char *b64 = cJSON_GetObjectItem(pl, "data_base64")->valuestring;
        size_t len = strlen(b64);
        char *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (!copy) {
            cJSON_Delete(pl);
            return hw_error("out_of_memory", "no room for the photo");
        }
        memcpy(copy, b64, len);
        muse_chat_attach_image(copy, len);
        cJSON_DeleteItemFromObject(pl, "data_base64");
        cJSON_AddBoolToObject(pl, "attached", true);
        return hw_ok(pl);
    }
#endif
    const char *save = hw_str(params, "save");
    if (save) {
        const char *b64 = cJSON_GetObjectItem(pl, "data_base64")->valuestring;
        size_t len = strlen(b64);
        uint8_t *jpeg = heap_caps_malloc(len / 4 * 3 + 4, MALLOC_CAP_SPIRAM);
        const char *err = jpeg ? hw_storage_save(save, jpeg, b64_decode(b64, len, jpeg), false) : "out of memory";
        free(jpeg);
        if (err) {
            cJSON_Delete(pl);
            return hw_error("save_failed", err);
        }
        cJSON_AddStringToObject(pl, "saved", save);
        if (!cJSON_IsTrue(cJSON_GetObjectItem(params, "include_image"))
            || strlen(cJSON_GetObjectItem(pl, "data_base64")->valuestring) > IMAGE_MAX) {
            cJSON_DeleteItemFromObject(pl, "data_base64");
        }
        return hw_ok(pl);
    }
    if (strlen(cJSON_GetObjectItem(pl, "data_base64")->valuestring) > IMAGE_MAX) {
        cJSON_Delete(pl);
        return hw_error("too_large", "the photo is too large to send; try a smaller resolution");
    }
    return hw_ok(pl);
}

// ---- camera.detect ---------------------------------------------------------

// The results of one INVOKE frame scoring min_score or more, in the frame's pixels.
static cJSON *detections(cJSON *data, int min_score) {
    cJSON *out = cJSON_CreateArray();
    cJSON *boxes = cJSON_GetObjectItem(data, "boxes");
    cJSON *item;
    cJSON_ArrayForEach(item, boxes) {   // [cx, cy, w, h, score, target]
        if (cJSON_GetArraySize(item) < 6 || cJSON_GetArrayItem(item, 4)->valueint < min_score) continue;
        int cx = cJSON_GetArrayItem(item, 0)->valueint, cy = cJSON_GetArrayItem(item, 1)->valueint;
        int w = cJSON_GetArrayItem(item, 2)->valueint, h = cJSON_GetArrayItem(item, 3)->valueint;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "label", label(cJSON_GetArrayItem(item, 5)->valueint));
        cJSON_AddNumberToObject(d, "score", cJSON_GetArrayItem(item, 4)->valueint);
        cJSON_AddNumberToObject(d, "x", cx - w / 2);
        cJSON_AddNumberToObject(d, "y", cy - h / 2);
        cJSON_AddNumberToObject(d, "w", w);
        cJSON_AddNumberToObject(d, "h", h);
        cJSON_AddItemToArray(out, d);
    }
    cJSON *classes = cJSON_GetObjectItem(data, "classes");
    cJSON_ArrayForEach(item, classes) {   // [score, target]
        if (cJSON_GetArraySize(item) < 2 || cJSON_GetArrayItem(item, 0)->valueint < min_score) continue;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "label", label(cJSON_GetArrayItem(item, 1)->valueint));
        cJSON_AddNumberToObject(d, "score", cJSON_GetArrayItem(item, 0)->valueint);
        cJSON_AddItemToArray(out, d);
    }
    cJSON *points = cJSON_GetObjectItem(data, "points");
    cJSON_ArrayForEach(item, points) {   // [x, y, score, target]
        if (cJSON_GetArraySize(item) < 4 || cJSON_GetArrayItem(item, 2)->valueint < min_score) continue;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "label", label(cJSON_GetArrayItem(item, 3)->valueint));
        cJSON_AddNumberToObject(d, "score", cJSON_GetArrayItem(item, 2)->valueint);
        cJSON_AddNumberToObject(d, "x", cJSON_GetArrayItem(item, 0)->valueint);
        cJSON_AddNumberToObject(d, "y", cJSON_GetArrayItem(item, 1)->valueint);
        cJSON_AddItemToArray(out, d);
    }
    return out;
}

static cJSON *run_detect(cJSON *params) {
    const char *model = hw_str(params, "model");
    int id = model_id(model ? model : "person");
    if (!id) return hw_error("invalid_params", "model is person, pet, gesture, custom or 1-4");
    int min_score = 50;
    hw_int(params, "min_score", &min_score);
    cJSON *img = cJSON_GetObjectItem(params, "with_image");
    bool with_image = cJSON_IsTrue(img);
    esp_err_t err = set_sensor(1);   // 416x416, as Seeed's own detection runs
    if (err == ESP_OK) err = set_model(id);
    if (err != ESP_OK) return camera_error(err, "loading the model");
    // The shipped Himax firmware (SSCMA-Micro 1.0.x) has no TSCORE: its
    // models report scores of 50 and up, and higher bars are applied here.
    char cmd[32];
    muse_camera_flush_events();
    snprintf(cmd, sizeof(cmd), "INVOKE=1,0,%d", with_image ? 0 : 1);
    err = muse_camera_request(cmd, NULL, NULL, REPLY_MS);
    if (err != ESP_OK) return camera_error(err, "running the model");
    cJSON *data = NULL;
    err = muse_camera_event("INVOKE", &data, FRAME_MS);
    if (err != ESP_OK) {
        cJSON_Delete(data);
        return camera_error(err, "waiting for the result");
    }
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddNumberToObject(pl, "model", id);
    cJSON_AddItemToObject(pl, "detections", detections(data, min_score));
    cJSON *res = cJSON_GetObjectItem(data, "resolution");
    if (res) cJSON_AddItemToObject(pl, "resolution", cJSON_Duplicate(res, true));
    if (with_image) {
        cJSON *image = image_payload(data);
        if (image) cJSON_AddItemToObject(pl, "data_base64", cJSON_DetachItemFromObject(image, "data_base64"));
        cJSON_Delete(image);
    }
    cJSON_Delete(data);
    return hw_ok(pl);
}

// ---- camera.models ---------------------------------------------------------

static cJSON *run_models(void) {
    cJSON *data = NULL;
    esp_err_t err = muse_camera_request("MODELS?", &data, NULL, REPLY_MS);
    if (err != ESP_OK) {
        cJSON_Delete(data);
        return camera_error(err, "listing models");
    }
    cJSON *out = cJSON_CreateArray();
    cJSON *m;
    cJSON_ArrayForEach(m, data) {
        cJSON *idj = cJSON_GetObjectItem(m, "id");
        if (!cJSON_IsNumber(idj)) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", idj->valueint);
        for (size_t i = 0; i < sizeof(FACTORY) / sizeof(FACTORY[0]); i++) {
            if (FACTORY[i].id != idj->valueint) continue;
            cJSON_AddStringToObject(e, "name", FACTORY[i].name);
            cJSON *labels = cJSON_AddArrayToObject(e, "labels");
            for (int k = 0; k < 4 && FACTORY[i].labels[k]; k++) {
                cJSON_AddItemToArray(labels, cJSON_CreateString(FACTORY[i].labels[k]));
            }
        }
        cJSON *size = cJSON_GetObjectItem(m, "size");
        if (cJSON_IsNumber(size)) cJSON_AddNumberToObject(e, "size", size->valuedouble);
        cJSON_AddItemToArray(out, e);
    }
    cJSON_Delete(data);
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddItemToObject(pl, "models", out);
    return hw_ok(pl);
}

// ---- Live: preview, watch ------------------------------------------

static bool s_preview, s_watch;
static int s_watch_model = 1, s_watch_min = 50;
static char s_live_cmd[32];        // the continuous SAMPLE or INVOKE running; "" if none
static int s_misses;
static uint32_t s_preview_seq;     // muse_hw's newest event when the preview began
static char s_preview_app[16], s_preview_widget[16];   // an app's image widget, or "" for the whole screen
static struct {
    char label[16];
    int64_t at;
} s_seen[SEEN_MAX];                // camera.watch: when each label was last reported

static bool live_on(void) {
    return s_preview || s_watch;
}

static void preview_off(void) {
    if (s_preview && !s_preview_app[0]) muse_ui_image_hide();   // not the last frame, frozen over the face
    s_preview = false;
}

static void live_off(void) {
    preview_off();
    s_watch = false;
}

static void live_want(char *cmd, size_t cap) {
    bool video = s_preview;
    if (s_watch) {
        snprintf(cmd, cap, "INVOKE=-1,0,%d", video ? 0 : 1);
    } else if (video) {
        snprintf(cmd, cap, "SAMPLE=-1");
    } else {
        cmd[0] = '\0';
    }
}

static void live_break(void) {
    if (s_live_cmd[0]) {
        muse_camera_request("BREAK", NULL, NULL, REPLY_MS);
        s_live_cmd[0] = '\0';
        muse_camera_flush_events();
    }
}

// Runs what the live modes want: one continuous command, restarted on change.
static esp_err_t live_sync(void) {
    char want[32];
    live_want(want, sizeof(want));
    if (!strcmp(want, s_live_cmd)) return ESP_OK;
    live_break();
    if (!want[0]) return ESP_OK;
    esp_err_t err = camera_up();
    if (err == ESP_OK) err = set_sensor(1);
    if (err == ESP_OK && s_watch) err = set_model(s_watch_model);
    if (err == ESP_OK) err = muse_camera_request(want, NULL, NULL, REPLY_MS);
    if (err == ESP_OK) {
        strlcpy(s_live_cmd, want, sizeof(s_live_cmd));
        s_misses = 0;
    }
    return err;
}

// camera.watch: each label once when it appears, then at most once a second.
static void watch_frame(cJSON *data) {
    cJSON *found = detections(data, s_watch_min);
    int64_t now = esp_timer_get_time();
    cJSON *d;
    cJSON_ArrayForEach(d, found) {
        const char *label = cJSON_GetObjectItem(d, "label")->valuestring;
        int slot = -1, oldest = 0;
        for (int i = 0; i < SEEN_MAX; i++) {
            if (!strncmp(s_seen[i].label, label, sizeof(s_seen[i].label) - 1)) slot = i;   // as stored: 15 characters
            if (s_seen[i].at < s_seen[oldest].at) oldest = i;
        }
        if (slot >= 0 && now - s_seen[slot].at < DETECT_REPEAT_US) continue;
        if (slot < 0) {
            slot = oldest;
            strlcpy(s_seen[slot].label, label, sizeof(s_seen[slot].label));
        }
        s_seen[slot].at = now;
        cJSON *x = cJSON_GetObjectItem(d, "x"), *w = cJSON_GetObjectItem(d, "w");
        muse_hw_detection(label, cJSON_GetObjectItem(d, "score")->valueint, x ? x->valueint : 0,
                          x ? cJSON_GetObjectItem(d, "y")->valueint : 0, w ? w->valueint : 0,
                          w ? cJSON_GetObjectItem(d, "h")->valueint : 0);
    }
    cJSON_Delete(found);
}

// ---- The preview: frames decoded onto the screen ----

typedef struct {
    const uint8_t *jpeg;
    size_t len, pos;
    uint8_t *out;          // RGB565, high byte first, screen-sized
    int w, h, dx, dy;      // the screen, and where the frame's (0, 0) lands on it
} decode_t;

static UINT dec_in(JDEC *jd, BYTE *buf, UINT len) {
    decode_t *d = jd->device;
    if (d->pos + len > d->len) len = (UINT)(d->len - d->pos);
    if (buf) memcpy(buf, d->jpeg + d->pos, len);
    d->pos += len;
    return len;
}

static UINT dec_out(JDEC *jd, void *bitmap, JRECT *r) {
    decode_t *d = jd->device;
    const uint8_t *rgb = bitmap;
    for (int y = r->top; y <= r->bottom; y++) {
        for (int x = r->left; x <= r->right; x++, rgb += 3) {
            int sx = x + d->dx, sy = y + d->dy;
            if (sx < 0 || sy < 0 || sx >= d->w || sy >= d->h) continue;
            uint16_t px = (rgb[0] & 0xF8) << 8 | (rgb[1] & 0xFC) << 3 | rgb[2] >> 3;
            uint8_t *o = d->out + ((size_t)sy * d->w + sx) * 2;
            o[0] = px >> 8;
            o[1] = px & 0xff;
        }
    }
    return 1;
}

static void fill(uint8_t *out, int w, int h, int x0, int y0, int x1, int y1, uint16_t px) {
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < h; y++) {
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < w; x++) {
            out[((size_t)y * w + x) * 2] = px >> 8;
            out[((size_t)y * w + x) * 2 + 1] = px & 0xff;
        }
    }
}

// Draws a frame, and the watch's boxes on it, over the face.
static void show_frame(const uint8_t *jpeg, size_t n, cJSON *data) {
    int w, h;
    if (!muse_ui_image_size(&w, &h)) return;
    static uint8_t *screen;
    static void *pool;
    if (!screen) screen = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (!pool) pool = heap_caps_malloc(JPEG_POOL, MALLOC_CAP_SPIRAM);
    if (!screen || !pool) return;
    decode_t d = { .jpeg = jpeg, .len = n, .out = screen, .w = w, .h = h };
    JDEC jd;
    if (jd_prepare(&jd, dec_in, pool, JPEG_POOL, &d) != JDR_OK) return;
    d.dx = (w - (int)jd.width) / 2;
    d.dy = (h - (int)jd.height) / 2;
    if (jd_decomp(&jd, dec_out, 0) != JDR_OK) return;
    if (data) {
        cJSON *found = detections(data, s_watch_min), *b;
        cJSON_ArrayForEach(b, found) {
            cJSON *xj = cJSON_GetObjectItem(b, "x"), *wj = cJSON_GetObjectItem(b, "w");
            if (!xj || !wj) continue;
            int x0 = xj->valueint + d.dx, y0 = cJSON_GetObjectItem(b, "y")->valueint + d.dy;
            int x1 = x0 + wj->valueint, y1 = y0 + cJSON_GetObjectItem(b, "h")->valueint;
            const uint16_t green = 0x07E0;
            fill(screen, w, h, x0, y0, x1, y0 + 2, green);
            fill(screen, w, h, x0, y1 - 2, x1, y1, green);
            fill(screen, w, h, x0, y0, x0 + 2, y1, green);
            fill(screen, w, h, x1 - 2, y0, x1, y1, green);
        }
        cJSON_Delete(found);
    }
    muse_ui_image_draw(0, 0, w, h, (const uint16_t *)screen);
}

// A tap, the wheel or a talk ends the preview, as they end an image.
static bool preview_interrupted(void) {
    if (muse_state_mode(NULL) != MUSE_MODE_IDLE) return true;
    // Every event since, a few at a time, each looked at once: detections
    // (camera.watch's, a second each) mustn't crowd a tap out of view.
    muse_hw_event_t ev[4];
    int n;
    while ((n = muse_hw_events(s_preview_seq, ev, 4, 0)) > 0) {
        for (int i = 0; i < n; i++) {
            s_preview_seq = ev[i].seq;
            switch (ev[i].type) {
                case MUSE_HW_TAP:
                case MUSE_HW_LONG_PRESS:
                case MUSE_HW_SWIPE:
                case MUSE_HW_WHEEL_CLICK:
                case MUSE_HW_WHEEL_TURN: return true;
                default: break;   // detections, buttons, app and pet events
            }
        }
    }
    return false;
}

static void live_step(void) {
    cJSON *data = NULL;
    esp_err_t err = muse_camera_event(s_watch ? "INVOKE" : "SAMPLE", &data, LIVE_EVENT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        cJSON_Delete(data);
        if (++s_misses >= LIVE_MISSES) {
            ESP_LOGW(TAG, "live frames stopped; restarting");
            s_live_cmd[0] = '\0';   // live_sync starts it again
        }
        return;
    }
    s_misses = 0;
    s_last_use = esp_timer_get_time();
    static int frames;
    static int64_t since;
    if (!since) since = s_last_use;
    if (++frames, s_last_use - since >= 5000000) {
        ESP_LOGI(TAG, "live: %d frames in %.1f s", frames, (s_last_use - since) / 1e6);
        frames = 0;
        since = s_last_use;
    }
    if (s_watch && data) watch_frame(data);
    cJSON *image = cJSON_GetObjectItem(data, "image");
    if (s_preview && cJSON_IsString(image)) {
        size_t len = strlen(image->valuestring);
        uint8_t *jpeg = heap_caps_malloc(len / 4 * 3 + 4, MALLOC_CAP_SPIRAM);
        if (jpeg) {
            size_t n = b64_decode(image->valuestring, len, jpeg);
            if (s_preview_app[0]) {
                muse_apps_set_jpeg(s_preview_app, s_preview_widget, jpeg, n);
            } else {
                show_frame(jpeg, n, s_watch ? data : NULL);
            }
            free(jpeg);
        }
    }
    cJSON_Delete(data);
    // Full screen, a tap or talk ends it; in an app's widget, the app's buttons need the taps.
    if (s_preview && !s_preview_app[0] && preview_interrupted()) preview_off();
}

static cJSON *live_status(void) {
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddBoolToObject(pl, "preview", s_preview);
    cJSON_AddBoolToObject(pl, "watch", s_watch);
    if (s_watch) {
        cJSON_AddNumberToObject(pl, "model", s_watch_model);
        cJSON_AddNumberToObject(pl, "min_score", s_watch_min);
    }
    return pl;
}

// camera.preview / camera.watch / camera.stop.
static cJSON *run_live(cJSON *params) {
    const char *what = hw_str(params, "what");
    cJSON *onj = cJSON_GetObjectItem(params, "on");
    bool on = !cJSON_IsBool(onj) || cJSON_IsTrue(onj);
    if (!strcmp(what, "stop")) {
        live_off();
    } else if (!strcmp(what, "preview")) {
        if (!on) preview_off();
        s_preview_app[0] = s_preview_widget[0] = '\0';
        const char *target = hw_str(params, "widget");   // "app.widget"
        const char *dot = target ? strchr(target, '.') : NULL;
        if (on && target && (!dot || dot == target || dot - target >= (int)sizeof(s_preview_app))) {
            return hw_error("invalid_params", "widget is app_id.widget_id");
        }
        if (on && dot) {
            snprintf(s_preview_app, sizeof(s_preview_app), "%.*s", (int)(dot - target), target);
            strlcpy(s_preview_widget, dot + 1, sizeof(s_preview_widget));
        }
        s_preview = on;
        if (on) s_preview_seq = muse_hw_last_seq();
    } else if (!strcmp(what, "watch")) {
        if (on) {
            const char *model = hw_str(params, "model");
            int id = model_id(model ? model : "person");
            if (!id) return hw_error("invalid_params", "model is person, pet, gesture, custom or 1-4");
            int min_score = 50;
            hw_int(params, "min_score", &min_score);
            s_watch_model = id;
            s_watch_min = hw_clamp(min_score, 50, 99);
            memset(s_seen, 0, sizeof(s_seen));
        }
        s_watch = on;
    }
    esp_err_t err = live_sync();
    if (err != ESP_OK) {
        live_off();
        live_break();
        return camera_error(err, "starting the camera");
    }
    return hw_ok(live_status());
}

// ---- The worker ------------------------------------------------------------

static const char *const LIVE_MODES[] = { "preview", "watch" };   // preview first: see live_end
static bool *const s_live_mode[] = { &s_preview, &s_watch };
#define LIVE_N (sizeof(LIVE_MODES) / sizeof(LIVE_MODES[0]))
// Who turned each mode on: a script's name, or "" for the agent or the console. The worker's own.
EXT_RAM_BSS_ATTR static char s_live_owner[LIVE_N][32];

// A live command that worked makes whoever sent it the owner of the mode it turned on.
static void live_own(const job_t *job, cJSON *result) {
    const char *what = hw_str(job->params, "what");
    if (!what || !cJSON_IsTrue(cJSON_GetObjectItem(result, "ok"))) return;
    const char *id = job->to.request_id;
    const char *name = job->to.session_generation == HW_SCRIPT_SESSION && !strncmp(id, "script:", 7)
                       ? strchr(id + 7, ':') : NULL;
    for (size_t i = 0; i < LIVE_N; i++) {
        if (*s_live_mode[i] && !strcmp(what, LIVE_MODES[i])) {
            strlcpy(s_live_owner[i], name ? name + 1 : "", sizeof(s_live_owner[i]));
        }
    }
}

// A script ended: the modes it owns go off, and no one else's.
static void live_end(const char *script) {
    if (!script || !script[0]) return;
    for (size_t i = 0; i < LIVE_N; i++) {
        if (!*s_live_mode[i] || strcmp(s_live_owner[i], script)) continue;
        if (i == 0) preview_off();
        else *s_live_mode[i] = false;
    }
}

static void camera_task(void *arg) {
    QueueHandle_t jobs = arg;
    for (;;) {
        for (size_t i = 0; i < LIVE_N; i++) {
            if (!*s_live_mode[i]) s_live_owner[i][0] = '\0';   // however it went off
        }
        job_t job;
        if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(live_on() ? 0 : 1000)) == pdTRUE) {
            if (job.kind == JOB_ENDED) {
                live_end(hw_str(job.params, "script"));
                if (!live_on()) live_break();
                cJSON_Delete(job.params);
                continue;
            }
            cJSON *result;
            if (job.kind != JOB_LIVE) live_break();   // one-off commands pause the live modes
            esp_err_t err = job.kind == JOB_LIVE ? ESP_OK : camera_up();
            if (err != ESP_OK) {
                result = camera_error(err, "starting the camera");
            } else {
                switch (job.kind) {
                    case JOB_AT: result = run_at(job.params); break;
                    case JOB_CAPTURE: result = run_capture(job.params); break;
                    case JOB_DETECT: result = run_detect(job.params); break;
                    case JOB_LIVE:
                        result = run_live(job.params);
                        live_own(&job, result);
                        break;
                    default: result = run_models(); break;
                }
                s_last_use = esp_timer_get_time();
            }
            hw_reply(&job.to, result);
            cJSON_Delete(job.params);
            continue;
        }
        if (live_on()) {
            if (live_sync() == ESP_OK) {
                live_step();
            } else {
                ESP_LOGW(TAG, "live modes stopped: the camera failed");
                live_off();
            }
        } else {
            live_break();
            if (s_open && esp_timer_get_time() - s_last_use > IDLE_OFF_US) camera_down();
        }
    }
}

// A script stopped or deleted: the live modes it turned on go with it, so a
// camera it left watching doesn't run on for nobody. Queued
// behind the script's own camera jobs, which the worker does first.
void muse_hw_camera_script_ended(const char *script) {
    QueueHandle_t jobs = __atomic_load_n(&s_jobs, __ATOMIC_ACQUIRE);
    if (!jobs || !script) return;
    job_t job = { .kind = JOB_ENDED, .params = cJSON_CreateObject() };
    if (!job.params || !cJSON_AddStringToObject(job.params, "script", script) ||
        xQueueSend(jobs, &job, pdMS_TO_TICKS(ENDED_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "couldn't stop %s's live modes", script);
        cJSON_Delete(job.params);
    }
}

// Starts the worker once: the link, the console and scripts all queue jobs.
static QueueHandle_t start_worker(void) {
    static bool starting;
    QueueHandle_t jobs = __atomic_load_n(&s_jobs, __ATOMIC_ACQUIRE);
    if (jobs) return jobs;
    while (__atomic_test_and_set(&starting, __ATOMIC_ACQUIRE)) vTaskDelay(1);   // another caller is starting it
    jobs = s_jobs;
    if (!jobs) {
        jobs = xQueueCreate(4, sizeof(job_t));
        // Stack in PSRAM: the worker never writes flash. Off the UI's core, where
        // its frame decoding would take turns with LVGL.
        if (jobs && xTaskCreatePinnedToCoreWithCaps(camera_task, "hw_camera", 8192, jobs, 4, NULL,
                                                    portNUM_PROCESSORS > 1 ? 1 - MUSE_UI_CORE : tskNO_AFFINITY,
                                                    MUSE_BIG_CAPS) != pdPASS) {
            // All or nothing: a queue with no worker would take jobs nobody answers.
            vQueueDelete(jobs);
            jobs = NULL;
        }
        __atomic_store_n(&s_jobs, jobs, __ATOMIC_RELEASE);
    }
    __atomic_clear(&starting, __ATOMIC_RELEASE);
    return jobs;
}

static cJSON *queue_job(job_kind_t kind, cJSON *params, const char *request_id,
                        noise_ctrl_session_generation_t session_generation) {
    if (!muse_camera_present()) return hw_error("unsupported", "this board has no AI camera");
    QueueHandle_t jobs = start_worker();
    if (!jobs) return hw_error("out_of_memory", "failed to start the camera worker");
    job_t job = { .kind = kind, .params = params ? cJSON_Duplicate(params, true) : cJSON_CreateObject() };
    hw_reply_to(&job.to, request_id, session_generation);
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
        cJSON_Delete(job.params);
        return hw_error("busy", "the camera has four commands waiting");
    }
    return hw_async();
}

// ---- Registration and dispatch ---------------------------------------------

void muse_hw_camera_register(cJSON *commands) {
    if (!muse_camera_present()) return;
    cJSON *cap = hw_params("resolution", hw_param("string", "240x240, 416x416, 480x480 or 640x480 (default)."));
    cJSON_AddItemToObject(cap, "save", hw_param("string", "A path on the SD card to save the JPEG to; then only include_image returns it."));
    cJSON_AddItemToObject(cap, "include_image", hw_param("boolean", "With save, return the image too."));
#if CONFIG_MUSE_HATCH
    cJSON_AddItemToObject(cap, "ask", hw_param("boolean", "On the press of an app's talk key with \"photo\": true: that "
                                                          "voice note carries the photo, ahead of the question, instead of "
                                                          "returning it, so Muse sees what it's asked about. Other notes leave it out."));
#endif
    hw_add(commands, "camera.capture",
           "Take a photo with the camera; returns a base64 JPEG. The camera starts in "
           "a second or two if it has been idle.",
           NULL, cap, 20000);
    cJSON *det = cJSON_CreateObject();
    cJSON_AddItemToObject(det, "model", hw_param("string", "person (default), pet (cat, dog), gesture (paper, rock, scissors) or custom."));
    cJSON_AddItemToObject(det, "min_score", hw_param("integer", "50 to 99; default 50."));
    cJSON_AddItemToObject(det, "with_image", hw_param("boolean", "Also return the frame as a base64 JPEG."));
    hw_add(commands, "camera.detect",
           "Run one of the camera's own AI models on a frame; returns detections: label, "
           "score and a box (x, y from the top left, w, h) in a 416x416 frame.",
           NULL, det, 25000);
    hw_add(commands, "camera.models", "List the AI models on the camera.", NULL, NULL, 15000);
    cJSON *on = hw_params("on", hw_param("boolean", "false turns it off; default true."));
    cJSON *prev = cJSON_Duplicate(on, true);
    cJSON_AddItemToObject(prev, "widget", hw_param("string", "app_id.widget_id: into an app's image widget instead."));
    hw_add(commands, "camera.preview",
           "Show the camera's live video on the device's screen until turned off, a tap, the "
           "wheel or a talk; or in an app's image widget until turned off. With camera.watch on, "
           "boxes mark what it sees (full screen).",
           NULL, prev, 20000);
    cJSON *watch = cJSON_Duplicate(on, true);
    cJSON_AddItemToObject(watch, "model", hw_param("string", "person (default), pet, gesture or custom."));
    cJSON_AddItemToObject(watch, "min_score", hw_param("integer", "50 to 99; default 50."));
    hw_add(commands, "camera.watch",
           "Keep running a camera model; each label it sees becomes an input.read event "
           "(type detection: label, score, box), at once and then at most every second "
           "while it stays. Runs until turned off or camera.stop.",
           NULL, watch, 20000);
    hw_add(commands, "camera.stop", "Stop the camera's preview and watch.", NULL, NULL, 10000);
    cJSON *at = cJSON_CreateObject();
    cJSON_AddItemToObject(at, "timeout_ms", hw_param("integer", "How long to wait for the reply; default 3000."));
    hw_add(commands, "camera.at",
           "Send a raw SSCMA AT command to the camera (without AT+), e.g. VER? or TSCORE=60; "
           "returns its reply's code and data.",
           hw_params("cmd", hw_param("string", "The command.")), at, 35000);
}

cJSON *muse_hw_camera_command(const char *command, cJSON *params, const char *request_id,
                              noise_ctrl_session_generation_t session_generation) {
    if (!strcmp(command, "camera.capture")) return queue_job(JOB_CAPTURE, params, request_id, session_generation);
    if (!strcmp(command, "camera.detect")) return queue_job(JOB_DETECT, params, request_id, session_generation);
    if (!strcmp(command, "camera.models")) return queue_job(JOB_MODELS, params, request_id, session_generation);
    if (!strcmp(command, "camera.at")) return queue_job(JOB_AT, params, request_id, session_generation);
    const char *live[] = { "camera.preview", "camera.watch", "camera.stop" };
    for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); i++) {
        if (strcmp(command, live[i])) continue;
        cJSON *p = params ? cJSON_Duplicate(params, true) : cJSON_CreateObject();
        cJSON_DeleteItemFromObject(p, "what");
        cJSON_AddStringToObject(p, "what", command + 7);
        cJSON *result = queue_job(JOB_LIVE, p, request_id, session_generation);
        cJSON_Delete(p);
        return result;
    }
    return NULL;
}
