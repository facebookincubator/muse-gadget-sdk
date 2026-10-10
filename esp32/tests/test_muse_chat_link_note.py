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

"""The voice note request muse_chat_link.c streams over Link, built with the
real WAV and base64 helpers (test_muse_chat_link_errors.py stubs them) and
checked against CPython's base64."""

import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_muse_voice_note import (  # noqa: E402
    ENV, MUSE, ROOT, SANITIZE, NoteRequestAssertions, parse_run, pcm_bytes)

CODE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_compat.h"
#include "cJSON.h"
#include "muse_chat.h"
#include "muse_chat_priv.h"
/* Not CHECK(): the fakes must still check under -DNDEBUG. */
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(3); } } while (0)
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define portMAX_DELAY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int SemaphoreHandle_t;
static int xSemaphoreCreateMutex(void) { return 1; }
static void xSemaphoreTake(int lock, int wait) { (void)lock; (void)wait; }
static void xSemaphoreGive(int lock) { (void)lock; }
static void vTaskDelay(int ticks) { (void)ticks; }
typedef struct { unsigned count, size; unsigned char data[8][80]; } fake_queue_t;
typedef fake_queue_t *QueueHandle_t;
static fake_queue_t queue;
static QueueHandle_t xQueueCreate(unsigned count, unsigned size)
{
    CHECK(count == 8 && size <= 80);
    queue.size = size;
    return &queue;
}
static void xQueueReset(QueueHandle_t q) { q->count = 0; }
static int xQueueSend(QueueHandle_t q, const void *p, int wait)
{
    (void)wait;
    if (q->count == 8) return 0;
    memcpy(q->data[q->count++], p, q->size);
    return 1;
}
static int xQueueReceive(QueueHandle_t q, void *p, int wait)
{
    (void)wait;
    if (!q->count) return 0;
    memcpy(p, q->data[0], q->size);
    memmove(q->data[0], q->data[1], --q->count * sizeof(q->data[0]));
    return 1;
}
static int64_t esp_timer_get_time(void) { return 10000000; }
static uint32_t esp_random(void) { return 42; }
static bool muse_link_hatch_linked(void) { return true; }
static bool muse_wifi_connected(void) { return true; }
static bool muse_link_req_ready(void) { return true; }
#define MUSE_LINK_REQ_TOO_LARGE (-2)   /* muse_link.h */
void muse_state_page(const char *text, int *cols, int *lines) { (void)text; *cols = 16; *lines = 2; }

