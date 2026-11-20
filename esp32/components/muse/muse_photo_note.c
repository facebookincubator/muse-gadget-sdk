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

#include "muse_photo_note.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse_chat.h"
#include "muse_settings.h"
#include "sdkconfig.h"

/* The board's capture, as for camera.capture: a malloc'd base64 JPEG. Then
 * capture_done(): on the Watcher, a live view that was open gave its frame and
 * closes, so its frames don't cover the note's screen. A board with another
 * camera adds its own here and to the Kconfig option. */
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#define capture_base64 watcher_camera_capture
#define capture_done watcher_camera_preview_close
#endif

static const char *TAG = "muse_photo";

#define WAIT_MS 3000      /* after the release: a capture takes under a second, start-up included */
#define HANDOFF_MS 1500   /* a photo being queued: muse_chat_attach_jpeg waits up to a second */

enum { PHOTO_NONE, PHOTO_TAKING, PHOTO_TAKEN, PHOTO_FAILED };

static atomic_int s_state;          /* this note's photo */
static atomic_bool s_capturing;     /* a capture task is running, maybe a late one */
static SemaphoreHandle_t s_done;    /* given by the capture task once its photo is queued or dropped */
static bool s_pending;              /* voice task: this note's capture hasn't been waited for */

static void capture_task(void *arg)
{
    uint32_t note = (uint32_t)(uintptr_t)arg;
    char *photo = NULL;
    const char *error = NULL;
    bool ok = capture_base64(&photo, &error);
    capture_done();
    int taking = PHOTO_TAKING;
    if (atomic_compare_exchange_strong(&s_state, &taking, ok ? PHOTO_TAKEN : PHOTO_FAILED)) {
        if (!ok) {
            ESP_LOGW(TAG, "no photo: %s", error ? error : "camera capture failed");
        } else if (!muse_chat_attach_jpeg(photo, strlen(photo), note)) {   /* takes the photo either way */
            ESP_LOGW(TAG, "no photo: couldn't add it to the note");
            taking = PHOTO_TAKEN;
            atomic_compare_exchange_strong(&s_state, &taking, PHOTO_FAILED);
        }
    } else {
        free(photo);   /* too late: its note has gone without it */
    }
    xSemaphoreGive(s_done);
    atomic_store(&s_capturing, false);
    vTaskDeleteWithCaps(NULL);
}

void muse_photo_note_begin(bool press)
{
    s_pending = false;
    atomic_store(&s_state, PHOTO_NONE);
    if (!press || !muse_settings_photo_notes()) {
        return;
    }
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
    }
    if (!s_done || atomic_load(&s_capturing)) {
        ESP_LOGW(TAG, "no photo: the camera is still busy");
        atomic_store(&s_state, PHOTO_FAILED);
        return;
    }
    xSemaphoreTake(s_done, 0);   /* a late capture's, never waited for */
    atomic_store(&s_state, PHOTO_TAKING);
    atomic_store(&s_capturing, true);
    void *note = (void *)(uintptr_t)muse_chat_note_id();   /* the photo goes with this note only */
    if (xTaskCreateWithCaps(capture_task, "photo_note", 8192, note, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "no photo: couldn't start the capture");
        atomic_store(&s_capturing, false);
        atomic_store(&s_state, PHOTO_FAILED);
        return;
    }
    s_pending = true;
}

void muse_photo_note_wait(void)
{
    if (!s_pending) {
        return;
    }
    s_pending = false;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(WAIT_MS)) == pdTRUE) {
        return;
    }
    int taking = PHOTO_TAKING;
    if (atomic_compare_exchange_strong(&s_state, &taking, PHOTO_FAILED)) {
        ESP_LOGW(TAG, "no photo: the camera took too long");
        return;   /* the capture task drops it */
    }
    xSemaphoreTake(s_done, pdMS_TO_TICKS(HANDOFF_MS));   /* taken just now: let it reach the note */
}

const char *muse_photo_note_label(void)
{
    switch (atomic_load(&s_state)) {
    case PHOTO_TAKING: return "TAKING A PHOTO";
    case PHOTO_TAKEN: return "PHOTO TAKEN";
    case PHOTO_FAILED: return "NO PHOTO";
    default: return NULL;
    }
}
