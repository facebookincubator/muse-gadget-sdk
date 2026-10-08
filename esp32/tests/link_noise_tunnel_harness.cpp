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

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "link_noise_control_test_support.h"

// SDK doubles; the included production .cpp has only SDK includes removed.
using UBaseType_t = unsigned;
constexpr int pdTRUE = 1, pdFALSE = 0;
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_DMA = 4;
struct Queue { unsigned capacity; std::deque<void *> items; };
using QueueHandle_t = Queue *;
static std::vector<Queue *> queues;
static std::vector<void *> allocations;
static size_t dma_free = 64 * 1024, dma_largest = 20 * 1024;
static unsigned free_reads, largest_reads;
static uint64_t now_us;
static std::vector<std::string> logs;

static QueueHandle_t xQueueCreate(unsigned capacity, unsigned item_size) {
    assert(item_size == sizeof(void *));
    auto *q = new Queue{capacity, {}};
    queues.push_back(q);
    return q;
}
static int xQueueSend(QueueHandle_t q, const void *item, int wait) {
    assert(wait == 0);
    if (q->items.size() == q->capacity) return pdFALSE;
    void *value;
    std::memcpy(&value, item, sizeof(value));
    q->items.push_back(value);
    return pdTRUE;
}
static int xQueueReceive(QueueHandle_t q, void *item, int wait) {
    assert(wait == 0);
    if (q->items.empty()) return pdFALSE;
    void *value = q->items.front();
    std::memcpy(item, &value, sizeof(value));
    q->items.pop_front();
    return pdTRUE;
}
static UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    return static_cast<unsigned>(q->items.size());
}
static void *heap_caps_malloc(size_t size, int caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    void *p = malloc(size);
    assert(p);
    allocations.push_back(p);
    return p;
}
static size_t heap_caps_get_free_size(int caps) {
    assert(caps == MALLOC_CAP_DMA);
    ++free_reads;
    return dma_free;
}
static size_t heap_caps_get_largest_free_block(int caps) {
    assert(caps == MALLOC_CAP_DMA);
    ++largest_reads;
    return dma_largest;
}
static uint64_t esp_timer_get_time() { return now_us; }
static void log_message(const char *, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    logs.emplace_back(buf);
}
#define ESP_LOGE log_message
#define ESP_LOGW log_message
#define ESP_LOGI log_message
#include "noise_tunnel.inc"

static std::vector<uint8_t> sent;
static bool exhaust_after_send, fail_send, refill, fail_open;
static bool session_alive = true;
static unsigned pings;
static bool open_stream(void *) {
    if (fail_open) { session_alive = false; poison_noise_session(); }
    return !fail_open;
}
static bool send_body(void *, const uint8_t *data, size_t len) {
    assert(len == 1);
    if (*data == 0) {
        ++pings;
        if (fail_send) { session_alive = false; poison_noise_session(); }
        return !fail_send;
    }
    sent.push_back(*data);
    if (refill) {
        const uint8_t next = 42;
        assert(noise_tunnel_send_packet(&next, 1));
    }
    if (exhaust_after_send || fail_send) {
        dma_free = 123;
        dma_largest = 64;
    }
    if (fail_send) { session_alive = false; poison_noise_session(); }
    return !fail_send;
}
static const noise_tunnel_emit_t emit = {nullptr, open_stream, send_body};
static void enqueue(uint8_t value) {
    assert(noise_tunnel_send_packet(&value, 1));
}
static void reopen() {
    noise_tunnel_on_session_down();
    session_alive = true;
    reset_noise_session();
    fail_open = false;
    pings = 0;
    assert(noise_tunnel_on_session_up(&emit));
    dma_free = 64 * 1024;
    dma_largest = 20 * 1024;
    exhaust_after_send = fail_send = refill = false;
    sent.clear();
    logs.clear();
    free_reads = largest_reads = 0;
}

