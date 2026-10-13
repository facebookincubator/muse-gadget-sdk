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

#include "image_fetch.h"
#include "led_status.h"
#include "sdkconfig.h"
#include "wifi_mgr.h"

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"

static const char *TAG = "link.image";

// The download task's stack. The TLS handshake runs on it for HTTPS.
#define FETCH_STACK_BYTES   6144
#define FETCH_TLS_STACK     8192
// Free byte-addressable RAM a download needs before it may start, beyond its
// stack: the decoder pool, HTTP client and socket buffers. HTTPS adds
// mbedTLS's 16 KB receive and 4 KB send record buffers plus handshake
// scratch, and the receive buffer needs one contiguous block, so without
// PSRAM it rarely fits next to the Noise session.
#define FETCH_HTTP_BYTES    (6 * 1024)
#if CONFIG_HOMEHUB_LED_BACKEND_MUSE && CONFIG_SPIRAM && CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
// On boards with the full UI, mbedTLS allocates from PSRAM, so HTTPS only adds a little
// socket state and needs no large internal block. The display backends keep
// the sizes they were tuned with.
#define FETCH_TLS_BYTES     (8 * 1024)
#define FETCH_TLS_BLOCK     1
#else
#define FETCH_TLS_BYTES     (26 * 1024)
#define FETCH_TLS_BLOCK     (17 * 1024)
#endif
// Left for Wi-Fi, lwIP and the Noise session: never start a download that
// would go below it, and abort one if free RAM drops under the floor.
#define FETCH_RESERVE_BYTES (4 * 1024)
#define FETCH_FLOOR_BYTES   (3 * 1024)
// Per socket operation, and for the whole download: a server that trickles
// bytes never trips the first, and would otherwise hold the task, its
// buffers and the busy flag indefinitely.
#define FETCH_TIMEOUT_MS    10000
#define FETCH_DEADLINE_MS   60000
#define FETCH_MAX_REDIRECTS 3
// Raw RGB565 is read and drawn this many rows at a time.
#define RAW_ROWS            4
// Work pool for the ROM JPEG decoder, the size its documentation asks for.
#define JPEG_POOL_BYTES     3100
// Largest block the decoder emits: a 16x16 MCU.
#define JPEG_MCU_PIXELS     (16 * 16)

typedef struct {
    char *url;
    int row;
    image_fetch_done_cb done;
    void *user;
    esp_http_client_handle_t http;
    int width, height;
    // The scheme the task's stack and memory budget were sized for. Redirects
    // may not change it: TLS on the HTTP stack overflows it, and an HTTPS
    // fetch must not continue in cleartext.
    bool https;
    int64_t deadline_us;
    // Two bytes read to sniff the format, handed back to the first read.
    uint8_t peek[2];
    size_t peek_len;
    size_t bytes;
    bool eof;
    bool low_memory;
    bool timed_out;
    bool scheme_changed;
    // JPEG output placement and one MCU converted for the panel.
    int x0, y0;
    uint16_t mcu[JPEG_MCU_PIXELS];
} fetch_t;

// Boards with the full UI also take IMAGE_FETCH_CENTRE, which centres a JPEG down the
// screen and starts raw data at the top. The display backends don't.
#if CONFIG_HOMEHUB_LED_BACKEND_MUSE
#define CENTRED(f) ((f)->row == IMAGE_FETCH_CENTRE)
#define MIN_ROW    IMAGE_FETCH_CENTRE
#else
#define CENTRED(f) false
#define MIN_ROW    0
#endif
// The row raw data starts at, and the top of the space a JPEG must fit.
#define TOP_ROW(f) (CENTRED(f) ? 0 : (f)->row)

static atomic_bool s_busy;

// Byte-addressable internal RAM. MALLOC_CAP_INTERNAL alone also counts
// IRAM that only takes 32-bit accesses, which malloc() never hands out.
#define BYTE_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static size_t internal_free(void) {
    return heap_caps_get_free_size(BYTE_CAPS);
}

