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
the send task. When a band's last piece fails, LVGL may be waiting for it or
still drawing the next band, and may take the core at any point the send task
doesn't hold a critical section. Whichever it is, LVGL must not queue another
band while the one after the failed band is still outstanding."""
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
#include <stdlib.h>
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
typedef int portMUX_TYPE;

/* One task runs at a time; switching hands the core over. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
enum { SENDER, LVGL };
static int turn = SENDER;
static void wait_turn(int me)
{
    pthread_mutex_lock(&mu);
    while (turn != me) {
        pthread_cond_wait(&cv, &mu);
    }
    pthread_mutex_unlock(&mu);
}
static void switch_to(int who, int me)
{
    pthread_mutex_lock(&mu);
    turn = who;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    wait_turn(me);
}

static fake_sem_t band_done_sem;
static fake_queue_t bands_q;
static lv_display_t disp_obj;
static lv_display_t *s_disp = &disp_obj;
static QueueHandle_t s_bands = &bands_q;
static SemaphoreHandle_t s_band_done = &band_done_sem;
static volatile bool s_band_busy;
static portMUX_TYPE s_lock __attribute__((unused));

static int outstanding, overlap, queued;
static SemaphoreHandle_t lvgl_waiting;   /* LVGL blocked in a take on it */
static int lvgl_drawing;                 /* LVGL runnable, drawing, but below the sender */
static int woken_by_give;

/* The send task's points where LVGL may take the core: the preempt_at'th
 * one (counting from 0) does, outside a critical section. */
static int preempt_at = -1, points, critical;
static void preempt_point(void)
{
    if (critical || points++ != preempt_at) {
        return;
    }
    if (lvgl_waiting) {
        woken_by_give = 0;   /* its wait times out */
        switch_to(LVGL, SENDER);
    } else if (lvgl_drawing) {
        switch_to(LVGL, SENDER);   /* whatever it was blocked on is free */
    }
}

__attribute__((unused)) static void enter_critical(portMUX_TYPE *m)
{
    (void)m;
    critical++;
}
__attribute__((unused)) static void exit_critical(portMUX_TYPE *m)
{
    (void)m;
    critical--;
    preempt_point();
}
#define portENTER_CRITICAL(m) enter_critical(m)
#define portEXIT_CRITICAL(m) exit_critical(m)

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

/* The send task's side: LVGL is above it, so a give it waits on runs it now. */
static BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    preempt_point();
    if (s->count) {
        return pdFALSE;
    }
    s->count = 1;
    if (lvgl_waiting == s) {
        woken_by_give = 1;
        switch_to(LVGL, SENDER);
    }
    preempt_point();
    return pdTRUE;
}

/* LVGL 9.5's: a store, safe from an interrupt. */
static void lv_display_flush_ready(lv_display_t *d)
{
    d->flushing = 0;
    preempt_point();
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
        d->flushing = 0;
    }
}

static uint8_t buf[2][16];

/* LVGL's task: flush a band, draw the next (blocking partway), wait, flush it. */
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
        lvgl_drawing = 1;
        switch_to(SENDER, LVGL);   /* drawing the next band */
        lvgl_drawing = 0;
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

/* Lets LVGL finish drawing until it waits (at most a few bands). */
static void run_lvgl(void)
{
    for (int i = 0; i < 3 && lvgl_drawing; i++) {
        switch_to(LVGL, SENDER);
    }
}

/* The send task takes the band and its last piece fails. */
static void send_and_fail(void)
{
    bands_q.items[0] = bands_q.items[1];
    bands_q.n--;
    outstanding--;
    preempt_point();
    band_failed();
}

/* argv: "waiting" or "drawing" (LVGL when A fails), then the preemption point. */
int main(int argc, char **argv)
{
    (void)argc;
    int drawing = !strcmp(argv[1], "drawing");
    pthread_t t;
    pthread_create(&t, NULL, lvgl_task, NULL);
    switch_to(LVGL, SENDER);   /* band A queued, drawing B */
    if (!drawing) {
        switch_to(LVGL, SENDER);   /* B drawn, waiting for A */
    }
    report("queued_a");
    preempt_at = atoi(argv[2]);
    send_and_fail();           /* A's last piece fails */
    printf("points=%%d\n", points);
    preempt_at = -1;
    run_lvgl();                /* LVGL queues B and must then wait for it */
    report("after_a");
    send_and_fail();           /* B fails too: LVGL carries on */
    run_lvgl();
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

    def run_case(self, lvgl, point):
        out = subprocess.run([str(self.exe), lvgl, str(point)], check=True, capture_output=True, text=True,
                             timeout=30).stdout
        states = {}
        for line in out.splitlines():
            when, *fields = line.split()
            if "=" in when:
                states["points"] = int(when.split("=")[1])
                continue
            states[when] = {k: int(v) for k, v in (f.split("=") for f in fields)}
        return states

    def check(self, lvgl):
        point, points = -1, 1
        while point < points:
            with self.subTest(lvgl=lvgl, preempted_at=point):
                s = self.run_case(lvgl, point)
                points = s["points"]
                self.assertEqual(s["queued_a"]["queued"], 1)
                # B is still queued: LVGL waits for it rather than queueing C.
                self.assertEqual(s["after_a"], dict(overlap=0, waiting=1, flushing=1, busy=1, queued=2))
                # Once B has gone, LVGL carries on.
                self.assertEqual(s["after_b"], dict(overlap=0, waiting=1, flushing=1, busy=1, queued=3))
            point += 1
        self.assertGreaterEqual(points, 3)

    def test_failed_last_piece_while_lvgl_waits(self):
        self.check("waiting")

    def test_failed_last_piece_while_lvgl_draws(self):
        self.check("drawing")


if __name__ == "__main__":
    unittest.main()