// Compile the actual sender, flush, queue dispatch, keepalive and cleanup.
// Only SDK/JSON/I/O and crypto boundaries are doubled.
#include "noise_control.h"
extern "C" {
#include "link_fakes/cJSON.h"
}
#include "pending_result.inc"
static std::deque<pending_result> result_queue;
static auto *s_result_q = &result_queue;
static unsigned result_receives, serializations, json_deletes;
static bool fail_serialization;
static int xQueueReceive(std::deque<pending_result> *q, pending_result *pr, int wait) {
    assert(wait == 0);
    ++result_receives;
    if (q->empty()) return pdFALSE;
    *pr = q->front();
    q->pop_front();
    return pdTRUE;
}
static void counted_json_delete(cJSON *json) { ++json_deletes; cJSON_Delete(json); }
#define cJSON_Delete counted_json_delete
static char *last_serialized;
static char *wrap_result_json(const char *, cJSON *json) {
    ++serializations;
    char *serialized = fail_serialization ? nullptr
        : strdup(cJSON_GetObjectItem(json, "payload")->valuestring);
    cJSON_Delete(json);
    last_serialized = serialized;
    return serialized;
}
struct esp_tls_t {};
static std::vector<uint8_t> control_bytes;
static std::vector<size_t> chunk_sizes;
static std::vector<bool> chunk_ends;
static bool pressure_after_chunk, fail_ws;
static bool ws_send_binary(esp_tls_t *, const uint8_t *data, size_t len) {
    if (fail_ws) return false;
    DecodedTransportFrame transport;
    assert(DecodeTransportFrame(ConstByteSpan(data, len - Transport::kTagSize), transport).ok());
    assert(transport.total_chunks == 1 && transport.chunk_index == 0);
    DecodedServiceRequestEnvelope envelope;
    assert(DecodeServiceRequestEnvelopeDetailed(transport.payload, envelope).ok());
    assert(envelope.service == ServiceType::Daemon);
    DecodedServiceFrame frame;
    assert(DecodeServiceFrameDetailed(envelope.payload, Span<HeaderView>(), frame).ok());
    assert(frame.stream_id == 1 && frame.kind == ServiceFrameKind::BodyChunk);
    const auto bytes = frame.body_chunk.data;
    control_bytes.insert(control_bytes.end(), bytes.data(), bytes.data() + bytes.size());
    chunk_sizes.push_back(bytes.size());
    chunk_ends.push_back(frame.body_chunk.end_body);
    if (pressure_after_chunk) dma_largest = 1;
    return true;
}

// Instrument allocations/copies in the production sender without changing
// their behavior. A reintroduced whole-payload prepend must fail the test.
static unsigned sender_allocations, retained_frees;
static size_t sender_copied;
static void *watched_json;
[[maybe_unused]] static void *sender_malloc(size_t n) {
    ++sender_allocations;
    return malloc(n);
}
static void *sender_memcpy(void *to, const void *from, size_t n) {
    sender_copied += n;
    return memcpy(to, from, n);
}
static void sender_free(void *p) {
    if (p && p == watched_json) { ++retained_frees; watched_json = nullptr; }
    free(p);
}
#define malloc sender_malloc
#define memcpy sender_memcpy
#define free sender_free
#include "control_sender.inc"
#undef malloc
#undef memcpy
#undef free

static pending_json_body control_tx;
static bool heartbeat_sent;
static uint64_t last_heartbeat_us;
static void handle_invoke_request(const char *, size_t,
                                  noise_ctrl_session_generation_t) {}
