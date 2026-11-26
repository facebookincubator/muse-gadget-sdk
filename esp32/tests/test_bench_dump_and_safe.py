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

"""The bench paths' fixes, compiled from the sources under AddressSanitizer:
- muse_video.c's ui.dump=buf buffer: the show task writes it while other tasks free (ui.dump=buf, release) and read it
  (ESP-Iris); the reader copies under the lock, a free mid-dump drops the rest of that dump and it's never marked
  ready, and no call leaves the lock held.
- muse_mosaico_iris.c's bench console: in SAFE its settings fallback never applies a line SAFE's console refused;
  outside SAFE it still does; the dump reader returns a copy."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function_source(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start : end + 1]
    raise AssertionError(signature)


def build_and_run(src: str) -> str:
    cc = shlex.split(os.environ.get("CC", "cc"))
    if not cc or shutil.which(cc[0]) is None:
        raise unittest.SkipTest("C compiler unavailable")
    with tempfile.TemporaryDirectory() as t:
        c, exe = Path(t, "a.c"), Path(t, "a")
        c.write_text(src)
        # The functions under test check optional hooks by address (`fn && fn(...)`), which the stand-ins
        # here define: GCC's -Waddress and Clang's -Wpointer-bool-conversion both flag that.
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-address",
                 "-Wno-pointer-bool-conversion", "-Wno-unknown-warning-option", "-g"]
        san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        r = subprocess.run([*cc, *flags, *san, str(c), "-o", str(exe)], capture_output=True, text=True)
        if r.returncode != 0:
            r = subprocess.run([*cc, *flags, str(c), "-o", str(exe)], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        run = subprocess.run([str(exe)], capture_output=True, text=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        assert run.returncode == 0, run.stdout + run.stderr
        return run.stdout


class DumpBuffer(unittest.TestCase):
    def test_a_free_mid_dump_never_leaves_a_reader_a_freed_buffer(self) -> None:
        v = (ROOT / "components/muse/muse_video.c").read_text()
        statics = v[v.index("static bool s_dump_to_buf;"):v.index("static uint32_t s_dgen, s_dump_gen;")] + \
            "static uint32_t s_dgen, s_dump_gen;\n"
        funcs = "".join(function_source(v, sig) + "\n" for sig in (
            "static void dlock(void)", "static void dunlock(void)", "static void dump_emit(", "static void dump_begin(",
            "static void dump_finish(", "static void dump_printf(", "void muse_video_dump(void)",
            "static void dbuf_free_locked(", "void muse_video_dump_to_buffer(", "void muse_video_dump_release(",
            "size_t muse_video_dump_read("))
        src = r"""#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int unused; } StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define portMAX_DELAY 0
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
static int s_depth, s_bad, s_mx;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *b) { (void)b; return &s_mx; }
static int xSemaphoreTake(SemaphoreHandle_t h, int t) { (void)h; (void)t; if (s_depth++) s_bad++; return 1; }
static int xSemaphoreGive(SemaphoreHandle_t h) { (void)h; if (--s_depth) s_bad++; return 1; }
static void *heap_caps_realloc(void *p, size_t n, int caps) { (void)caps; return realloc(p, n); }
static volatile bool s_dump_req;
""" + statics + funcs + r"""
static char buf[4096];
static size_t rd(size_t off, size_t cap, size_t *total) { return muse_video_dump_read(off, buf, cap, total); }
int main(void) {
    size_t total, n;
    /* a whole dump: ready, read in parts */
    muse_video_dump_to_buffer(); dump_begin();
    dump_printf("@uidump %d\n", 1); dump_printf("@Y 0 AAAA\n"); dump_printf("@uidump end\n");
    n = rd(0, 4096, &total); printf("%zu %zu ", n, total);
    n = rd(5, 4, &total); printf("%.*s ", (int)n, buf);
    n = rd(999, 10, &total); printf("%zu ", n);
    dump_finish(); n = rd(0, 4096, &total); printf("%zu %zu ", n, total);
    n = rd(4, 4, &total); printf("[%.*s] ", (int)n, buf);
    /* released mid-dump (ESP-Iris's reader, another task): the rest of that dump goes nowhere, never ready */
    muse_video_dump_to_buffer(); dump_begin(); dump_printf("@uidump 2\n");
    muse_video_dump_release();
    for (int i = 0; i < 200; i++) dump_printf("@P %d BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\n", i);
    dump_finish(); n = rd(0, 4096, &total); printf("%zu %zu ", n, total);
    /* ui.dump=buf again mid-dump: the old dump's lines are dropped, the new dump alone is read */
    muse_video_dump_to_buffer(); dump_begin(); dump_printf("old\n");
    muse_video_dump_to_buffer(); dump_printf("old2\n"); dump_finish();
    n = rd(0, 4096, &total); printf("%zu ", n);
    dump_begin(); dump_printf("new\n"); dump_finish();
    n = rd(0, 4096, &total); printf("%.*s", (int)n, buf);
    /* the console dump (ui.dump) never marks the buffer ready */
    muse_video_dump(); dump_begin(); dump_finish(); n = rd(0, 4096, &total);
    printf(" %zu %d %d\n", total, s_depth, s_bad);
    muse_video_dump_release();
    return 0;
}
"""
        out = build_and_run(src).split(" ")
        # the "@P" lines of the released dump go nowhere (the console's lines would go to stdout, none here)
        # before the dump is complete, nothing to read; then all 32 bytes, in parts
        self.assertEqual(out[:8], ["0", "0", "", "0", "32", "32", "[ump", "]"], out)
        self.assertEqual(out[8:11], ["0", "0", "0"], out)   # released mid-dump; ui.dump=buf again mid-dump
        self.assertEqual(out[11:], ["new\n", "0", "0", "0\n"], out)   # the new dump alone; console dump; lock balanced

    def test_ESP_Iris_reads_a_copy(self) -> None:
        g = (ROOT / "components_mosaico/muse_mosaico_iris/muse_mosaico_iris.c").read_text()
        self.assertNotIn("muse_video_dump_buffer", g)
        self.assertIn("muse_video_dump_read(off, out + 4, cap - 4, &total)", function_source(g, "static esp_err_t bench_dump("))
        self.assertNotIn("muse_video_dump_buffer", (ROOT / "components/muse/muse_video.h").read_text())


class SafeConsole(unittest.TestCase):
    def test_SAFE_refused_settings_stay_refused(self) -> None:
        g = (ROOT / "components_mosaico/muse_mosaico_iris/muse_mosaico_iris.c").read_text()
        src = r"""#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_SIZE 1
