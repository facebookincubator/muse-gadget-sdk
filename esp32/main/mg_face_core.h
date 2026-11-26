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
 * The avatar face (the face.jpg route), without ESP-IDF: everything mg_face.c decides,
 * for the host tests (tests/test_mg_face.py).
 *
 * The picture is a baseline JPEG on the user's VM at
 * workspace/devices/<node_id>/face.jpg (whoever puts it there calls face.set).
 * The gadget reads it over its own session to Muse: GET /fs/raw with
 * `Range: bytes=0-0` to learn its size and ETag (no HEAD: a phone relay
 * refuses it), then ranges no larger than the session's frame bound with
 * If-Range. Only a picture that decodes whole is stored, and only on flash
 * once it did (the old one stays until then); the face draws over it with a
 * scrim baked under every band with text or controls.
 *
 * Privacy: the picture and the Muse's name are the user's. Neither is ever
 * sent anywhere or logged: logs carry the slot, size and ETag hash only
 * (face_etag_tag). Both are erased with the pairing (Forget, Reset pairing,
 * every Link setup, a setup reset, a factory reset) and when the stored
 * record names another node.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;

#define FACE_PATH_FMT     "workspace/devices/%s/face.jpg"
#define FACE_PATH_MAX     112
#define FACE_NODE_MAX     64
/* What the gadget takes: the record (header + JPEG) must fit
 * NVS beside the old one while it's replaced. */
#define FACE_JPEG_MAX     40960
#define FACE_ETAG_MAX     96
#define FACE_NAME_MAX     48      /* bytes, UTF-8, the label's whole */
#define FACE_NAME_DEFAULT "My Muse"
#define FACE_PIXEL_NAME   "Pixel Muse"
/* GET /identity's reply is about 3 KB (the ESP32's Wi-Fi session reads the
 * same); more than this isn't kept. */
#define FACE_IDENTITY_MAX 8192

/* ---- The probe: GET Range bytes=0-0 ------------------------------------- */

typedef struct {
    int status;               /* HTTP status; 0 none yet; < 0 the request failed */
    bool have_total;          /* Content-Range's total, or a 200's Content-Length */
    uint32_t total;
    uint32_t content_length;
    bool have_length;
    char etag[FACE_ETAG_MAX]; /* strong only ("" for none or W/) */
    bool range_start_ok;      /* a 206's Content-Range starts where asked */
    uint32_t range_start;
} face_head_t;

void face_head_reset(face_head_t *h);
/* One header of the response head (name and value not NUL-terminated). */
void face_head_header(face_head_t *h, const char *name, size_t name_len,
                      const char *value, size_t value_len);

typedef enum {
    FACE_PROBE_FAILED = 0,   /* no answer, or one that says nothing: try again later */
    FACE_PROBE_ABSENT,       /* 404 / 410 / an empty file: there's no picture to read */
    FACE_PROBE_SAME,         /* the ETag we already have: nothing to read */
    FACE_PROBE_CHANGED,      /* read it */
    FACE_PROBE_TOO_LARGE,    /* over FACE_JPEG_MAX: refused, never read */
} face_probe_t;

/* stored_etag: the stored picture's ETag ("" or NULL: none stored). A reply
 * without a strong ETag can't say it's unchanged: it's CHANGED only when no
 * picture is stored or the size differs from stored_len. */
face_probe_t face_probe_decide(const face_head_t *h, const char *stored_etag, uint32_t stored_len,
                               uint32_t cap);
const char *face_probe_name(face_probe_t p);

/* ---- The ranges --------------------------------------------------------- */

typedef struct {
    uint8_t *buf;     /* total bytes */
    uint32_t total;
    uint32_t have;    /* the next range starts here */
} face_asm_t;

/* The next range to ask for (inclusive), at most chunk bytes. False once
 * every byte is in. */
bool face_asm_next(const face_asm_t *a, uint32_t chunk, uint32_t *first, uint32_t *last);
/* A range's reply: its start (from Content-Range), its bytes. False, and
 * nothing kept, when it isn't the next piece or overruns the total. */
