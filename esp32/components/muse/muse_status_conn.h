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

#pragma once

/*
 * The Status page's connection row (it tells which phone and app the gadget
 * is connected to). Pure text rules, no LVGL (muse_home.c draws them;
 * tests/test_muse_status_conn.py runs them on the host):
 *
 *   a client whose app is ready  "Connected: <name>": the name the gadget
 *                                reads itself, the central's GAP Device Name,
 *                                else "phone" (the read pending or failed; the
 *                                client's client_name, 18, is reserved); never
 *                                plain "Connected"
 *                                nor "Connected: Muse app"
 *   Wi-Fi up, no such client     "Connected: <Wi-Fi network>"
 *   a central, its app not ready "Bluetooth only"
 *   nothing                      "Not connected"
 *
 * A tap on the row shows the detail for 5 s (MUSE_STATUS_DETAIL_MS) or until
 * the next tap: "<app name>: <version>" from client_app (setting 19), or
 * "App version unknown"; over Wi-Fi "Wi-Fi: <IP>, <RSSI> dBm". Names get the
 * screen fonts' ASCII stand-ins (muse_text.h). A row too wide for its line
 * wraps onto a second one (no "...", the whole name
 * readable), at a space where that keeps both lines whole, else at whole
 * UTF-8 characters; only what still doesn't fit the second line is cut, at
 * whole characters (muse_status_wrap).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_STATUS_DETAIL_MS 5000

typedef enum {
    MUSE_CONN_NONE,      /* "Not connected" */
    MUSE_CONN_BT_ONLY,   /* a central, its app not ready: "Bluetooth only" */
    MUSE_CONN_APP,       /* a client whose app is ready */
    MUSE_CONN_WIFI,      /* Wi-Fi up, no such client */
} muse_conn_kind_t;

typedef struct {
    muse_conn_kind_t kind;
    const char *gap_name;      /* the central's GAP Device Name ("" or NULL: none: "phone") */
    const char *app_name;      /* setting 19's two fields */
    const char *app_version;
    const char *ssid;          /* MUSE_CONN_WIFI: the network */
    const char *ip;            /* ...its address ("" none yet) */
    int rssi;                  /* ...dBm */
} muse_conn_info_t;

/* The row's text into out (n bytes; cut at a whole character if it must). */
void muse_status_conn_text(const muse_conn_info_t *in, char *out, size_t n);
/* The tap's text into out; "" where there's nothing to show (Not connected). */
void muse_status_detail_text(const muse_conn_info_t *in, char *out, size_t n);

/* The longest row text, in bytes, with its terminator. */
#define MUSE_STATUS_LINE_MAX 96

/* text onto at most two lines as measure() measures them (the label's font):
 * line 1 (w1 wide) whole if it fits (MUSE_STATUS_WRAP_ONE, line2 ""); else
 * broken after a space when the rest then fits line 2 (w2 wide), else after
 * as many whole UTF-8 characters as fit; line 2 the rest (MUSE_STATUS_WRAP_TWO),
 * cut at whole characters if it's still too wide, never "..."
 * (MUSE_STATUS_WRAP_CUT: a smaller font may hold it whole). Each line n bytes. */
#define MUSE_STATUS_WRAP_ONE 1
#define MUSE_STATUS_WRAP_TWO 2
#define MUSE_STATUS_WRAP_CUT 3
int muse_status_wrap(const char *text, int w1, int w2, int (*measure)(const char *s, void *ctx), void *ctx,
                     char *line1, char *line2, size_t n);

/* The tap toggle: a tap shows the detail (has_detail) or puts the row back;
 * it goes back by itself MUSE_STATUS_DETAIL_MS after the tap. */
typedef struct {
    bool detail;
    int64_t since_ms;
} muse_status_tap_t;

void muse_status_tap(muse_status_tap_t *t, int64_t now_ms, bool has_detail);
/* Whether the detail shows at now_ms (expiring it after 5 s). */
bool muse_status_detail_shown(muse_status_tap_t *t, int64_t now_ms);

#ifdef __cplusplus
}
#endif