static void log_memory(const char *stage) {
    ESP_LOGI(TAG, "%s: int=%uK/%uK min=%uK dma=%uK/%uK min=%uK", stage,
             (unsigned)(internal_free() / 1024),
             (unsigned)(heap_caps_get_largest_free_block(BYTE_CAPS) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(BYTE_CAPS) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_DMA) / 1024));
}

// Bound the next socket operation by the time left. False once it is up,
// when the next operation is left 1 ms so that a call in progress returns.
static bool within_deadline(fetch_t *f) {
    int64_t left_ms = (f->deadline_us - esp_timer_get_time()) / 1000;
    if (left_ms <= 0) {
        f->timed_out = true;
        esp_http_client_set_timeout_ms(f->http, 1);
        return false;
    }
    esp_http_client_set_timeout_ms(f->http,
                                   left_ms < FETCH_TIMEOUT_MS ? (int)left_ms : FETCH_TIMEOUT_MS);
    return true;
}

// Read up to `len` bytes, waiting for all of them unless the body ends.
// Returns the count read, or -1 on a network error, low memory or timeout.
static int fetch_read(fetch_t *f, uint8_t *buf, size_t len) {
    size_t have = 0;
    while (have < len && f->peek_len) {
        buf[have++] = f->peek[0];
        f->peek[0] = f->peek[1];
        f->peek_len--;
    }
    while (have < len && !f->eof) {
        if (internal_free() < FETCH_FLOOR_BYTES) {
            f->low_memory = true;
            return -1;
        }
        if (!within_deadline(f)) return -1;
        int n = esp_http_client_read(f->http, (char *)buf + have, (int)(len - have));
        if (n < 0) return -1;
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(f->http)) f->eof = true;
            else return -1;
        }
        have += (size_t)n;
        f->bytes += (size_t)n;
    }
    return (int)have;
}

// ---- Raw RGB565 --------------------------------------------------------------

static const char *draw_raw(fetch_t *f, int *rows_drawn) {
    const size_t row_bytes = (size_t)f->width * sizeof(uint16_t);
    uint8_t *buf = malloc(RAW_ROWS * row_bytes);
    if (!buf) return "out of memory";
    const char *err = NULL;
    int y = TOP_ROW(f);
    for (;;) {
        int n = fetch_read(f, buf, RAW_ROWS * row_bytes);
        if (n < 0) {
            err = f->low_memory ? "memory ran low" : "download failed";
            break;
        }
        if (n % row_bytes != 0) {
            err = "raw data must be whole rows of RGB565, high byte first";
            break;
        }
        int rows = n / (int)row_bytes;
        if (y + rows > f->height) {
            err = "image runs past the last row";
            break;
        }
        if (rows && !led_status_draw_rect(0, y, f->width, rows, (const uint16_t *)buf)) {
            err = "display write failed";
            break;
        }
        y += rows;
        if (rows < RAW_ROWS) break;
    }
    free(buf);
    *rows_drawn = y - TOP_ROW(f);
    return err;
}

// ---- JPEG --------------------------------------------------------------------

static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT len) {
    fetch_t *f = jd->device;
    if (buf) {
        int n = fetch_read(f, buf, len);
        return n < 0 ? 0 : (UINT)n;
    }
    // Skip: read into the MCU buffer, which is free between output calls.
    UINT skipped = 0;
    while (skipped < len) {
        UINT chunk = len - skipped;
        if (chunk > sizeof(f->mcu)) chunk = sizeof(f->mcu);
        int n = fetch_read(f, (uint8_t *)f->mcu, chunk);
        if (n <= 0) break;
        skipped += (UINT)n;
    }
    return skipped;
}

