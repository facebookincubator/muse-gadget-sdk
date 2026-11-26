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
 * The avatar face over the gadget's own session to Muse: what
 * mg_face_core.h decides, run on a worker task that exists only while there's
 * work (mg_face.h).
 */
#include "mg_face.h"

#if CONFIG_MUSE_AVATAR_FACE

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "config_store.h"
#include "identity.h"
#include "mg_face_core.h"
#include "mg_face_jpeg.h"
#include "muse_board.h"
#include "muse_bootguard.h"
#include "muse_home.h"
#include "muse_ui.h"
#include "noise_control.h"
#include "muse_voice.h"
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif

/* "mg" prefixed like mg.session and mg.image: the Nordic UART mirrors it.
 * Never a byte of the picture, the ETag or the Muse's name: the slot, the
 * size and the ETag's tag only. */
static const char *const TAG = "mg.face";

#define FACE_NS          CONFIG_STORE_FACE_NS
#define FACE_KEY_REC     "rec"
#define FACE_KEY_PICK    "pick"
#define FACE_KEY_NAME    "name"
#define FACE_KEY_SIZE    "size"   /* Large (0, the default) or Small (1): Settings > Watch face's Size */
/* No internal RAM held while the face is idle or a call is on (the face
 * only draws now and then), on every board: its state, its
 * semaphores and every buffer it takes are PSRAM (the panel picture, Small's
 * sums, the scrim's rows, the TJpgDec pool, NVS reads); its
 * worker task exists only while there's work (the boot's check, a look, a
 * store, a Pick or Size change), started by mg_face_tick() on the app's own
 * loop (no timer, no task of its own) and deleted once nothing is due. The
 * worker's stack is internal while it runs (an NVS write from a PSRAM stack
 * asserts where spi_flash turns the cache off), and it never starts during
 * a call or a video stream, nor when internal RAM is short. Only a spinlock
 * and a few atomic flags stay internal. */
#define FACE_WORKER_STACK    4608   /* 2,504 B used at most (ESP-Mosaico: a fetch, decode and store) */
#define FACE_WORKER_HEADROOM 8192   /* internal RAM the worker leaves, beside its stack */
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#define FACE_PSRAM_BSS   EXT_RAM_BSS_ATTR
#else
#define FACE_PSRAM_BSS
#endif
#define FACE_MALLOC(n)   heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define FACE_REQ_MS      15000   /* one request's answer */
#define FACE_CHUNK_MAX   8192    /* a range at most this (and the session's frame bound) */

/* ---- State --------------------------------------------------------------- */

/* Internal: the spinlock and the flags other tasks set. Everything else is
 * PSRAM, read and written by the worker, the console, the command and the UI
 * under s_mux. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool s_enabled;        /* mg_face_start ran: not SAFE, a colour panel */
static atomic_bool s_running;        /* a worker task exists */
static face_sched_t s_sched FACE_PSRAM_BSS;
static face_pick_t s_pick FACE_PSRAM_BSS;
static face_size_t s_size FACE_PSRAM_BSS;           /* the avatar face's Size (Large / Small) */
static bool s_have FACE_PSRAM_BSS;                  /* a picture is stored (and checked) */
static face_rec_t s_rec FACE_PSRAM_BSS;             /* its header */
static char s_name[FACE_NAME_MAX] FACE_PSRAM_BSS;   /* the Muse's name, "" until known */
static char s_node[FACE_NODE_MAX] FACE_PSRAM_BSS;
static char s_path[FACE_PATH_MAX] FACE_PSRAM_BSS;
static bool s_loaded FACE_PSRAM_BSS;                /* the boot's stored picture was looked at */
static bool s_shown FACE_PSRAM_BSS;                 /* the picture is on the face */
static uint64_t s_gen FACE_PSRAM_BSS;               /* sessions seen (a reopen counts) */
static bool s_was_open FACE_PSRAM_BSS;
static int64_t s_hold_until FACE_PSRAM_BSS;          /* the session task held until (mg_face_tick's) */
static atomic_bool s_check_now;
static atomic_bool s_pick_dirty;     /* a pick change: saved and drawn by the worker */
static atomic_bool s_size_dirty;     /* a Size change: saved and drawn again by the worker */
static atomic_uint s_forgets;        /* mg_face_forget() calls */
static unsigned s_forgets_seen FACE_PSRAM_BSS;
static char s_last[96] FACE_PSRAM_BSS;   /* the last look's outcome, for face? */
/* The ETag of a file refused for what it is (PNG, WebP, a bad JPEG): not read
 * again until it changes. */
static char s_refused[FACE_ETAG_MAX] FACE_PSRAM_BSS;
/* The largest JPEG this board's NVS takes (face_nvs_cap). */
static uint32_t s_cap FACE_PSRAM_BSS;   /* set by mg_face_start */
/* A face.set waiting for its look's result. */
static bool s_set_pending FACE_PSRAM_BSS;
static noise_ctrl_session_generation_t s_set_gen FACE_PSRAM_BSS;
static char s_set_id[64] FACE_PSRAM_BSS;
/* The worker's last run, for face?: its stack's high-water mark, internal RAM
 * before and lowest during it, how long it ran, how many runs, and starts put
 * off (a call, a stream, or short internal RAM). */
static unsigned s_w_stack_used FACE_PSRAM_BSS, s_w_int_before FACE_PSRAM_BSS, s_w_int_low FACE_PSRAM_BSS,
    s_w_ms FACE_PSRAM_BSS, s_w_runs FACE_PSRAM_BSS, s_w_deferred FACE_PSRAM_BSS;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void set_last(const char *fmt, const char *what)
{
    taskENTER_CRITICAL(&s_mux);
    snprintf(s_last, sizeof(s_last), fmt, what);
    taskEXIT_CRITICAL(&s_mux);
}