bool face_asm_put(face_asm_t *a, uint32_t start, const uint8_t *data, size_t len);
/* "bytes=F-L" into out. */
void face_range_header(char *out, size_t cap, uint32_t first, uint32_t last);

/* ---- The bytes ---------------------------------------------------------- */

typedef enum {
    FACE_FMT_UNKNOWN = 0,
    FACE_FMT_JPEG,
    FACE_FMT_PNG,
    FACE_FMT_WEBP,
    FACE_FMT_GIF,
} face_fmt_t;

face_fmt_t face_sniff(const uint8_t *p, size_t len);
const char *face_fmt_name(face_fmt_t f);
/* A JPEG's frame: true for a baseline (SOF0/SOF1) frame before any scan, with
 * its size; false for progressive, arithmetic, lossless or a damaged header. */
bool face_jpeg_frame(const uint8_t *p, size_t len, int *width, int *height, bool *progressive);

/* The picture's placement on a w x h panel: the JPEG scale (1, 2, 4 or 8) the
 * decoder shrinks it by, and where its top left lands (may be negative: the
 * middle is kept and the rest cut). False if no scale fits it sensibly
 * (smaller than a quarter of the panel, or more than 8x larger). */
bool face_place(int img_w, int img_h, int panel_w, int panel_h, int *scale, int *x0, int *y0);

/* CRC-32 (IEEE), for the stored record. */
uint32_t face_crc32(uint32_t crc, const uint8_t *p, size_t len);

/* A stable short tag for an ETag in logs (never the ETag or the bytes): the
 * low 32 bits of its CRC. */
uint32_t face_etag_tag(const char *etag);

/* ---- The stored record (one NVS blob) ----------------------------------- */

#define FACE_REC_MAGIC   0x4D474641u   /* "MGFA" */
#define FACE_REC_VERSION 1
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t jpeg_len;
    uint32_t crc;                 /* over the JPEG */
    uint16_t width, height;       /* the JPEG's own size */
    char etag[FACE_ETAG_MAX];
    char node[FACE_NODE_MAX];     /* the node it was read for */
    char name[FACE_NAME_MAX];     /* the Muse's name, "" until known */
} face_rec_t;

/* header || JPEG into out (out_cap >= sizeof(face_rec_t) + jpeg_len). The
 * length, 0 if it doesn't fit. */
size_t face_rec_build(uint8_t *out, size_t out_cap, const uint8_t *jpeg, uint32_t jpeg_len,
                      int width, int height, const char *etag, const char *node, const char *name);

typedef enum {
    FACE_REC_OK = 0,
    FACE_REC_SHORT,      /* not even a header */
    FACE_REC_BAD_MAGIC,
    FACE_REC_BAD_LEN,
    FACE_REC_BAD_CRC,
    FACE_REC_BAD_JPEG,   /* not a baseline JPEG */
    FACE_REC_OTHER_NODE, /* read for another node: the pairing changed */
} face_rec_check_t;

/* Checks a stored record against the node now set up (NULL/"" : none, which
 * is OTHER_NODE too). On OK, *hdr and *jpeg point into blob. */
face_rec_check_t face_rec_check(const uint8_t *blob, size_t len, const char *node,
                                face_rec_t *hdr, const uint8_t **jpeg);
const char *face_rec_check_name(face_rec_check_t c);

/* ---- NVS headroom ------------------------------------------------------- */

/* NVS stores 32-byte entries, 126 to a 4 KB page. The face never takes the
 * room pairing, tokens and settings need: after any store at least
 * FACE_NVS_RESERVE of the partition's entries (and at least
 * FACE_NVS_RESERVE_MIN) stay free. */
#define FACE_NVS_RESERVE_PCT 25
#define FACE_NVS_RESERVE_MIN 256   /* entries: 8 KB */

/* Entries a blob of len bytes takes (data, its chunk headers, the index). */
uint32_t face_nvs_entries(uint32_t len);
/* The largest JPEG this partition takes (total_entries: nvs_get_stats'
 * total): one record, beside the old one while it's replaced where there's
 * room, within FACE_JPEG_MAX. */
uint32_t face_nvs_cap(uint32_t total_entries);