static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect) {
    fetch_t *f = jd->device;
    int w = rect->right - rect->left + 1;
    int h = rect->bottom - rect->top + 1;
    const uint8_t *rgb = bitmap;
    for (int i = 0; i < w * h; i++, rgb += 3) {
        uint16_t px = ((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3);
        f->mcu[i] = (uint16_t)((px >> 8) | (px << 8));
    }
    return led_status_draw_rect(f->x0 + rect->left, f->y0 + rect->top, w, h, f->mcu);
}

static const char *draw_jpeg(fetch_t *f, int *out_w, int *out_h, int *out_scale) {
    void *pool = malloc(JPEG_POOL_BYTES);
    if (!pool) return "out of memory";
    const char *err = NULL;
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, jpeg_in, pool, JPEG_POOL_BYTES, f);
    if (rc == JDR_OK) {
        // Shrink by the smallest power of two that fits below `row`.
        int avail = f->height - TOP_ROW(f);
        uint8_t scale = 0;
        while (scale < 3 && ((int)(jd.width >> scale) > f->width
                             || (int)(jd.height >> scale) > avail)) {
            scale++;
        }
        int w = (int)(jd.width >> scale), h = (int)(jd.height >> scale);
        *out_w = w;
        *out_h = h;
        *out_scale = 1 << scale;
        if (w > f->width || h > avail) {
            err = "JPEG is too large even at 1/8 scale";
        } else {
            f->x0 = (f->width - w) / 2;
            f->y0 = CENTRED(f) ? (f->height - h) / 2 : f->row;
            rc = jd_decomp(&jd, jpeg_out, scale);
        }
    }
    if (!err && rc != JDR_OK) {
        err = f->low_memory ? "memory ran low"
            : rc == JDR_INP ? "download failed"
            : rc == JDR_INTR ? "display write failed"
            : rc == JDR_FMT3 ? "unsupported JPEG: use baseline, not progressive"
            : "not a valid JPEG";
    }
    free(pool);
    return err;
}

// ---- Task --------------------------------------------------------------------

// esp_http_client_read keeps reading until it has all it was asked for, so a
// server trickling bytes holds a single call far past the deadline. It
// raises an event after each socket read, which re-clamps the next one.
static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    fetch_t *f = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA || evt->event_id == HTTP_EVENT_ON_HEADER) {
        within_deadline(f);
    }
    return ESP_OK;
}

// Does the client's current URL still use the scheme the fetch was sized for?
static bool same_scheme(fetch_t *f) {
    // get_url truncates; the scheme is all that is needed.
    char url[16];
    if (esp_http_client_get_url(f->http, url, sizeof(url)) != ESP_OK) return false;
    return f->https ? strncasecmp(url, "https://", 8) == 0
                    : strncasecmp(url, "http://", 7) == 0;
}

static esp_err_t open_following_redirects(fetch_t *f, int *status) {
    for (int i = 0;; i++) {
        if (!within_deadline(f)) return ESP_ERR_TIMEOUT;
        esp_err_t err = esp_http_client_open(f->http, 0);
        if (err != ESP_OK) return err;
        esp_http_client_fetch_headers(f->http);
        *status = esp_http_client_get_status_code(f->http);
        bool redirect = *status == 301 || *status == 302 || *status == 303
                     || *status == 307 || *status == 308;
        if (!redirect || i == FETCH_MAX_REDIRECTS) return ESP_OK;
        err = esp_http_client_set_redirection(f->http);
        esp_http_client_close(f->http);
        if (err != ESP_OK) return err;
        if (!same_scheme(f)) {
            f->scheme_changed = true;
            return ESP_FAIL;
        }
    }
}

