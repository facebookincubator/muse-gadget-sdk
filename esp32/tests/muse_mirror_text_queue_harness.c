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

#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include "muse_mirror_session.h"
#include "muse_mirror_core.h"

#define pdTRUE 1
#define SEND_MS 3000
#define pdMS_TO_TICKS(ms) (ms)
static muse_mirror_text_t queue[32];
static int count, wait_ms, drops, reports;
static int s_text_in = 1, s_text_sent = -1;
static bool connected = true, send_ok = true, s_text_viewer;
static atomic_bool s_text_field;
static uint8_t report[2];
static int xQueueSend(int q, const muse_mirror_text_t *m, int ticks) {
    assert(q == s_text_in);
    wait_ms = ticks;
    if (count == 32) return 0;
    queue[count++] = *m;
    return pdTRUE;
}
static int xQueueReceive(int q, muse_mirror_text_t *m, int ticks) {
    assert(q == s_text_in && !ticks);
    if (!count) return 0;
    *m = queue[0];
    memmove(queue, queue + 1, --count * sizeof(*queue));
    return pdTRUE;
}
static void xQueueReset(int q) { assert(q == s_text_in); count = 0; }
bool muse_mirror_connected(void) { return connected; }
static bool viewer_send(const void *p, size_t n, int ms) {
    assert(n == 2 && ms == SEND_MS);
    memcpy(report, p, n);
    reports++;
    return send_ok;
}
static void viewer_drop(void);
#include "text_session.inc"
static void viewer_drop(void) { drops++; connected = false; text_reset(); }

int main(void) {
    muse_mirror_text_t m;
    memset(&m, 0xcc, sizeof(m));
    assert(!muse_mirror_text_read(&m) && m.edit == 0 && m.text[0] == 0);
    text_pump();
    assert(!reports);
    input((const uint8_t *)"O", 1);
    text_pump();
    assert(reports == 1 && report[0] == 'O' && report[1] == 0);
    text_pump();
    assert(reports == 1);
    input((const uint8_t *)"Iignored", 8);
    assert(!count);
    muse_mirror_text_open(true);
    text_pump();
    assert(reports == 2 && report[1] == 1);
    input((const uint8_t *)"Ihello", 6);
    assert(count == 1 && wait_ms == SEND_MS);
    assert(muse_mirror_text_read(&m) && m.edit == 0 && !strcmp(m.text, "hello"));
    input((const uint8_t *)"I\xc0\xaf", 3);
    input((const uint8_t *)"K\x06", 2);
    assert(!count);
    input((const uint8_t *)"K\x05", 2);
    assert(muse_mirror_text_read(&m) && m.edit == MUSE_MIRROR_TEXT_ENTER && !m.text[0]);
    for (int i = 0; i < 32; i++) input((const uint8_t *)"Ix", 2);
    assert(count == 32 && !drops);
    input((const uint8_t *)"Ix", 2);
    assert(drops == 1 && !connected && !count && wait_ms == SEND_MS);
    assert(!s_text_viewer && s_text_sent == -1);
    connected = true;
    text_pump();
    assert(reports == 2);
    input((const uint8_t *)"O", 1);
    text_pump();
    assert(reports == 3 && report[1] == 1);
    input((const uint8_t *)"Iold", 4);
    text_reset();
    assert(!muse_mirror_text_read(&m));
    input((const uint8_t *)"O", 1);
    send_ok = false;
    text_pump();
    assert(drops == 2 && !connected && !s_text_viewer);
    return 0;
}
