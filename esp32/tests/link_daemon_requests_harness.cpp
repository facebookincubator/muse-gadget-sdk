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

// The daemon request API (noise_ctrl_req_* in main/noise_control.cpp) in a
// Link-only build: no Muse UI (CONFIG_MUSE_ENABLED 0), requests on. The
// production request section and public functions are compiled from the
// source (test_link_daemon_requests.py slices them in) and run over a real,
// established ClientSession; FreeRTOS and the heap are host stand-ins.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "link_noise_control_test_support.h"

#define CONFIG_MUSE_ENABLED 0
#define CONFIG_HOMEHUB_DAEMON_REQUESTS 1
#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#define SMALL_CONTROL_SESSION 0
#define SVC_FRAME_SCRATCH (64 * 1024)  // CONFIG_SPIRAM=1 in this harness
#define OUT_SVC_SCRATCH 12288
#define OUT_ENV_SCRATCH 12288
#define MALLOC_CAP_8BIT 2
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
typedef int TickType_t;
typedef int BaseType_t;
typedef struct esp_tls esp_tls_t;

static const char *TAG = "test";
static volatile bool s_connected = true;
static bool s_req_too_large;

// ---- FreeRTOS queue (one item size, FIFO) and timing ----
struct fake_queue {
    size_t item, cap;
    std::deque<std::vector<uint8_t>> items;
};
typedef fake_queue *QueueHandle_t;
static QueueHandle_t xQueueCreate(unsigned length, unsigned item) { return new fake_queue{item, length, {}}; }
static BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t) {
    if (q->items.size() >= q->cap) return pdFALSE;
    const uint8_t *p = static_cast<const uint8_t *>(item);
    q->items.emplace_back(p, p + q->item);
    return pdTRUE;
}
static BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t) {
    if (q->items.empty()) return pdFALSE;
    memcpy(item, q->items.front().data(), q->item);
    q->items.pop_front();
    return pdTRUE;
}
static unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return (unsigned)q->items.size(); }
static int64_t s_now;
static int64_t esp_timer_get_time(void) { return s_now; }
static void vTaskDelay(TickType_t ticks) { s_now += (int64_t)ticks * 1000; }

// ---- TX gates and the wire ----
static bool s_headroom = true;
static size_t heap_caps_get_largest_free_block(int) { return 32768; }
static bool noise_tx_has_dma_headroom_reclaiming(size_t) { return s_headroom; }
static bool noise_tx_has_contiguous_dma_headroom(void) { return s_headroom; }

static std::string s_wire;   // plaintext of everything sent (TestCrypto seals as identity)
static bool flush_outbound(esp_tls_t *, ClientSession &session, uint8_t *buf) {
    while (session.HasOutboundWebSocketPayload()) {
        auto r = session.WriteNextOutboundWebSocketPayload(ByteSpan(buf, ClientSession::kMaxOutboundWebSocketPayloadSize));
        if (!r.ok()) return false;
        s_wire.append(reinterpret_cast<const char *>(buf), r.size());
    }
    return true;
}

#include "noise_control.h"
#include "daemon_requests.inc"   // the request section: queue, streams, pump, frames
#include "daemon_requests_api.inc"   // noise_ctrl_req_open/send/cancel

// ---- Callback log ----
struct event {
    void *ctx;
    int status;
    std::string data;
    bool end;
};
static std::vector<event> s_events;
static void on_event(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    s_events.push_back({ctx, status, std::string(reinterpret_cast<const char *>(data ? data : (const uint8_t *)""), len), end});
}

static void pump_all() {
    for (int i = 0; i < 32; i++) {
        bool did = false;
        assert(req_pump_tx(nullptr, *noise_session, svc_scratch, env_scratch, ws_buf, &did));
        if (!did) return;
    }
}

static void respond(int64_t id, int status, const char *body, bool end) {
    DecodedServiceFrame f;
    f.stream_id = id;
    f.kind = ServiceFrameKind::Response;
    f.response.status = status;
    f.response.body = ConstByteSpan(reinterpret_cast<const uint8_t *>(body), strlen(body));
    f.response.end_body = end;
    assert(req_on_frame(f));
}