#include "control_inbound.inc"
static unsigned ws_pings, delays, stack_polls;
static uint64_t last_activity_us;
static bool ws_send_ping(esp_tls_t *) { ++ws_pings; return true; }
static void vTaskDelay(int ticks) { assert(ticks == 1); ++delays; }
static uint64_t last_busy_us;
static void idle_wait(esp_tls_t *, uint64_t) { vTaskDelay(1); }   // traffic still recent
static void stack_monitor_poll(void *) { ++stack_polls; }
// Muse's extra daemon requests (noise_ctrl_req_*) aren't built here.
static void req_end_all() {}
#define ESP_LOGD log_message
#define KEEPALIVE_INTERVAL_MS 60000
static bool control_tx_turn(bool tunnel_opened = true,
                            noise_ctrl_session_generation_t session_generation = 1) {
    ClientSession &session = *noise_session;
    const auto tun_emit = emit;
    const bool s_register_acked = true;
    esp_tls_t *tls = nullptr;
    void *stack = nullptr;
    bool error = false, did = false;
    do {
#include "control_tx_turn.inc"
    } while (false);
    return error;
}
static void cleanup_control() {
    // Extracted from run_session's common cleanup label, including early errors.
#include "control_cleanup.inc"
}
static void drain_stale_results() {
#include "control_start_drain.inc"
}
static void queue_result(const std::string &json,
                         noise_ctrl_session_generation_t generation = 1) {
    cJSON *result = cJSON_CreateObject();
    assert(cJSON_AddStringToObject(result, "payload", json.c_str()));
    result_queue.push_back({generation, strdup("request"), result});
}
static std::vector<uint8_t> old_wire_bytes(const std::string &json) {
    uint32_t len = static_cast<uint32_t>(json.size());
    std::vector<uint8_t> bytes;
    for (unsigned i = 0; i < 4; ++i) bytes.push_back((len >> (8 * i)) & 0xff);
    bytes.insert(bytes.end(), json.begin(), json.end());
    return bytes;
}
static void reset_control() {
    cleanup_control();
    drain_stale_results();
    reopen();
    control_bytes.clear(); chunk_sizes.clear(); chunk_ends.clear();
    result_receives = serializations = json_deletes = 0;
    sender_allocations = retained_frees = 0; sender_copied = 0;
    ws_pings = delays = stack_polls = 0;
    pressure_after_chunk = fail_ws = fail_serialization = false;
#include "control_registration_reset.inc"
    heartbeat_sent = false;
    last_heartbeat_us = 0;
    last_activity_us = now_us;
}
static std::string payload(size_t len) {
    std::string json(len, ' ');
    for (size_t i = 0; i < len; ++i) json[i] = '!' + (i * 37 + i / 97) % 90;
    return json;
}
static void test_control_stream() {
    // Empty, prefix boundary, exact chunks, today's and future discovery sizes.
    for (size_t len : {0u, 1u, 8187u, 8188u, 8189u, 8192u, 16380u,
                       16384u, 30000u, 256u * 1024u}) {
        for (bool end_body : {false, true}) {
            reset_control();
            std::string json = payload(len);
            control_tx = {strdup(json.c_str()), len, 0, false, 1};
            watched_json = control_tx.json;
            unsigned turns = 0;
            while (true) {
                auto status = send_json_body_chunk(nullptr, *noise_session, control_tx,
                    svc_scratch, env_scratch, ws_buf, end_body);
                ++turns;
                assert(status != json_send_status::Failed);
                assert(!noise_session->HasOutboundWebSocketPayload());
                if (status == json_send_status::Complete) break;
                assert(turns <= (len + 4 + 8191) / 8192);
            }
            assert(turns == (len + 4 + 8191) / 8192);
            assert(control_bytes == old_wire_bytes(json));
            assert(control_tx.json_offset == len && control_tx.prefix_sent);
            for (size_t i = 0; i < chunk_sizes.size(); ++i) {
                assert(chunk_sizes[i] <= 8192);
                assert(chunk_ends[i] == (end_body && i + 1 == chunk_sizes.size()));
            }
            assert(sender_allocations == 0);
            assert(sender_copied == std::min(len, size_t(8188)));
            cleanup_control();
            assert(retained_frees == 1 && !control_tx.json);
        }
    }
}
static void test_control_pressure() {
    reset_control();
    const std::string discovery = payload(256 * 1024);
    queue_result(discovery); queue_result("second");
    dma_free = 16 * 1024 - 1;
    assert(!control_tx_turn());
    assert(result_queue.size() == 2 && result_receives == 0 && serializations == 0);
    assert(control_bytes.empty() && !control_tx.json);

    // Total free passes the queue gate but fragmentation blocks the first seal.
    dma_free = 64 * 1024; dma_largest = 2 * 1024 - 1;
    assert(!control_tx_turn());
    assert(result_queue.size() == 1 && serializations == 1);
    assert(control_tx.json == last_serialized && !control_tx.prefix_sent);
    watched_json = control_tx.json;
    for (int i = 0; i < 3; ++i) assert(!control_tx_turn());
    assert(crypto.seals == 0 && control_tx.json_offset == 0 && retained_frees == 0);
    assert(result_receives == 1 && serializations == 1);

    dma_largest = 2 * 1024;
    pressure_after_chunk = true;
    assert(!control_tx_turn());
    assert(chunk_sizes.size() == 1 && control_tx.json_offset == 8188);
    assert(control_tx.prefix_sent && crypto.seals == 1);
    // Between chunks the actual tunnel pump/tick, WS keepalive and loop delay
    // still execute. A pressure-only turn must not refresh TX activity.
    enqueue(77);
    now_us += 60000000;
    const uint8_t pong = 0;
    assert(!control_tx_turn());
    assert(sent == std::vector<uint8_t>{77} && pings == 0);
    // The infrequent tunnel ping now defers under the same fragmentation;
    // batches still use total-free only, and WS keepalives do not seal.
    assert(chunk_sizes.size() == 1 && control_tx.json_offset == 8188);
    now_us += 60000000;
    noise_tunnel_on_inbound(&pong, 1);
    assert(!control_tx_turn(false));
    assert(ws_pings == 1);
    unsigned old_delays = delays;
    assert(!control_tx_turn());
    assert(delays == old_delays + 1 && stack_polls > 0);
    assert(chunk_sizes.size() == 1 && control_tx.json_offset == 8188);
    assert(result_receives == 1 && serializations == 1 && retained_frees == 0);

    // Resume at every subsequent chunk boundary, repeatedly fragmenting heap.
    while (control_tx.json) {
        dma_largest = 2 * 1024;
        size_t before = chunk_sizes.size();
        assert(!control_tx_turn());
        assert(chunk_sizes.size() == before + 1);
        if (control_tx.json) {
            assert(!control_tx_turn());
            assert(chunk_sizes.size() == before + 1);
        }
    }
    assert(control_bytes == old_wire_bytes(discovery));
    assert(retained_frees == 1 && result_queue.size() == 1 && serializations == 1);
    pressure_after_chunk = false; dma_largest = 2 * 1024;
    assert(!control_tx_turn());
    auto expected = old_wire_bytes(discovery);
    auto second = old_wire_bytes("second");
    expected.insert(expected.end(), second.begin(), second.end());
    assert(control_bytes == expected && result_queue.empty());
    assert(serializations == 2 && sender_allocations == 0);
}
static void test_sustained_fragmentation() {
    // Never "heal" the largest block back to the 16 KiB burst reserve. Cover
    // queued results, a registration-style retained slot, and the reported
    // stall after byte 8,188. Keep RX/tunnel/WS alive for 600 simulated seconds.
    for (int mode : {2, 0, 1}) {
        reset_control();
        const std::string json = mode == 1 ? "{\"method\":\"link.register\"}"
                                           : payload(256 * 1024);
        if (mode == 1) {
            control_tx = {strdup(json.c_str()), json.size(), 0, false, 1};
        } else {
            queue_result(json);
        }
        if (mode == 2) {
            assert(!control_tx_turn());
            assert(control_tx.json_offset == 8188);
        }
        dma_free = 64 * 1024;
        dma_largest = 8 * 1024;
        const size_t max_turns = (json.size() + 4 + 8191) / 8192;
        size_t completed_turn = 0;
        for (size_t turn = 1; turn <= 600; ++turn) {
            now_us += 1000000;
            const uint8_t pong = 0;
            noise_tunnel_on_inbound(&pong, 1);
            assert(!control_tx_turn());
            assert(noise_session->isEstablished() && noise_tunnel_is_connected());
            assert(dma_free == 64 * 1024 && dma_largest == 8 * 1024);
            if (!control_tx.json && result_queue.empty() && !completed_turn) {
                completed_turn = turn;
            }
        }
        if (!completed_turn || completed_turn > max_turns) {
            fprintf(stderr, "fragmentation mode=%d stalled at offset=%zu after 600 turns\n",
                    mode, control_tx.json_offset);
        }
        assert(completed_turn && completed_turn <= max_turns);
        assert(control_bytes == old_wire_bytes(json));
        assert(ws_pings > 0);
    }
}
static void test_control_burst_pressure() {
    for (bool partial : {false, true}) {
        reset_control();
        const std::string json = payload(30000);
        // Registration is staged without the result-queue admission gate.
        control_tx = {strdup(json.c_str()), json.size(), 0, false, 1};
        if (partial) assert(!control_tx_turn());
        const size_t offset = control_tx.json_offset;
        const unsigned seals = crypto.seals;
        dma_free = 16 * 1024 - 1;
        dma_largest = 4 * 1024;  // Contiguous room but insufficient burst margin.
        for (int i = 0; i < 5; ++i) {
            assert(!control_tx_turn());
            assert(control_tx.json_offset == offset && crypto.seals == seals);
        }
        dma_free = 16 * 1024;
        for (int i = 0; i < 4 && control_tx.json; ++i) assert(!control_tx_turn());
        assert(!control_tx.json && control_bytes == old_wire_bytes(json));
    }
}
static void test_ping_fragmentation() {
    reset_control();
    now_us += TUN_KEEPALIVE_INTERVAL_US;
    const uint64_t previous_ping = s_last_ping_us;
    dma_free = 64 * 1024;
    dma_largest = 2 * 1024 - 1;
    noise_tunnel_tick(&emit);
    assert(pings == 0 && s_last_ping_us == previous_ping);
    assert(noise_tunnel_is_connected());
    dma_largest = 2 * 1024;
    noise_tunnel_tick(&emit);
    assert(pings == 1 && s_last_ping_us == now_us);
}
static void test_control_lifecycle() {
    // Teardown of a partially emitted message releases its only owned buffer.
    reset_control(); queue_result(payload(30000)); queue_result("queued old");
    assert(!control_tx_turn());
    watched_json = control_tx.json;
    assert(control_tx.json_offset == 8188);
    cleanup_control();
    assert(!control_tx.json && !control_tx.prefix_sent && control_tx.json_offset == 0);
    assert(retained_frees == 1);
    cleanup_control(); assert(retained_frees == 1);
    drain_stale_results();
    assert(result_queue.empty() && json_deletes == 2);

    // Defensive session-start drain also releases a retained message.
    reset_control(); queue_result(payload(30000)); assert(!control_tx_turn());
    watched_json = control_tx.json;
    drain_stale_results(); assert(retained_frees == 1 && !control_tx.json);

    // Exercise production dispatch (the obsolete dispatch-helper test is gone):
    // stale queued results must be deleted without serialization or a send,
    // and a generation change cannot resume an old prefix on a new connection.
    reset_control(); queue_result(payload(30000)); assert(!control_tx_turn());
    watched_json = control_tx.json;
    control_bytes.clear(); chunk_sizes.clear();
    queue_result("stale queued", 1); queue_result("new session", 2);
    assert(!control_tx_turn(true, 2));
    assert(retained_frees == 1 && serializations == 2);
    assert(control_bytes == old_wire_bytes("new session"));
    assert(result_queue.empty() && !control_tx.json);

    // Serialization failure consumes that tree, then permits the next result.
    reset_control(); queue_result("unserializable"); fail_serialization = true;
    assert(!control_tx_turn());
    assert(json_deletes == 1 && !control_tx.json && control_bytes.empty());
    fail_serialization = false; queue_result("valid");
    assert(!control_tx_turn()); assert(control_bytes == old_wire_bytes("valid"));
}
static void test_control_failures() {
    for (bool first : {true, false}) {
        for (bool seal : {true, false}) {
            reset_control(); queue_result(payload(30000)); queue_result("still queued");
            if (!first) assert(!control_tx_turn());
            crypto.fail_seal = seal; fail_ws = !seal;
            assert(control_tx_turn());
            assert(result_queue.size() == 1 && control_tx.json);
            assert(noise_session->isEstablished() == !seal);
            assert(control_tx.json_offset == (first ? 0 : 8188));
            watched_json = control_tx.json;
            cleanup_control(); assert(retained_frees == 1);
        }
    }
    // StartOutbound failure is also fatal and cannot consume the next result.
    reset_control(); queue_result("failed start"); queue_result("still queued");
    BodyChunkView body;
    assert(noise_session->StartOutboundBodyChunk(ServiceType::Daemon, 2, body,
        ByteSpan(svc_scratch), ByteSpan(env_scratch)).ok());
    assert(control_tx_turn());
    assert(result_queue.size() == 1 && crypto.seals == 0);
    reset_control();
}

