# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise the PSRAM chat session's one-shot startup and image ownership."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ChatSessionStartup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        command_types = source[source.index('enum cmd_type_t'):source.index('struct ev_t')]
        image_ownership = source[source.index('static void free_image_job('):
                                 source.index('/* Streams the already-sized JSON')]
        startup = source[source.index('extern "C" void muse_hatch_start('):
                         source.index('extern "C" void muse_hatch_chat_connect')]
        image_api = source[source.index('extern "C" muse_chat_image_result_t muse_chat_send_jpeg('):
                           source.index('extern "C" muse_hatch_ev_t muse_hatch_turn_event')]
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include "muse_chat_priv.h"

#define ESP_LOGE(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define IN_BYTES 128
#define OUT_BYTES 128
#define DICT_CHUNK_BYTES 64
#define MP3_BUF 256
#define NOTE_PART_BYTES 48
#define VOICE_NOTE 1
#define MAX_MSGS 8
#define TEXT_MAX 64
#define MINIMP3_MAX_SAMPLES_PER_FRAME 32
#define NDJSON_LINE_MAX 96
#define MAX_STREAMS 6

static std::unordered_set<void *> allocations;
static void *tracked_malloc(size_t n) {
    void *p = std::malloc(n ? n : 1);
    assert(p && allocations.insert(p).second);
    return p;
}
static void *tracked_calloc(size_t n, size_t size) {
    void *p = tracked_malloc(n * size);
    memset(p, 0, n * size);
    return p;
}
static void tracked_free(void *p) {
    if (!p) return;
    assert(allocations.erase(p) == 1);
    std::free(p);
}

#define calloc tracked_calloc
#define free tracked_free

enum fail_mode_t { FAIL_NONE, FAIL_QUEUES, FAIL_STREAMS, FAIL_PSRAM, FAIL_TASK };
static fail_mode_t fail_mode;
static int psram_calls, fail_psram_at, task_attempts, tasks_running;
static bool configured = true, wifi = true, force_queue_full;

struct fake_queue_t {
    size_t item_size;
    bool has_item;
    unsigned char item[128];
};
using QueueHandle_t = fake_queue_t *;
struct fake_stream_buffer_t { int unused; };
using StreamBufferHandle_t = fake_stream_buffer_t *;

static QueueHandle_t make_queue(size_t item_size) {
    if (fail_mode == FAIL_QUEUES) return nullptr;
    auto *q = static_cast<fake_queue_t *>(tracked_calloc(1, sizeof(fake_queue_t)));
    assert(item_size <= sizeof(q->item));
    q->item_size = item_size;
    return q;
}
static QueueHandle_t xQueueCreateWithCaps(unsigned, size_t item_size, int) {
    return make_queue(item_size);
}
static QueueHandle_t xQueueCreate(unsigned, size_t item_size) {
    return make_queue(item_size);
}
static int xQueueSend(QueueHandle_t q, const void *item, int) {
    if (!q || q->has_item || force_queue_full) return 0;
    memcpy(q->item, item, q->item_size);
    q->has_item = true;
    return pdTRUE;
}
static bool receive(QueueHandle_t q, void *item) {
    if (!q || !q->has_item) return false;
    memcpy(item, q->item, q->item_size);
    q->has_item = false;
    return true;
}
static StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t, size_t, int) {
    if (fail_mode == FAIL_STREAMS) return nullptr;
    return static_cast<fake_stream_buffer_t *>(tracked_calloc(1, sizeof(fake_stream_buffer_t)));
}

static void *psram_alloc(size_t n) {
    psram_calls++;
    if (fail_mode == FAIL_PSRAM && psram_calls == fail_psram_at) return nullptr;
    return tracked_malloc(n);
}
static void heap_caps_free(void *p) { tracked_free(p); }
static void *json_alloc(size_t n) { return psram_alloc(n); }
struct cJSON_Hooks { void *(*malloc_fn)(size_t); void (*free_fn)(void *); };
static void cJSON_InitHooks(cJSON_Hooks *) {}

static void hatch_task(void *) {}
static int xTaskCreatePinnedToCoreWithCaps(void (*)(void *), const char *, unsigned, void *, int,
                                            void *, int, int) {
    task_attempts++;
    if (fail_mode == FAIL_TASK) return 0;
    tasks_running++;
    return pdPASS;
}
static bool muse_wifi_connected(void) { return wifi; }
extern "C" bool muse_hatch_configured(void) { return configured; }