typedef enum {
    FACE_NVS_REFUSE = 0,   /* would cut into the reserve: the old record is kept */
    FACE_NVS_BESIDE,       /* written beside the old one, then the old dropped (power-safe) */
    FACE_NVS_IN_PLACE,     /* only fits once the old one is erased first */
} face_nvs_plan_t;

/* free_entries: nvs_get_stats' free now; old_len: the stored record's size
 * (0 none); new_len: the new record's. */
face_nvs_plan_t face_nvs_plan(uint32_t total_entries, uint32_t free_entries, uint32_t old_len,
                              uint32_t new_len);

/* ---- When to look ------------------------------------------------------ */

/* Never a re-check of what the gadget has: one look is owed at boot, at the
 * face's selection, at setup (a restart: the boot's) and on face.set or a
 * console check, and nothing else asks for one. An owed look waits for a
 * session (taken at the next open) and is retried after a failure (30 s
 * doubling to 5 min); one that got an answer, picture or none, is done. While
 * a look is owed (its first 15 min), face_sched_wants() says so: the device
 * session's active task. */
#define FACE_RETRY_FIRST_MS      (30 * 1000)        /* a failed look: 30 s, doubling, */
#define FACE_RETRY_MAX_MS        (5 * 60 * 1000)    /* ...to 5 min */
#define FACE_OWED_HOLD_MS        (15 * 60 * 1000)   /* an owed look holds the session's task at most this */

typedef struct {
    uint64_t session;          /* the session generation seen last (0 none) */
    int64_t next_us;           /* the next look; 0: none planned */
    int retry_ms;              /* the next failure's wait */
    bool owed;                 /* a look is owed (boot, selection, face.set) */
    int64_t owed_us;           /* ...since */
} face_sched_t;

void face_sched_init(face_sched_t *s);
/* A session (re)opened: a look owed goes now; nothing else. */
void face_sched_on_session(face_sched_t *s, uint64_t generation, int64_t now_us);
/* A look ended: ok (an answer that said something, picture or none). */
void face_sched_on_look(face_sched_t *s, bool ok, int64_t now_us);
/* Look now? Only with a session open. */
bool face_sched_due(const face_sched_t *s, bool session_open, int64_t now_us);
/* A look owed now: boot, the face's selection, face.set, a console check. */
void face_sched_now(face_sched_t *s, int64_t now_us);
/* The device session's task: a look owed (its first FACE_OWED_HOLD_MS);
 * *until_us: when that hold ends. */
bool face_sched_wants(const face_sched_t *s, int64_t now_us, int64_t *until_us);

/* ---- The face choice ---------------------------------------------------- */

typedef enum {
    FACE_PICK_UNSET = 0,   /* nobody picked: Pixel Muse shows, the first picture picks Avatar */
    FACE_PICK_PIXEL = 1,
    FACE_PICK_AVATAR = 2,
} face_pick_t;

/* What the pick becomes once a picture is stored: only UNSET changes (once). */
face_pick_t face_pick_after_store(face_pick_t pick);
/* The avatar face shows: picked and a picture stored. */
bool face_avatar_shown(face_pick_t pick, bool have_picture);

/* ---- The Muse's name ----------------------------------- */

/* GET /identity's reply ({"ok":true,"result":{"name":...}}): the name,
 * trimmed to the label (whitespace and control characters out, cut at a UTF-8
 * boundary). False if there's none. */
bool face_identity_name(const char *json, size_t len, char *out, size_t cap);
/* The label to show: the stored name, or "My Muse". */
const char *face_label(const char *stored_name);

/* ---- face.set ----------------------------------------------------------- */

/* face.set {path, byte_len, etag?}: only this gadget's own face.jpg path, a
 * byte_len within the cap. False with an error code and message. */
bool face_set_parse(const cJSON *params, const char *own_path, uint32_t cap,
                    uint32_t *byte_len, char *etag, size_t etag_cap,
                    const char **code, const char **message);

/* ---- The scrim -------------------------------------------------- */

typedef struct {
    int y0, y1;   /* rows [y0, y1) with text or controls */
} face_band_t;

/* Darkness inside a band: white text over a white pixel reaches 4.5:1 at
 * 54 %; the home's dim text (COLOR_DIM #8B84A8) needs 89 %. */
