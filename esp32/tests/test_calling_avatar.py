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

"""The calling screen with the avatar face (from the Mosaico demo).

With the avatar face picked and stored, its plain picture (the JPEG at the
panel's size: no Size, no scrim) goes full-panel at 40 % behind "Calling…",
with no clock, page dots or scrim; the pixel face or no picture stays black.
Bench/debug images (CONFIG_MUSE_BENCH_IMAGE), or any image while the RAM
console word call.steps=1 is set, add one bottom line: the BLE call start's
step (asked, connecting, live) and the seconds since the tap.

The hand-over (mg_face.c's face_plain_* and face_apply, muse_ui.c's
muse_ui_face_plain_set and muse_ui_face_image) runs on the host against a
harness with synthetic pixels; the screen's wiring is read from muse_call.c.
"""

import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_mg_face import FAKES, MAIN, ROOT, compile_and_run, function

MUSE = ROOT / "components/muse"

HARNESS = r"""
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_face_core.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define CONFIG_MUSE_GADGET_BLE_LIVE_CALL 1

/* ---- muse_ui's side ---- */
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_RGB565 0x12
typedef struct { struct { int magic, cf, w, h, stride; } header; size_t data_size; const uint8_t *data; } lv_image_dsc_t;
static int s_drops, s_frees;
static void lv_image_cache_drop(const void *src) { (void)src; s_drops++; }
static void heap_caps_free(void *p) { if (p) s_frees++; free(p); }
#define FACE_BG_NONE ((uintptr_t)1)
#define FACE_BG_PSRAM
static int s_w = 48, s_h = 40;
@UI_IMAGE@
@UI_PLAIN_SET@

/* ---- mg_face's side ---- */
typedef int portMUX_TYPE;
static portMUX_TYPE s_mux;
#define taskENTER_CRITICAL(m) (void)(m)
#define taskEXIT_CRITICAL(m) (void)(m)
#define FACE_MALLOC(n) malloc(n)
#define FACE_PSRAM_BSS
static struct { int width, height; } s_board = { 48, 40 }, *muse_board = &s_board;
static face_pick_t s_pick;
static bool s_have, s_shown;
static uint16_t *s_bg;   /* what the home's face got (scrim, Size) */
static void muse_ui_face_bg_set(uint16_t *px) { free(s_bg); s_bg = px; }
static bool s_decode_fails;
/* The JPEG decoded: every pixel 0x7bef (a mid grey), no scrim. */
static const char *mg_face_jpeg_decode(const uint8_t *jpeg, size_t len, uint16_t *px, int w, int h, int *iw,
                                       int *ih, int *sc)
{
    (void)jpeg; (void)len;
    if (s_decode_fails) return "bad JPEG";
    for (int i = 0; i < w * h; i++) px[i] = 0x7bef;
    *iw = w; *ih = h; *sc = 1;
    return NULL;
}
@PLAIN@
@APPLY@

/* The home's own picture: dark bands baked in (the scrim), as face_render makes it. */
static uint16_t *home_render(void)
{
    uint16_t *px = malloc(48 * 40 * 2);
    for (int i = 0; i < 48 * 40; i++) px[i] = i < 48 * 8 ? 0x0000 : 0x7bef;
    return px;
}

int main(void)
{
    static const uint8_t jpeg[4] = { 0xff, 0xd8, 0xff, 0xd9 };
    uint32_t v0 = 0, v;
    CHECK(muse_ui_face_image(&v0) == NULL);   /* nothing yet: black */

    /* the avatar face picked and stored: the plain picture, full panel, no scrim */
    s_pick = FACE_PICK_AVATAR;
    s_have = true;
    face_plain_render(jpeg, sizeof(jpeg));
    face_apply(home_render());
    const lv_image_dsc_t *d = muse_ui_face_image(&v);
    CHECK(d && v != v0);
    CHECK(d->header.w == 48 && d->header.h == 40 && d->header.stride == 96 && d->data_size == 48 * 40 * 2);
    CHECK(d->header.cf == LV_COLOR_FORMAT_RGB565 && d->header.magic == LV_IMAGE_HEADER_MAGIC);
    const uint16_t *px = (const uint16_t *)d->data;
    CHECK(px[0] == 0x7bef && px[48 * 4] == 0x7bef && px[48 * 39 + 47] == 0x7bef);   /* the clock's rows too */
    CHECK(s_bg && s_bg[0] == 0x0000);   /* the home's own picture kept its scrim */
    uint32_t v1 = v;
    CHECK(muse_ui_face_image(&v) == d && v == v1);   /* unchanged: same version */

    /* a new picture: a new version, the old pixels freed once */
    int frees = s_frees;
    face_plain_render(jpeg, sizeof(jpeg));
    face_apply(home_render());
    CHECK(muse_ui_face_image(&v) != NULL && v != v1);
    CHECK(s_frees == frees + 1);
    uint32_t v2 = v;

    /* the pixel face: black (none), the old pixels freed */
    s_pick = FACE_PICK_PIXEL;
    face_plain_render(jpeg, sizeof(jpeg));
    face_apply(home_render());
    CHECK(muse_ui_face_image(&v) == NULL && v != v2);
    uint32_t v3 = v;

    /* picked but no picture stored: black */
    s_pick = FACE_PICK_AVATAR;
    s_have = false;
    face_plain_render(jpeg, sizeof(jpeg));
    face_apply(NULL);
    CHECK(muse_ui_face_image(&v) == NULL);

    /* a picture that doesn't decode: black, nothing left over */
    s_have = true;
    s_decode_fails = true;
    face_plain_render(jpeg, sizeof(jpeg));
    CHECK(s_plain == NULL);
    face_apply(NULL);
    CHECK(muse_ui_face_image(&v) == NULL);
    s_decode_fails = false;

    /* stored then forgotten (the store failed): dropped, never handed over */
    face_plain_render(jpeg, sizeof(jpeg));
    face_plain_drop();
    CHECK(s_plain == NULL && muse_ui_face_image(&v) == NULL);
    (void)v3;

    /* two hand-overs before the screen looks: the first is freed unseen */
    frees = s_frees;
    face_plain_render(jpeg, sizeof(jpeg));
    face_apply(home_render());
    face_plain_render(jpeg, sizeof(jpeg));
    face_apply(home_render());
    CHECK(s_frees == frees + 1);
    CHECK(muse_ui_face_image(&v) != NULL);
    muse_ui_face_bg_set(NULL);
    muse_ui_face_plain_set(NULL);
    CHECK(muse_ui_face_image(&v) == NULL);
    printf("plain ok\n");
    return 0;
}
"""

