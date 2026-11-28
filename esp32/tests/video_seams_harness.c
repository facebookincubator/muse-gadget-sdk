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
 * Band seams (tests/test_video_untrusted.py): muse_video.c's drawing of a
 * two-band picture as the panel driver asks for it (boards/muse_lcd_bands.c:
 * pieces of 8 rows, each filled into a DMA buffer of the panel's width x 8
 * rows), on pictures from the real decoder (muse_vdec.c on tinyh264).
 *
 *   seams PANEL_W PANEL_H[r] UI HQ WIDTH HEIGHT TOP.264 BOTTOM.264|- PHASE EXACT [TOP_ROWS]
 *
 * PANEL_H "466r": a round panel. BOTTOM "-": one band. PHASE 0: the slot as the show task sees it while the
 * bands decode (on_progress); 1: once the decode task has checked them.
 * EXACT 1: each piece's buffer is exactly the driver's (AddressSanitizer
 * reports any byte past it); 0: guard bytes follow it, and the bytes written
 * past the piece's rows and past the buffer are counted. TOP_ROWS: the top
 * band's picture taken as that many of its decoded rows (a band of any
 * height, as VP8's may be: every seam position). The picture drawn is
 * compared with the same rows (the top band's, then the bottom's, to the
 * stream's height) drawn as one band. Prints one JSON line.
 *
 *   conv
 *
 * Each converter (muse_yuv.h) on slices of 1 to 12 rows starting at every
 * row mod 8, into buffers of exactly the slice with guard bytes after: the
 * bytes written past each slice, and the rows that differ from the same rows
 * of the whole picture converted at once.
 *
 *   bands H0 H1 HEIGHT [H0 H1 HEIGHT ...]
 *
 * muse_video_bands_ok() for each triple: one 0/1 a line.
 */
#include "video_slots/harness.c"

#define GUARD 8192

static uint8_t *read_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    *len = (uint32_t)fread(b, 1, (size_t)n, f);
    fclose(f);
    return b;
}

static bool decode_band(muse_vdec_t *d, const char *path, muse_pic_t *out)
{
    uint32_t n = 0;
    uint8_t *b = read_file(path, &n);
    if (!b) {
        return false;
    }
    muse_vdec_result_t rc = muse_vdec_decode(d, b, n, out);
    free(b);
    return rc == MUSE_VDEC_PICTURE;
}

typedef struct {
    long past_slice;   /* bytes written in the buffer past the piece's rows */
    long past_chunk;   /* bytes written past the buffer */
} spill_t;

/* One piece, filled twice over two different backgrounds: a byte that differs
 * from the background in either pass was written. */
static void piece(fill_arg_t *a, int y, int h, size_t row, size_t chunk, bool exact, uint8_t *img, spill_t *sp)
{
    size_t cap = exact ? chunk : chunk + GUARD;
    uint8_t *buf[2];
    for (int pass = 0; pass < 2; pass++) {
        buf[pass] = heap_caps_aligned_alloc(16, cap, MALLOC_CAP_DMA);
        memset(buf[pass], pass ? 0xA5 : 0x5A, cap);
        fill_piece(buf[pass], (int)row, y, h, a);
    }
    if (!exact) {
        for (size_t i = (size_t)h * row; i < cap; i++) {
            if (buf[0][i] != 0x5A || buf[1][i] != 0xA5) {
                if (i < chunk) {
                    sp->past_slice++;
                } else {
                    sp->past_chunk++;
                }
            }
        }
    }
    memcpy(img + (size_t)y * row, buf[0], (size_t)h * row);
    free(buf[0]);
    free(buf[1]);
}

/* The window as muse_lcd_bands_frame sends it: rows of the window's width,
 * PIECE_ROWS a piece or what the panel's buffer holds (whole pairs). */
static void draw(fill_arg_t *a, int panel_w, bool exact, uint8_t *img, spill_t *sp)
{
    s_cov.blk = -1;   /* a new picture (show_task) */
    size_t row = (size_t)a->out_w * 2, chunk = (size_t)panel_w * 8 * 2;
    int rows = PIECE_ROWS;
    if ((size_t)rows * row > chunk) {
        rows = (int)(chunk / row) & ~1;
    }
    for (int y = 0; y < a->out_h; y += rows) {
        piece(a, y, a->out_h - y < rows ? a->out_h - y : rows, row, chunk, exact, img, sp);
    }
}

