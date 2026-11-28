# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The avatar face (CONFIG_MUSE_AVATAR_FACE, main/mg_face.h): the
face.jpg read over the gadget's own session in ranges, unchanged ETags read
nothing, a store that fails keeps the old picture, the pairing's end erases
it, PNG and WebP are refused, a baseline JPEG decodes (libjpeg on the host
behind TJpgDec's API, so this runs mg_face_jpeg.c's glue, not the ROM
decoder; skipped without libjpeg), the
scrim keeps text at 4.5:1 over a bright busy picture, and nothing of the
picture, its ETag or the Muse's name is ever logged. Synthetic pictures only
(tests/face_vectors, generated: no person's avatar)."""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
FAKES = ROOT / "tests/link_fakes"
FACE_FAKES = ROOT / "tests/face_fakes"
VECTORS = ROOT / "tests/face_vectors"


def function(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise AssertionError(signature)


def libjpeg_flags() -> list[str]:
    """Where libjpeg is (Homebrew or the system), for the decoder behind the fake TJpgDec."""
    for prefix in (os.environ.get("JPEG_PREFIX", ""), "/opt/homebrew", "/usr/local", "/usr"):
        if prefix and Path(prefix, "include/jpeglib.h").exists():
            return [f"-I{prefix}/include", f"-L{prefix}/lib", "-ljpeg"]
    pc = shutil.which("pkg-config")
    if pc:
        r = subprocess.run([pc, "--cflags", "--libs", "libjpeg"], text=True, capture_output=True)
        if r.returncode == 0:
            return r.stdout.split()
    raise unittest.SkipTest("libjpeg not found (brew install jpeg-turbo or libjpeg-dev, or set JPEG_PREFIX)")


def compile_and_run(test: unittest.TestCase, sources: list[Path], args: list[str] = (), extra: list[str] = (),
                    includes: list[Path] = ()) -> str:
    with tempfile.TemporaryDirectory(prefix="face-") as name:
        exe = Path(name) / "a.out"
        cc = shlex.split(os.environ.get("CC", "cc"))
        inc = [f"-I{p}" for p in (*includes, FAKES, MAIN)]
        cmd = [*cc, "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
               *inc, *map(str, sources), *extra, "-lm", "-o", str(exe)]
        r = subprocess.run(cmd, text=True, capture_output=True)
        test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = subprocess.run([str(exe), *args], text=True, capture_output=True)
        test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout


CORE_HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mg_face_core.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static cJSON *cJSON_Parse(const char *s) { return cJSON_ParseWithLength(s, strlen(s)); }

static void hdr(face_head_t *h, const char *n, const char *v) { face_head_header(h, n, strlen(n), v, strlen(v)); }

static void t_ranges(void)
{
    /* A 10,000-byte file read in 3,328-byte ranges: 4 ranges, reassembled exactly. */
    enum { N = 10000 };
    static uint8_t file[N], buf[N];
    for (int i = 0; i < N; i++) file[i] = (uint8_t)(i * 7 + 3);
    face_asm_t a = { .buf = buf, .total = N, .have = 0 };
    uint32_t f, l;
    int ranges = 0;
    char range[48];
    while (face_asm_next(&a, 3328, &f, &l)) {
        face_range_header(range, sizeof(range), f, l);
        if (ranges == 0) CHECK(!strcmp(range, "bytes=0-3327"));
        CHECK(l - f + 1 <= 3328);
        /* a range that isn't the next piece is refused and nothing kept */
        CHECK(!face_asm_put(&a, f + 1, file + f + 1, l - f));
        CHECK(face_asm_put(&a, f, file + f, l - f + 1));
        ranges++;
    }
    CHECK(ranges == 4);
    CHECK(a.have == N && !memcmp(buf, file, N));
    CHECK(!strcmp(range, "bytes=9984-9999"));
    /* nothing past the total */
    CHECK(!face_asm_put(&a, N, file, 1));
    printf("ranges ok\n");
}

static void t_head(void)
{
    face_head_t h;
    face_head_reset(&h);
    h.status = 206;
    hdr(&h, "content-range", "bytes 0-0/22972");
    hdr(&h, "ETag", "\"abc123\"");
    CHECK(h.have_total && h.total == 22972 && h.range_start_ok && h.range_start == 0);
    CHECK(!strcmp(h.etag, "\"abc123\""));
    /* the same ETag: unchanged, nothing read */
    CHECK(face_probe_decide(&h, "\"abc123\"", 22972, FACE_JPEG_MAX) == FACE_PROBE_SAME);
    CHECK(face_probe_decide(&h, "\"old\"", 22972, FACE_JPEG_MAX) == FACE_PROBE_CHANGED);
    CHECK(face_probe_decide(&h, "", 0, FACE_JPEG_MAX) == FACE_PROBE_CHANGED);
    /* a weak ETag can't say unchanged; the size decides */
    face_head_reset(&h);
    h.status = 206;
    hdr(&h, "Content-Range", "bytes 0-0/5000");
    hdr(&h, "etag", "W/\"x\"");
    CHECK(h.etag[0] == '\0');
    CHECK(face_probe_decide(&h, "\"x\"", 5000, FACE_JPEG_MAX) == FACE_PROBE_SAME);
    CHECK(face_probe_decide(&h, "\"x\"", 4000, FACE_JPEG_MAX) == FACE_PROBE_CHANGED);
    /* missing, empty, too large, ignored Range, nothing useful */
    face_head_reset(&h); h.status = 404;
    CHECK(face_probe_decide(&h, NULL, 0, FACE_JPEG_MAX) == FACE_PROBE_ABSENT);
    face_head_reset(&h); h.status = 416; hdr(&h, "Content-Range", "bytes */0");
    CHECK(face_probe_decide(&h, NULL, 0, FACE_JPEG_MAX) == FACE_PROBE_ABSENT);
    face_head_reset(&h); h.status = 206; hdr(&h, "Content-Range", "bytes 0-0/40961");
    CHECK(face_probe_decide(&h, NULL, 0, FACE_JPEG_MAX) == FACE_PROBE_TOO_LARGE);
    face_head_reset(&h); h.status = 200; hdr(&h, "Content-Length", "1234");
    CHECK(face_probe_decide(&h, NULL, 0, FACE_JPEG_MAX) == FACE_PROBE_CHANGED);
    face_head_reset(&h); h.status = 206;
    CHECK(face_probe_decide(&h, NULL, 0, FACE_JPEG_MAX) == FACE_PROBE_FAILED);
    face_head_reset(&h); h.status = -1;
    CHECK(face_probe_decide(&h, NULL, 0, FACE_JPEG_MAX) == FACE_PROBE_FAILED);
    face_head_reset(&h); h.status = 500;
    CHECK(face_probe_decide(&h, NULL, 0, FACE_JPEG_MAX) == FACE_PROBE_FAILED);
    /* malformed headers change nothing */
    face_head_reset(&h);
    hdr(&h, "Content-Range", "bytes 5-2/100");
    hdr(&h, "Content-Range", "bytes 0-0/99999999999");
    hdr(&h, "Content-Length", "12x");
    CHECK(!h.have_total && !h.have_length && !h.range_start_ok);
    printf("head ok\n");
}

static void t_sniff(void)
{
    static const uint8_t png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0 };
    static const uint8_t webp[] = { 'R', 'I', 'F', 'F', 1, 2, 3, 4, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' ' };
    static const uint8_t jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0 };
    CHECK(face_sniff(png, sizeof(png)) == FACE_FMT_PNG);
    CHECK(face_sniff(webp, sizeof(webp)) == FACE_FMT_WEBP);
    CHECK(face_sniff(jpg, sizeof(jpg)) == FACE_FMT_JPEG);
    CHECK(face_sniff(jpg, 2) == FACE_FMT_UNKNOWN);
    int w, h, s, x0, y0;
    bool prog;
    CHECK(!face_jpeg_frame(png, sizeof(png), &w, &h, &prog));
    /* placement: 480 on 480 one to one; 466 on 466; 233 centered on 480; 960 at 1/2 */
    CHECK(face_place(480, 480, 480, 480, &s, &x0, &y0) && s == 1 && x0 == 0 && y0 == 0);
    CHECK(face_place(466, 466, 466, 466, &s, &x0, &y0) && s == 1);
    CHECK(face_place(240, 240, 480, 480, &s, &x0, &y0) && s == 1 && x0 == 120 && y0 == 120);
    CHECK(face_place(960, 960, 480, 480, &s, &x0, &y0) && s == 2 && x0 == 0);
    CHECK(face_place(512, 512, 466, 466, &s, &x0, &y0) && s == 1 && x0 == -23);
    CHECK(!face_place(100, 100, 480, 480, &s, &x0, &y0));
    CHECK(!face_place(8000, 100, 480, 480, &s, &x0, &y0));
    printf("sniff ok\n");
}

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    CHECK(fread(p, 1, *len, f) == *len);
    fclose(f);
    return p;
}