typedef struct { const uint8_t *payload; size_t payload_size; } esp_iris_rpc_request_t;
typedef enum { MUSE_BOOT_NORMAL, MUSE_BOOT_SAFE } muse_boot_mode_t;
static muse_boot_mode_t s_mode;
static muse_boot_mode_t muse_bootguard_mode(void) { return s_mode; }
static int s_settings;
static bool muse_input_console_line(char *line) { (void)line; return false; }   /* SAFE's console: refused */
typedef enum { MUSE_LINE_WIRELESS = 0, MUSE_LINE_WIRED = 1 } muse_line_origin_t;
static void muse_ble_command(char *cmd, muse_line_origin_t origin) { (void)cmd; s_settings += origin == MUSE_LINE_WIRELESS; }
""" + function_source(g, "static esp_err_t bench_line(") + r"""
static uint8_t call(const char *line) {
    uint8_t out[4] = { 0 };
    size_t len = 0;
    esp_iris_rpc_request_t rq = { (const uint8_t *)line, strlen(line) };
    bench_line(&rq, out, sizeof(out), &len, NULL);
    return out[0];
}
int main(void) {
    s_mode = MUSE_BOOT_SAFE;
    int a = call(">volume=80"), b = s_settings;
    s_mode = MUSE_BOOT_NORMAL;
    int c = call(">volume=80"), d = s_settings, e = call(">nonsense");
    printf("%d%d %d%d %d\n", a, b, c, d, e);
    return 0;
}
"""
        self.assertEqual(build_and_run(src).strip(), "00 11 0")


if __name__ == "__main__":
    unittest.main()