static muse_board_t s_panel;

/* ---- conv ---------------------------------------------------------------- */

enum { CW = 64, CH = 48 };

/* Output rows [row, row + rows) of a CW x CH test picture by converter k
 * (0 up2, 1 1x, 2 up32, 3 scaled to 80 x 60, 4 1x_hq), as fill() calls it,
 * into out (stride bytes a row). Returns the output width. */
static int convert(int k, const uint8_t *y, const uint8_t *u, const uint8_t *v, int row, int rows, uint8_t *out,
                   int stride)
{
    const int ys = CW, cs = CW / 2;
    switch (k) {
    case 0: {
        int sr = row / 2;
        muse_yuv_up2(y + sr * ys, u + (sr / 2) * cs, v + (sr / 2) * cs, ys, cs, CW, rows, out, stride);
        return 2 * CW;
    }
    case 1:
        muse_yuv_1x(y + row * ys, u + (row / 2) * cs, v + (row / 2) * cs, ys, cs, CW, rows, out, stride);
        return CW;
    case 2:
        muse_yuv_up32(y, u, v, ys, cs, CW, CH, row, rows, out, stride);
        return CW * 3 / 2;
    case 3:
        muse_yuv_scaled(y, u, v, ys, cs, CW, CH, 80, 60, row, rows, out, stride);
        return 80;
    default:
        muse_yuv_1x_hq(y, u, v, ys, cs, CW, CH, row, rows, out, stride);
        return CW;
    }
}

static int conv_main(void)
{
    static uint8_t y[CW * CH], u[CW * CH / 4], v[CW * CH / 4];
    srand(7);
    for (size_t i = 0; i < sizeof(y); i++) y[i] = (uint8_t)rand();
    for (size_t i = 0; i < sizeof(u); i++) u[i] = (uint8_t)rand(), v[i] = (uint8_t)rand();
    static const char *names[] = { "up2", "1x", "up32", "scaled", "1x_hq" };
    static const int out_h[] = { 2 * CH, CH, CH * 3 / 2, 60, CH };
    /* Rows a slice may start on and still match the whole picture: the
     * converter's own unit (fill() keeps seams on it); any start is bounded. */
    static const int unit[] = { 4, 2, 2, 1, 1 };
    long cases = 0, spill = 0, mismatch = 0;
    for (int k = 0; k < 5; k++) {
        int ow = k == 0 ? 2 * CW : k == 2 ? CW * 3 / 2 : k == 3 ? 80 : CW, stride = ow * 2;
        uint8_t *whole = heap_caps_aligned_alloc(16, (size_t)stride * out_h[k], 0);
        convert(k, y, u, v, 0, out_h[k], whole, stride);
        long ks = 0, km = 0;
        for (int start = 0; start < 8; start++) {
            for (int n = 1; n <= 12; n++) {
                for (int base = start; base + n <= out_h[k]; base += 8) {
                    size_t cap = (size_t)stride * n + GUARD;
                    uint8_t *buf[2];
                    for (int pass = 0; pass < 2; pass++) {
                        buf[pass] = heap_caps_aligned_alloc(16, cap, 0);
                        memset(buf[pass], pass ? 0xA5 : 0x5A, cap);
                        convert(k, y, u, v, base, n, buf[pass], stride);
                    }
                    for (size_t i = (size_t)stride * n; i < cap; i++) {
                        ks += buf[0][i] != 0x5A || buf[1][i] != 0xA5;
                    }
                    if (base % unit[k] == 0) {
                        for (int r = 0; r < n; r++) {
                            km += memcmp(buf[0] + (size_t)r * stride, whole + (size_t)(base + r) * stride, stride) != 0;
                        }
                    }
                    free(buf[0]);
                    free(buf[1]);
                    cases++;
                }
            }
        }
        printf("{\"converter\": \"%s\", \"spill\": %ld, \"mismatch_rows\": %ld}\n", names[k], ks, km);
        spill += ks;
        mismatch += km;
        free(whole);
    }
    printf("{\"cases\": %ld, \"spill\": %ld, \"mismatch_rows\": %ld}\n", cases, spill, mismatch);
    return 0;
}