static void chunk(int64_t id, const char *body, bool end) {
    DecodedServiceFrame f;
    f.stream_id = id;
    f.kind = ServiceFrameKind::BodyChunk;
    f.body_chunk.data = ConstByteSpan(reinterpret_cast<const uint8_t *>(body), strlen(body));
    f.body_chunk.end_body = end;
    assert(req_on_frame(f));
}

static void reset_frame(int64_t id) {
    DecodedServiceFrame f;
    f.stream_id = id;
    f.kind = ServiceFrameKind::Reset;
    assert(req_on_frame(f));
}

static void fresh() {
    reset_noise_session();
    s_wire.clear();
    s_events.clear();
    s_connected = true;
    s_headroom = true;
    s_req_too_large = false;
}

int main() {
    req_init();
    int a = 1, b = 2, c = 3, d = 4;

    // GET /chat/history: one request, the head and a later chunk, then done.
    fresh();
    int64_t h = noise_ctrl_req_open("GET", "/chat/history?limit=20", nullptr, true, on_event, &a);
    assert(h >= REQ_STREAM_BASE);
    pump_all();
    assert(s_wire.find("/chat/history?limit=20") != std::string::npos);
    assert(s_wire.find("GET") != std::string::npos);
    respond(h, 200, "{\"messages\":[", false);
    chunk(h, "]}", true);
    assert(s_events.size() == 2 && s_events[0].status == 200 && !s_events[0].end);
    assert(s_events[1].status == 0 && s_events[1].data == "]}" && s_events[1].end);
    chunk(h, "late", true);   // the stream is done: dropped
    assert(s_events.size() == 2);
    assert(!req_find(h));

    // POST /fs/read with a JSON body sent after the open, and a header.
    fresh();
    const char *headers[] = {"content-type", "application/json", nullptr};
    int64_t r = noise_ctrl_req_open("POST", "/fs/read", headers, false, on_event, &b);
    assert(r > h);
    const char *body = "{\"path\":\"notes/today.md\",\"offset\":0,\"len\":4096}";
    assert(noise_ctrl_req_send(r, body, strlen(body), true, 0));
    pump_all();
    assert(s_wire.find("/fs/read") != std::string::npos);
    assert(s_wire.find("application/json") != std::string::npos);
    assert(s_wire.find(body) != std::string::npos);
    respond(r, 404, "{\"error\":\"not found\"}", true);
    assert(s_events.size() == 1 && s_events[0].ctx == &b && s_events[0].status == 404 && s_events[0].end);

    // A server reset ends the request with -1; a cancel resets it and drops later frames.
    fresh();
    int64_t x = noise_ctrl_req_open("POST", "/fs/read", nullptr, true, on_event, &a);
    int64_t y = noise_ctrl_req_open("GET", "/chat/history", nullptr, true, on_event, &b);
    pump_all();
    reset_frame(x);
    assert(s_events.size() == 1 && s_events[0].ctx == &a && s_events[0].status == -1 && s_events[0].end);
    noise_ctrl_req_cancel(y);
    size_t wire_before = s_wire.size();
    pump_all();
    assert(s_wire.size() > wire_before);   // the reset went out
    respond(y, 200, "{}", true);
    assert(s_events.size() == 1);   // nothing for the cancelled one

    // At most 3 at once: a 4th is refused with -1, and the first 3 keep going.
    fresh();
    int64_t ids[4];
    void *ctxs[4] = {&a, &b, &c, &d};
    for (int i = 0; i < 4; i++) {
        ids[i] = noise_ctrl_req_open("GET", "/chat/history", nullptr, true, on_event, ctxs[i]);
        assert(ids[i]);
    }
    pump_all();
    assert(s_events.size() == 1 && s_events[0].ctx == &d && s_events[0].status == -1);
    for (int i = 0; i < 3; i++) respond(ids[i], 200, "{}", true);
    assert(s_events.size() == 4);

    // The session ending fails every request, open or still queued; a frame
    // too large reports NOISE_CTRL_REQ_TOO_LARGE.
    fresh();
    int64_t o = noise_ctrl_req_open("GET", "/chat/history", nullptr, true, on_event, &a);
    pump_all();
    s_headroom = false;   // this one stays queued
    int64_t q = noise_ctrl_req_open("GET", "/chat/history", nullptr, true, on_event, &b);
    pump_all();
    assert(o && q && s_events.empty());
    s_req_too_large = true;
    req_end_all();
    assert(s_events.size() == 2);
    for (auto &e : s_events) assert(e.end && (e.ctx == &b ? e.status == -1 : e.status == NOISE_CTRL_REQ_TOO_LARGE));

    // Refusals at the API: down, bad arguments, odd or too many headers, and
    // sends or cancels for ids that aren't requests.
    fresh();
    s_connected = false;
    assert(!noise_ctrl_req_open("GET", "/chat/history", nullptr, true, on_event, &a));
    s_connected = true;
    assert(!noise_ctrl_req_open(nullptr, "/chat/history", nullptr, true, on_event, &a));
    assert(!noise_ctrl_req_open("GET", "/chat/history", nullptr, true, nullptr, &a));
    const char *odd[] = {"x-a", nullptr};
    assert(!noise_ctrl_req_open("GET", "/chat/history", odd, true, on_event, &a));
    const char *many[] = {"a", "1", "b", "2", "c", "3", "d", "4", "e", "5", "f", "6", "g", "7", nullptr};
    assert(!noise_ctrl_req_open("GET", "/chat/history", many, true, on_event, &a));
    assert(!noise_ctrl_req_send(1, "x", 1, true, 0));   // Link's own streams are below REQ_STREAM_BASE
    noise_ctrl_req_cancel(1);
    assert(uxQueueMessagesWaiting(s_req_q) == 0);
    // Frames on Link's own streams aren't the request API's.
    DecodedServiceFrame link_frame;
    link_frame.stream_id = 1;
    link_frame.kind = ServiceFrameKind::Response;
    assert(!req_on_frame(link_frame));

    // Queued body bytes are bounded (16 KB with PSRAM): a send past it waits,
    // then fails, and the request carries on once the queue drains.
    fresh();
    int64_t w = noise_ctrl_req_open("POST", "/fs/write", nullptr, false, on_event, &a);
    pump_all();
    s_headroom = false;
    static char big[REQ_QUEUE_BYTES + 1];
    memset(big, 'x', sizeof(big));
    assert(noise_ctrl_req_send(w, big, 8192, false, 0));
    assert(noise_ctrl_req_send(w, big, 8192, false, 0));
    assert(!noise_ctrl_req_send(w, big, 2, false, 50));
    s_headroom = true;
    pump_all();
    assert(s_events.empty());
    assert(noise_ctrl_req_send(w, big, 2, true, 0));
    pump_all();
    respond(w, 200, "{\"ok\":true}", true);
    assert(s_events.size() == 1 && s_events[0].status == 200);

    // One send larger than the outbound scratch can't be framed: that request
    // ends with -1 (callers send in pieces of at most 8 KB), the session doesn't.
    fresh();
    int64_t v = noise_ctrl_req_open("POST", "/fs/write", nullptr, false, on_event, &a);
    int64_t u = noise_ctrl_req_open("GET", "/chat/history", nullptr, true, on_event, &b);
    pump_all();
    assert(noise_ctrl_req_send(v, big, OUT_SVC_SCRATCH, true, 0));
    pump_all();
    assert(s_events.size() == 1 && s_events[0].ctx == &a && s_events[0].status == -1);
    respond(u, 200, "{}", true);
    assert(s_events.size() == 2 && s_events[1].ctx == &b && s_events[1].status == 200);

    // A session that can no longer send: the open fails, and only that request ends.
    fresh();
    poison_noise_session();
    int64_t z = noise_ctrl_req_open("GET", "/chat/history", nullptr, true, on_event, &a);
    pump_all();
    assert(z && s_events.size() == 1 && s_events[0].status == -1 && s_events[0].end);

    printf("ok\n");
    return 0;
}
