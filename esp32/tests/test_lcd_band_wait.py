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

"""Compile muse_lcd_bands.c's band hand-off (queue_band, wait_band,
band_failed) on the host against a two-task model of the UI core: LVGL's task,
running above the send task, as it does once it has inherited a priority, and
the send task, whose give of s_band_done switches straight to LVGL. A band
whose last piece fails must not leave LVGL free to queue another band while
the one after it is still outstanding."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components/muse/boards/muse_lcd_bands.c"


def function(source, name):
    match = re.search(r"^static[^;{]*\b" + name + r"\([^)]*\)\s*\{", source, re.M | re.S)
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


HARNESS = r'''
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef void *esp_lcd_panel_handle_t;
typedef int BaseType_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct { int flushing; } lv_display_t;
typedef struct { const uint8_t *data; int x1, y1, x2, y2; } band_t;
typedef struct { int count; } fake_sem_t;
typedef fake_sem_t *SemaphoreHandle_t;
typedef struct { int n; band_t items[2]; } fake_queue_t;
typedef fake_queue_t *QueueHandle_t;

enum { PLAIN, TIMEOUT_AFTER_FLUSH, TIMEOUT_BEFORE_GIVE };
static int mode, armed;

/* One task runs at a time; switching hands the core over. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
enum { SENDER, LVGL };
static int turn = SENDER;
static void switch_to(int who, int me)
{
    pthread_mutex_lock(&mu);
    turn = who;
    pthread_cond_broadcast(&cv);
    while (turn != me) {
        pthread_cond_wait(&cv, &mu);
    }
    pthread_mutex_unlock(&mu);
}
static void wait_turn(int me)
{
    pthread_mutex_lock(&mu);
    while (turn != me) {
        pthread_cond_wait(&cv, &mu);
    }
    pthread_mutex_unlock(&mu);
}

static fake_sem_t band_done_sem;
static fake_queue_t bands_q;
static lv_display_t disp_obj;
static lv_display_t *s_disp = &disp_obj;
static QueueHandle_t s_bands = &bands_q;
static SemaphoreHandle_t s_band_done = &band_done_sem;
static volatile bool s_band_busy;

static int outstanding, overlap, queued;
static SemaphoreHandle_t lvgl_waiting;
static int woken_by_give;

/* LVGL's side: blocks until a give (pdTRUE) or a timeout (pdFALSE). */
static BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    if (s->count) {
        s->count--;
        return pdTRUE;
    }
    if (!ticks) {
        return pdFALSE;
    }
    lvgl_waiting = s;
    switch_to(SENDER, LVGL);
    lvgl_waiting = NULL;
    if (woken_by_give) {
        woken_by_give = 0;
        s->count--;
        return pdTRUE;
    }
    return pdFALSE;
}

/* LVGL's wait timing out now. */
static void lvgl_times_out(void)
{
    if (armed && lvgl_waiting) {
        woken_by_give = 0;
        switch_to(LVGL, SENDER);
    }
}

/* The send task's side: LVGL is above it, so a give it waits on runs it now. */
static BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (mode == TIMEOUT_BEFORE_GIVE) {
        lvgl_times_out();
    }
    if (s->count) {
        return pdFALSE;
    }
    s->count = 1;
    if (lvgl_waiting == s) {
        woken_by_give = 1;
        switch_to(LVGL, SENDER);
    }
    return pdTRUE;
}

static void lv_display_flush_ready(lv_display_t *d)
{
    d->flushing = 0;
    if (mode == TIMEOUT_AFTER_FLUSH) {
        lvgl_times_out();
    }
}

static BaseType_t xQueueSend(QueueHandle_t q, const band_t *b, TickType_t ticks)
{
    (void)ticks;
    if (q->n == 2) {
        return pdFALSE;
    }
    q->items[q->n++] = *b;
    overlap |= outstanding > 0;
    outstanding++;
    queued++;
    return pdTRUE;
}

%s

%s

%s

/* LVGL 9.5's wait_for_flushing with a flush_wait_cb. */
static void wait_for_flushing(lv_display_t *d)
{
    if (d->flushing) {
        wait_band(d);
    }
    d->flushing = 0;
}

static uint8_t buf[2][16];

/* LVGL's task: flush a band, draw the next (which blocks), wait, flush it. */
static void *lvgl_task(void *arg)
{
    (void)arg;
    wait_turn(LVGL);
    for (int i = 0; i < 4; i++) {
        if (i) {
            wait_for_flushing(s_disp);
        }
        s_disp->flushing = 1;
        queue_band(s_disp, NULL, 0, i * 8, 8, i * 8 + 8, buf[i & 1], NULL);
        switch_to(SENDER, LVGL);   /* drawing the next band */
    }
    for (;;) {
        switch_to(SENDER, LVGL);
    }
    return NULL;
}

static void report(const char *when)
{
    printf("%%s overlap=%%d waiting=%%d flushing=%%d busy=%%d queued=%%d\n", when, overlap,
           lvgl_waiting == s_band_done, s_disp->flushing, (int)s_band_busy, queued);
}

/* The send task takes the band and its last piece fails. */
static void send_and_fail(void)
{
    bands_q.items[0] = bands_q.items[1];
    bands_q.n--;
    outstanding--;
    band_failed();
}

int main(int argc, char **argv)
{
    mode = !strcmp(argv[1], "timeout_after_flush") ? TIMEOUT_AFTER_FLUSH
         : !strcmp(argv[1], "timeout_before_give") ? TIMEOUT_BEFORE_GIVE : PLAIN;
    pthread_t t;
    pthread_create(&t, NULL, lvgl_task, NULL);
    switch_to(LVGL, SENDER);   /* band A queued, drawing B */
    switch_to(LVGL, SENDER);   /* B drawn, waiting for A */
    report("queued_a");
    armed = 1;
    send_and_fail();           /* A's last piece fails: LVGL queues B, draws C */
    armed = 0;
    switch_to(LVGL, SENDER);   /* C drawn: LVGL must wait for B */
    report("after_a");
    send_and_fail();           /* B fails too: LVGL queues C */
    report("after_b");
    fflush(stdout);
    return 0;
}
'''


class LcdBandWaitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = SOURCE.read_text()
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "bands.c"
        src.write_text(HARNESS % tuple(function(source, n) for n in ("queue_band", "wait_band", "band_failed")))
        cls.exe = Path(cls.tmp.name) / "bands"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-pthread", "-o", str(cls.exe), str(src)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_mode(self, mode):
        out = subprocess.run([str(self.exe), mode], check=True, capture_output=True, text=True,
                             timeout=30).stdout
        states = {}
        for line in out.splitlines():
            when, *fields = line.split()
            states[when] = {k: int(v) for k, v in (f.split("=") for f in fields)}
        return states

    def check(self, mode):
        s = self.run_mode(mode)
        self.assertEqual(s["queued_a"], dict(overlap=0, waiting=1, flushing=1, busy=1, queued=1))
        # B is still queued: LVGL waits for it rather than flushing C.
        self.assertEqual(s["after_a"], dict(overlap=0, waiting=1, flushing=1, busy=1, queued=2), mode)
        # Once B has gone, LVGL carries on.
        self.assertEqual(s["after_b"]["overlap"], 0, mode)
        self.assertEqual(s["after_b"]["queued"], 3, mode)
        self.assertEqual(s["after_b"]["busy"], 1, mode)

    def test_failed_last_piece_while_lvgl_is_above(self):
        self.check("plain")

    def test_lvgl_wait_times_out_after_flush_ready(self):
        self.check("timeout_after_flush")

    def test_lvgl_wait_times_out_before_the_give(self):
        self.check("timeout_before_give")


if __name__ == "__main__":
    unittest.main()
