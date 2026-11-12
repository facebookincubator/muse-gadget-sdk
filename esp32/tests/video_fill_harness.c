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
 * The video drawing path on the host: fill() and place() from
 * components/muse/muse_video.c and the converters in muse_yuv.c, on
 * pictures laid out as the decoders hand them over (muse_vdec.c: packed
 * macroblock-aligned planes), drawn in pieces cut the way
 * boards/muse_lcd_bands.c cuts them for the ESP-Mosaico's 480 px panel.
 *
 * Every piece must be written exactly: each of its rows, and not one byte
 * before or after it in the DMA buffer (guard bytes checked after each
 * piece). With MUSE_FILL_EXACT=1 in the environment each piece is an
 * allocation of exactly its size instead, for AddressSanitizer; pictures
 * are exact-size allocations either way, so a read past one is caught.
 *
 * Two bands are drawn as the show task draws them while they decode, with
 * any band heights (not only the ones the decode task's check accepts), and
 * the protocol's layouts must look exactly like the same picture in one band.
 */
#include "muse_video.c"

#include <stdio.h>
#include <stdlib.h>

const muse_board_t *muse_board;
const muse_board_t *muse_board_get(void)
{
    return muse_board;
}
void muse_state_poke(void) {}
void muse_state_set_asleep(bool asleep)
{
    (void)asleep;
}
bool muse_state_asleep(void)
{
    return false;
}

/* The decoders aren't run: fill() only sees their pictures. */
muse_vdec_t *muse_vdec_open(uint8_t codec, int max_w, int max_h)
{
    (void)codec;
    (void)max_w;
    (void)max_h;
    return NULL;
}
bool muse_vdec_may_realloc(const muse_vdec_t *d, const uint8_t *buf, uint32_t len)
{
    (void)d;
    (void)buf;
    (void)len;
    return false;
}
void muse_vdec_close(muse_vdec_t *d)
{
    (void)d;
}
bool muse_vdec_has(uint8_t codec)
{
    return codec == MUSE_VIDEO_CODEC_H264;
}
void muse_vdec_set_progress(muse_vdec_t *d, muse_vdec_progress_t cb, void *ctx)
{
    (void)d;
    (void)cb;
    (void)ctx;
}
muse_vdec_result_t muse_vdec_decode(muse_vdec_t *d, const uint8_t *buf, uint32_t len, muse_pic_t *pic)
{
    (void)d;
    (void)buf;
    (void)len;
    (void)pic;
    return MUSE_VDEC_ERROR;
}

#define PANEL 480
#define CHUNK_BYTES (PANEL * 8 * 2) /* board_esp_mosaico.c: LCD_CHUNK_BYTES */
#define PRE 64
#define GUARD 4096
#define UNSET 0x5A
#define FENCE 0xA5

static int fails, frames, pieces;
static bool exact;
static const char *what;

static void fail(const char *fmt, int a, int b, int c)
{
    if (fails++ < 20) {
        printf("FAIL %s: ", what);
        printf(fmt, a, b, c);
        printf("\n");
    }
}

/* A decoder's picture: exactly w x h luma and two (w / 2) x (h / 2) chroma
 * planes in one allocation, as muse_vdec.c's h264_pic() lays them out. */
typedef struct {
    uint8_t *buf;
    muse_pic_t p;
} pic_t;

static uint32_t s_seed = 1;
static uint8_t rnd(void)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return (uint8_t)(s_seed >> 24);
}

static pic_t make_pic(int w, int h)
{
    size_t n = (size_t)w * h + 2 * (size_t)(w / 2) * (h / 2);
    pic_t pc = { malloc(n ? n : 1), { 0 } };
    for (size_t i = 0; i < n; i++) {
        pc.buf[i] = rnd();
    }
    pc.p = (muse_pic_t){ pc.buf, pc.buf + (size_t)w * h, pc.buf + (size_t)w * h + (size_t)(w / 2) * (h / 2), w, w / 2,
                         w, h };
    return pc;
}