static void t_record(const char *jpeg_path, const char *png_path)
{
    size_t jl, pl;
    uint8_t *jpeg = load(jpeg_path, &jl), *png = load(png_path, &pl);
    int w, h;
    bool prog;
    CHECK(face_jpeg_frame(jpeg, jl, &w, &h, &prog) && w == 480 && h == 480 && !prog);
    size_t cap = sizeof(face_rec_t) + jl;
    uint8_t *blob = malloc(cap);
    size_t n = face_rec_build(blob, cap, jpeg, (uint32_t)jl, w, h, "\"e1\"", "node-a", "Mika");
    CHECK(n == cap);
    face_rec_t r;
    const uint8_t *j;
    CHECK(face_rec_check(blob, n, "node-a", &r, &j) == FACE_REC_OK);
    CHECK(r.jpeg_len == jl && !memcmp(j, jpeg, jl) && !strcmp(r.etag, "\"e1\"") && !strcmp(r.name, "Mika"));
    /* another pairing's node: not ours */
    CHECK(face_rec_check(blob, n, "node-b", &r, &j) == FACE_REC_OTHER_NODE);
    CHECK(face_rec_check(blob, n, "", &r, &j) == FACE_REC_OTHER_NODE);
    /* damage: a flipped bit, a cut, a wrong header */
    blob[sizeof(face_rec_t) + jl / 2] ^= 0x10;
    CHECK(face_rec_check(blob, n, "node-a", &r, &j) == FACE_REC_BAD_CRC);
    blob[sizeof(face_rec_t) + jl / 2] ^= 0x10;
    CHECK(face_rec_check(blob, n - 1, "node-a", &r, &j) == FACE_REC_BAD_LEN);
    CHECK(face_rec_check(blob, 10, "node-a", &r, &j) == FACE_REC_SHORT);
    blob[0] ^= 1;
    CHECK(face_rec_check(blob, n, "node-a", &r, &j) == FACE_REC_BAD_MAGIC);
    /* a PNG never makes a record that checks */
    uint8_t *pb = malloc(sizeof(face_rec_t) + pl);
    size_t pn = face_rec_build(pb, sizeof(face_rec_t) + pl, png, (uint32_t)pl, 64, 64, "", "node-a", "");
    CHECK(pn && face_rec_check(pb, pn, "node-a", &r, &j) == FACE_REC_BAD_JPEG);
    /* too large to build */
    CHECK(face_rec_build(blob, cap, jpeg, FACE_JPEG_MAX + 1, w, h, "", "n", "") == 0);
    free(pb); free(blob); free(jpeg); free(png);
    printf("record ok\n");
}