/* The note's request: everything sent on /chat/stream, chunk by chunk. */
typedef void (*frame_fn)(void *, int, const uint8_t *, size_t, bool);
static int64_t note_id, next_id = 100;
static char sent[1 << 20];
static size_t sent_len, part_len[1024], nparts;
static bool part_end[1024];
static int64_t muse_link_req_open(const char *verb, const char *path, const char *const *headers,
                                  bool end, frame_fn cb, void *ctx)
{
    (void)headers; (void)cb; (void)ctx;
    CHECK(!strcmp(verb, "POST") && !end);
    if (!strcmp(path, "/chat/stream")) return note_id = ++next_id;
    CHECK(!strcmp(path, "/chat/subscribe"));
    return ++next_id;
}
static bool muse_link_req_send(int64_t id, const void *data, size_t len, bool end, int wait)
{
    (void)wait;
    if (id != note_id) return true;
    CHECK(nparts < 1024 && sent_len + len <= sizeof(sent) && (!nparts || !part_end[nparts - 1]));
    memcpy(sent + sent_len, data, len);
    sent_len += len;
    part_len[nparts] = len;
    part_end[nparts++] = end;
    return true;
}
static void muse_link_req_cancel(int64_t id) { (void)id; }
''' + '{source}' + r'''
/* argv: frames per muse_hatch_turn_audio call, cycled. stdin is the PCM. */
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    size_t sched[16], sched_n = 0, cap = 1 << 21, len = 0;
    for (char *p = argv[1]; *p && sched_n < 16; p += strcspn(p, ",") + (p[strcspn(p, ",")] == ',')) {
        sched[sched_n++] = strtoul(p, NULL, 10);
    }
    uint8_t *pcm = malloc(cap);
    for (size_t got; (got = fread(pcm + len, 1, cap - len, stdin)) > 0;) len += got;
    muse_hatch_start();
    muse_hatch_turn_begin();
    CHECK(s_turn.phase == T_TALKING);
    for (size_t at = 0, i = 0; at < len; i++) {
        size_t frames = sched[i % sched_n] < (len - at) / 2 ? sched[i % sched_n] : (len - at) / 2;
        muse_hatch_turn_audio((const int16_t *)(pcm + at), frames);
        at += frames * 2;
    }
    muse_hatch_turn_end();
    printf("ok=%d failed=%s pcm=%zu parts=", s_turn.phase == T_ACK, s_turn.error[0] ? "error" : "-", len);
    for (size_t i = 0; i < nparts; i++) printf("%s%zu:%d", i ? "," : "", part_len[i], part_end[i]);
    putchar('\n');
    fwrite(sent, 1, sent_len, stdout);
    muse_hatch_turn_cancel();
    free(pcm);
    return 0;
}
'''


class LinkVoiceNote(NoteRequestAssertions, unittest.TestCase):
    PART_PCM, PART_CHARS = 1536, 2048   # STAGE_BYTES and CHUNK_BYTES

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (MUSE / 'muse_chat_link.c').read_text()
        # Platform includes only; the helpers it calls are the real ones.
        source = re.sub(r'^#include .*$', '', source, flags=re.MULTILINE)
        (out / 'link.c').write_text(CODE.replace('{source}', source))
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        cc = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-Wall', '-Wextra', '-Werror']
        helpers = ['-include', str(ROOT / 'tests/host_compat.h'), '-I', str(MUSE)]
        commands = [
            # As in test_muse_chat_session.py: cJSON on its own, unsanitized,
            # since macOS deprecates its sprintf calls under these flags.
            [*cc, '-c', str(cjson / 'cJSON.c'), '-o', str(out / 'cjson.o')],
            [*cc, *SANITIZE, *helpers, '-c', str(MUSE / 'muse_chat_text.c'), '-o', str(out / 'text.o')],
            [*cc, *SANITIZE, *helpers, '-c', str(MUSE / 'muse_text.c'), '-o', str(out / 'muse_text.o')],
            [*cc, *SANITIZE, '-I', str(cjson), '-I', str(ROOT / 'tests'), '-I', str(MUSE),
             str(out / 'link.c'), str(out / 'text.o'), str(out / 'muse_text.o'), str(out / 'cjson.o'),
             '-o', str(out / 'link')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'link'

    def talk(self, pcm, frames):
        ran = subprocess.run([str(self.binary), ','.join(map(str, frames))],
                             input=pcm, capture_output=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode(errors='replace'))
        self.assertEqual(ran.stderr, b'')
        run = parse_run(ran.stdout)
        self.assertTrue(run['ok'])
        self.assertEqual(run['failed'], '-')
        return run

    def test_uneven_frames_across_several_stages(self):
        pcm = pcm_bytes(self.PART_PCM - 44 + 5 * self.PART_PCM + 700, 7)
        # The second schedule stops the first stage 2 bytes short of full.
        for frames in ((1, 7, 160, 513, 2, 1000), (745, 1, 767, 1)):
            with self.subTest(frames=frames):
                self.assert_request(self.talk(pcm, frames), pcm, self.PART_PCM, self.PART_CHARS)

    def test_stage_boundaries(self):
        # The header shares the first stage: 1492 bytes of PCM fill it.
        for n in (0, 2, self.PART_PCM - 44, 2 * self.PART_PCM - 44, 2 * self.PART_PCM - 42):
            pcm = pcm_bytes(n, n)
            with self.subTest(pcm=n):
                self.assert_request(self.talk(pcm, (160,)), pcm, self.PART_PCM, self.PART_CHARS)


if __name__ == '__main__':
    unittest.main()