''' + command_types + r'''
struct ev_t { int type; uint32_t gen; char text[72]; };
struct stream_t { char *line; size_t cap; };
struct turn_t {
    uint8_t *chunk;
    uint8_t *mp3;
    uint8_t *note;
    char *texts;
    int tts_msg;
};
static QueueHandle_t s_cmds, s_events;
static StreamBufferHandle_t s_in, s_out;
static std::atomic<uint32_t> s_gen{0};
static std::atomic<bool> s_task_ready{false};
static turn_t s_turn;
static stream_t s_streams[MAX_STREAMS];
static int16_t *s_pcm, *s_pcm16;

''' + image_ownership + startup + image_api + r'''
static int callbacks;
static muse_chat_image_result_t callback_result;
static void image_done(muse_chat_image_result_t result, void *) {
    callbacks++;
    callback_result = result;
}
static char *photo() {
    char *p = static_cast<char *>(tracked_malloc(9));
    memcpy(p, "/9j/AAAA", 9);
    return p;
}
/* A failed start keeps what it allocated (WithCaps objects can't be freed
 * with the plain deletes) and is never retried; turns stay refused. */
static void expect_failed_start(fail_mode_t mode, int psram_at = 0) {
    fail_mode = mode;
    fail_psram_at = psram_at;
    psram_calls = task_attempts = 0;
    muse_hatch_start();
    assert(!s_task_ready.load() && !tasks_running);
    size_t kept = allocations.size();
    int attempts = task_attempts, calls = psram_calls;
    fail_mode = FAIL_NONE;
    muse_hatch_start();
    assert(allocations.size() == kept && task_attempts == attempts && psram_calls == calls);
    assert(!s_task_ready.load() && !tasks_running);
    assert(muse_chat_send_jpeg(photo(), 8, "", image_done, nullptr)
           == MUSE_CHAT_IMAGE_NETWORK_ERROR);
    assert(!callbacks && allocations.size() == kept);
}
static void start_ok() {
    fail_mode = FAIL_NONE;
    psram_calls = task_attempts = 0;
    muse_hatch_start();
    assert(s_task_ready.load() && s_cmds && tasks_running == 1 && task_attempts == 1);
    size_t live = allocations.size();
    muse_hatch_start();
    assert(task_attempts == 1 && allocations.size() == live); /* idempotent */
}
static void finish_queued(muse_chat_image_result_t result) {
    cmd_t cmd{};
    assert(receive(s_cmds, &cmd));
    assert(cmd.type == CMD_IMAGE && cmd.image);
    finish_image_job(cmd.image, result);
}
static void accepted_busy_and_repeated_jobs() {
    start_ok();
    size_t baseline = allocations.size();
    force_queue_full = true;
    assert(muse_chat_send_jpeg(photo(), 8, "busy", image_done, nullptr)
           == MUSE_CHAT_IMAGE_BUSY);
    assert(!callbacks && allocations.size() == baseline);
    force_queue_full = false;

    assert(muse_chat_send_jpeg(photo(), 8, "one", image_done, nullptr)
           == MUSE_CHAT_IMAGE_QUEUED);
    assert(tasks_running == 1 && !callbacks);
    finish_queued(MUSE_CHAT_IMAGE_SENT);
    assert(callbacks == 1 && callback_result == MUSE_CHAT_IMAGE_SENT);
    assert(allocations.size() == baseline);

    assert(muse_chat_send_jpeg(photo(), 8, "two", image_done, nullptr)
           == MUSE_CHAT_IMAGE_QUEUED);
    finish_queued(MUSE_CHAT_IMAGE_BUSY);
    assert(callbacks == 2 && callback_result == MUSE_CHAT_IMAGE_BUSY);
    assert(allocations.size() == baseline);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: expect_failed_start(FAIL_QUEUES); break;
    case 1: expect_failed_start(FAIL_STREAMS); break;
    case 2: expect_failed_start(FAIL_PSRAM, 1); break; /* required turn buffer */
    case 3: expect_failed_start(FAIL_PSRAM, 7); break; /* first per-stream receive buffer */
    case 4: expect_failed_start(FAIL_TASK); break;
    case 5: accepted_busy_and_repeated_jobs(); break;
    default: return 2;
    }
}
'''
        source_file = out / 'startup.cpp'
        source_file.write_text(code)
        image_object = out / 'image.o'
        c_flags = ['-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'components/muse')]
        commands = [
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *c_flags, '-c',
             str(ROOT / 'components/muse/muse_chat_image.c'), '-o', str(image_object)],
            [*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *c_flags,
             str(source_file), str(image_object), '-o', str(out / 'startup')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'startup'

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_failed_allocations_refuse_turns_and_are_not_retried(self):
        for case in range(4):
            with self.subTest(case=case):
                self.run_case(case)

    def test_task_creation_failure_refuses_turns_and_is_not_retried(self):
        self.run_case(4)

    def test_only_a_running_consumer_accepts_repeated_jobs_and_busy_frees_ownership(self):
        self.run_case(5)