static void t_sched(void)
{
    face_sched_t s;
    face_sched_init(&s);
    int64_t t = 1000000, until = 0;
    const int64_t S = 1000000;
    CHECK(!face_sched_due(&s, true, t) && !face_sched_wants(&s, t, &until));
    face_sched_on_session(&s, 1, t);              /* an open with nothing owed: no look */
    CHECK(!face_sched_due(&s, true, t));
    face_sched_now(&s, t);                         /* the boot's one look */
    CHECK(face_sched_due(&s, true, t) && !face_sched_due(&s, false, t));   /* never without a session */
    CHECK(face_sched_wants(&s, t, &until) && until == t + 900 * S);        /* the session task, 15 min at most */
    CHECK(!face_sched_wants(&s, t + 900 * S, &until));
    face_sched_on_look(&s, true, t);               /* answered: done, never again by itself */
    CHECK(s.next_us == 0 && !face_sched_wants(&s, t, &until));
    for (int64_t k = 1; k <= 48; k++) {            /* a day of reopens: not one look */
        face_sched_on_session(&s, 1 + (uint64_t)k, t + k * 1800 * S);
        CHECK(!face_sched_due(&s, true, t + k * 1800 * S));
    }
    face_sched_now(&s, t);                         /* face.set / selection / a check: one more */
    face_sched_on_look(&s, true, t);               /* answered, no picture (a 404): done too */
    CHECK(s.next_us == 0 && !face_sched_due(&s, true, t + 60 * S) && !face_sched_due(&s, true, t + 86400 * S));
    face_sched_now(&s, t);
    face_sched_on_session(&s, 100, t);             /* owed and an open: look */
    CHECK(face_sched_due(&s, true, t));
    int waits[6], exp[6] = { 30, 60, 120, 240, 300, 300 };
    for (int i = 0; i < 6; i++) {                  /* failures: 30 s doubling to 5 min, still owed */
        face_sched_on_look(&s, false, t);
        waits[i] = (int)((s.next_us - t) / 1000000);
        CHECK(waits[i] == exp[i]);
    }
    CHECK(s.owed);
    face_sched_on_session(&s, 101, t + 5 * S);     /* an owed look goes at the next open */
    CHECK(face_sched_due(&s, true, t + 5 * S));
    face_sched_on_look(&s, true, t);
    face_sched_now(&s, t);
    face_sched_on_look(&s, false, t);
    CHECK((s.next_us - t) / 1000000 == 30);        /* a success resets the backoff */
    face_sched_on_look(&s, true, t);
    printf("sched ok\n");
}

static void t_pick_name_set(void)
{
    CHECK(face_pick_after_store(FACE_PICK_UNSET) == FACE_PICK_AVATAR);   /* once */
    CHECK(face_pick_after_store(FACE_PICK_PIXEL) == FACE_PICK_PIXEL);    /* never over the user's pick */
    CHECK(face_pick_after_store(FACE_PICK_AVATAR) == FACE_PICK_AVATAR);
    CHECK(face_avatar_shown(FACE_PICK_AVATAR, true) && !face_avatar_shown(FACE_PICK_AVATAR, false));
    CHECK(!face_avatar_shown(FACE_PICK_PIXEL, true) && !face_avatar_shown(FACE_PICK_UNSET, true));
    char name[FACE_NAME_MAX];
    const char *id = "{\"ok\":true,\"result\":{\"name\":\"  Mika\\n the\\tPanda \",\"avatar\":{}}}";
    CHECK(face_identity_name(id, strlen(id), name, sizeof(name)) && !strcmp(name, "Mika the Panda"));
    CHECK(!face_identity_name("{\"ok\":true,\"result\":{}}", 23, name, sizeof(name)) && name[0] == 0);
    CHECK(!face_identity_name("not json", 8, name, sizeof(name)));
    char small[6];
    const char *u = "{\"result\":{\"name\":\"ab\xc3\xa9\xc3\xa9\"}}";   /* ab + two 2-byte chars */
    CHECK(face_identity_name(u, strlen(u), small, sizeof(small)) && !strcmp(small, "ab\xc3\xa9"));
    CHECK(!strcmp(face_label(""), "My Muse") && !strcmp(face_label("Mika"), "Mika"));
    const char *own = "workspace/devices/n1/face.jpg";
    uint32_t len;
    char etag[FACE_ETAG_MAX];
    const char *code, *msg;
    cJSON *p = cJSON_Parse("{\"path\":\"workspace/devices/n1/face.jpg\",\"byte_len\":22972,\"etag\":\"\\\"e\\\"\"}");
    CHECK(face_set_parse(p, own, FACE_JPEG_MAX, &len, etag, sizeof(etag), &code, &msg) && len == 22972 &&
          !strcmp(etag, "\"e\""));
    cJSON_Delete(p);
    p = cJSON_Parse("{\"path\":\"~/workspace/devices/n1/face.jpg\",\"byte_len\":10}");
    CHECK(face_set_parse(p, own, FACE_JPEG_MAX, &len, etag, sizeof(etag), &code, &msg));
    cJSON_Delete(p);
    p = cJSON_Parse("{\"path\":\"workspace/devices/n2/face.jpg\",\"byte_len\":10}");   /* another gadget's */
    CHECK(!face_set_parse(p, own, FACE_JPEG_MAX, &len, etag, sizeof(etag), &code, &msg) && !strcmp(code, "invalid_params"));
    cJSON_Delete(p);
    p = cJSON_Parse("{\"path\":\"workspace/devices/n1/face.jpg\",\"byte_len\":40961}");
    CHECK(!face_set_parse(p, own, FACE_JPEG_MAX, &len, etag, sizeof(etag), &code, &msg) && !strcmp(code, "too_large"));
    cJSON_Delete(p);
    p = cJSON_Parse("{\"path\":\"workspace/devices/n1/face.jpg\"}");
    CHECK(!face_set_parse(p, own, FACE_JPEG_MAX, &len, etag, sizeof(etag), &code, &msg));
    cJSON_Delete(p);
    printf("pick name set ok\n");
}