/* Two bands stacked into the one-band picture they code. */
static pic_t stack(const pic_t *a, const pic_t *b)
{
    int w = a->p.w, h0 = a->p.h, h1 = b->p.h;
    pic_t pc = make_pic(w, h0 + h1);
    memcpy((uint8_t *)pc.p.y, a->p.y, (size_t)w * h0);
    memcpy((uint8_t *)pc.p.y + (size_t)w * h0, b->p.y, (size_t)w * h1);
    memcpy((uint8_t *)pc.p.u, a->p.u, (size_t)(w / 2) * (h0 / 2));
    memcpy((uint8_t *)pc.p.u + (size_t)(w / 2) * (h0 / 2), b->p.u, (size_t)(w / 2) * (h1 / 2));
    memcpy((uint8_t *)pc.p.v, a->p.v, (size_t)(w / 2) * (h0 / 2));
    memcpy((uint8_t *)pc.p.v + (size_t)(w / 2) * (h0 / 2), b->p.v, (size_t)(w / 2) * (h1 / 2));
    return pc;
}

/* The whole frame as drawn, for comparisons (NULL: not kept). */
static uint8_t *s_frame;
static int s_frame_w, s_frame_h;

/* boards/muse_lcd_bands.c's send_frame(): pieces of `rows` rows (fewer if
 * the DMA buffer can't hold them), each filled straight into a DMA buffer. */
static void fake_draw_frame(int x1, int y1, int x2, int y2, int rows,
                            void (*fill_fn)(uint8_t *dst, int stride, int row, int rows, void *arg), void *arg)
{
    size_t row = (size_t)(x2 - x1) * 2;
    if ((size_t)rows * row > CHUNK_BYTES) {
        rows = (int)(CHUNK_BYTES / row) & ~1;
    }
    s_frame_w = x2 - x1;
    s_frame_h = y2 - y1;
    for (int y = y1; y < y2; y += rows) {
        int h = y2 - y < rows ? y2 - y : rows;
        size_t slice = (size_t)h * row;
        uint8_t *buf, *dst;
        if (exact) {
            buf = dst = malloc(slice);
            memset(dst, UNSET, slice);
        } else {
            buf = malloc(PRE + CHUNK_BYTES + GUARD);
            dst = buf + PRE;
            memset(buf, FENCE, PRE + CHUNK_BYTES + GUARD);
            memset(dst, UNSET, slice);
        }
        fill_fn(dst, (int)row, y - y1, h, arg);
        pieces++;
        if (!exact) {
            for (int i = 0; i < PRE; i++) {
                if (buf[i] != FENCE) {
                    fail("piece at row %d (%d rows): byte %d before it written", y - y1, h, PRE - i);
                    break;
                }
            }
            for (size_t i = slice; i < CHUNK_BYTES + GUARD; i++) {
                if (dst[i] != FENCE) {
                    fail("piece at row %d (%d rows): %d bytes past it written", y - y1, h, (int)(i - slice) + 1);
                    break;
                }
            }
        }
        for (int r = 0; r < h; r++) {
            const uint8_t *p = dst + (size_t)r * row;
            size_t k = 0;
            while (k < row && p[k] == UNSET) {
                k++;
            }
            if (k == row) {
                fail("piece at row %d (%d rows): its row %d never written", y - y1, h, r);
                break;
            }
        }
        if (s_frame) {
            memcpy(s_frame + (size_t)(y - y1) * row, dst, slice);
        }
        free(buf);
    }
}

static const muse_board_t s_panel = {
    .name = "host", .width = PANEL, .height = PANEL, .draw_frame = fake_draw_frame,
};

/* One frame through place() and fill(), as the show task draws it while the
 * bands decode: the slot's h is the stream's height rounded to a macroblock
 * and h0 the top band's (on_progress()); bot NULL for one band. Returns the
 * scale used, -1 when place() shows nothing. */
static int draw(int W, int H, const pic_t *top, const pic_t *bot, uint8_t *frame)
{
    s_params.width = (uint16_t)W;
    s_params.height = (uint16_t)H;
    s_params.tiles = bot ? 2 : 1;
    slot_t *sl = &s_slot[0];
    memset(sl, 0, sizeof(*sl));
    sl->bands = bot ? 2 : 1;
    sl->w = top->p.w;
    sl->h = bot ? (H + 15) & ~15 : top->p.h;
    sl->h0 = top->p.h;
    sl->p0 = top->p;
    sl->pic = top->p.y;
    sl->rows = top->p.h;
    if (bot) {
        sl->p1 = bot->p;
        sl->pic1 = bot->p.y;
        sl->rows1 = bot->p.h;
    }
    sl->done = true;
    fill_arg_t a;
    int x1, y1;
    if (!place(sl, &a, &x1, &y1)) {
        return -1;
    }
    s_frame = frame;
    muse_board->draw_frame(x1, y1, x1 + a.out_w, y1 + a.out_h, PIECE_ROWS, fill, &a);
    s_frame = NULL;
    frames++;
    return a.scale;
}

