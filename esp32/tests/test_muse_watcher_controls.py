# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise wheel click/hold decisions and retained reply navigation."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


class WatcherControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        (out / "freertos").mkdir()
        (out / "esp_err.h").write_text("typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 1\n")
        (out / "esp_heap_caps.h").write_text("#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n#define heap_caps_malloc(n, caps) malloc(n)\n")
        (out / "freertos/FreeRTOS.h").write_text("#define portMAX_DELAY 0\n")
        (out / "freertos/semphr.h").write_text("typedef int SemaphoreHandle_t;\n#define xSemaphoreCreateMutex() 1\n#define xSemaphoreTake(lock, wait) ((void)0)\n#define xSemaphoreGive(lock) ((void)0)\n")
        source = (MUSE / "muse_input.c").read_text()
        # The function contains conditional camera blocks; take through its final brace.
        start = source.index("static void watcher_buttons(")
        end = source.index("\n/* A pairing prompt", start)
        watcher = source[start:end].rsplit("\n#endif", 1)[0]
        harness = out / "controls.c"
        harness.write_text(r'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_wheel.h"
#include "muse_review.h"
#include "muse_chat_priv.h"
static int cols = 10, lines = 2;
void muse_state_page(bool cjk, int *c, int *l) { (void)cjk; *c = cols; *l = lines; }
static void click(void) {
    muse_wheel_t s = {0};
    assert(muse_wheel_update(&s, true, 100, false, false) == MUSE_WHEEL_NONE);
    assert(muse_wheel_update(&s, true, 399, false, false) == MUSE_WHEEL_NONE);
    assert(muse_wheel_update(&s, false, 399, false, false) == MUSE_WHEEL_SLEEP);
    assert(!s.recording);
    assert(muse_wheel_update(&s, false, 400, true, false) == MUSE_WHEEL_NONE);
    assert(muse_wheel_update(&s, true, 500, true, false) == MUSE_WHEEL_WAKE);
    assert(muse_wheel_update(&s, false, 550, false, false) == MUSE_WHEEL_NONE);
    assert(muse_wheel_update(&s, true, 600, false, false) == MUSE_WHEEL_NONE);
    assert(muse_wheel_update(&s, true, 900, false, false) == MUSE_WHEEL_TALK_DOWN);
    assert(muse_wheel_update(&s, true, 1200, false, false) == MUSE_WHEEL_NONE);
    assert(muse_wheel_update(&s, false, 1300, false, false) == MUSE_WHEEL_TALK_UP);
}
static void pairing(void) {
    muse_wheel_t s = {0};
    assert(muse_wheel_update(&s, true, 0, false, true) == MUSE_WHEEL_NONE);
    assert(muse_wheel_update(&s, true, 1000, false, false) == MUSE_WHEEL_NONE);
    assert(muse_wheel_update(&s, false, 1100, false, false) == MUSE_WHEEL_NONE);
    assert(muse_wheel_update(&s, true, UINT32_MAX - 100, true, false) == MUSE_WHEEL_WAKE);
    assert(muse_wheel_update(&s, true, 199, false, false) == MUSE_WHEEL_TALK_DOWN);
    assert(muse_wheel_update(&s, false, 200, false, false) == MUSE_WHEEL_TALK_UP);
}
static void pages(void) {
    char text[256];
    const char *reply = "the quick brown fox jumps over the lazy dog";
    assert(muse_hatch_caption_page(reply, 0, text, sizeof(text)) == 4);
    assert(!strcmp(text, "the quick\nbrown fox"));
    assert(muse_hatch_caption_page(reply, 1, text, sizeof(text)) == 4);
    assert(!strcmp(text, "brown fox\njumps over"));
    assert(muse_hatch_caption_page(reply, 999, text, sizeof(text)) == 4);
    assert(!strcmp(text, "the lazy\ndog"));
    assert(muse_hatch_caption_page(reply, -99, text, sizeof(text)) == 4);
    assert(!strcmp(text, "the quick\nbrown fox"));
    assert(!muse_hatch_caption_page("", 0, text, sizeof(text)) && !text[0]);
    assert(!muse_hatch_caption_page(reply, 0, text, 0));
    cols = 4; lines = 2;
    assert(muse_hatch_caption_page("一二三四五，六七八九十", 0, text, sizeof(text)) == 2);
    assert(!strcmp(text, "一二三四\n五，六七"));
    assert(muse_hatch_caption_page("一二三四五，六七八九十", 1, text, sizeof(text)) == 2);
    assert(!strcmp(text, "五，六七\n八九十"));
    cols = 10; lines = 1;
    assert(muse_hatch_caption_page(reply, 2, text, sizeof(text)) == 5);
    assert(!strcmp(text, "jumps over"));
}
static void retained(void) {
    cols = 10; lines = 2;
    assert(!muse_review_step(1));
    assert(muse_review_init() == ESP_OK);
    assert(!muse_review_step(1));
    muse_review_store(1, "the quick brown fox jumps over the lazy dog");
    assert(muse_review_step(-1));
    char text[256]; int page, total;
    assert(muse_review_page(text, sizeof(text), &page, &total));
    assert(page == 1 && total == 4);
    assert(muse_review_step(1));
    assert(muse_review_page(text, sizeof(text), &page, &total));
    assert(page == 2 && !strcmp(text, "brown fox\njumps over"));
    for (int i = 0; i < 10; i++) muse_review_step(-1);
    assert(muse_review_page(text, sizeof(text), &page, &total) && page == 1);
    for (int i = 0; i < 10; i++) muse_review_step(1);
    assert(muse_review_page(text, sizeof(text), &page, &total) && page == total);
    muse_review_close();
    assert(!muse_review_active());
    assert(muse_review_step(1));
    assert(muse_review_page(text, sizeof(text), &page, &total) && page == 1);
    muse_review_store(2, "new reply");
    assert(!muse_review_active());
    muse_review_store(3, ""); // no response: the last reply remains available
    muse_review_store(2, "second part");
    assert(muse_review_step(1));
    assert(muse_review_page(text, sizeof(text), &page, &total));
    assert(!strcmp(text, "new reply\nsecond"));
    muse_review_step(1);
    assert(muse_review_page(text, sizeof(text), &page, &total));
    assert(!strcmp(text, "second\npart"));
    muse_review_store(4, "“café”");
    assert(muse_review_step(1));
    assert(muse_review_page(text, sizeof(text), &page, &total));
    assert(text[0] == '"' && !strncmp(text + 1, "cafe", 4) && text[5] == '"' && !text[6]);
}
static void long_reply(void) {
    cols = 10; lines = 2;
    char *large = malloc(40000);
    memset(large, 'a', 39999); large[39999] = 0;
    muse_review_init();
    muse_review_store(1, large);
    assert(muse_review_step(1));
    char page[256]; int at, total;
    assert(muse_review_page(page, sizeof(page), &at, &total));
    assert(total > 3000); // full reply, rather than the 1024-byte live-caption buffer
    for (int i = 0; i < total + 5; i++) muse_review_step(1);
    assert(muse_review_page(page, sizeof(page), &at, &total) && at == total);
    free(large);
}

#define MUSE_BTN_TALK_PRESS 1
#define MUSE_BTN_TALK_RELEASE 2
#define MUSE_BTN_REVIEW_PREV (1u << 10)
#define MUSE_BTN_REVIEW_NEXT (1u << 11)
#define portTICK_PERIOD_MS 1
#define ESP_LOGI(...) ((void)0)
static bool asleep, confirm;
static uint32_t tick;
static int talks, releases, cameras;
static uint32_t xTaskGetTickCount(void) { return tick; }
static bool muse_link_talk_press(void) { bool c = confirm; confirm = false; return c; }
static bool muse_state_asleep(void) { return asleep; }
static void muse_state_poke(void) {}
static void muse_state_set_caption(const char *text) { (void)text; }
static void set_asleep(bool value, const char *why) { (void)why; asleep = value; }
static void talk_button(unsigned ev) { talks += !!(ev & 1); releases += !!(ev & 2); }
#if CONFIG_MUSE_WATCHER_CAMERA
static void watcher_camera_preview_toggle(void) { cameras++; }
#endif
''' + watcher + r'''
static void input(unsigned ev, uint32_t now) { tick = now; watcher_buttons(ev); }
static void routing(void) {
    muse_review_init();
    input(1, 100); input(2, 200); input(0, 600);
    assert(asleep && !talks && !releases);
    input(1, 700); input(2, 800);
    assert(!asleep && !talks);
    input(1, 1000); input(2, 1100);
    input(1, 1200); input(2, 1250); input(0, 1600);
    assert(!asleep && !talks);
    assert(cameras == CONFIG_MUSE_WATCHER_CAMERA);
    input(1, 2000); input(0, 2299);
    assert(!talks);
    input(0, 2300); input(2, 2800);
    assert(talks == 1 && releases == 1 && !asleep);
    confirm = true;
    input(1, 3000); input(0, 4000); input(2, 4100);
    assert(talks == 1 && releases == 1 && !asleep);
    muse_review_store(1, "the quick brown fox jumps over the lazy dog");
    input(MUSE_BTN_REVIEW_NEXT, 4200);
    assert(muse_review_active() && talks == 1);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "click")) click();
    else if (!strcmp(argv[1], "pairing")) pairing();
    else if (!strcmp(argv[1], "pages")) pages();
    else if (!strcmp(argv[1], "retained")) retained();
    else if (!strcmp(argv[1], "long")) long_reply();
    else if (!strcmp(argv[1], "routing")) routing();
    else return 2;
    return 0;
}
''')
        cls.binary = out / "controls"
        command = [
            *shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-DCONFIG_MUSE_BOARD_SENSECAP_WATCHER=1",
            "-include", str(ROOT / "tests/host_compat.h"), "-I", str(out), "-I", str(MUSE),
            str(harness), str(MUSE / "muse_review.c"), str(MUSE / "muse_chat_text.c"),
            str(MUSE / "muse_text.c"), "-o", str(cls.binary),
        ]
        for camera in (0, 1):
            binary = out / ("controls" if not camera else "controls-camera")
            result = subprocess.run(command[:-1] + [str(binary), f"-DCONFIG_MUSE_WATCHER_CAMERA={camera}"],
                                    capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.camera_binary = out / "controls-camera"

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_click_sleeps_and_hold_records(self):
        self.run_case("click")

    def test_pairing_wake_and_clock_wrap(self):
        self.run_case("pairing")

    def test_page_boundaries_and_utf8(self):
        self.run_case("pages")

    def test_review_retains_and_replaces_reply(self):
        self.run_case("retained")

    def test_review_handles_long_reply(self):
        self.run_case("long")


    def test_input_routes_click_hold_pairing_and_rotation(self):
        self.run_case("routing")

    def test_camera_double_click_does_not_record_or_sleep(self):
        result = subprocess.run([str(self.camera_binary), "routing"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