/* ---- NVS ---------------------------------------------------------------- */

static void pick_save(face_pick_t pick)
{
    nvs_handle_t h;
    if (nvs_open(FACE_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, FACE_KEY_PICK, (uint8_t)pick);
        nvs_commit(h);
        nvs_close(h);
    }
}

static face_pick_t pick_load(void)
{
    nvs_handle_t h;
    uint8_t v = FACE_PICK_UNSET;
    if (nvs_open(FACE_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, FACE_KEY_PICK, &v);
        nvs_close(h);
    }
    return v == FACE_PICK_PIXEL || v == FACE_PICK_AVATAR ? (face_pick_t)v : FACE_PICK_UNSET;
}

static void size_save(face_size_t size)
{
    nvs_handle_t h;
    if (nvs_open(FACE_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, FACE_KEY_SIZE, (uint8_t)size);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Large unless Small was saved (nothing saved, or anything else: Large). */
static face_size_t size_load(void)
{
    nvs_handle_t h;
    uint8_t v = FACE_SIZE_LARGE;
    if (nvs_open(FACE_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, FACE_KEY_SIZE, &v);
        nvs_close(h);
    }
    return v == FACE_SIZE_SMALL ? FACE_SIZE_SMALL : FACE_SIZE_LARGE;
}

static face_size_t size_now(void)
{
    taskENTER_CRITICAL(&s_mux);
    face_size_t size = s_size;
    taskEXIT_CRITICAL(&s_mux);
    return size;
}

static void name_load(char *out, size_t cap)
{
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open(FACE_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = cap;
        if (nvs_get_str(h, FACE_KEY_NAME, out, &n) != ESP_OK) {
            out[0] = '\0';
        }
        nvs_close(h);
    }
}

static void name_save(const char *name)
{
    nvs_handle_t h;
    if (nvs_open(FACE_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, FACE_KEY_NAME, name);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* The stored record into a new PSRAM buffer; NULL if none. */
static uint8_t *rec_read(size_t *len)
{
    nvs_handle_t h;
    *len = 0;
    if (nvs_open(FACE_NS, NVS_READONLY, &h) != ESP_OK) {
        return NULL;
    }
    size_t n = 0;
    uint8_t *blob = NULL;
    if (nvs_get_blob(h, FACE_KEY_REC, NULL, &n) == ESP_OK && n > 0 && n <= sizeof(face_rec_t) + FACE_JPEG_MAX) {
        blob = FACE_MALLOC(n);
        if (blob && nvs_get_blob(h, FACE_KEY_REC, blob, &n) != ESP_OK) {
            free(blob);
            blob = NULL;
        }
    }
    nvs_close(h);
    *len = blob ? n : 0;
    return blob;
}

/* Stores a record without ever taking the room pairing, tokens and settings
 * need (face_nvs_plan: a quarter of the partition stays free). NVS writes the
 * new value whole before it drops the old one (a power cut leaves the old).
 * On a partition with room for one record only, the old goes first: the new
 * picture already decoded whole, so only a cut in that window loses both (the
 * default face shows). False: the old one is kept (refused or failed). */
static unsigned s_store_refused FACE_PSRAM_BSS;
static bool rec_write(const uint8_t *blob, size_t len)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(FACE_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    size_t old = 0;
    if (nvs_get_blob(h, FACE_KEY_REC, NULL, &old) != ESP_OK) {
        old = 0;
    }
    face_nvs_plan_t plan = face_nvs_plan((uint32_t)st.total_entries, (uint32_t)st.free_entries, (uint32_t)old,
                                         (uint32_t)len);
    ESP_LOGI(TAG, "nvs before: %u of %u entries free; record %u bytes (%u entries), old %u: %s",
             (unsigned)st.free_entries, (unsigned)st.total_entries, (unsigned)len,
             (unsigned)face_nvs_entries((uint32_t)len), (unsigned)old,
             plan == FACE_NVS_BESIDE ? "beside" : plan == FACE_NVS_IN_PLACE ? "in place" : "refused");
    if (plan == FACE_NVS_REFUSE) {
        nvs_close(h);
        s_store_refused++;
        ESP_LOGW(TAG, "store refused: it would cut into NVS's reserve (old picture kept; %u refused)",
                 s_store_refused);
        return false;
    }
    esp_err_t err = ESP_OK;
    if (plan == FACE_NVS_IN_PLACE) {
        nvs_erase_key(h, FACE_KEY_REC);
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(h, FACE_KEY_REC, blob, len);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "store failed: %s", esp_err_to_name(err));
    } else if (nvs_get_stats(NULL, &st) == ESP_OK) {
        ESP_LOGI(TAG, "nvs after: %u of %u entries free", (unsigned)st.free_entries, (unsigned)st.total_entries);
    }
    return err == ESP_OK;
}

static void rec_erase(const char *why)
{
    nvs_handle_t h;
    if (nvs_open(FACE_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, FACE_KEY_REC);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "stored picture erased (%s)", why);
}

/* ---- The face on screen ------------------------------------------------- */

/* The rows with text on the home (muse_home_geom), for the scrim. */
static int face_bands(int w, int h, face_band_t *b, int max)
{
    muse_home_geom_t g;
    muse_home_geom(w, h, muse_board->round, muse_board->corner_px, &g);
    int n = 0, top = g.top;
#define BAND(a, z) do { if (n < max) { b[n].y0 = (a) < 0 ? 0 : (a); b[n].y1 = (z) > h ? h : (z); n++; } } while (0)
    BAND(top + g.batt_top - 2, top + g.batt_top + g.batt_h + 2);
    BAND(top + g.time_top - 4, top + g.time_top + g.time_digits + 4);
    BAND(top + g.date_top - 4, top + g.date_top + g.date_caps + 4);
    /* Speaker and Call need none: their filled discs carry their own contrast,
     * and a band at Muse's line would cut the avatar's face in half (Mosaico). */
    BAND(g.dot_cy - g.dot_d - 4, g.dot_cy + g.dot_d + 4);
#undef BAND
    return n;
}

static int face_ring_px(int w, int h)
{
    muse_home_geom_t g;
    muse_home_geom(w, h, muse_board->round, muse_board->corner_px, &g);
    return g.ring > 0 ? g.ring : 0;
}

/* Decodes jpeg into a new panel buffer (PSRAM) at the face's Size with the
 * scrim baked in: once per picture, Size change or boot, never per frame.
 * Small: the decoded panel picture area-averaged to 0.70 in
 * place, centered across, its ear row at the reference height, black around it
 * (face_small_draw), and the scrim only under the bands that reach into it.
 * NULL with *why on failure ("out of memory" is never the picture's fault). */
static uint16_t *face_render(const uint8_t *jpeg, size_t len, face_size_t size, const char **why, int *iw,
                             int *ih, int *scale)
{
    int w = muse_board->width, h = muse_board->height;
    uint16_t *px = FACE_MALLOC((size_t)w * h * sizeof(uint16_t));
    if (!px) {
        *why = "out of memory";
        return NULL;
    }
    *why = mg_face_jpeg_decode(jpeg, len, px, w, h, iw, ih, scale);
    if (*why) {
        free(px);
        return NULL;
    }
    face_band_t bands[8];
    int n = face_bands(w, h, bands, 8);
    if (size == FACE_SIZE_SMALL) {
        uint32_t *work = FACE_MALLOC(face_small_work_words(w) * sizeof(uint32_t));
        if (!work) {
            free(px);
            *why = "out of memory";
            return NULL;
        }
        face_small_t sm;
        face_small_draw(px, w, h, muse_board->round, false, work, &sm);
        free(work);
        n = face_small_bands(bands, n, sm.top);
    }
    uint8_t *dark = FACE_MALLOC((size_t)h);
    if (!dark) {
        free(px);
        *why = "out of memory";
        return NULL;
    }
    face_scrim_rows(h, bands, n, h / 12, dark);   /* the bands fade over ~a twelfth; the edges over a quarter */
    face_bake(px, w, h, dark, muse_board->round, face_ring_px(w, h), false);
    free(dark);
    return px;
}

/* What shows now: the avatar face over the picture, or the default. */
static void face_apply(uint16_t *rendered)
{
    taskENTER_CRITICAL(&s_mux);
    bool show = face_avatar_shown(s_pick, s_have);
    taskEXIT_CRITICAL(&s_mux);
    if (show && rendered) {
        muse_ui_face_bg_set(rendered);   /* takes it */
        s_shown = true;
    } else {
        if (rendered) {
            free(rendered);
        }
        if (!show && s_shown) {
            muse_ui_face_bg_set(NULL);
            s_shown = false;
        }
    }
}

/* Renders the stored picture again (a pick change, the boot). */
static void face_show_stored(void)
{
    size_t len;
    uint8_t *blob = rec_read(&len);
    face_rec_t hdr;
    const uint8_t *jpeg;
    face_rec_check_t c = blob ? face_rec_check(blob, len, s_node, &hdr, &jpeg) : FACE_REC_SHORT;
    if (c != FACE_REC_OK) {
        free(blob);
        face_apply(NULL);
        return;
    }
    const char *why;
    int iw, ih, sc;
    uint16_t *px = face_render(jpeg, hdr.jpeg_len, size_now(), &why, &iw, &ih, &sc);
    free(blob);
    if (!px) {
        ESP_LOGW(TAG, "stored picture not drawn: %s", why);
    }
    face_apply(px);
}

/* The boot's look at the stored picture, once the boot is healthy (so the
 * recovery tiers and SAFE never depend on it): a damaged one is erased. */
static void face_boot_load(void)
{
    size_t len;
    uint8_t *blob = rec_read(&len);
    if (!blob) {
        ESP_LOGI(TAG, "no stored picture; face %s", s_pick == FACE_PICK_AVATAR ? "avatar" : "pixel");
        return;
    }
    face_rec_t hdr;
    const uint8_t *jpeg;
    face_rec_check_t c = face_rec_check(blob, len, s_node, &hdr, &jpeg);
    const char *why = NULL;
    uint16_t *px = NULL;
    int iw = 0, ih = 0, sc = 1;
    if (c == FACE_REC_OK) {
        px = face_render(jpeg, hdr.jpeg_len, size_now(), &why, &iw, &ih, &sc);
    }
    free(blob);
    if (c != FACE_REC_OK || (!px && why && strcmp(why, "out of memory") != 0)) {
        ESP_LOGW(TAG, "stored picture damaged (%s): erasing it", c != FACE_REC_OK ? face_rec_check_name(c) : why);
        rec_erase("damaged");
        name_save("");
        s_name[0] = '\0';
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    s_have = true;
    s_rec = hdr;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "stored picture: slot rec, %u bytes, %dx%d, etag #%08x; face %s, size %s",
             (unsigned)hdr.jpeg_len, iw, ih, (unsigned)face_etag_tag(hdr.etag),
             s_pick == FACE_PICK_AVATAR ? "avatar" : s_pick == FACE_PICK_PIXEL ? "pixel (picked)" : "pixel",
             face_size_name(size_now()));
    face_apply(px);
}

/* ---- Requests on the session -------------------------------------------- */

typedef struct {
    SemaphoreHandle_t done;
    uint32_t gen;
    face_head_t head;
    uint8_t *buf;
    size_t cap, len;
    bool overflow, finished;
} face_req_t;

static face_req_t s_req FACE_PSRAM_BSS;
/* Both in PSRAM, made once (mg_face_start): taken by the worker and given by
 * the session task's callbacks, never with the cache off. */
static SemaphoreHandle_t s_req_lock FACE_PSRAM_BSS;

static void req_hdr(void *ctx, const char *name, size_t nl, const char *value, size_t vl)
{
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    if ((uint32_t)(uintptr_t)ctx == s_req.gen && !s_req.finished) {
        face_head_header(&s_req.head, name, nl, value, vl);
    }
    xSemaphoreGive(s_req_lock);
}

static void req_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    bool signal = false;
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    if ((uint32_t)(uintptr_t)ctx == s_req.gen && !s_req.finished) {
        if (status) {
            s_req.head.status = status;
        }
        if (len && s_req.buf) {
            size_t room = s_req.cap - s_req.len, n = len < room ? len : room;
            if (n < len) {
                s_req.overflow = true;
            }
            memcpy(s_req.buf + s_req.len, data, n);
            s_req.len += n;
        }
        if (end || status < 0 || s_req.overflow) {
            s_req.finished = true;
            signal = true;
        }
    }
    xSemaphoreGive(s_req_lock);
    if (signal) {
        xSemaphoreGive(s_req.done);
    }
}

/* One GET, answered or failed within FACE_REQ_MS. The reply's head in *head,
 * its body in buf; a body longer than cap fails it (a range, /identity). The
 * probe passes no buf: its body isn't kept. False if it never finished. */
static bool face_request(const char *path, const char *const *headers, uint8_t *buf, size_t cap,
                         face_head_t *head, size_t *got)
{
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    uint32_t gen = ++s_req.gen;
    face_head_reset(&s_req.head);
    s_req.buf = buf;
    s_req.cap = cap;
    s_req.len = 0;
    s_req.overflow = s_req.finished = false;
    xSemaphoreGive(s_req_lock);
    xSemaphoreTake(s_req.done, 0);
    int64_t id = noise_ctrl_req_open_hdr("GET", path, headers, true, req_frame, req_hdr, (void *)(uintptr_t)gen);
    bool ok = id != 0;
    if (ok) {
        ok = xSemaphoreTake(s_req.done, pdMS_TO_TICKS(FACE_REQ_MS)) == pdTRUE;
    }
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    bool overflow = s_req.overflow;
    *head = s_req.head;
    *got = s_req.len;
    s_req.gen++;   /* anything still to come is dropped */
    s_req.buf = NULL;
    xSemaphoreGive(s_req_lock);
    if (id && (!ok || overflow)) {
        noise_ctrl_req_cancel(id);
    }
    if (!ok && head->status == 0) {
        head->status = -1;
    }
    return ok && !overflow;
}

/* A Forget since this pass began (mg_face_forget bumps s_forgets before it
 * erases): a job writes nothing of the last owner's back, and erases again
 * when one came while it wrote. */
static bool face_forgot(void)
{
    return atomic_load(&s_forgets) != s_forgets_seen;
}

/* GET /identity answered 403 or 404 this boot (a session whose node role has
 * no such route, such as the phone-relayed device session): not asked again. */
static bool s_identity_refused;

static void read_name(void)
{
    if (s_identity_refused) {
        return;
    }
    char *buf = FACE_MALLOC(FACE_IDENTITY_MAX);
    if (!buf) {
        return;
    }
    face_head_t head;
    size_t got = 0;
    char name[FACE_NAME_MAX];
    if (face_request("/identity", NULL, (uint8_t *)buf, FACE_IDENTITY_MAX, &head, &got) &&
        head.status == 200 && face_identity_name(buf, got, name, sizeof(name))) {
        if (face_forgot()) {
            free(buf);
            return;
        }
        if (strcmp(name, s_name) != 0) {
            taskENTER_CRITICAL(&s_mux);
            snprintf(s_name, sizeof(s_name), "%s", name);
            taskEXIT_CRITICAL(&s_mux);
            name_save(name);
            if (face_forgot()) {
                config_erase_face();
            }
            ESP_LOGI(TAG, "the Muse's name: %u bytes (kept with the picture)", (unsigned)strlen(name));
        }
    } else {
        ESP_LOGW(TAG, "GET /identity: %d (%u bytes)", head.status, (unsigned)got);
        s_identity_refused = head.status == 403 || head.status == 404;
    }
    free(buf);
}

/* Downloads, checks and stores a changed picture. NULL on success, else why. */
static const char *fetch_store(const face_head_t *probe, uint32_t *out_len, int *out_w, int *out_h)
{
    uint32_t total = probe->status == 206 ? probe->total : probe->content_length;
    uint8_t *jpeg = FACE_MALLOC(total);
    if (!jpeg) {
        return "out of memory";
    }
    face_asm_t a = { .buf = jpeg, .total = total, .have = 0 };
    size_t chunk = noise_ctrl_req_max_body();
    if (chunk > FACE_CHUNK_MAX) chunk = FACE_CHUNK_MAX;
    if (chunk < 512) chunk = 512;
    uint8_t *piece = FACE_MALLOC(chunk);
    const char *err = piece ? NULL : "out of memory";
    char etag[FACE_ETAG_MAX];
    snprintf(etag, sizeof(etag), "%s", probe->etag);
    int ranges = 0;
    int64_t t0 = now_us();
    uint32_t first, last;
    while (!err && face_asm_next(&a, (uint32_t)chunk, &first, &last)) {
        char range[48];
        face_range_header(range, sizeof(range), first, last);
        const char *headers[5] = { "Range", range, NULL, NULL, NULL };
        if (etag[0]) {
            headers[2] = "If-Range";
            headers[3] = etag;
        }
        face_head_t head;
        size_t got = 0;
        if (!face_request(s_path, headers, piece, chunk, &head, &got)) {
            err = head.status == -2 ? "a reply too large for the session" : "a range wasn't answered";
            break;
        }
        ranges++;
        if (head.status == 206 && head.range_start_ok && head.have_total && head.total == total &&
            face_asm_put(&a, head.range_start, piece, got) && got == last - first + 1) {
            continue;
        }
        /* A 200: the daemon ignored Range (or If-Range didn't match: the file
         * changed under us). Only usable if it is the whole file at once. */
        if (head.status == 200) {
            err = "the file changed while it was read (or the VM ignored Range)";
        } else {
            err = "a range came back wrong";
        }
    }
    free(piece);
    if (!err && a.have != total) {
        err = "short read";
    }
    if (err) {
        free(jpeg);
        return err;
    }
    int ms = (int)((now_us() - t0) / 1000);
    face_fmt_t fmt = face_sniff(jpeg, total);
    if (fmt != FACE_FMT_JPEG) {
        free(jpeg);
        return fmt == FACE_FMT_PNG ? "PNG refused: the face takes a baseline JPEG"
             : fmt == FACE_FMT_WEBP ? "WebP refused: the face takes a baseline JPEG" : "not a JPEG";
    }
    /* Decode whole before anything is stored: the old picture stays until
     * the new one verifies. */
    const char *why = NULL;
    int iw = 0, ih = 0, sc = 1;
    uint16_t *px = face_render(jpeg, total, size_now(), &why, &iw, &ih, &sc);
    if (!px) {
        free(jpeg);
        return why;
    }
    size_t cap = sizeof(face_rec_t) + total;
    uint8_t *blob = FACE_MALLOC(cap);
    size_t blen = blob ? face_rec_build(blob, cap, jpeg, total, iw, ih, etag, s_node, s_name) : 0;
    free(jpeg);
    if (face_forgot()) {
        free(blob);
        free(px);
        return "forgotten meanwhile: not stored";
    }
    if (!blen || !rec_write(blob, blen)) {
        free(blob);
        free(px);
        return "couldn't store it (the old picture is kept)";
    }
    if (face_forgot()) {
        config_erase_face();
        free(blob);
        free(px);
        return "forgotten meanwhile: not stored";
    }
    face_rec_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    free(blob);
    taskENTER_CRITICAL(&s_mux);
    s_have = true;
    s_rec = hdr;
    face_pick_t before = s_pick;
    s_pick = face_pick_after_store(s_pick);
    face_pick_t after = s_pick;
    taskEXIT_CRITICAL(&s_mux);
    if (after != before) {
        pick_save(after);
        ESP_LOGI(TAG, "the first picture picked the avatar face");
    }
    ESP_LOGI(TAG, "stored: slot rec, %u bytes in %d ranges, %d ms, %dx%d (1/%d), etag #%08x", (unsigned)total,
             ranges, ms, iw, ih, sc, (unsigned)face_etag_tag(etag));
    face_apply(px);
    *out_len = total;
    *out_w = iw;
    *out_h = ih;
    return NULL;
}

static void finish_set(bool ok, const char *code, const char *message, uint32_t bytes, int w, int h)
{
    taskENTER_CRITICAL(&s_mux);
    bool pending = s_set_pending;
    noise_ctrl_session_generation_t gen = s_set_gen;
    char id[sizeof(s_set_id)];
    memcpy(id, s_set_id, sizeof(id));
    s_set_pending = false;
    taskEXIT_CRITICAL(&s_mux);
    if (!pending) {
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", ok);
    if (ok) {
        cJSON *p = cJSON_AddObjectToObject(result, "payload");
        cJSON_AddNumberToObject(p, "bytes", bytes);
        cJSON_AddNumberToObject(p, "width", w);
        cJSON_AddNumberToObject(p, "height", h);
    } else {
        cJSON *e = cJSON_AddObjectToObject(result, "error");
        cJSON_AddStringToObject(e, "code", code);
        cJSON_AddStringToObject(e, "message", message);
    }
    noise_ctrl_send_command_result(gen, id, result);
}

/* One look: the name, then the file's size and ETag, then what to do. */
static void face_look(bool forced)
{
    if (!s_name[0] || forced) {
        read_name();
    }
    static const char *const probe_headers[] = { "Range", "bytes=0-0", NULL };
    face_head_t head;
    size_t got = 0;
    /* The body isn't kept (the one byte of a 206, or a 404's message). */
    bool answered = face_request(s_path, probe_headers, NULL, 0, &head, &got);
    taskENTER_CRITICAL(&s_mux);
    char stored[FACE_ETAG_MAX];
    snprintf(stored, sizeof(stored), "%s", s_have ? s_rec.etag : "");
    uint32_t stored_len = s_have ? s_rec.jpeg_len : 0;
    taskEXIT_CRITICAL(&s_mux);
    face_probe_t p = answered ? face_probe_decide(&head, stored, stored_len, s_cap) : FACE_PROBE_FAILED;
    bool refused_again = p == FACE_PROBE_CHANGED && head.etag[0] && !strcmp(head.etag, s_refused);
    if (refused_again && !forced) {
        p = FACE_PROBE_SAME;
    }
    ESP_LOGI(TAG, "look: %s (HTTP %d, %u bytes, etag #%08x)", face_probe_name(p), head.status,
             (unsigned)(head.status == 206 ? head.total : head.content_length), (unsigned)face_etag_tag(head.etag));
    bool ok = p != FACE_PROBE_FAILED;
    switch (p) {
    case FACE_PROBE_ABSENT:
        set_last("%s", "no picture on the VM yet");
        finish_set(false, "not_found", "workspace/devices/<node_id>/face.jpg isn't there", 0, 0, 0);
        break;
    case FACE_PROBE_SAME:
        set_last("%s", refused_again ? "the file on the VM was refused" : "up to date");
        finish_set(true, NULL, NULL, stored_len, s_rec.width, s_rec.height);
        break;
    case FACE_PROBE_TOO_LARGE:
        set_last("%s", "refused: too large for this gadget");
        finish_set(false, "too_large", "the JPEG is larger than this gadget takes: make it smaller and call again", 0, 0, 0);
        break;
    case FACE_PROBE_CHANGED: {
        uint32_t len = 0;
        int w = 0, h = 0;
        const char *err = fetch_store(&head, &len, &w, &h);
        if (err) {
            ESP_LOGW(TAG, "not stored: %s", err);
            set_last("not stored: %s", err);
            ok = strncmp(err, "PNG", 3) == 0 || strncmp(err, "WebP", 4) == 0 || strstr(err, "JPEG") != NULL;
            if (ok) {
                snprintf(s_refused, sizeof(s_refused), "%s", head.etag);
            }
            finish_set(false, "bad_image", err, 0, 0, 0);
        } else {
            set_last("%s", "stored and shown");
            finish_set(true, NULL, NULL, len, w, h);
        }
        break;
    }
    default:
        set_last("%s", "no answer");
        finish_set(false, "unreachable", "the gadget couldn't read the file", 0, 0, 0);
        break;
    }
    face_sched_on_look(&s_sched, ok, now_us());
}

static void face_forget_now(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_have = false;
    memset(&s_rec, 0, sizeof(s_rec));
    s_name[0] = '\0';
    s_pick = FACE_PICK_UNSET;
    s_size = FACE_SIZE_LARGE;   /* its key went with the rest (config_erase_face) */
    taskEXIT_CRITICAL(&s_mux);
    atomic_store(&s_size_dirty, false);
    if (s_shown) {
        muse_ui_face_bg_set(NULL);
        s_shown = false;
    }
}

/* A Size change (Settings or the console): saved, and the stored picture
 * drawn again at the new Size if the avatar face shows it (decoded and
 * scaled once, here, never per frame). */
static void face_size_changed(void)
{
    taskENTER_CRITICAL(&s_mux);
    face_size_t size = s_size;
    bool show = face_avatar_shown(s_pick, s_have);
    taskEXIT_CRITICAL(&s_mux);
    size_save(size);
    if (show) {
        face_show_stored();
    }
}

/* A call or a video stream is on: the face's work waits (nobody's looking at
 * it, and the call needs the RAM). */
static bool face_busy_elsewhere(void)
{
    bool busy = muse_voice_in_call();
#if CONFIG_MUSE_VIDEO
    busy = busy || muse_video_decoding();
#endif
    return busy;
}

/* Whether the worker has anything to do now (any task). */
static bool face_work_due(bool open, int64_t now)
{
    if (!muse_board || !muse_board->width) {
        return false;   /* the board and its UI first (this starts beside Link's session) */
    }
    if (atomic_load(&s_pick_dirty) || atomic_load(&s_size_dirty) || atomic_load(&s_check_now)) {
        return true;
    }
    taskENTER_CRITICAL(&s_mux);
    bool due = atomic_load(&s_forgets) != s_forgets_seen || (!s_loaded && muse_bootguard_is_healthy()) ||
               (s_loaded && face_sched_due(&s_sched, open, now));
    taskEXIT_CRITICAL(&s_mux);
    return due;
}

/* One pass over what's due (the worker's). */
static void face_work_once(void)
{
    unsigned forgets = atomic_load(&s_forgets);
    if (forgets != s_forgets_seen) {
        s_forgets_seen = forgets;
        face_forget_now();
    }
    if (!s_loaded) {
        if (!muse_bootguard_is_healthy()) {
            return;   /* the boot's check waits for the healthy mark (recovery never depends on it) */
        }
        s_loaded = true;
        if (muse_ui_image_bits() == 2) {
            ESP_LOGI(TAG, "a 4-gray panel: no avatar face");
            atomic_store(&s_enabled, false);
            return;
        }
        face_boot_load();
        taskENTER_CRITICAL(&s_mux);
        face_sched_now(&s_sched, now_us());   /* the boot's one look, at the session's next open */
        taskEXIT_CRITICAL(&s_mux);
    }
    if (atomic_exchange(&s_pick_dirty, false)) {
        taskENTER_CRITICAL(&s_mux);
        face_pick_t pick = s_pick;
        bool have = s_have;
        taskEXIT_CRITICAL(&s_mux);
        pick_save(pick);
        if (pick != FACE_PICK_AVATAR) {
            s_shown = false;
        } else {
            if (have) {
                face_show_stored();
            }
            atomic_store(&s_check_now, true);   /* the selection's one look */
        }
    }
    if (atomic_exchange(&s_size_dirty, false)) {
        face_size_changed();
    }
    bool open = noise_ctrl_is_connected();
    int64_t now = now_us();
    bool forced = atomic_exchange(&s_check_now, false);
    if (forced) {
        taskENTER_CRITICAL(&s_mux);
        face_sched_now(&s_sched, now);
        taskEXIT_CRITICAL(&s_mux);
    }
    taskENTER_CRITICAL(&s_mux);
    bool due = face_sched_due(&s_sched, open, now);
    taskEXIT_CRITICAL(&s_mux);
    if (due) {
        face_look(forced);
    } else if (forced && !open) {
        finish_set(false, "unreachable", "no session open", 0, 0, 0);
    }
}

static size_t int_free(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/* The worker: what's due, then gone (its stack and control block go back to
 * the heap once the idle task has cleaned up). */
/* The face is the node's (workspace/devices/<node_id>): a setup that gives
 * the gadget a new node id moves it there, from the next job on. */
static void face_sync_node(void)
{
    const char *node = identity_node_id();
    if (strcmp(s_node, node) != 0) {
        snprintf(s_node, sizeof(s_node), "%s", node);
        snprintf(s_path, sizeof(s_path), "/fs/raw/" FACE_PATH_FMT, s_node);
        ESP_LOGI(TAG, "node id changed: the face is now read for it");
    }
}

static void face_worker(void *arg)
{
    (void)arg;
    int64_t t0 = now_us();
    size_t low = int_free();
    for (;;) {
        face_sync_node();
        face_work_once();
        size_t f = int_free();
        low = f < low ? f : low;
        atomic_store(&s_running, false);
        /* More came in meanwhile (a face.set during a look): go round again,
         * unless another kick has started a new worker. */
        bool expect = false;
        if (!atomic_load(&s_enabled) || !face_work_due(noise_ctrl_is_connected(), now_us()) ||
            !atomic_compare_exchange_strong(&s_running, &expect, true)) {
            break;
        }
    }
    unsigned used = FACE_WORKER_STACK - (unsigned)uxTaskGetStackHighWaterMark(NULL);
    unsigned ms = (unsigned)((now_us() - t0) / 1000);
    taskENTER_CRITICAL(&s_mux);
    s_w_stack_used = used;
    s_w_int_low = (unsigned)low;
    s_w_ms = ms;
    s_w_runs++;
    unsigned before = s_w_int_before;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "worker done in %u ms: stack %u of %u B used; internal free %u B before it, %u B lowest during",
             ms, used, (unsigned)FACE_WORKER_STACK, before, (unsigned)low);
    vTaskDelete(NULL);
}

/* Starts the worker if work is due and none runs (any task; never blocks). */
static void face_kick(void)
{
    if (!atomic_load(&s_enabled) || atomic_load(&s_running)) {
        return;
    }
    if (!face_work_due(noise_ctrl_is_connected(), now_us())) {
        return;
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (face_busy_elsewhere() || largest < FACE_WORKER_STACK + FACE_WORKER_HEADROOM) {
        taskENTER_CRITICAL(&s_mux);
        s_w_deferred++;
        taskEXIT_CRITICAL(&s_mux);
        return;   /* the next tick tries again */
    }
    bool expect = false;
    if (!atomic_compare_exchange_strong(&s_running, &expect, true)) {
        return;
    }
    size_t before = int_free();
    taskENTER_CRITICAL(&s_mux);
    s_w_int_before = (unsigned)before;
    taskEXIT_CRITICAL(&s_mux);
    if (xTaskCreate(face_worker, "mg_face", FACE_WORKER_STACK, NULL, 2, NULL) != pdPASS) {
        atomic_store(&s_running, false);
        ESP_LOGW(TAG, "worker not started (no memory); trying again later");
    }
}

/* ---- The public side ---------------------------------------------------- */

void mg_face_start(void)
{
    /* On the app's task at boot (an internal stack: NVS reads). Starts no
     * task: mg_face_tick() starts the worker when there's work. */
    if (atomic_load(&s_enabled) || muse_bootguard_mode() == MUSE_BOOT_SAFE) {
        return;   /* once; never in SAFE */
    }
    snprintf(s_node, sizeof(s_node), "%s", identity_node_id());
    snprintf(s_path, sizeof(s_path), "/fs/raw/" FACE_PATH_FMT, s_node);
    face_sched_init(&s_sched);
    s_cap = FACE_JPEG_MAX;
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
        s_cap = face_nvs_cap((uint32_t)st.total_entries);
        ESP_LOGI(TAG, "nvs: %u of %u entries free; the face takes JPEGs up to %u bytes", (unsigned)st.free_entries,
                 (unsigned)st.total_entries, (unsigned)s_cap);
    }
    s_pick = pick_load();
    s_size = size_load();
    name_load(s_name, sizeof(s_name));
    size_t free0 = int_free();
    size_t big0 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_req.done = xSemaphoreCreateBinaryWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_req_lock = xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_req.done || !s_req_lock) {
        ESP_LOGE(TAG, "couldn't start");
        return;
    }
    atomic_store(&s_enabled, true);
    size_t free1 = int_free();
    size_t big1 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    /* (Another task's allocation in between would show here too.) */
    ESP_LOGI(TAG, "internal bytes held by the face: %d (internal free %u -> %u, largest block %u -> %u; worker "
             "only while there's work)",
             free0 >= free1 ? (int)(free0 - free1) : -(int)(free1 - free0), (unsigned)free0, (unsigned)free1,
             (unsigned)big0, (unsigned)big1);
}

void mg_face_tick(void)
{
    if (!atomic_load(&s_enabled)) {
        return;
    }
    bool open = noise_ctrl_is_connected();
    int64_t now = now_us();
    taskENTER_CRITICAL(&s_mux);
    if (open && !s_was_open) {
        face_sched_on_session(&s_sched, ++s_gen, now);   /* a look owed goes now */
    }
    s_was_open = open;
    int64_t until = 0;
    bool wants = face_sched_wants(&s_sched, now, &until);
    taskEXIT_CRITICAL(&s_mux);
    /* The device session's active task: only while a look is owed; ended
     * as soon as it isn't. */
    if (wants && until != s_hold_until) {
        noise_ctrl_task_hold(NOISE_TASK_WAIT_FACE, (uint32_t)((until - now + 999) / 1000));
        s_hold_until = until;
    } else if (!wants && s_hold_until) {
        noise_ctrl_task_hold(NOISE_TASK_WAIT_FACE, 0);
        s_hold_until = 0;
    }
    face_kick();
}

bool mg_face_forget(const char *why)
{
    /* The record, the name and the pick go now, on the caller's task (a
     * reset may restart right after); the screen lets go on the worker. */
    atomic_fetch_add(&s_forgets, 1);   /* first: a job writing now sees it (face_forgot) */
    bool erased = config_erase_face();
    ESP_LOGI(TAG, "picture and name %s (%s)", erased ? "erased" : "NOT erased", why ? why : "forget");
    face_kick();   /* the screen lets go now (or at the next tick) */
    return erased;
}

const char *mg_face_own_path(void)
{
    if (!s_path[0]) {
        snprintf(s_node, sizeof(s_node), "%s", identity_node_id());
        snprintf(s_path, sizeof(s_path), "/fs/raw/" FACE_PATH_FMT, s_node);
    }
    return s_path + strlen("/fs/raw/");
}

uint32_t mg_face_max_bytes(void)
{
    return s_cap ? s_cap : FACE_JPEG_MAX;
}

cJSON *mg_face_command(cJSON *params, const char *request_id, noise_ctrl_session_generation_t generation)
{
    uint32_t byte_len = 0;
    char etag[FACE_ETAG_MAX];
    const char *code, *message;
    cJSON *result;
    if (!atomic_load(&s_enabled)) {
        code = "unavailable";
        message = "this gadget has no avatar face";
    } else if (face_set_parse(params, mg_face_own_path(), s_cap, &byte_len, etag, sizeof(etag), &code,
                              &message)) {
        taskENTER_CRITICAL(&s_mux);
        bool busy = s_set_pending;
        if (!busy) {
            s_set_pending = true;
            s_set_gen = generation;
            snprintf(s_set_id, sizeof(s_set_id), "%s", request_id ? request_id : "");
        }
        taskEXIT_CRITICAL(&s_mux);
        if (busy) {
            code = "busy";
            message = "a face.set is already being read; try again shortly";
        } else {
            ESP_LOGI(TAG, "face.set: %u bytes", (unsigned)byte_len);
            atomic_store(&s_check_now, true);
            face_kick();
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "_async", true);
            return result;
        }
    }
    ESP_LOGW(TAG, "face.set refused: %s", message);
    result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *e = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return result;
}

bool mg_face_avatar_picked(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool v = s_pick == FACE_PICK_AVATAR;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

void mg_face_pick(bool avatar)
{
    /* From any task (Settings runs on LVGL's, whose stack may be in PSRAM):
     * the pick changes now, its NVS write and drawing happen on the face's
     * worker. */
    face_pick_t pick = avatar ? FACE_PICK_AVATAR : FACE_PICK_PIXEL;
    taskENTER_CRITICAL(&s_mux);
    s_pick = pick;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "face picked: %s", avatar ? "avatar" : "pixel");
    if (!avatar) {
        muse_ui_face_bg_set(NULL);
    }
    atomic_store(&s_pick_dirty, true);
    face_kick();
}

bool mg_face_size_small(void)
{
    return size_now() == FACE_SIZE_SMALL;
}

void mg_face_size_set(bool small)
{
    /* From any task (Settings runs on LVGL's): the Size changes now, its NVS
     * write and the new drawing happen on the face's worker. */
    face_size_t size = small ? FACE_SIZE_SMALL : FACE_SIZE_LARGE;
    taskENTER_CRITICAL(&s_mux);
    bool changed = s_size != size;
    s_size = size;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "size picked: %s", face_size_name(size));
    if (changed) {
        atomic_store(&s_size_dirty, true);
        face_kick();
    }
}