static void t_scrim(void)
{
    /* The worst case: a white picture with busy bright noise; every band row
     * must keep white and dim (COLOR_DIM, #8B84A8) text at 4.5:1. */
    enum { W = 466, H = 466 };
    static uint16_t px[W * H];
    uint32_t seed = 1;
    for (int i = 0; i < W * H; i++) {
        seed = seed * 1103515245u + 12345u;
        px[i] = (seed >> 16) & 1 ? 0xFFFF : (uint16_t)(0xC000 | ((seed >> 8) & 0x3FFF));
    }
    face_band_t bands[] = { { 40, 70 }, { 90, 170 }, { 380, 430 } };
    uint8_t dark[H];
    face_scrim_rows(H, bands, 3, H / 12, dark);
    CHECK(dark[0] >= FACE_SCRIM_EDGE && dark[H - 1] >= FACE_SCRIM_EDGE);   /* the edge gradients */
    CHECK(dark[H / 2] == 0);                                                /* the middle clear */
    face_bake(px, W, H, dark, true, 10, false);
    int worst_white = 100000, worst_dim = 100000;
    for (int b = 0; b < 3; b++) {
        for (int y = bands[b].y0; y < bands[b].y1; y++) {
            for (int x = 0; x < W; x++) {
                uint16_t v = px[y * W + x];
                if (!v) continue;   /* the ring band */
                int cw = face_contrast_x100(0xFFFFFF, v), cd = face_contrast_x100(0x8B84A8, v);
                if (cw < worst_white) worst_white = cw;
                if (cd < worst_dim) worst_dim = cd;
            }
        }
    }
    printf("worst contrast white %d.%02d dim %d.%02d\n", worst_white / 100, worst_white % 100, worst_dim / 100,
           worst_dim % 100);
    CHECK(worst_white >= 450 && worst_dim >= 450);
    /* the ring band is black (the avatar never enters it); the middle untouched */
    CHECK(px[(H / 2) * W + 2] == 0 && px[2 * W + W / 2] == 0);
    CHECK(px[(H / 2) * W + W / 2] != 0);
    /* swapped byte order bakes the same picture */
    static uint16_t a[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF }, sw[4];
    for (int i = 0; i < 4; i++) sw[i] = (uint16_t)((a[i] >> 8) | (a[i] << 8));
    uint8_t d2[1] = { 128 };
    face_bake(a, 4, 1, d2, false, 0, false);
    face_bake(sw, 4, 1, d2, false, 0, true);
    for (int i = 0; i < 4; i++) CHECK(sw[i] == (uint16_t)((a[i] >> 8) | (a[i] << 8)));
    printf("scrim ok\n");
}

/* A w x h picture on a background: an orange block (a "head") from row top0
 * down, columns x0 .. x1 - 1. */
static void block(uint16_t *px, int w, int h, uint16_t bg, int top0, int x0, int x1)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) px[y * w + x] = (y >= top0 && x >= x0 && x < x1) ? 0xFC00 : bg;
}

static uint32_t work[9 * 400 + 128];