/* The converters on their own: exactly rows rows written for every count,
 * from source planes holding only the rows those rows come from, and each
 * row the same as when the count is whole row pairs (whole patterns at 3/2)
 * from a taller picture: a row left out or drawn wrong fails. */
static void converters(void)
{
    enum { W = 64, OS = 2 * W * 2, TALL = 32 };
    static uint8_t out[PRE + 16 * OS + GUARD], ref[16 * OS];
    pic_t tall = make_pic(W, TALL);
    for (int conv = 0; conv < 3; conv++) {
        /* Bytes each output row gets: 2x doubles the width, 3/2 halves again. */
        size_t row_bytes = conv == 0 ? 2 * W * 2 : conv == 1 ? W * 2 : W * 3 / 2 * 2;
        for (int rows = 1; rows <= 13; rows++) {
            int src_rows = conv == 0 ? (rows + 1) / 2 : conv == 1 ? rows : muse_yuv_up32_src_rows(1 << 20, rows);
            int h = (src_rows + 1) & ~1;
            pic_t pc = make_pic(W, h);
            /* The same pixels as the tall picture's first rows. */
            memcpy((uint8_t *)pc.p.y, tall.p.y, (size_t)W * h);
            memcpy((uint8_t *)pc.p.u, tall.p.u, (size_t)(W / 2) * (h / 2));
            memcpy((uint8_t *)pc.p.v, tall.p.v, (size_t)(W / 2) * (h / 2));
            pc.p.h = src_rows;
            memset(out, FENCE, sizeof(out));
            uint8_t *dst = out + PRE;
            memset(dst, UNSET, (size_t)rows * OS);
            memset(ref, UNSET, sizeof(ref));
            what = conv == 0 ? "muse_yuv_up2" : conv == 1 ? "muse_yuv_1x" : "muse_yuv_up32";
            if (conv == 0) {
                muse_yuv_up2(pc.p.y, pc.p.u, pc.p.v, W, W / 2, W, rows, dst, OS);
                muse_yuv_up2(tall.p.y, tall.p.u, tall.p.v, W, W / 2, W, (rows + 3) & ~3, ref, OS);
            } else if (conv == 1) {
                muse_yuv_1x(pc.p.y, pc.p.u, pc.p.v, W, W / 2, W, rows, dst, OS);
                muse_yuv_1x(tall.p.y, tall.p.u, tall.p.v, W, W / 2, W, (rows + 1) & ~1, ref, OS);
            } else {
                muse_yuv_up32(pc.p.y, pc.p.u, pc.p.v, W, W / 2, W, src_rows, 0, rows, dst, OS);
                muse_yuv_up32(tall.p.y, tall.p.u, tall.p.v, W, W / 2, W, TALL, 0, (rows + 1) & ~1, ref, OS);
            }
            for (int i = 0; i < PRE; i++) {
                if (out[i] != FENCE) {
                    fail("%d rows: byte %d before them written%.0d", rows, PRE - i, 0);
                    break;
                }
            }
            for (size_t i = (size_t)rows * OS; i < 16 * OS + GUARD; i++) {
                if (dst[i] != FENCE) {
                    fail("%d rows: %d bytes past them written%.0d", rows, (int)(i - (size_t)rows * OS) + 1, 0);
                    break;
                }
            }
            for (int r = 0; r < rows; r++) {
                if (memcmp(dst + (size_t)r * OS, ref + (size_t)r * OS, row_bytes) != 0) {
                    fail("%d rows: row %d not the picture's%.0d", rows, r, 0);
                    break;
                }
            }
            free(pc.buf);
        }
    }
    free(tall.buf);
}