STEPS_WORD_HARNESS = r"""
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define MUSE_CALL_BENCH_IMAGE @BENCH@
static volatile bool s_steps_word;   /* as muse_call.c: off at boot */
@VIS@
@WORD@
int main(void)
{
    printf("boot=%d\n", muse_call_steps_visible(MUSE_CALL_BENCH_IMAGE, s_steps_word));
    bool w = muse_call_steps_word("call.steps=1");
    printf("after_on=%d\n", w && muse_call_steps_visible(MUSE_CALL_BENCH_IMAGE, s_steps_word));
    muse_call_steps_word("call.steps?");
    muse_call_steps_word("call.steps=0");
    printf("after_off=%d\n", muse_call_steps_visible(MUSE_CALL_BENCH_IMAGE, s_steps_word));
    printf("not_word=%d\n", muse_call_steps_word("call.stepsx") || muse_call_steps_word("call.steps=2"));
    return 0;
}
"""

STEP_HARNESS = r"""
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef enum { MUSE_CALL_NONE, MUSE_CALL_ASKED, MUSE_CALL_CONNECTING, MUSE_CALL_LIVE } muse_call_phase_t;
@STEP@
int main(void)
{
    printf("none=%s|\n", muse_call_step_name(MUSE_CALL_NONE, true));
    printf("asked=%s|\n", muse_call_step_name(MUSE_CALL_ASKED, true));
    printf("asked_noapp=%s|\n", muse_call_step_name(MUSE_CALL_ASKED, false));
    printf("connecting=%s|\n", muse_call_step_name(MUSE_CALL_CONNECTING, true));
    printf("live=%s|\n", muse_call_step_name(MUSE_CALL_LIVE, true));
    return 0;
}
"""


def guarded(text: str, guard: str) -> list[tuple[int, str, bool]]:
    """Each line with whether it sits inside #if guard (not its #else)."""
    stack, out = [], []
    for i, ln in enumerate(text.split("\n"), 1):
        s = ln.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", s)
        if m:
            kind, rest = m.group(1), m.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                stack.append([kind != "ifndef" and guard in rest, False])
            elif kind == "elif" and stack:
                stack[-1] = [guard in rest, False]
            elif kind == "else" and stack:
                stack[-1][1] = True
            elif kind == "endif" and stack:
                stack.pop()
            continue
        out.append((i, ln, any(g and not e for g, e in stack)))
    return out


class CallingAvatarTest(unittest.TestCase):
    face = (MAIN / "mg_face.c").read_text()
    ui = (MUSE / "muse_ui.c").read_text()
    call = (MUSE / "muse_call.c").read_text()

    def test_plain_picture_handed_over(self) -> None:
        # The avatar picked and stored: the plain picture; the pixel face or no picture: none (black).
        plain = self.face[self.face.index("static uint16_t *s_plain FACE_PSRAM_BSS;"):]
        plain = plain[:plain.index("#else")]
        ui = self.ui[self.ui.index("static lv_image_dsc_t s_face_plain_dsc FACE_BG_PSRAM;"):]
        ui = ui[:ui.index(function(ui, "void muse_ui_face_plain_set(")) + len(function(ui, "void muse_ui_face_plain_set("))]
        harness = (HARNESS.replace("@UI_IMAGE@", ui).replace("@UI_PLAIN_SET@", "")
                   .replace("@PLAIN@", plain)
                   .replace("@APPLY@", function(self.face, "static void face_apply(")))
        with tempfile.TemporaryDirectory(prefix="calling-") as d:
            h = Path(d) / "h.c"
            h.write_text(harness)
            out = compile_and_run(self, [h, MAIN / "mg_face_core.c", FAKES / "cJSON.c"])
        self.assertIn("plain ok", out)

if __name__ == "__main__":
    unittest.main()