static void t_small(void)
{
    /* The reference panel gives its numbers back; 480 and a round 466 scale them. */
    face_small_geom_t g;
    face_small_geom(360, 360, &g);
    CHECK(g.w == 252 && g.h == 252 && g.x0 == 54 && g.ear_y == 148 && g.ear_max == 40 && g.ear_px == 3);
    face_small_geom(480, 480, &g);
    CHECK(g.w == 336 && g.h == 336 && g.x0 == 72 && g.ear_y == 197 && g.ear_max == 53 && g.ear_px == 4);
    face_small_geom(466, 466, &g);
    CHECK(g.w == 326 && g.h == 326 && g.x0 == 70 && g.ear_y == 192 && g.ear_max == 52 && g.ear_px == 4);
    CHECK(face_small_work_words(480) <= sizeof(work) / sizeof(work[0]));
    CHECK(!strcmp(face_size_name(FACE_SIZE_LARGE), "large") && !strcmp(face_size_name(FACE_SIZE_SMALL), "small"));

    /* 480 x 480 (the ESP-Mosaico): the head from row 60 lands its ear row
     * (small row 42) at y 197, the picture 336 px wide at x 72, black around. */
    enum { W = 480 };
    static uint16_t px[W * W], ref[W * W];
    block(px, W, W, 0x0000, 60, 100, 380);
    face_small_t sm;
    face_small_draw(px, W, W, false, false, work, &sm);
    CHECK(sm.ear == 42 && sm.top == 197 - 42 && sm.field == 0);
    CHECK(px[196 * W + 240] == 0 && px[197 * W + 240] == 0xFC00);     /* the ears' row at y 197 */
    CHECK(px[250 * W + 141] == 0 && px[250 * W + 142] == 0xFC00);     /* x 100 at 0.70: column 70 */
    for (int y = 0; y < W; y++) {
        CHECK(px[y * W + 71] == 0 && px[y * W + 408] == 0);            /* black either side */
        if (y < sm.top) CHECK(px[y * W + 240] == 0);                   /* and above */
    }
    /* A head lower than the ear row's limit is taken at the limit (top at 197 - 53). */
    block(px, W, W, 0x0000, 200, 100, 380);
    face_small_draw(px, W, W, false, false, work, &sm);
    CHECK(sm.ear == 53 && sm.top == 144);
    /* The ear row is found against the picture's own background (the outer
     * ring's median), yet black goes around it. */
    block(px, W, W, 0x2104, 60, 100, 380);
    face_small_draw(px, W, W, false, false, work, &sm);
    CHECK(sm.field == 0x2104 && sm.ear == 42);
    CHECK(px[150 * W + 240] == 0 && px[300 * W + 20] == 0 && px[300 * W + 100] == 0x2104);

    /* An area average, not nearest: 1 px stripes keep their mean and come out
     * in between; a flat field comes back exact. */
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) px[y * W + x] = (x & 1) ? 0xFFFF : 0x0000;
    face_small_draw(px, W, W, false, false, work, &sm);
    long sum = 0;
    int between = 0;
    for (int y = 0; y < 336; y++) {
        int yy = sm.top + y;
        if (yy >= W) break;
        for (int x = 72; x < 408; x++) {
            int g6 = (px[yy * W + x] >> 5) & 63;
            sum += g6;
            between += g6 > 8 && g6 < 55;
        }
    }
    int rows = W - sm.top < 336 ? W - sm.top : 336;
    double mean = (double)sum / (rows * 336.0);
    printf("stripes: mean g %.2f of 63, %d of %d pixels in between\n", mean, between, rows * 336);
    CHECK(mean > 31.0 && mean < 32.5 && between > rows * 336 / 2);

    /* Byte-swapped panels (LVGL's swapped order) come out the same, swapped. */
    block(ref, W, W, 0x0000, 60, 100, 380);
    for (int i = 0; i < W * W; i++) px[i] = (uint16_t)((ref[i] >> 8) | (ref[i] << 8));
    face_small_draw(ref, W, W, false, false, work, &sm);
    face_small_t sw_sm;
    face_small_draw(px, W, W, false, true, work, &sw_sm);
    CHECK(sw_sm.top == sm.top && sw_sm.ear == sm.ear);
    for (int i = 0; i < W * W; i++) CHECK(px[i] == (uint16_t)((ref[i] >> 8) | (ref[i] << 8)));

    /* A round 466 panel: 326 px at x 70, the ear row at y 192, black outside. */
    enum { R = 466 };
    static uint16_t rp[R * R];
    block(rp, R, R, 0x0000, 60, 100, 366);
    face_small_draw(rp, R, R, true, false, work, &sm);
    CHECK(sm.ear == 42 && sm.top == 192 - 42);
    CHECK((rp[191 * R + 233] >> 11) <= 1 && rp[192 * R + 233] == 0xFC00);   /* (row 41 holds 3 % of row 60) */
    for (int y = 0; y < R; y++) CHECK(rp[y * R + 69] == 0 && rp[y * R + 396] == 0);

    /* The scrim's bands: a band over the black above the picture is dropped
     * (its fade would only dim the ears); one reaching into it is kept. */
    face_band_t b[4] = { { 21, 45 }, { 55, 110 }, { 127, 164 }, { 440, 456 } };
    CHECK(face_small_bands(b, 4, 155) == 2 && b[0].y0 == 127 && b[1].y0 == 440);
    face_band_t c[4] = { { 21, 45 }, { 55, 110 }, { 127, 164 }, { 440, 456 } };
    CHECK(face_small_bands(c, 4, 180) == 1 && c[0].y0 == 440);
    printf("small ok\n");
}

int main(int argc, char **argv)
{
    t_ranges();
    t_head();
    t_sniff();
    t_record(argv[1], argv[2]);
    t_sched();
    t_pick_name_set();
    t_scrim();
    t_small();
    (void)argc;
    return 0;
}
"""

JPEG_HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_face_core.h"
#include "mg_face_jpeg.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    CHECK(fread(p, 1, *len, f) == *len);
    fclose(f);
    return p;
}

static void rgb(uint16_t v, int *r, int *g, int *b)
{
    *r = ((v >> 11) & 31) * 255 / 31;
    *g = ((v >> 5) & 63) * 255 / 63;
    *b = (v & 31) * 255 / 31;
}

int main(int argc, char **argv)
{
    (void)argc;
    const char *dir = argv[1];
    char path[512];
    size_t len;
    int w, h, s, r, g, b;
    static uint16_t px[480 * 480];
    /* 480x480 baseline on a 480 panel: one to one; the synthetic head is
     * orange in the middle, the background near black, the rim white. */
    snprintf(path, sizeof(path), "%s/synthetic_480.jpg", dir);
    uint8_t *j = load(path, &len);
    const char *err = mg_face_jpeg_decode(j, len, px, 480, 480, &w, &h, &s);
    CHECK(err == NULL && w == 480 && h == 480 && s == 1);
    rgb(px[230 * 480 + 240], &r, &g, &b);
    CHECK(r > 180 && b < 120);
    rgb(px[10 * 480 + 10], &r, &g, &b);
    CHECK(r < 40 && g < 40 && b < 40);
    rgb(px[230 * 480 + 240 + 160], &r, &g, &b);
    CHECK(r > 200 && g > 200 && b > 200);
    /* ...and on a 466 panel: one to one, the middle kept, 7 px cut each side */
    static uint16_t p466[466 * 466];
    CHECK(mg_face_jpeg_decode(j, len, p466, 466, 466, &w, &h, &s) == NULL && s == 1);
    rgb(p466[223 * 466 + 233], &r, &g, &b);
    CHECK(r > 180);
    /* a cut stream is refused, nothing half drawn kept by the caller */
    CHECK(mg_face_jpeg_decode(j, len / 2, px, 480, 480, &w, &h, &s) != NULL);
    free(j);
    /* 233x233 centered on 480: black around it */
    snprintf(path, sizeof(path), "%s/synthetic_233.jpg", dir);
    j = load(path, &len);
    CHECK(mg_face_jpeg_decode(j, len, px, 480, 480, &w, &h, &s) == NULL && w == 233);
    CHECK(px[5 * 480 + 5] == 0 && px[240 * 480 + 5] == 0);
    rgb(px[235 * 480 + 240], &r, &g, &b);
    CHECK(r > 150);
    free(j);
    /* progressive, PNG and WebP: refused by name */
    snprintf(path, sizeof(path), "%s/synthetic_480_progressive.jpg", dir);
    j = load(path, &len);
    err = mg_face_jpeg_decode(j, len, px, 480, 480, &w, &h, &s);
    CHECK(err && strstr(err, "progressive"));
    free(j);
    snprintf(path, sizeof(path), "%s/synthetic_64.png", dir);
    j = load(path, &len);
    err = mg_face_jpeg_decode(j, len, px, 480, 480, &w, &h, &s);
    CHECK(err && strstr(err, "PNG refused"));
    free(j);
    snprintf(path, sizeof(path), "%s/synthetic_64.webp", dir);
    j = load(path, &len);
    err = mg_face_jpeg_decode(j, len, px, 480, 480, &w, &h, &s);
    CHECK(err && strstr(err, "WebP refused"));
    free(j);
    printf("jpeg ok\n");
    return 0;
}
"""