static void test_heartbeat() {
    const auto heartbeat = old_wire_bytes("{\"method\":\"link.heartbeat\"}");
    now_us = 9000000000ULL;
    reset_control();
    assert(!control_tx_turn());
    assert(control_bytes.empty() && !heartbeat_sent);

    // Feed length-prefixed replies through the actual registration parser.
    const std::string registered =
        R"({"id":"register-id","type":"res","result":{"status":"registered"}})";
    auto receive = [](const std::string &json) {
        auto bytes = old_wire_bytes(json);
        process_inbound_body_chunk(ConstByteSpan(bytes.data(), bytes.size()), 1);
    };
    for (const char *reply : {
            "{", "{}",
            R"({"type":"res","result":{"status":"registered"}})",
            R"({"id":"other-id","type":"res","result":{"status":"registered"}})",
            R"({"id":"register-id","result":{"status":"registered"}})",
            R"({"id":"register-id","type":"event","result":{"status":"registered"}})",
            R"({"id":"register-id","type":"res"})",
            R"({"id":"register-id","type":"res","result":{"status":"pending"}})",
            R"({"id":"register-id","type":"res","result":{"status":true}})",
            R"({"id":"register-id","type":"res","result":{"status":"registered"},"error":"denied"})",
            R"({"id":"register-id","type":"res","result":{"status":"registered"},"error":null})"}) {
        reset_control();
        strcpy(s_register_req_id, "register-id");
        receive(reply);
        assert(!s_heartbeat_registered);
        assert(!control_tx_turn());
        assert(control_bytes.empty() && !control_tx.json && !heartbeat_sent);
    }
    // A rejected registration isn't acked, so the tunnel doesn't open, and the
    // reason is logged. A later success for the same request still counts.
    for (const auto &[reply, reason] : std::vector<std::pair<const char *, const char *>>{
            {R"({"id":"register-id","type":"res","result":{"status":"registered"},"error":"denied"})",
             "link.register rejected: denied"},
            {R"({"id":"register-id","type":"res","error":{"code":"invalid","message":"too many commands"}})",
             "link.register rejected: too many commands"},
            {R"({"id":"register-id","type":"res","error":{"code":"invalid"}})",
             "link.register rejected: no reason given"}}) {
        reset_control();
        strcpy(s_register_req_id, "register-id");
        logs.clear();
        receive(reply);
        assert(!s_register_acked && !s_heartbeat_registered);
        assert(std::find(logs.begin(), logs.end(), reason) != logs.end());
        receive(registered);
        assert(s_register_acked && s_heartbeat_registered);
    }
    // "error": null or false is no error: acked, though not a heartbeat
    // registration.
    for (const char *reply : {
            R"({"id":"register-id","type":"res","result":{"status":"registered"},"error":null})",
            R"({"id":"register-id","type":"res","result":{"status":"registered"},"error":false})"}) {
        reset_control();
        strcpy(s_register_req_id, "register-id");
        receive(reply);
        assert(s_register_acked && !s_heartbeat_registered);
    }

    // An unrelated reply doesn't consume registration; a reconnect needs a new ACK.
    for (int connection = 0; connection < 2; ++connection) {
        reset_control();
        strcpy(s_register_req_id, "register-id");
        assert(!s_register_acked && !s_heartbeat_registered);
        assert(!control_tx_turn());
        assert(control_bytes.empty());
        receive(R"({"id":"other-id","type":"res","result":{"status":"registered"}})");
        assert(!s_register_acked);
        receive(registered);
        assert(s_register_acked && s_heartbeat_registered);
        assert(!control_tx_turn());
        assert(control_bytes == heartbeat && heartbeat_sent);
        receive(registered);
        assert(!control_tx_turn());
        assert(control_bytes == heartbeat); // Repeated ACK does not send twice.
    }

    // A due heartbeat owns the slot while DMA pressure defers the actual seal.
    reset_control();
    receive(registered);
    dma_free = 16 * 1024 - 1;
    assert(!control_tx_turn());
    assert(control_tx.json && control_bytes.empty() && !heartbeat_sent);
    watched_json = control_tx.json;
    queue_result("queued result");
    now_us += 60000000;
    assert(!control_tx_turn(false));
    assert(ws_pings == 1 && crypto.seals == 0 && last_heartbeat_us == 0);
    assert(result_queue.size() == 1 && serializations == 0);
    dma_free = 64 * 1024;
    dma_largest = 2 * 1024 - 1;
    assert(!control_tx_turn());
    assert(crypto.seals == 0 && !heartbeat_sent);
    dma_largest = 2 * 1024;
    now_us += 1000;
    const uint64_t activity_before = last_activity_us;
    assert(!control_tx_turn());
    assert(control_bytes == heartbeat && !control_tx.json && retained_frees == 1);
    assert(heartbeat_sent && last_heartbeat_us == now_us);
    assert(last_activity_us == activity_before); // Heartbeat does not postpone WS ping.
    assert(!control_tx_turn());
    auto expected = heartbeat;
    auto result = old_wire_bytes("queued result");
    expected.insert(expected.end(), result.begin(), result.end());
    assert(control_bytes == expected);

    // The next deadline is measured from completion, not from enqueue time.
    control_bytes.clear();
    now_us = last_heartbeat_us + 86399999999ULL;
    assert(!control_tx_turn(false));
    assert(control_bytes.empty());
    ++now_us;
    assert(!control_tx_turn(false));
    assert(control_bytes == heartbeat && last_heartbeat_us == now_us);

    // A heartbeat must never split the prefix/body of a paused large result.
    reset_control();
    const std::string discovery = payload(30000);
    queue_result(discovery);
    pressure_after_chunk = true;
    assert(!control_tx_turn());
    assert(control_tx.json_offset == 8188);
    s_heartbeat_registered = true;
    assert(!control_tx_turn());
    assert(control_tx.json_offset == 8188 && !heartbeat_sent);
    pressure_after_chunk = false;
    dma_largest = 2 * 1024;
    for (int i = 0; i < 3; ++i) assert(!control_tx_turn());
    assert(!control_tx.json && !heartbeat_sent);
    assert(control_bytes == old_wire_bytes(discovery));
    assert(!control_tx_turn());
    expected = old_wire_bytes(discovery);
    expected.insert(expected.end(), heartbeat.begin(), heartbeat.end());
    assert(control_bytes == expected && heartbeat_sent);

    // Attempted seal/write failures remain fatal; teardown owns the buffer.
    for (bool seal : {true, false}) {
        reset_control();
        s_heartbeat_registered = true;
        crypto.fail_seal = seal;
        fail_ws = !seal;
        assert(control_tx_turn());
        assert(!heartbeat_sent && last_heartbeat_us == 0 && control_tx.json);
        watched_json = control_tx.json;
        cleanup_control();
        assert(retained_frees == 1 && !control_tx.json);
    }
    reset_control();
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "tunnel") != 0) {
        if (strcmp(argv[1], "heartbeat") == 0) test_heartbeat();
        else if (strcmp(argv[1], "stream") == 0) test_control_stream();
        else if (strcmp(argv[1], "pressure") == 0) test_control_pressure();
        else if (strcmp(argv[1], "fragmentation") == 0) test_sustained_fragmentation();
        else if (strcmp(argv[1], "burst") == 0) test_control_burst_pressure();
        else if (strcmp(argv[1], "ping_fragmentation") == 0) test_ping_fragmentation();
        else if (strcmp(argv[1], "lifecycle") == 0) test_control_lifecycle();
        else if (strcmp(argv[1], "failures") == 0) test_control_failures();
        else assert(false);
        cleanup_control();
        drain_stale_results();
        for (void *p : allocations) free(p);
        for (Queue *q : queues) delete q;
        return 0;
    }
    assert(noise_tunnel_pump_tx(&emit) == 0);
    reopen();
    dma_free = 0;
    assert(noise_tunnel_pump_tx(&emit) == 0);
    assert(free_reads == 0 && largest_reads == 0);  // empty: no heap query

    // Full pool survives gated turns; additional producers see backpressure.
    for (uint8_t i = 1; i <= 8; ++i) enqueue(i);
    const uint8_t overflow = 9;
    assert(!noise_tunnel_send_packet(&overflow, 1));
    dma_free = 16 * 1024 - 1;
    dma_largest = 4096;
    assert(noise_tunnel_pump_tx(&emit) == 0);
    assert(sent.empty() && logs.size() == 1);
    assert(logs.back().find("dma_free=16383 dma_largest=4096") != std::string::npos);
    for (int i = 0; i < 100; ++i) assert(noise_tunnel_pump_tx(&emit) == 0);
    assert(logs.size() == 1 && largest_reads == 1);
    assert(!noise_tunnel_send_packet(&overflow, 1));
    now_us = 5000000;
    assert(noise_tunnel_pump_tx(&emit) == 0);
    assert(logs.size() == 2 && largest_reads == 2);

    // Ping seals use the same gate. A deferred ping must retry next tick,
    // without waiting another 20s after pressure clears.
    now_us = 20000000;
    noise_tunnel_tick(&emit);
    assert(pings == 0 && noise_tunnel_is_connected() && sent.empty());
    assert(s_last_ping_us == 0);
    now_us += 1000;
    noise_tunnel_tick(&emit);
    assert(pings == 0 && s_last_ping_us == 0);

    // Exact threshold resumes in order; only the ping scans the largest block.
    dma_free = 16 * 1024;
    noise_tunnel_tick(&emit);
    assert(pings == 1 && s_last_ping_us == now_us);
    noise_tunnel_tick(&emit);
    assert(pings == 1);  // successful pings retain interval limiting
    assert(noise_tunnel_pump_tx(&emit) == 8);
    assert((sent == std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
    assert(largest_reads == 3);  // two pressure logs plus one ping, no batch walks
    for (uint8_t i = 1; i <= 8; ++i) enqueue(i);  // all slots returned

    // Pressure is checked before every batch, including partway through a turn.
    reopen();
    enqueue(11);
    enqueue(12);
    exhaust_after_send = true;
    assert(noise_tunnel_pump_tx(&emit) == 1);
    assert((sent == std::vector<uint8_t>{11}));
    assert(noise_tunnel_pump_tx(&emit) == 0);
    exhaust_after_send = false;
    dma_free = 64 * 1024;
    assert(noise_tunnel_pump_tx(&emit) == 1);
    assert((sent == std::vector<uint8_t>{11, 12}));

    // Concurrent refills cannot monopolize the session owner.
    reopen();
    enqueue(21);
    enqueue(22);
    refill = true;
    assert(noise_tunnel_pump_tx(&emit) == 2);
    assert((sent == std::vector<uint8_t>{21, 22}));
    refill = false;
    assert(noise_tunnel_pump_tx(&emit) == 2);
    assert((sent == std::vector<uint8_t>{21, 22, 42, 42}));

    // Failure returns to the caller for teardown, with no second seal/retry.
    reopen();
    enqueue(31);
    enqueue(32);
    fail_send = true;
    assert(noise_tunnel_pump_tx(&emit) == -1);
    assert((sent == std::vector<uint8_t>{31}));
    assert(logs.size() == 1);
    assert(logs.back().find("send_body failed (dma_free=123 dma_largest=64)") != std::string::npos);
    for (uint8_t i = 1; i <= 7; ++i) enqueue(i);  // failed slot was returned
    assert(!noise_tunnel_send_packet(&overflow, 1));
    noise_tunnel_on_session_down();
    assert(noise_tunnel_pump_tx(&emit) == 0 && sent.size() == 1);

    // RX timeout remains active even when every ping is deferred. Reopen
    // intentionally discards the stale queued batch and restores pool capacity.
    reopen();
    enqueue(51);
    dma_free = 0;
    now_us = s_last_rx_us + TUN_KEEPALIVE_TIMEOUT_US - 1;
    noise_tunnel_tick(&emit);
    assert(noise_tunnel_is_connected() && pings == 0);
    ++now_us;
    noise_tunnel_tick(&emit);
    assert(!noise_tunnel_is_connected() && pings == 0);
    dma_free = 16 * 1024;
    noise_tunnel_maybe_reopen(&emit);
    assert(noise_tunnel_is_connected());
    assert(noise_tunnel_pump_tx(&emit) == 0 && sent.empty());
    for (uint8_t i = 1; i <= 8; ++i) enqueue(i);

    // The void ping hook poisons Noise while there are no data/results queued.
    // The actual loop must notice this on the same turn, before WS keepalive.
    reopen();
    fail_send = true;
    now_us += TUN_KEEPALIVE_INTERVAL_US;
    assert(control_tx_turn());
    assert(!session_alive && !noise_tunnel_is_connected() && pings == 1);

    // Both initial tunnel open and later reopen can poison the shared session.
    reopen();
    noise_tunnel_on_session_down();
    fail_open = true;
    assert(control_tx_turn(false));
    assert(!session_alive);
    reopen();
    noise_tunnel_on_session_down();
    fail_open = true;
    now_us += TUN_REOPEN_INTERVAL_US;
    assert(control_tx_turn());
    assert(!session_alive);

    // Any future ungated seal failure is caught even with both TX queues empty.
    reopen();
    session_alive = false;
    poison_noise_session();
    assert(control_tx_turn());

    cleanup_control();
    drain_stale_results();

    for (void *p : allocations) free(p);
    for (Queue *q : queues) delete q;
    puts("PASS tunnel/control DMA pressure, ownership/order, fairness, keepalive timeout and poisoned-session teardown");
}
