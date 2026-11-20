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

// The /link-control stream's framing (process_inbound_body_chunk in
// main/noise_control.cpp, sliced in by test_link_control_stream.py): u32-LE
// prefixed JSON messages cut across BodyChunks anywhere, a message too long
// to hold skipped whole, and a partial message dropped with its session.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <xplat/noise/core/Span.h>

extern "C" {
#include "cJSON.h"
}
#include "noise_control.h"

using namespace musegadgets::noise::core;

static void log_line(const char *, const char *, ...) {}
#define ESP_LOGI(tag, ...) log_line(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) log_line(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) log_line(tag, __VA_ARGS__)

static const char *TAG = "test";
static char s_register_req_id[40] = {0};
static bool s_register_acked = false;
static bool s_heartbeat_registered = false;

struct handled {
    std::string json;
    noise_ctrl_session_generation_t generation;
};
static std::vector<handled> s_handled;

static void handle_invoke_request(const char *json, size_t len, noise_ctrl_session_generation_t generation) {
    s_handled.push_back({std::string(json, len), generation});
}

#include "control_stream.inc"

static std::vector<uint8_t> framed(const std::string &json) {
    std::vector<uint8_t> out(4);
    uint32_t n = static_cast<uint32_t>(json.size());
    for (int i = 0; i < 4; i++) out[i] = static_cast<uint8_t>(n >> (8 * i));
    out.insert(out.end(), json.begin(), json.end());
    return out;
}

static void feed(const std::vector<uint8_t> &stream, const std::vector<size_t> &cuts,
                 noise_ctrl_session_generation_t generation) {
    size_t from = 0;
    for (size_t cut : cuts) {
        process_inbound_body_chunk(ConstByteSpan(stream.data() + from, cut - from), generation);
        from = cut;
    }
}

int main() {
    const std::string first = "{\"method\":\"link.invoke\",\"id\":\"split-1\",\"command\":\"device.health\"}";
    const std::string huge = "{\"method\":\"link.invoke\",\"id\":\"huge\",\"params\":{\"x\":\"" +
                             std::string(40000, 'z') + "\"}}";
    const std::string second = "{\"method\":\"link.invoke\",\"id\":\"split-2\",\"command\":\"device.health\"}";
    std::vector<uint8_t> stream = framed(first);
    for (const std::string *m : {&huge, &second}) {
        std::vector<uint8_t> f = framed(*m);
        stream.insert(stream.end(), f.begin(), f.end());
    }

    // Cut inside a length prefix, inside messages, and many times inside the
    // message too long to hold: the two small ones arrive whole, in order.
    feed(stream, {2, 30, 90, 3000, 10000, 20000, 30000, 39000, stream.size() - 50, stream.size() - 3,
                  stream.size()}, 1);
    assert(s_handled.size() == 2);
    assert(s_handled[0].json == first && s_handled[1].json == second);

    // Whole messages in one chunk, and several in one, still work.
    s_handled.clear();
    std::vector<uint8_t> two = framed(first);
    std::vector<uint8_t> f = framed(second);
    two.insert(two.end(), f.begin(), f.end());
    feed(two, {two.size()}, 1);
    assert(s_handled.size() == 2 && s_handled[0].json == first && s_handled[1].json == second);

    // A message cut short when its session ends isn't finished by the next
    // session's bytes.
    s_handled.clear();
    std::vector<uint8_t> one = framed(first);
    feed(one, {10}, 2);
    feed(framed(second), {framed(second).size()}, 3);
    assert(s_handled.size() == 1 && s_handled[0].json == second && s_handled[0].generation == 3);

    // The session's end frees a message it left cut short.
    s_handled.clear();
    feed(one, {10}, 5);
    assert(s_ctrl_rx.body != nullptr);
    drop_held_control_message();
    assert(s_ctrl_rx.body == nullptr);
    feed(framed(second), {framed(second).size()}, 5);
    assert(s_handled.size() == 1 && s_handled[0].json == second);

    // The link.register reply, split across chunks, still marks the
    // registration acknowledged.
    s_handled.clear();
    strcpy(s_register_req_id, "reg-1");
    const std::string ack = "{\"type\":\"res\",\"id\":\"reg-1\",\"result\":{\"status\":\"registered\"}}";
    std::vector<uint8_t> acked = framed(ack);
    feed(acked, {3, 20, acked.size()}, 4);
    assert(s_register_acked && s_heartbeat_registered);
    assert(s_handled.size() == 1 && s_handled[0].json == ack);

    std::printf("ok\n");
    return 0;
}