# rec_write() and rec_erase() from main/mg_face.c over an in-memory NVS that
# can fail a write: the old record stays unless the new one is written whole.
STORE_HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_face_core.h"

typedef int esp_err_t;
typedef uint32_t nvs_handle_t;
typedef struct { size_t used_entries, free_entries, available_entries, total_entries, namespace_count; } nvs_stats_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define FACE_NS "mg_face"
#define FACE_KEY_REC "rec"
#define ESP_LOGW(tag, ...) do { (void)tag; printf("W "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define ESP_LOGI(tag, ...) do { (void)tag; printf("I "); printf(__VA_ARGS__); printf("\n"); } while (0)
static const char *TAG = "mg.face";
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

/* The flash: one record and the partition's entries (NVS keeps the old value
 * until a new one is written whole, so a write needs room beside it). */
static uint8_t *s_rec;
static size_t s_len;
static size_t s_total = 4032, s_free = 4000;
static int s_fail_set;          /* the next set fails half way (a power cut / flash error) */

static const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "error"; }
static esp_err_t nvs_get_stats(const char *part, nvs_stats_t *st)
{
    (void)part;
    memset(st, 0, sizeof(*st));
    st->total_entries = s_total;
    st->free_entries = s_free;
    return ESP_OK;
}
static esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
static void nvs_close(nvs_handle_t h) { (void)h; }
static esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *n)
{
    (void)h; (void)k; (void)v;
    if (!s_len) return ESP_ERR_NVS_NOT_FOUND;
    *n = s_len;
    return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    (void)h; (void)k;
    if (s_len) s_free += face_nvs_entries((uint32_t)s_len);
    free(s_rec); s_rec = NULL; s_len = 0;
    return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n)
{
    (void)h; (void)k;
    size_t need = face_nvs_entries((uint32_t)n);
    if (need > s_free) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;   /* the old value stays */
    if (s_fail_set) { s_fail_set = 0; return ESP_FAIL; }       /* never completes: old kept */
    s_free -= need;
    if (s_len) s_free += face_nvs_entries((uint32_t)s_len);
    free(s_rec);
    s_rec = malloc(n);
    memcpy(s_rec, v, n);
    s_len = n;
    return ESP_OK;
}
static unsigned s_store_refused;
/* Something else's write (a token, a setting): two entries. */
static bool other_write(void) { if (s_free < 2) return false; s_free -= 2; return true; }

@REC_WRITE@

@REC_ERASE@

static uint8_t *pic(size_t n, uint8_t fill) { uint8_t *p = malloc(n); memset(p, fill, n); return p; }

int main(void)
{
    uint8_t *a = pic(20000, 'a'), *b = pic(30000, 'b'), *c = pic(40000, 'c');
    /* the Mosaico's 128 KB NVS: the cap is the full 40 KB */
    CHECK(face_nvs_cap(4032) == FACE_JPEG_MAX);
    CHECK(rec_write(a, 20000) && s_len == 20000 && s_rec[0] == 'a');
    /* a write that fails keeps the old record */
    s_fail_set = 1;
    CHECK(!rec_write(b, 30000));
    CHECK(s_len == 20000 && s_rec[0] == 'a');
    /* a write that succeeds replaces it, beside the old one */
    CHECK(rec_write(b, 30000) && s_len == 30000 && s_rec[0] == 'b');
    /* full NVS: the face is refused (old kept), the reserve stays for tokens and settings */
    size_t before = s_free;
    s_free = 1100;   /* 4032 total: the reserve is 1008 */
    CHECK(face_nvs_plan(4032, 1100, 0, 40000) == FACE_NVS_REFUSE);   /* within the size cap: the reserve refuses it */
    CHECK(!rec_write(c, 40000) && s_len == 30000 && s_rec[0] == 'b' && s_free >= 1008);
    for (int i = 0; i < 100; i++) CHECK(other_write());   /* a token write never fails for the face */
    s_free = before;
    /* the StopWatch's 48 KB NVS (1512 entries): a smaller cap, replaced in place when two don't fit */
    nvs_erase_key(1, FACE_KEY_REC);
    s_total = 1512; s_free = 1512 - 126 - 200;
    uint32_t cap = face_nvs_cap(1512);
    printf("cap 1512 entries: %u bytes\n", (unsigned)cap);
    CHECK(cap >= 12000 && cap < 20000);
    CHECK(rec_write(a, cap) && s_len == cap);
    CHECK(face_nvs_plan(1512, (uint32_t)s_free, (uint32_t)s_len, cap) == FACE_NVS_IN_PLACE);
    CHECK(rec_write(b, cap) && s_rec[0] == 'b');
    CHECK(s_free >= 378);   /* a quarter of 1512 */
    rec_erase("test");
    CHECK(s_len == 0);
    free(a); free(b); free(c); free(s_rec);
    printf("store ok\n");
    return 0;
}
"""


# The Size kept across a reboot (NVS key "size" in mg_face, Large unless Small
# was saved), drawn again on the face task when it changes (never on the
# caller's: Settings runs on LVGL's task), and erased with the face's other
# keys: size_save/size_load/mg_face_size_set/face_size_changed from
# main/mg_face.c and config_erase_face() from main/config_store.c over an
# NVS that survives the "reboot".
SIZE_HARNESS = r"""
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_face_core.h"