void mg_face_label(char *out, size_t cap)
{
    taskENTER_CRITICAL(&s_mux);
    snprintf(out, cap, "%s", face_label(s_name));
    taskEXIT_CRITICAL(&s_mux);
}

bool mg_face_console(const char *line)
{
    if (!strcmp(line, "face?")) {
        taskENTER_CRITICAL(&s_mux);
        bool have = s_have;
        unsigned len = have ? s_rec.jpeg_len : 0;
        unsigned tag = have ? face_etag_tag(s_rec.etag) : 0;
        int w = have ? s_rec.width : 0, h = have ? s_rec.height : 0;
        face_pick_t pick = s_pick;
        face_size_t size = s_size;
        bool named = s_name[0] != '\0';
        char last[sizeof(s_last)];
        memcpy(last, s_last, sizeof(last));
        int64_t next = s_sched.next_us;
        unsigned runs = s_w_runs, used = s_w_stack_used, ms = s_w_ms, before = s_w_int_before, low = s_w_int_low,
                 deferred = s_w_deferred;
        taskEXIT_CRITICAL(&s_mux);
        char line_out[400];
        /* int= internal free/largest now; worker: running or not, its runs,
         * the last one's stack use, internal free before and lowest during
         * it, its time, and starts put off (a call, a stream, short RAM). */
        snprintf(line_out, sizeof(line_out),
                 "face pick=%s size=%s shown=%d stored=%d bytes=%u pic=%dx%d etag=#%08x name=%s session=%d "
                 "next=%llds cap=%u int=%u/%u worker=%s runs=%u stack=%u/%u int_before=%u int_low=%u ms=%u "
                 "deferred=%u last=%s",
                 pick == FACE_PICK_AVATAR ? "avatar" : pick == FACE_PICK_PIXEL ? "pixel" : "unset",
                 face_size_name(size), s_shown, have, len, w, h, tag, named ? "known" : "default",
                 noise_ctrl_is_connected(),
                 next ? (long long)((next - now_us()) / 1000000) : -1LL, (unsigned)s_cap, (unsigned)int_free(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 atomic_load(&s_running) ? "running" : "none", runs, used, (unsigned)FACE_WORKER_STACK, before, low,
                 ms, deferred, last[0] ? last : "-");
        ESP_LOGI(TAG, "%s", line_out);
        printf("@%s\n", line_out);
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "face.check")) {
        atomic_store(&s_check_now, true);
        face_kick();
        printf("@face.check queued\n");
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "face.pick=avatar") || !strcmp(line, "face.pick=pixel")) {
        mg_face_pick(line[10] == 'a');
        printf("@%s ok\n", line);
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "face.size=large") || !strcmp(line, "face.size=small")) {
        mg_face_size_set(line[10] == 's');
        printf("@%s ok\n", line);
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "face.erase")) {
        mg_face_forget("console");
        printf("@face.erase ok\n");
        fflush(stdout);
        return true;
    }
    return false;
}

#endif  // CONFIG_MUSE_AVATAR_FACE
