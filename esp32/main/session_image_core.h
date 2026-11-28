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
 * display.show_image and the "Ask Muse for an image" test (session_image.c),
 * without ESP-IDF: the parameters' checks, the request the test sends Muse,
 * and the run's state and timing, for the host tests
 * (tests/test_session_image.py).
 *
 * A run: the test asks Muse (POST /chat/stream on the gadget's own session),
 * Muse calls display.show_image with a file on its VM, the gadget reads it
 * (GET /fs/raw in ranges), draws it, shows it for duration_ms and goes back.
 * Muse may also call display.show_image on its own: that's a run too.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;

#define SIMG_DEFAULT_MS     5000
#define SIMG_MIN_MS         1000
#define SIMG_MAX_MS         60000
#define SIMG_PATH_MAX       200
/* From the press until Muse calls display.show_image: making a picture can
 * take Muse a while. */
#define SIMG_ASK_TIMEOUT_MS 120000
/* The command's own timeout in link.register: the download over BLE. */
#define SIMG_COMMAND_TIMEOUT_MS 90000

typedef enum {
    SIMG_IDLE = 0,
    SIMG_ASKING,    /* the request went to Muse; waiting for display.show_image */
    SIMG_FETCHING,  /* display.show_image came: downloading and drawing */
    SIMG_SHOWING,   /* on screen for duration_ms */
    SIMG_SHOWN,     /* shown and gone */
    SIMG_FAILED,
} simg_state_t;

typedef struct {
    simg_state_t state;
    bool asked;             /* started by the test (else Muse called on its own) */
    int64_t press_us;       /* the test's press */
    int64_t chat_us;        /* POST /chat/stream answered */
    int64_t invoke_us;      /* display.show_image arrived */
    int64_t drawn_us;       /* the whole picture on screen */
    int chat_status;        /* HTTP status of POST /chat/stream; 0 none yet */
    int duration_ms;
    unsigned bytes;
    int width, height, scale;
    int fetch_ms;
    char error[128];   /* SIMG_NOT_CONNECTED, the longest, whole */
} simg_run_t;

/* display.show_image's parameters: path (required) and duration_ms
 * (optional: 0 keeps the picture until dismissed, else clamped to
 * SIMG_MIN_MS..SIMG_MAX_MS). False with an error code and message for Muse. */
bool simg_parse(const cJSON *params, char *path, size_t path_cap, int *duration_ms,
                const char **code, const char **message);

/* The test's request to Muse, a JSON body for POST /chat/stream: plain
 * words naming this gadget, nothing about the command (Muse finds it from
 * link.register's description), and its node_id at the top level (the one
 * link.register sends). The length, or 0 if it doesn't fit. */
size_t simg_chat_body(char *out, size_t cap, const char *gadget_name, const char *node_id);

/* The run, as events happen (now in microseconds on any clock). */
void simg_on_ask(simg_run_t *r, int64_t now);
void simg_on_ask_failed(simg_run_t *r, const char *why, int64_t now);
/* The chat request's status: SIMG_CHAT_NO_SESSION (noise_control.h's
 * NOISE_CTRL_REQ_NO_SESSION) when it waited 10 s for the device session and
 * none opened: fails with SIMG_NOT_CONNECTED. */
#define SIMG_CHAT_NO_SESSION (-3)
#define SIMG_NOT_CONNECTED "Muse isn't connected yet; asked the phone to connect, press again in a few seconds " \
                           "(the Muse app must be open on the phone)"
void simg_on_chat_status(simg_run_t *r, int status, int64_t now);
void simg_on_invoke(simg_run_t *r, int duration_ms, int64_t now);
void simg_on_drawn(simg_run_t *r, bool ok, unsigned bytes, int width, int height, int scale,
                   int fetch_ms, const char *error, int64_t now);
/* Timeouts and the end of the showing. */
void simg_tick(simg_run_t *r, int64_t now);
/* A busy run: asking, or a picture on its way. */
bool simg_busy(const simg_run_t *r);

/* One line for the Tests page. */
void simg_status_text(const simg_run_t *r, int64_t now, char *out, size_t cap);
/* One line for the console's img?: the state and every mark, in ms from the
 * press (or from display.show_image when Muse called on its own). */
void simg_report(const simg_run_t *r, int64_t now, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