typedef int esp_err_t;
typedef uint32_t nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define CONFIG_STORE_FACE_NS "mg_face"
#define FACE_NS CONFIG_STORE_FACE_NS
#define FACE_KEY_SIZE "size"
#define ESP_LOGI(tag, ...) do { (void)tag; printf("I "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define ESP_LOGW(tag, ...) do { (void)tag; printf("W "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static const char *TAG = "mg.face";
typedef int portMUX_TYPE;
static portMUX_TYPE s_mux;
static int s_crit;
#define taskENTER_CRITICAL(m) do { (void)(m); s_crit++; } while (0)
#define taskEXIT_CRITICAL(m) do { (void)(m); s_crit--; } while (0)

/* The flash: mg_face's keys, kept across a reboot. */
static struct { char key[16]; uint8_t v; bool set; } s_nvs[8];
static int find(const char *k)
{
    for (int i = 0; i < 8; i++) if (s_nvs[i].set && !strcmp(s_nvs[i].key, k)) return i;
    return -1;
}
static const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "error"; }
static esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{
    CHECK(!strcmp(ns, "mg_face"));
    (void)mode;
    *h = 1;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t h) { (void)h; }
static esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{
    (void)h;
    int i = find(k);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    *v = s_nvs[i].v;
    return ESP_OK;
}
static esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)
{
    (void)h;
    int i = find(k);
    for (int j = 0; i < 0 && j < 8; j++) if (!s_nvs[j].set) i = j;
    CHECK(i >= 0);
    snprintf(s_nvs[i].key, sizeof(s_nvs[i].key), "%s", k);
    s_nvs[i].v = v;
    s_nvs[i].set = true;
    return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    (void)h;
    int i = find(k);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    s_nvs[i].set = false;
    return ESP_OK;
}

static face_pick_t s_pick;
static bool s_have;
static face_size_t s_size;
static atomic_bool s_size_dirty;
static int s_kicks;   /* the worker started (mg_face.c's face_kick) */
static void face_kick(void) { s_kicks++; }
static int s_renders;
static face_size_t s_rendered;
static void face_show_stored(void) { s_renders++; s_rendered = s_size; }   /* the re-render, at the Size now */

@SIZE_SAVE@
@SIZE_LOAD@
@SIZE_NOW@
@SIZE_CHANGED@
@SIZE_SET@
@SIZE_SMALL@
@ERASE@

/* face_task's step for it (main/mg_face.c), and a reboot (RAM gone, NVS kept). */
/* The worker's pass (face_work_once): what a kick started. */
static void task_tick(void) { if (atomic_exchange(&s_size_dirty, false)) face_size_changed(); }
static void reboot(void) { s_size = (face_size_t)77; atomic_store(&s_size_dirty, false); s_size = size_load(); }

