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

"""C6 flush ordering regression, without ESP-IDF or any shared dependencies.

Compile actual board flush_done/start/wait functions and registration statements
against deterministic FreeRTOS/LVGL/adapter fakes. The fake caller follows LVGL
9.5's wait_for_flushing and call_flush_cb ordering: test flushing, optionally
wait, set flushing=1, send FLUSH_START, then submit DMA. Adapter OTHER/NONE
completion gives bookkeeping notification and calls lv_display_flush_ready.

This is a single-core scheduling model, not a DMA/panel/hardware test. It
exercises early, delayed, check-to-wait-window, and synchronous completion,
including leftovers across many frames. Removing drain is a negative control.

Run: python3 -m unittest discover -s esp32/tests -p test_muse_c6_flush.py -v
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

BOARD = Path(__file__).resolve().parents[1] / "components/muse/boards/board_waveshare_c6_18.c"
CC = shlex.split(os.environ.get("CC", "cc"))


def mask_noncode(source):
    # Preserve offsets while excluding braces/semicolons in comments/strings.
    return re.sub(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match[0]), source, flags=re.S,
    )


def function(source, name):
    masked = mask_noncode(source)
    match = re.search(
        rf"\bstatic\s+[^;{{}}]+?\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{", masked,
    )
    if not match:
        raise AssertionError(f"actual board function missing: {name}")
    depth = 1
    for index in range(match.end(), len(masked)):
        depth += (masked[index] == "{") - (masked[index] == "}")
        if depth == 0:
            return source[match.start():index + 1]
    raise AssertionError(f"unclosed actual board function: {name}")


def actual_snippets():
    source = BOARD.read_text()
    callbacks = "\n\n".join(function(source, name) for name in ("flush_done", "flush_start", "flush_wait"))
    start = function(source, "display_start")
    statements = []
    for pattern in (
        r"const\s+esp_lcd_panel_io_callbacks_t\s+cbs\s*=\s*\{[^;]*\}\s*;",
        r"esp_lcd_panel_io_register_event_callbacks\s*\([^;]*\)\s*;",
        r"lv_display_set_flush_wait_cb\s*\([^;]*\)\s*;",
        r"lv_display_add_event_cb\s*\(\s*s_disp\s*,\s*flush_start\s*,[^;]*\)\s*;",
    ):
        matches = list(re.finditer(pattern, start))
        if len(matches) != 1:
            raise AssertionError(f"expected exactly one actual registration: {pattern}")
        match = matches[0]
        if match.end() >= start.index("esp_lv_adapter_start()"):
            raise AssertionError("flush callbacks must register before adapter starts")
        statements.append(match[0])
    return callbacks, "\n".join(statements)


PRELUDE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define IRAM_ATTR
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define LV_EVENT_FLUSH_START 123
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))
#define FLUSH_WAIT_MS 1000
#define ESP_LOGE(tag, ...) ((void)(tag))
static const char *TAG = "board";

typedef int BaseType_t;
typedef void *SemaphoreHandle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef struct { int unused; } esp_lcd_panel_io_event_data_t;
typedef struct { int unused; } lv_event_t;
typedef struct { bool flushing; } lv_display_t;
typedef bool (*done_cb_t)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *);
typedef struct { done_cb_t on_color_trans_done; } esp_lcd_panel_io_callbacks_t;

static lv_display_t display;
static lv_display_t *s_disp = &display;
static int semaphore_storage;
static SemaphoreHandle_t s_flush_done = &semaphore_storage;
static done_cb_t registered_done;
static void (*registered_wait)(lv_display_t *);
static void (*registered_start)(lv_event_t *);
static int token;
static bool dma_active, waiter, notify_yield, fake_woken;
static unsigned submitted, completed, gives, notifies, blocking_takes, drains, waits, early_skips;

static void require(bool condition, const char *why)
{
    if (!condition) { fprintf(stderr, "%s\n", why); exit(1); }
}
static BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken)
{
    require(s == s_flush_done && dma_active, "IRQ without current DMA");
    require(token == 0, "current completion collided with stale token");
    dma_active = false;
    completed++;
    gives++;
    token = 1;
    *woken = fake_woken || waiter ? pdTRUE : pdFALSE;
    return pdTRUE;
}
static bool esp_lv_adapter_display_notify_color_trans_done_from_isr(lv_display_t *d)
{
    require(d == s_disp && token == 1 && gives == notifies + 1,
            "semaphore must be given BEFORE adapter clears flushing");
    notifies++;
    d->flushing = false;  /* adapter OTHER/NONE reaches lv_display_flush_ready */
    return notify_yield;
}
static void complete_irq(void)
{
    bool expected = notify_yield || fake_woken || waiter;
    require(registered_done != NULL, "missing actual completion registration");
    require(registered_done(NULL, NULL, NULL) == expected, "wrong ISR yield result");
}
static BaseType_t xSemaphoreTake(SemaphoreHandle_t s, uint32_t ticks)
{
    require(s == s_flush_done, "wrong semaphore");
    if (ticks == 0) {
        /* FLUSH_START must be after old completion and before NEW submission. */
        require(!dma_active, "drain happened during active DMA");
        drains++;
        bool had = token != 0;
        token = 0;
        return had ? pdTRUE : pdFALSE;
    }
    require(ticks == pdMS_TO_TICKS(FLUSH_WAIT_MS), "flush wait must block, bounded by FLUSH_WAIT_MS");
    blocking_takes++;
    if (!token) {
        require(dma_active, "wait lacks either token or pending DMA");
        waiter = true;
        /* Deterministically deliver the pending IRQ while the task is blocked. */
        complete_irq();
        waiter = false;
    }
    /* The assertion that catches the original skipped-wait stale-token bug. */
    require(!dma_active && completed == submitted, "premature DMA release: stale token consumed");
    require(token == 1, "wait woke without completion");
    token = 0;
    return pdTRUE;
}
static void esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io,
                                                     const esp_lcd_panel_io_callbacks_t *cbs, void *ctx)
{
    (void)io; (void)ctx;
    registered_done = cbs->on_color_trans_done;
}
static void lv_display_set_flush_wait_cb(lv_display_t *d, void (*cb)(lv_display_t *))
{
    require(d == s_disp, "wait callback attached to wrong display");
    registered_wait = cb;
}
static void lv_display_add_event_cb(lv_display_t *d, void (*cb)(lv_event_t *), int event, void *ctx)
{
    (void)ctx;
    require(d == s_disp && event == LV_EVENT_FLUSH_START, "drain registered for wrong display/event");
    registered_start = cb;
}
'''

POSTLUDE = r'''
static void register_actual_callbacks(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    /* ACTUAL_REGISTRATIONS */
}

static void wait_for_previous(bool irq_in_check_window)
{
    /* LVGL 9.5 lv_refr.c:1449..1455: predicate checked before wait callback. */
    bool checked_flushing = display.flushing;
    if (irq_in_check_window && checked_flushing) complete_irq();
    if (checked_flushing) {
        waits++;
        registered_wait(s_disp);
        display.flushing = false;
    } else {
        early_skips++;
    }
    require(!dma_active, "LVGL released a still-active buffer");
}

static void trial(unsigned schedule, bool drain_enabled)
{
    require(!dma_active && !display.flushing, "previous transfer not released");
    /* draw_buf_flush: flushing=1; call_flush_cb sends FLUSH_START, then driver
     * flush_cb submits DMA. This reproduces the ordering of pinned LVGL. */
    display.flushing = true;
    if (drain_enabled) registered_start(NULL);
    require(token == 0 || !drain_enabled, "old token survived actual flush_start");
    dma_active = true;
    submitted++;
    notify_yield = (submitted & 1) != 0;
    fake_woken = (submitted & 2) != 0;
    switch (schedule) {
    case 0: /* Early IRQ, before LVGL reaches its next flag check. */
        complete_irq();
        wait_for_previous(false);
        break;
    case 1: /* Delayed IRQ: wait callback blocks and receives current completion. */
        wait_for_previous(false);
        break;
    case 2: /* IRQ between flag check and entry to board's flush_wait. */
        wait_for_previous(true);
        break;
    case 3: /* Completion synchronously before driver's flush_cb returns. */
        complete_irq();
        wait_for_previous(false);
        break;
    default: require(false, "bad schedule");
    }
    require(completed == submitted && gives == notifies, "lost transfer or adapter notification");
}

int main(int argc, char **argv)
{
    register_actual_callbacks();
    require(registered_done == flush_done && registered_wait == flush_wait &&
            registered_start == flush_start, "actual board callback registration changed");
    bool drain_enabled = !(argc == 2 && strcmp(argv[1], "--without-drain") == 0);
    if (!drain_enabled) {
        /* A completes early; B is deliberately delayed. Without the new hook,
         * A's leftover token is consumed while B DMA is still outstanding. */
        trial(0, false);
        trial(1, false);
        require(false, "negative control failed to reproduce stale-token bug");
    }
    /* All adjacent completion timing pairs, including early completion at end
     * of a frame leaving a token through an arbitrary idle interval. */
    for (unsigned a = 0; a < 4; a++) {
        for (unsigned b = 0; b < 4; b++) {
            trial(a, drain_enabled);
            trial(b, drain_enabled);
        }
    }
    /* Thousands of strips across simulated frames, with varying IRQ yields. */
    uint32_t rng = 0x9e3779b9;
    for (unsigned frame = 0; frame < 128; frame++) {
        for (unsigned strip = 0; strip < 28; strip++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            trial(rng & 3, drain_enabled);
        }
    }
    require(submitted == 3616 && completed == submitted && gives == submitted && notifies == submitted,
            "incomplete schedule coverage");
    require(drains == submitted && waits && blocking_takes == waits && early_skips,
            "drain/wait/skip branches not covered");
    printf("%u DMA schedules passed: early/delayed/check-window/synchronous; no premature release; give-before-notify and actual callback registration checked\n", submitted);
    return 0;
}
'''


def run(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise AssertionError(f"command failed: {shlex.join(map(str, args))}\n{result.stdout}{result.stderr}")
    return result


def build(directory, sanitized=False):
    callbacks, registration = actual_snippets()
    directory = Path(directory)
    source = directory / "c6_flush.c"
    source.write_text(PRELUDE + callbacks + POSTLUDE.replace("/* ACTUAL_REGISTRATIONS */", registration))
    exe = directory / "c6_flush"
    flags = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror"]
    if sanitized:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    run(CC + flags + [str(source), "-o", str(exe)])
    return exe


class MuseC6FlushTests(unittest.TestCase):

    def test_actual_brightness_calls_use_recursive_adapter_lock(self):
        prelude = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define ESP_OK 0
static int depth, calls, locks, unlocks, value;
static bool fail_lock;
static void require(bool ok, const char *why) {
    if (!ok) { fprintf(stderr, "%s\n", why); exit(1); }
}
int esp_lv_adapter_lock(int timeout) {
    require(timeout == -1, "wrong adapter lock timeout");
    locks++;
    if (fail_lock) return 1;
    depth++;
    return ESP_OK;
}
void esp_lv_adapter_unlock(void) {
    require(depth > 0, "unlock without ownership");
    depth--;
    unlocks++;
}
int bsp_display_brightness_set(int pct) {
    require(depth > 0, "QSPI brightness without LVGL lock");
    calls++;
    value = pct;
    return ESP_OK;
}
'''
        postlude = r'''
int main(void) {
    set_brightness(81);
    require(depth == 0 && calls == 1 && value == 81, "external caller failed");
    depth = 1; /* LVGL timer already owns the recursive adapter mutex. */
    set_brightness(40);
    require(depth == 1 && calls == 2 && value == 40, "recursive caller failed");
    fail_lock = true;
    set_brightness(0);
    require(depth == 1 && calls == 2 && value == 40 && locks == 3 && unlocks == 2,
            "failed lock wrote panel or released caller's ownership");
    puts("brightness serialized for external and recursive callers");
    return 0;
}
'''
        actual = function(BOARD.read_text(), "set_brightness")
        with tempfile.TemporaryDirectory(prefix="muse-c6-brightness-") as directory:
            source = Path(directory) / "brightness.c"
            exe = Path(directory) / "brightness"
            flags = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror"]
            for sanitized in (False, True):
                source.write_text(prelude + actual + postlude)
                sanitizer = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
                             if sanitized else [])
                run(CC + flags + sanitizer + [str(source), "-o", str(exe)])
                env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1")
                result = run([str(exe)], env=env, timeout=30)
                self.assertIn("brightness serialized", result.stdout)
                self.assertEqual(result.stderr, "")
            # The original naked same-IO writer must fail this harness.
            source.write_text(prelude +
                              "static void set_brightness(int pct) { bsp_display_brightness_set(pct); }" +
                              postlude)
            run(CC + flags + [str(source), "-o", str(exe)])
            result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("QSPI brightness without LVGL lock", result.stderr)


    def test_original_stale_token_bug_is_detected_without_drain(self):
        with tempfile.TemporaryDirectory(prefix="muse-c6-flush-negative-") as directory:
            result = subprocess.run([str(build(directory)), "--without-drain"], text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("premature DMA release: stale token consumed", result.stderr)

    def test_actual_callbacks_under_asan_and_ubsan(self):
        with tempfile.TemporaryDirectory(prefix="muse-c6-flush-san-") as directory:
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
            result = run([str(build(directory, sanitized=True))], env=env, timeout=30)
            self.assertIn("3616 DMA schedules passed", result.stdout)
            self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
