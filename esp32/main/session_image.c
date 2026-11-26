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

#include "session_image.h"

#if CONFIG_MUSE_SESSION_IMAGE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "identity.h"
#include "image_fetch.h"
#include "led_status.h"
#include "muse_input.h"
#include "muse_ui.h"
#include "session_image_core.h"

/* "mg" prefixed, as the device session's mg.session: the Nordic UART mirrors
 * it (CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS), so a phone-side tester reads the
 * run with mg_ble_client.py log. Never the request's words or the picture. */
static const char *const IMG_TAG = "mg.image";

/* The run, read by the UI and the console, written by the session task
 * (the request's answer, the command), the download task and the press. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static simg_run_t s_run;
/* Frames of an earlier request carry an older number and are dropped. */
static uint32_t s_ask_gen;
static unsigned s_ask_bytes;

typedef struct {
    noise_ctrl_session_generation_t generation;   /* 0: the console's img.show, no result */
    char request_id[64];
    int duration_ms;
} show_ctx_t;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static cJSON *error_result(const char *code, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

/* ---- The picture ------------------------------------------------------------ */

/* On the download task, once the picture is drawn or the download failed. */
static void on_drawn(const image_fetch_result_t *r, void *user)
{
    show_ctx_t *ctx = user;
    if (r->ok) {
        /* The showing starts now; muse_ui hides it from its frame tick (0:
         * until a tap, a talk press or display.show_animation). */
        if (ctx->duration_ms) {
            muse_ui_image_hide_after(ctx->duration_ms);
        }
    } else {
        /* What arrived of a broken download isn't the picture: back at once. */
        led_status_show_animation();
    }
    taskENTER_CRITICAL(&s_mux);
    simg_on_drawn(&s_run, r->ok, (unsigned)r->bytes, r->width, r->height, r->scale, r->ms,
                  r->ok ? NULL : r->message, now_us());
    taskEXIT_CRITICAL(&s_mux);
    noise_ctrl_task_hold(NOISE_TASK_WAIT_IMAGE, 0);   /* nothing more to wait for */
    if (r->ok) {
        ESP_LOGI(IMG_TAG, "on screen: %dx%d (1/%d), %u bytes in %d ms; showing it %d ms", r->width, r->height,
                 r->scale, (unsigned)r->bytes, r->ms, ctx->duration_ms);
    } else {
        ESP_LOGW(IMG_TAG, "not shown: %s (%s, %u bytes in %d ms)", r->message, r->code ? r->code : "failed",
                 (unsigned)r->bytes, r->ms);
    }
    if (ctx->generation) {
        cJSON *result;
        if (r->ok) {
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "ok", true);
            cJSON *payload = cJSON_AddObjectToObject(result, "payload");
            cJSON_AddNumberToObject(payload, "shown_for_ms", ctx->duration_ms);
            cJSON_AddNumberToObject(payload, "width", r->width);
            cJSON_AddNumberToObject(payload, "height", r->height);
            cJSON_AddNumberToObject(payload, "scale", r->scale);
            cJSON_AddNumberToObject(payload, "bytes", (double)r->bytes);
            cJSON_AddNumberToObject(payload, "download_ms", r->ms);
        } else {
            result = error_result(r->code ? r->code : "download_failed", r->message ? r->message : "failed");
        }
        noise_ctrl_send_command_result(ctx->generation, ctx->request_id, result);
    }
    free(ctx);
}