static int bands_main(int argc, char **argv)
{
    for (int i = 2; i + 2 < argc; i += 3) {
        printf("%d\n", muse_video_bands_ok(atoi(argv[i]), atoi(argv[i + 1]), atoi(argv[i + 2])) ? 1 : 0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "conv")) {
        return conv_main();
    }
    if (argc >= 2 && !strcmp(argv[1], "bands")) {
        return bands_main(argc, argv);
    }
    if ((argc != 12 && argc != 13) || strcmp(argv[1], "seams")) {
        fprintf(stderr, "usage: seams PANEL_W PANEL_H UI HQ WIDTH HEIGHT TOP BOTTOM|- PHASE EXACT [TOP_ROWS] | conv | bands ...\n");
        return 2;
    }
    int pw = atoi(argv[2]), ph = atoi(argv[3]), ui = atoi(argv[4]), hq = atoi(argv[5]);
    bool round = strchr(argv[3], 'r') != NULL;   /* "466r": a round panel (one band: the cover) */
    int W = atoi(argv[6]), H = atoi(argv[7]), phase = atoi(argv[10]);
    bool exact = atoi(argv[11]) != 0;
    s_panel = (muse_board_t){ "seams", pw, ph, round, NULL, 2 };
    muse_board = &s_panel;
    s_params.codec = MUSE_VIDEO_CODEC_H264;
    s_params.width = (uint16_t)W;
    s_params.height = (uint16_t)H;
    s_params.tiles = 2;
    s_ui = ui != 0;
    s_hq = hq != 0;

    int bands = strcmp(argv[9], "-") ? 2 : 1;
    s_params.tiles = (uint8_t)bands;
    muse_vdec_t *d0 = muse_vdec_open(MUSE_VIDEO_CODEC_H264, W, H), *d1 = muse_vdec_open(MUSE_VIDEO_CODEC_H264, W, H);
    muse_pic_t top, bot = { 0 };
    if (!d0 || !d1 || !decode_band(d0, argv[8], &top) || (bands == 2 && !decode_band(d1, argv[9], &bot))) {
        printf("{\"decoded\": false}\n");
        return 0;
    }
    if (argc == 13 && atoi(argv[12]) > 0 && atoi(argv[12]) < top.h) {
        top.h = atoi(argv[12]);   /* a band of that height (its decoded rows, the rest unused) */
    }

    /* The slot as on_progress / on_progress1 and then the decode task leave it. */
    static slot_t sl;
    sl.bands = bands;
    sl.p0 = top;
    sl.pic = top.y;
    sl.rows = top.h;
    sl.w = top.w;
    sl.h0 = bands == 2 ? top.h : (int)(H / 2);
    sl.h = top.h;
    if (bands == 2) {
        sl.p1 = bot;
        sl.pic1 = bot.y;
        sl.rows1 = bot.h;
        sl.h = phase ? top.h + bot.h : (H + 15) & ~15;
    }
    sl.done = true;

    /* The show task's fit (show_task). */
    muse_video_fit_in_t fin = {
        .panel_w = pw,
        .panel_h = ph,
        .round = round,
        .x_align = 2,
        .max_src = round ? 240 : MAX_SRC,   /* a round panel's board: the S3's */
        .ui = s_ui,
        .bands = bands,
        .pic_w = W <= sl.w ? W : sl.w,
        .pic_h = H <= sl.h ? H : sl.h,
    };
    muse_video_fit_t fit;
    if (!muse_video_fit(&fin, &fit)) {
        printf("{\"decoded\": true, \"fits\": false}\n");
        return 0;
    }
    if (fit.cover) {
        s_cov.buf = heap_caps_aligned_alloc(16, MUSE_VIDEO_COVER_BUF(240), 0);
    }
    fill_arg_t a = { &sl, fit.vis_w, fit.vis_h, fit.scale, fit.out_w, fit.out_h, fit.x, fit.y, fit };
    size_t img_n = (size_t)fit.out_w * fit.out_h * 2;
    uint8_t *img = calloc(1, img_n), *ref = calloc(1, img_n);
    spill_t sp = { 0, 0 };
    draw(&a, pw, exact, img, &sp);

    /* The same rows as one band: the top band's, then the bottom's. */
    int vis_h = fit.vis_h, n0 = bands == 1 || top.h > vis_h ? vis_h : top.h;
    int ys = (top.w + 15) & ~15, cs = ((top.w / 2) + 15) & ~15;
    uint8_t *py = heap_caps_aligned_alloc(16, (size_t)ys * (vis_h + 2), 0);
    uint8_t *pu = heap_caps_aligned_alloc(16, (size_t)cs * (vis_h / 2 + 2), 0);
    uint8_t *pv = heap_caps_aligned_alloc(16, (size_t)cs * (vis_h / 2 + 2), 0);
    memset(py, 0, (size_t)ys * (vis_h + 2));
    memset(pu, 128, (size_t)cs * (vis_h / 2 + 2));
    memset(pv, 128, (size_t)cs * (vis_h / 2 + 2));
    for (int r = 0; r < vis_h; r++) {
        const muse_pic_t *p = r < n0 ? &top : &bot;
        int br = r < n0 ? r : r - n0;
        if (br < p->h) {
            memcpy(py + (size_t)r * ys, p->y + (size_t)br * p->y_stride, (size_t)top.w);
            if (!(r & 1)) {
                memcpy(pu + (size_t)(r / 2) * cs, p->u + (size_t)(br / 2) * p->c_stride, (size_t)top.w / 2);
                memcpy(pv + (size_t)(r / 2) * cs, p->v + (size_t)(br / 2) * p->c_stride, (size_t)top.w / 2);
            }
        }
    }
    static slot_t one;
    one.bands = 1;
    one.p0 = (muse_pic_t){ py, pu, pv, ys, cs, top.w, vis_h };
    one.pic = py;
    one.rows = vis_h;
    one.w = top.w;
    one.h = vis_h;
    one.done = true;
    fill_arg_t b = a;
    b.s = &one;
    spill_t sp1 = { 0, 0 };
    draw(&b, pw, false, ref, &sp1);
    int mismatch = 0;
    size_t row = (size_t)fit.out_w * 2;
    for (int r = 0; r < fit.out_h; r++) {
        mismatch += memcmp(img + r * row, ref + r * row, row) != 0;
    }
    /* Where the seam falls (muse_video_band_seam), and where it fell before
     * it (the top band's share of the slot's height): for the coverage of the test. */
    /* (With edges extended, for no black bars, the seam is the picture's: its ih rows inside the panel.) */
    int pic_h = fit.extend ? fit.ih : fit.out_h;
    int vis0 = 0, seam = bands == 2 ? muse_video_band_seam(fit.scale, vis_h, pic_h, sl.h0, &vis0) : -1;
    int old_seam = bands == 1 ? -1 : fit.scale == 1 ? n0 : pic_h * sl.h0 / sl.h;
    printf("{\"decoded\": true, \"fits\": true, \"bands\": %d, \"w\": %d, \"h0\": %d, \"h1\": %d, \"slot_h\": %d, "
           "\"scale\": %d, \"vis_h\": %d, \"out_w\": %d, \"out_h\": %d, \"seam\": %d, \"old_seam\": %d, "
           "\"past_slice\": %ld, \"past_chunk\": %ld, \"mismatch_rows\": %d, \"ref_spill\": %ld, \"extend\": %d}\n",
           bands, top.w, top.h, bot.h, sl.h, fit.scale, vis_h, fit.extend ? fit.iw : fit.out_w, pic_h, seam, old_seam,
           sp.past_slice, sp.past_chunk, mismatch, sp1.past_slice + sp1.past_chunk, fit.extend);
    free(s_cov.buf);
    s_cov.buf = NULL;
    free(img);
    free(ref);
    free(py);
    free(pu);
    free(pv);
    muse_vdec_close(d0);
    muse_vdec_close(d1);
    return 0;
}