/* The protocol's layouts look the same in two bands as in one. */
static void compare(int W, int H, const pic_t *top, const pic_t *bot)
{
    static uint8_t one[PANEL * PANEL * 2], two[PANEL * PANEL * 2];
    pic_t whole = stack(top, bot);
    int s1 = draw(W, H, &whole, NULL, one);
    int w1 = s_frame_w, h1 = s_frame_h;
    int s2 = draw(W, H, top, bot, two);
    if (s1 != s2 || w1 != s_frame_w || h1 != s_frame_h) {
        fail("%dx%d: drawn differently in two bands (scale %d)", W, H, s2);
    } else if (s2 >= 1 && memcmp(one, two, (size_t)w1 * h1 * 2) != 0) {
        fail("%dx%d at scale %d: two bands differ from one", W, H, s2);
    }
    free(whole.buf);
}

int main(void)
{
    const char *e = getenv("MUSE_FILL_EXACT");
    exact = e && *e == '1';
    muse_board = &s_panel;
    converters();

    /* 240x68 in bands of 32 and 48 (2x, the seam inside a piece), 32 and 36
     * (cropped), and its aligned neighbour 240x64 in 32 and 32. */
    static const int cases[][4] = {
        { 240, 68, 32, 48 }, { 240, 68, 32, 36 }, { 240, 64, 32, 32 }, { 320, 320, 18, 302 }, { 320, 320, 192, 128 },
        { 480, 480, 288, 192 }, { 480, 100, 64, 48 }, { 400, 300, 176, 128 }, { 480, 480, 0, 480 },
        { 480, 480, 480, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        what = "case";
        pic_t t = make_pic((cases[i][0] + 15) & ~15, cases[i][2]), b = make_pic((cases[i][0] + 15) & ~15, cases[i][3]);
        draw(cases[i][0], cases[i][1], &t, &b, NULL);
        free(t.buf);
        free(b.buf);
    }

    /* Every seam position at every scale: 2x (240), 3/2 (320), one to one
     * (480) and scaled (400), any top band height, the bottom band what's
     * left, a whole macroblock more, or a full picture. */
    static const int widths[] = { 240, 320, 480, 400, 232 };
    static const int tall[] = { 160, 240, 320, 400, 480 };
    for (size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
        int W = widths[wi], pw = (W + 15) & ~15;
        for (int hi = 0; hi < 29 + 5; hi++) {
            int H = hi < 29 ? 16 + 4 * hi : tall[hi - 29], H16 = (H + 15) & ~15;
            /* Every top band height up to 48 rows (every seam position mod 8
             * at every scale), then whole macroblocks, and the last rows. */
            for (int h0 = 1; h0 <= H16; h0 = h0 < 48 || H16 - h0 <= 8 ? h0 + 1 : H16 - h0 > 24 ? (h0 + 16) & ~15 : H16 - 8) {
                int rest[3] = { H16 - h0, H16 - h0 + 16, H16 };
                for (int k = 0; k < 3; k++) {
                    if (rest[k] <= 0 || (k && rest[k] == rest[k - 1])) {
                        continue;
                    }
                    what = "sweep";
                    pic_t t = make_pic(pw, h0), b = make_pic(pw, rest[k]);
                    draw(W, H, &t, &b, NULL);
                    if (k == 0 && h0 % 16 == 0 && h0 < H) {
                        what = "layout";
                        compare(W, H, &t, &b);
                    }
                    free(t.buf);
                    free(b.buf);
                }
            }
        }
    }

    /* The decode task's check: only the protocol's layouts. */
    what = "bands_fit";
    s_params.height = 68;
    if (bands_fit(32, 48) == false || bands_fit(48, 32) == false) {
        fail("68 rows: a coded layout refused%.0d%.0d%.0d", 0, 0, 0);
    }
    if (bands_fit(18, 62) || bands_fit(80, 16) || bands_fit(64, 32) || bands_fit(0, 80) || bands_fit(32, 40)) {
        fail("68 rows: a layout the protocol doesn't code accepted%.0d%.0d%.0d", 0, 0, 0);
    }
    s_params.height = 480;
    if (!bands_fit(288, 192) || bands_fit(480, 16) || bands_fit(18, 462) || bands_fit(288, 208)) {
        fail("480 rows: wrong verdict%.0d%.0d%.0d", 0, 0, 0);
    }

    printf("%d frames, %d pieces%s: %s\n", frames, pieces, exact ? " (exact)" : "", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