/* Starts the download and the run. NULL when started, else the error. */
static cJSON *start_show(const char *path, int duration_ms, const char *request_id,
                         noise_ctrl_session_generation_t generation)
{
    show_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return error_result("out_of_memory", "failed to allocate");
    }
    ctx->generation = generation;
    if (request_id) {
        strncpy(ctx->request_id, request_id, sizeof(ctx->request_id) - 1);
    }
    ctx->duration_ms = duration_ms;
    /* An earlier picture's showing ends here: this one's starts once drawn. */
    muse_ui_image_hide_after(0);
    const char *code = "download_failed", *message = "failed";
    if (!image_fetch_start_vm(path, IMAGE_FETCH_CENTRE, on_drawn, ctx, &code, &message)) {
        free(ctx);
        ESP_LOGW(IMG_TAG, "display.show_image refused: %s", message);
        return error_result(code, message);
    }
    taskENTER_CRITICAL(&s_mux);
    simg_on_invoke(&s_run, duration_ms, now_us());
    bool asked = s_run.asked;
    int64_t since = s_run.press_us ? (now_us() - s_run.press_us) / 1000 : -1;
    taskEXIT_CRITICAL(&s_mux);
    if (asked) {
        ESP_LOGI(IMG_TAG, "display.show_image: %s for %d ms (%lld ms after the press)", path, duration_ms,
                 (long long)since);
    } else {
        ESP_LOGI(IMG_TAG, "display.show_image: %s for %d ms", path, duration_ms);
    }
    return NULL;
}

cJSON *session_image_command(cJSON *params, const char *request_id,
                             noise_ctrl_session_generation_t session_generation)
{
    char path[SIMG_PATH_MAX + 1];
    int duration_ms;
    const char *code, *message;
    if (!simg_parse(params, path, sizeof(path), &duration_ms, &code, &message)) {
        ESP_LOGW(IMG_TAG, "display.show_image: %s", message);
        return error_result(code, message);
    }
    cJSON *error = start_show(path, duration_ms, request_id, session_generation);
    if (error) {
        return error;
    }
    cJSON *async = cJSON_CreateObject();
    cJSON_AddBoolToObject(async, "_async", true);
    return async;
}

/* ---- The test's request to Muse --------------------------------------------- */

/* POST /chat/stream's answer, on the session task: the status, and the
 * size of the acknowledgement (never its content). */
_Static_assert(SIMG_CHAT_NO_SESSION == NOISE_CTRL_REQ_NO_SESSION, "session_image_core's no-session status");

static void on_chat(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    (void)data;
    if ((uint32_t)(uintptr_t)ctx != s_ask_gen) {
        return;
    }
    s_ask_bytes += (unsigned)len;
    if (status) {
        taskENTER_CRITICAL(&s_mux);
        simg_on_chat_status(&s_run, status, now_us());
        int64_t since = (now_us() - s_run.press_us) / 1000;
        taskEXIT_CRITICAL(&s_mux);
        if (status > 0) {
            ESP_LOGI(IMG_TAG, "Muse took the request: HTTP %d (%lld ms after the press)", status, (long long)since);
        } else {
            ESP_LOGW(IMG_TAG, "the request to Muse ended: %d (%lld ms after the press)", status, (long long)since);
            noise_ctrl_task_hold(NOISE_TASK_WAIT_IMAGE, 0);   /* no picture coming */
        }
    }
    if (end) {
        ESP_LOGI(IMG_TAG, "request answered: %u bytes", s_ask_bytes);
    }
}