int main(void)
{
    reboot();
    CHECK(s_size == FACE_SIZE_LARGE && !mg_face_size_small());   /* nothing saved: Large */
    s_pick = FACE_PICK_AVATAR;
    s_have = true;
    mg_face_size_set(true);
    CHECK(mg_face_size_small() && s_renders == 0 && find("size") < 0);   /* the face task saves and draws */
    task_tick();
    CHECK(s_renders == 1 && s_rendered == FACE_SIZE_SMALL && find("size") >= 0);
    task_tick();
    mg_face_size_set(true);   /* the same Size: nothing to draw again */
    task_tick();
    CHECK(s_renders == 1);
    reboot();
    CHECK(s_size == FACE_SIZE_SMALL);   /* kept across a reboot */
    mg_face_size_set(false);
    task_tick();
    CHECK(s_renders == 2 && s_rendered == FACE_SIZE_LARGE);
    reboot();
    CHECK(s_size == FACE_SIZE_LARGE);
    /* Pixel Muse showing: saved, nothing drawn (the avatar face draws it when picked) */
    s_pick = FACE_PICK_PIXEL;
    mg_face_size_set(true);
    task_tick();
    CHECK(s_renders == 2);
    reboot();
    CHECK(s_size == FACE_SIZE_SMALL);
    /* no picture stored yet: nothing drawn either */
    s_pick = FACE_PICK_AVATAR;
    s_have = false;
    mg_face_size_set(false);
    task_tick();
    CHECK(s_renders == 2);
    /* erased with the face's other keys (Forget, Reset pairing, a Link setup, face.erase) */
    nvs_set_u8(1, "size", FACE_SIZE_SMALL);
    nvs_set_u8(1, "pick", FACE_PICK_AVATAR);
    CHECK(config_erase_face());
    CHECK(find("size") < 0 && find("pick") < 0);
    reboot();
    CHECK(s_size == FACE_SIZE_LARGE);
    /* anything else stored reads as Large */
    nvs_set_u8(1, "size", 7);
    reboot();
    CHECK(s_size == FACE_SIZE_LARGE);
    CHECK(s_crit == 0);
    printf("size ok\n");
    return 0;
}
"""


class FaceCoreTest(unittest.TestCase):
    def test_core(self) -> None:
        with tempfile.TemporaryDirectory(prefix="face-") as d:
            h = Path(d) / "h.c"
            h.write_text(CORE_HARNESS)
            out = compile_and_run(self, [h, MAIN / "mg_face_core.c", FAKES / "cJSON.c"],
                                  [str(VECTORS / "synthetic_480.jpg"), str(VECTORS / "synthetic_64.png")])
        for line in ("ranges ok", "head ok", "sniff ok", "record ok", "sched ok", "pick name set ok", "scrim ok",
                     "small ok"):
            self.assertIn(line, out)

    def test_jpeg_decode_and_refusals(self) -> None:
        with tempfile.TemporaryDirectory(prefix="face-") as d:
            h = Path(d) / "h.c"
            h.write_text(JPEG_HARNESS)
            out = compile_and_run(self, [h, MAIN / "mg_face_jpeg.c", MAIN / "mg_face_core.c", FAKES / "cJSON.c",
                                         FACE_FAKES / "tjpgd_libjpeg.c"], [str(VECTORS)],
                                  extra=libjpeg_flags(), includes=[FACE_FAKES])
        self.assertIn("jpeg ok", out)


    def test_size_kept_and_drawn_again(self) -> None:
        src = (MAIN / "mg_face.c").read_text()
        store = (MAIN / "config_store.c").read_text()
        harness = SIZE_HARNESS
        for tag, sig in (("@SIZE_SAVE@", "static void size_save("), ("@SIZE_LOAD@", "static face_size_t size_load("),
                         ("@SIZE_NOW@", "static face_size_t size_now("),
                         ("@SIZE_CHANGED@", "static void face_size_changed("),
                         ("@SIZE_SET@", "void mg_face_size_set("), ("@SIZE_SMALL@", "bool mg_face_size_small(")):
            harness = harness.replace(tag, function(src, sig))
        harness = harness.replace("@ERASE@", function(store, "bool config_erase_face("))
        with tempfile.TemporaryDirectory(prefix="face-") as d:
            h = Path(d) / "h.c"
            h.write_text(harness)
            out = compile_and_run(self, [h, MAIN / "mg_face_core.c", FAKES / "cJSON.c"])
        self.assertIn("size ok", out)

    def test_atomic_store(self) -> None:
        src = (MAIN / "mg_face.c").read_text()
        harness = STORE_HARNESS.replace("@REC_WRITE@", function(src, "static bool rec_write(")).replace(
            "@REC_ERASE@", function(src, "static void rec_erase("))
        with tempfile.TemporaryDirectory(prefix="face-") as d:
            h = Path(d) / "h.c"
            h.write_text(harness)
            out = compile_and_run(self, [h, MAIN / "mg_face_core.c", FAKES / "cJSON.c"])
        self.assertIn("store ok", out)


class FaceWiringTest(unittest.TestCase):
    src = (MAIN / "mg_face.c").read_text()

    def test_erased_with_the_pairing(self) -> None:
        store = (MAIN / "config_store.c").read_text()
        for fn in ("bool config_clear_pairing(", "bool config_clear_setup("):
            self.assertIn("config_erase_face()", function(store, fn), fn)
        erase = function(store, "bool config_erase_face(")
        for key in ('"rec"', '"pick"', '"name"', '"size"'):   # every key mg_face.c keeps
            self.assertIn(key, erase)
        self.assertIn("nvs_commit", erase)
        self.assertIn('#define CONFIG_STORE_FACE_NS "mg_face"', (MAIN / "config_store.h").read_text())
        app = (MAIN / "app.c").read_text()
        # passcode_forget(): Reset pairing, Forget, every Link setup, a setup reset.
        self.assertIn("mg_face_forget(why);", function(app, "static bool passcode_forget("))
        self.assertGreaterEqual(app.count('passcode_forget("Link setup")'), 2)
        self.assertIn('passcode_forget("setup reset")', app)
        forget = function(self.src, "bool mg_face_forget(")
        self.assertIn("config_erase_face();", forget)
        # The screen lets go too, and the name and pick go with the picture.
        drop = function(self.src, "static void face_forget_now(")
        self.assertIn("muse_ui_face_bg_set(NULL)", drop)
        self.assertIn("s_name[0] = '\\0'", drop)


    def test_never_logs_bytes_or_names(self) -> None:
        # Every log line's arguments: sizes, counts, statuses, tags; never the
        # picture, a body, an ETag or the Muse's name.
        for m in re.finditer(r"ESP_LOG[EWID]\(TAG,(.*?)\);\n", self.src, re.S):
            args = m.group(1)
            for banned in ("s_name", ", name)", "jpeg,", "blob", "buf,", "piece", "body,", "head.etag)",
                           "etag)", "hdr.etag)", "s_rec.etag)"):
                if banned.endswith(")") and "face_etag_tag(" + banned[:-1] in args:
                    continue
                self.assertNotIn(banned, args.replace("face_etag_tag(etag)", "").replace(
                    "face_etag_tag(head.etag)", "").replace("face_etag_tag(hdr.etag)", ""), m.group(0))
        self.assertNotIn("%s\", s_name", self.src)
        core = (MAIN / "mg_face_core.c").read_text()
        self.assertNotIn("printf(", core.replace("snprintf(", ""))
        self.assertNotIn("ESP_LOG", core)
        # the console line says whether the name is known, never what it is
        console = function(self.src, "bool mg_face_console(")
        self.assertIn('named ? "known" : "default"', console)


if __name__ == "__main__":
    unittest.main()