#define FACE_SCRIM_BAND   228    /* of 255 */
/* The edge gradient: about 45 % at the panel's edge band. */
#define FACE_SCRIM_EDGE   115

/* Per-row darkness (0 clear .. 255 black): FACE_SCRIM_BAND inside each band,
 * fading to clear over feather rows either side; the top and bottom edge
 * gradients (FACE_SCRIM_EDGE at the edge, clear a quarter of the height in)
 * under it. */
void face_scrim_rows(int height, const face_band_t *bands, int n, int feather, uint8_t *dark);

/* Darkens an RGB565 picture (byte_swapped: LVGL's swapped order) row by row,
 * and blacks out the mic ring band (the avatar never enters it): a
 * round panel's outer ring_px, a rectangle's edges. */
void face_bake(uint16_t *px, int width, int height, const uint8_t *dark, bool round, int ring_px,
               bool byte_swapped);

/* WCAG contrast of a text colour (0xRRGGBB) over an RGB565 pixel, x100. */
int face_contrast_x100(uint32_t text_rgb, uint16_t bg565);

/* ---- Size: Large / Small (Settings > Watch face) ------------------ */

typedef enum {
    FACE_SIZE_LARGE = 0,   /* the picture fills the panel (head and shoulders): the default */
    FACE_SIZE_SMALL = 1,   /* the picture at 0.70, centered across, its ears below the clock */
} face_size_t;

const char *face_size_name(face_size_t size);   /* "large", "small" */

/* Small as designed on a 360 px round reference panel (the ears about 10 px
 * below the clock's 10:10, the picture at 0.70x): on that panel the picture at 7/10 (252 px, an area average), centered across, its
 * ear row (the first row with more than 3 pixels off the picture's own
 * background) at y 148, running off the bottom; an ear row lower than 40
 * small rows is taken as 40. A firmware constant, not a setting. Scaled
 * here from the panel's size (480: 336 px, ear row at y 197). */
#define FACE_SMALL_NUM     7
#define FACE_SMALL_DEN     10
#define FACE_SMALL_REF     360   /* the reference panel, px */
#define FACE_SMALL_REF_EAR 148   /* ...its ear row's y */
#define FACE_SMALL_REF_W   252   /* ...its small picture's side */
#define FACE_SMALL_REF_MAX 40    /* ...the lowest ear row taken, small rows */
#define FACE_SMALL_REF_PX  3     /* ...more pixels than this off the background make the ear row */

typedef struct {
    int w, h;      /* the small picture: 7/10 of the panel's width and height */
    int x0;        /* its column 0 on the panel (centered) */
    int ear_y;     /* the panel row its ear row lands on */
    int ear_max;   /* the lowest ear row taken, small rows */
    int ear_px;    /* more pixels than this off the background make a row the ear row */
} face_small_geom_t;

void face_small_geom(int panel_w, int panel_h, face_small_geom_t *g);

/* The 32-bit words of work face_small_draw() takes for a panel this wide
 * (the area average's sums and the background's histograms). */
size_t face_small_work_words(int panel_w);

typedef struct {
    int top;          /* the panel row of the small picture's row 0 (ear_y - ear) */
    int ear;          /* its ear row, 0 .. ear_max */
    uint16_t field;   /* the picture's background: the per-channel median of its outer ring */
} face_small_t;

/* Redraws px, a w x h panel picture as decoded for Large (the scrim not yet
 * baked), as Small in place: the whole picture area-averaged to 7/10, its
 * ear row found against the background, placed (centered across, the ear
 * row at ear_y, what falls below the panel cut), black around it. work:
 * face_small_work_words(w) words (PSRAM is fine: once per picture or size
 * change, never per frame). */
void face_small_draw(uint16_t *px, int w, int h, bool round, bool byte_swapped, uint32_t *work,
                     face_small_t *out);

/* The scrim's bands for a Small picture whose row 0 lands at top: only those
 * that reach into the picture (a band over the black above it needs no
 * scrim, and its fade would only dim the ears). Returns the bands kept. */
int face_small_bands(face_band_t *bands, int n, int top);

#ifdef __cplusplus
}
#endif
