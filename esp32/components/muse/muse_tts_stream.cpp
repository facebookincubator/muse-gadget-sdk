// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#include "muse_tts_stream.h"
#include <atomic>
#include <cstring>
#include <new>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"

// A board/application component can supply a strong definition at link time.
__attribute__((weak)) muse_tts_provider muse_tts_get_provider() { return nullptr; }

static StreamBufferHandle_t audio;
static std::atomic<bool> running{false}, cancelled{false};
static uint64_t request_id;
static bool format_accepted;
static muse_tts_result result;
struct job { char *text; muse_tts_provider provider; };
static bool stopped(void *) { return cancelled.load(); }
static bool format(void *, muse_tts_format f) {
    format_accepted = f == muse_tts_format::mp3 && !stopped(nullptr);
    return format_accepted;
}
static bool write_audio(void *, const uint8_t *data, size_t size)
{
    if (!format_accepted) return false;
    while (size && !stopped(nullptr)) {
        size_t n = xStreamBufferSend(audio, data, size, pdMS_TO_TICKS(20));
        data += n;
        size -= n;
    }
    return !size && !stopped(nullptr);
}
static void worker(void *arg)
{
    auto *j = static_cast<job *>(arg);
    muse_tts_sink sink{nullptr, format, write_audio, stopped};
    result = j->provider(j->text, sink);
    if (!format_accepted && result == muse_tts_result::ok) result = muse_tts_result::failed;
    heap_caps_free(j->text);
    delete j;
    // Publish completion only after the producer has stopped touching the queue.
    running.store(false);
    vTaskDeleteWithCaps(nullptr);
}
bool muse_tts_busy() { return running.load(); }
void muse_tts_cancel() { cancelled.store(true); }
bool muse_tts_start(uint64_t id, const char *text, muse_tts_provider provider)
{
    if (!id || !text || !text[0] || !provider || running.load()) return false;
    if (!audio) audio = xStreamBufferCreateWithCaps(32 * 1024 + 1, 1, MALLOC_CAP_SPIRAM);
    if (!audio) return false;
    auto *j = new (std::nothrow) job;
    if (!j) return false;
    j->text = static_cast<char *>(heap_caps_malloc(strlen(text) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!j->text) { delete j; return false; }
    strcpy(j->text, text);
    j->provider = provider;
    xStreamBufferReset(audio);
    request_id = id;
    format_accepted = false;
    cancelled.store(false);
    running.store(true);
    if (xTaskCreateWithCaps(worker, "muse_tts", 16384, j, 4, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        running.store(false);
        heap_caps_free(j->text);
        delete j;
        return false;
    }
    return true;
}
size_t muse_tts_read(uint64_t id, uint8_t *out, size_t capacity)
{
    return audio && id == request_id && !cancelled.load() && capacity
        ? xStreamBufferReceive(audio, out, capacity, 0) : 0;
}
bool muse_tts_end(uint64_t id, muse_tts_result *out)
{
    if (!audio || id != request_id || running.load() || (!cancelled.load() && xStreamBufferBytesAvailable(audio))) return false;
    *out = cancelled.load() ? muse_tts_result::cancelled : result;
    return true;
}