static void fetch_task(void *arg) {
    fetch_t *f = arg;
    wifi_mgr_transfer(true);
    int64_t start = esp_timer_get_time();
    f->deadline_us = start + FETCH_DEADLINE_MS * 1000LL;
    image_fetch_result_t result = { .code = "download_failed" };
    log_memory("image download start");

    esp_http_client_config_t cfg = {
        .url = f->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = FETCH_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .disable_auto_redirect = true,
        .event_handler = on_http_event,
        .user_data = f,
    };
    f->http = esp_http_client_init(&cfg);
    int status = 0;
    esp_err_t err = f->http ? open_following_redirects(f, &status) : ESP_ERR_NO_MEM;
    log_memory("image download connected");
    char msg[64];
    if (f->scheme_changed) {
        result.message = "redirect changed scheme";
    } else if (f->timed_out) {
        result.message = "download timed out";
    } else if (err != ESP_OK) {
        snprintf(msg, sizeof(msg), "could not connect: %s", esp_err_to_name(err));
        result.message = msg;
    } else if (status != 200) {
        snprintf(msg, sizeof(msg), "server answered HTTP %d", status);
        result.message = msg;
    } else {
        int n = fetch_read(f, f->peek, sizeof(f->peek));
        f->peek_len = n > 0 ? (size_t)n : 0;
        const char *fail;
        if (f->peek_len == 2 && f->peek[0] == 0xFF && f->peek[1] == 0xD8) {
            result.format = "jpeg";
            fail = draw_jpeg(f, &result.width, &result.height, &result.scale);
        } else {
            int rows = 0;
            result.format = "rgb565";
            result.width = f->width;
            result.scale = 1;
            fail = draw_raw(f, &rows);
            result.height = rows;
        }
        // Show what arrived, even if the download broke off.
        led_status_draw_done();
        if (fail && f->timed_out) fail = "download timed out";
        if (fail) {
            result.message = fail;
            if (f->low_memory) result.code = "out_of_memory";
            else if (!f->timed_out && strcmp(fail, "download failed") != 0) {
                result.code = "invalid_image";
            }
        } else {
            result.ok = true;
            result.code = NULL;
        }
    }
    if (f->http) esp_http_client_cleanup(f->http);
    wifi_mgr_transfer(false);

    result.bytes = f->bytes;
    result.ms = (int)((esp_timer_get_time() - start) / 1000);
    ESP_LOGI(TAG, "image download %s: %s %dx%d (1/%d) %u bytes in %d ms, stack %u free",
             result.ok ? "done" : "failed", result.ok ? result.format : result.message,
             result.width, result.height, result.scale, (unsigned)result.bytes, result.ms,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    log_memory("image download end");
    f->done(&result, f->user);
    free(f->url);
    free(f);
    atomic_store(&s_busy, false);
    vTaskDelete(NULL);
}

bool image_fetch_start(const char *url, int row, image_fetch_done_cb done,
                       void *user, const char **code, const char **message) {
    int width, height;
    if (!led_status_display_info(&width, &height)) {
        *code = "unsupported";
        *message = "no display";
        return false;
    }
    bool https = strncasecmp(url, "https://", 8) == 0;
    if (!https && strncasecmp(url, "http://", 7) != 0) {
        *code = "invalid_params";
        *message = "url must be http:// or https://";
        return false;
    }
    if (row < MIN_ROW || row >= height) {
        *code = "invalid_params";
        *message = "row is off the screen";
        return false;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        *code = "busy";
        *message = "another image is still downloading";
        return false;
    }

    size_t stack = https ? FETCH_TLS_STACK : FETCH_STACK_BYTES;
    size_t need = stack + FETCH_RESERVE_BYTES
                + (https ? FETCH_TLS_BYTES : FETCH_HTTP_BYTES);
    size_t block = heap_caps_get_largest_free_block(BYTE_CAPS);
    if (internal_free() < need || block < stack
        || (https && block < FETCH_TLS_BLOCK)) {
        log_memory("image download refused");
        atomic_store(&s_busy, false);
        *code = "out_of_memory";
        *message = https ? "not enough free memory for an HTTPS download; try http://"
                         : "not enough free memory for a download";
        return false;
    }

    fetch_t *f = calloc(1, sizeof(*f));
    if (f) f->url = strdup(url);
    if (!f || !f->url) {
        free(f);
        atomic_store(&s_busy, false);
        *code = "out_of_memory";
        *message = "failed to allocate";
        return false;
    }
    f->https = https;
    f->row = row;
    f->done = done;
    f->user = user;
    f->width = width;
    f->height = height;
    if (xTaskCreate(fetch_task, "image", stack, f, 4, NULL) != pdPASS) {
        free(f->url);
        free(f);
        atomic_store(&s_busy, false);
        *code = "out_of_memory";
        *message = "failed to start the download task";
        return false;
    }
    return true;
}

#else

bool image_fetch_start(const char *url, int row, image_fetch_done_cb done,
                       void *user, const char **code, const char **message) {
    (void)url;
    (void)row;
    (void)done;
    (void)user;
    *code = "unsupported";
    *message = "no display";
    return false;
}

#endif