bool session_image_ask(char *why, size_t why_cap)
{
    taskENTER_CRITICAL(&s_mux);
    simg_tick(&s_run, now_us());
    bool busy = simg_busy(&s_run);
    taskEXIT_CRITICAL(&s_mux);
    if (busy) {
        snprintf(why, why_cap, "already asking");
        return false;
    }
    const char *fail = NULL;
    char body[400];
    size_t len = simg_chat_body(body, sizeof(body), identity_ble_name(), identity_node_id());
    /* No session open: the request waits for one (up to 10 s, asked for at
     * once, as mgcommands.h says), and only then fails with "isn't
     * connected yet" (on_chat, NOISE_CTRL_REQ_NO_SESSION). */
    if (!len) {
        fail = "the request doesn't fit";
    }
    int64_t id = 0;
    if (!fail) {
        static const char *const headers[] = { "Content-Type", "application/json", NULL };
        taskENTER_CRITICAL(&s_mux);
        uint32_t gen = ++s_ask_gen;
        s_ask_bytes = 0;
        simg_on_ask(&s_run, now_us());
        taskEXIT_CRITICAL(&s_mux);
        id = noise_ctrl_req_open("POST", "/chat/stream", headers, false, on_chat, (void *)(uintptr_t)gen);
        if (!id) {
            fail = "couldn't send the request to Muse (the session is busy or down)";
        } else if (!noise_ctrl_req_send(id, body, len, true, 200)) {
            noise_ctrl_req_cancel(id);
            fail = "couldn't send the request to Muse";
        }
    }
    if (fail) {
        taskENTER_CRITICAL(&s_mux);
        s_ask_gen++;   /* drop anything still to come for it */
        simg_on_ask_failed(&s_run, fail, now_us());
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW(IMG_TAG, "img.ask: %s", fail);
        snprintf(why, why_cap, "%s", fail);
        return false;
    }
    ESP_LOGI(IMG_TAG, "img.ask: asked Muse for a picture (POST /chat/stream, %u bytes)", (unsigned)len);
    /* Waiting for the picture: the device
     * session stays up for it, at most until the ask's own timeout. */
    noise_ctrl_task_hold(NOISE_TASK_WAIT_IMAGE, SIMG_ASK_TIMEOUT_MS);
    return true;
}

bool session_image_status(char *out, size_t cap)
{
    simg_run_t run;
    int64_t now = now_us();
    taskENTER_CRITICAL(&s_mux);
    simg_state_t before = s_run.state;
    simg_tick(&s_run, now);
    run = s_run;
    taskEXIT_CRITICAL(&s_mux);
    if (before == SIMG_ASKING && run.state == SIMG_FAILED) {
        ESP_LOGW(IMG_TAG, "img.ask: %s", run.error);
    }
    bool running = simg_busy(&run) || run.state == SIMG_SHOWING;
    if (running) {
        /* The Tests page is on show and Muse is making the picture: keep
         * the panel lit (as a talk turn does) so the line and then
         * the picture are seen; a dark panel stays dark. */
        muse_input_keep_lit("picture test");
    }
    simg_status_text(&run, now, out, cap);
    return running;
}

bool session_image_console(const char *line)
{
    if (!strcmp(line, "img.ask")) {
        char why[96];
        bool ok = session_image_ask(why, sizeof(why));
        printf("@img.ask %s\n", ok ? "asked" : why);
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "img.show=", 9)) {
        /* img.show=PATH[,MS]: a file on the VM, shown as display.show_image
         * would without Muse (the bench's check of the download and the
         * screen alone). The path ends at the last comma before digits. */
        char path[SIMG_PATH_MAX + 1];
        snprintf(path, sizeof(path), "%s", line + 9);
        int ms = SIMG_DEFAULT_MS;
        char *comma = strrchr(path, ',');
        if (comma && comma[1] && strspn(comma + 1, "0123456789") == strlen(comma + 1)) {
            ms = atoi(comma + 1);
            *comma = '\0';
            ms = ms == 0 ? 0 : ms < SIMG_MIN_MS ? SIMG_MIN_MS : ms > SIMG_MAX_MS ? SIMG_MAX_MS : ms;
        }
        cJSON *error = path[0] ? start_show(path, ms, NULL, 0) : error_result("invalid_params", "no path");
        if (error) {
            cJSON *e = cJSON_GetObjectItem(error, "error");
            const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
            ESP_LOGW(IMG_TAG, "img.show: %s", m ? m : "refused");
            printf("@img.show %s\n", m ? m : "refused");
            cJSON_Delete(error);
        } else {
            printf("@img.show started\n");
        }
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "img?")) {
        simg_run_t run;
        int64_t now = now_us();
        taskENTER_CRITICAL(&s_mux);
        simg_tick(&s_run, now);
        run = s_run;
        taskEXIT_CRITICAL(&s_mux);
        char report[256];
        simg_report(&run, now, report, sizeof(report));
        ESP_LOGI(IMG_TAG, "%s", report);
        printf("@%s\n", report);
        fflush(stdout);
        return true;
    }
    return false;
}

#endif  // CONFIG_MUSE_SESSION_IMAGE
