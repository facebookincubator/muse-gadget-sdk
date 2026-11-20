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

/* Drives muse_photo_note.c for test_muse_photo_note.py with the camera, the
 * chat and FreeRTOS faked. The capture task runs when a scenario says so, or
 * while the voice task waits for it (on_wait). argv[1] names the scenario. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "muse_photo_note.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(3); } } while (0)

static bool setting_on = true;
bool muse_settings_photo_notes(void) { return setting_on; }

static uint32_t note_id = 41;
uint32_t muse_chat_note_id(void) { return note_id; }

static int attaches;
static uint32_t attached_note;
static bool attach_ok = true;
bool muse_chat_attach_jpeg(char *jpeg_base64, size_t len, uint32_t note)
{
    CHECK(jpeg_base64 && len == strlen("/9j/AAAA") && !memcmp(jpeg_base64, "/9j/AAAA", len));
    attaches++;
    attached_note = note;
    free(jpeg_base64);   /* it takes the photo either way */
    return attach_ok;
}

static bool capture_ok = true;
static int captures, closes;
void watcher_camera_preview_close(void) { CHECK(closes == captures - 1); closes++; }
bool watcher_camera_capture(char **jpeg_base64, const char **error)
{
    captures++;
    if (!capture_ok) {
        *jpeg_base64 = NULL;
        *error = "Himax camera did not respond";
        return false;
    }
    *jpeg_base64 = strdup("/9j/AAAA");
    *error = NULL;
    return true;
}

static TaskFunction_t task_fn;
static void *task_arg;
static int tasks;
static bool create_fails;
BaseType_t xTaskCreateWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                               UBaseType_t prio, TaskHandle_t *handle, uint32_t caps)
{
    (void)name; (void)prio; (void)handle;
    CHECK(stack >= 8192 && caps);
    if (create_fails) return pdFAIL;
    CHECK(!task_fn);   /* one capture at a time */
    task_fn = fn;
    task_arg = arg;
    tasks++;
    return pdPASS;
}
void vTaskDeleteWithCaps(TaskHandle_t task) { CHECK(!task); }

static void run_task(void)
{
    CHECK(task_fn);
    TaskFunction_t fn = task_fn;
    task_fn = NULL;
    fn(task_arg);
}

static int given;
static bool on_wait;   /* the capture finishes while the voice task waits */
static TickType_t waited;
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; given = 1; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    (void)s;
    if (!given && ticks && on_wait && task_fn) run_task();
    if (given) {
        given = 0;
        return pdTRUE;
    }
    waited += ticks;
    return pdFALSE;
}

static const char *label(void)
{
    const char *l = muse_photo_note_label();
    return l ? l : "-";
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *s = argv[1];
    if (!strcmp(s, "off") || !strcmp(s, "not_press")) {
        setting_on = !strcmp(s, "not_press");
        muse_photo_note_begin(!strcmp(s, "off"));
        CHECK(!tasks && !strcmp(label(), "-"));
        muse_photo_note_wait();
        CHECK(!waited && !captures);
    } else if (!strcmp(s, "taken")) {
        muse_photo_note_begin(true);
        CHECK(tasks == 1 && !strcmp(label(), "TAKING A PHOTO"));
        note_id = 42;   /* a later press: the photo still names its own note */
        run_task();
        CHECK(attaches == 1 && attached_note == 41 && !strcmp(label(), "PHOTO TAKEN"));
        muse_photo_note_wait();
        CHECK(!waited);
    } else if (!strcmp(s, "during_wait")) {
        muse_photo_note_begin(true);
        on_wait = true;
        muse_photo_note_wait();   /* returns once the photo is with the note */
        CHECK(attaches == 1 && !waited && !strcmp(label(), "PHOTO TAKEN"));
    } else if (!strcmp(s, "failed") || !strcmp(s, "refused")) {
        capture_ok = !strcmp(s, "refused");
        attach_ok = false;
        muse_photo_note_begin(true);
        run_task();
        CHECK(attaches == (capture_ok ? 1 : 0) && !strcmp(label(), "NO PHOTO"));
    } else if (!strcmp(s, "too_slow")) {
        muse_photo_note_begin(true);
        muse_photo_note_wait();   /* times out: the note goes without it */
        CHECK(waited && !strcmp(label(), "NO PHOTO"));
        run_task();               /* late: dropped, never added to a note */
        CHECK(captures == 1 && !attaches && !strcmp(label(), "NO PHOTO"));
    } else if (!strcmp(s, "busy")) {
        muse_photo_note_begin(true);
        muse_photo_note_wait();
        note_id = 42;
        muse_photo_note_begin(true);   /* the last capture is still running */
        CHECK(tasks == 1 && !strcmp(label(), "NO PHOTO"));
        run_task();
        CHECK(!attaches && !strcmp(label(), "NO PHOTO"));
        note_id = 43;
        muse_photo_note_begin(true);   /* free again; the late capture's give is cleared */
        CHECK(tasks == 2 && !strcmp(label(), "TAKING A PHOTO"));
        waited = 0;
        muse_photo_note_wait();
        CHECK(waited && !attaches);
        run_task();
        CHECK(!attaches);
    } else if (!strcmp(s, "no_task")) {
        create_fails = true;
        muse_photo_note_begin(true);
        CHECK(!tasks && !strcmp(label(), "NO PHOTO"));
        muse_photo_note_wait();
        CHECK(!waited);
    } else {
        CHECK(!"unknown scenario");
    }
    CHECK(closes == captures);   /* the live view, if open, closes after each capture */
    puts("ok");
    return 0;
}
