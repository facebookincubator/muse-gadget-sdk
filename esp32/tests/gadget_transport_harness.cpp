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
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "link_noise_control_test_support.h"
#include <xplat/noise/core/TransportFrameCodec.h>
extern "C" {
#include "cJSON.h"
}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define WS_HEADROOM 32
#define WS_BUF_SIZE ClientSession::kMaxOutboundWebSocketPayloadSize
#define OUT_SVC_SCRATCH 12288
#define OUT_ENV_SCRATCH 12288
#define CTRL_STREAM_ID 1
#define TUNNEL_STREAM_ID 2
#define IDENTITY_STREAM_ID 3
#define IDENTITY_BODY_MAX 8192
struct esp_tls_t {};
struct sent_frame {
    ServiceType service;
    int64_t stream;
    ServiceFrameKind kind;
    std::string path, verb, body;
};
static std::vector<sent_frame> sent;
static bool ws_send_frame_inplace(esp_tls_t *, uint8_t *data, size_t len) {
    DecodedTransportFrame transport;
    assert(DecodeTransportFrame(ConstByteSpan(data, len - Transport::kTagSize), transport).ok());
    DecodedServiceRequestEnvelope envelope;
    assert(DecodeServiceRequestEnvelopeDetailed(transport.payload, envelope).ok());
    DecodedServiceFrame frame;
    assert(DecodeServiceFrameDetailed(envelope.payload, Span<HeaderView>(), frame).ok());
    sent_frame record{envelope.service, frame.stream_id, frame.kind, {}, {}, {}};
    if (frame.kind == ServiceFrameKind::Request) {
        record.path.assign(frame.request.path.data(), frame.request.path.size());
        record.verb.assign(frame.request.verb.data(), frame.request.verb.size());
    } else if (frame.kind == ServiceFrameKind::BodyChunk) {
        record.body.assign(reinterpret_cast<const char *>(frame.body_chunk.data.data()), frame.body_chunk.data.size());
    }
    sent.push_back(record);
    return true;
}
@CONSTANTS@
@FLUSH@
@TUNNEL@
static std::string agent_name;
static void got_name(const char *name) { agent_name = name; }
static void (*s_agent_name_cb)(const char *) = got_name;
@IDENTITY@
static bool open_control() {
    esp_tls_t *tls = nullptr;
    auto &session = *noise_session;
    const char *end_reason = nullptr;
@OPEN_CONTROL@
    return true;
cleanup:
    assert(end_reason); return false;
}
static void expect_last(int stream, ServiceFrameKind kind, const char *path = "") {
    assert(!sent.empty());
    const auto &f = sent.back();
    if (f.service != ServiceType::Gadget || f.stream != stream || f.kind != kind) {
        fprintf(stderr, "frame %zu: service=%d stream=%lld kind=%d, want stream=%d kind=%d\n",
                sent.size(), (int)f.service, (long long)f.stream, (int)f.kind, stream, (int)kind);
        abort();
    }
    assert(f.path == path);
}
static void identity_reply(const char *json, int status = 200) {
    identity_fetch f{};
    DecodedServiceFrame frame{};
    frame.stream_id = IDENTITY_STREAM_ID;
    frame.kind = ServiceFrameKind::Response;
    frame.response.status = status;
    size_t size = strlen(json), first = size / 2;
    frame.response.body = ConstByteSpan(reinterpret_cast<const uint8_t *>(json), first);
    frame.response.end_body = false;
    identity_on_frame(f, frame);
    frame.kind = ServiceFrameKind::BodyChunk;
    frame.body_chunk.data = ConstByteSpan(reinterpret_cast<const uint8_t *>(json + first), size - first);
    frame.body_chunk.end_body = true;
    identity_on_frame(f, frame);
    assert(f.done);
    free(f.body);
}
int main() {
    reset_noise_session();
    assert(open_control());
    expect_last(1, ServiceFrameKind::Request, "/api/gadget/control");
    assert(sent.back().verb == "POST");
    tunnel_emit_ctx ctx{nullptr, noise_session.get(), svc_scratch, env_scratch, ws_buf};
    assert(tunnel_open_stream(&ctx));
    expect_last(2, ServiceFrameKind::Request, "/api/gadget/tunnel");
    const uint8_t packet[] = {0x02, 0x00, 0x11, 0x22};
    assert(tunnel_send_body(&ctx, packet, sizeof(packet)));
    expect_last(2, ServiceFrameKind::BodyChunk);
    assert(sent.back().body == std::string(reinterpret_cast<const char *>(packet), sizeof(packet)));
    uint8_t inplace[512]{};
    const size_t head = 256;
    memcpy(inplace + head, packet, sizeof(packet));
    assert(tunnel_send_body_inplace(&ctx, inplace, head, sizeof(packet), sizeof(inplace) - head - sizeof(packet)));
    expect_last(2, ServiceFrameKind::BodyChunk);
    assert(sent.back().body == std::string(reinterpret_cast<const char *>(packet), sizeof(packet)));
    assert(tunnel_reset_stream(&ctx));
    expect_last(2, ServiceFrameKind::Reset);
    assert(identity_request(nullptr, *noise_session, svc_scratch, env_scratch, ws_buf));
    expect_last(3, ServiceFrameKind::Request, "/api/gadget/me");
    assert(sent.back().verb == "GET");
    identity_reply("{\"ok\":true,\"result\":{\"muse_name\":\"Muse\"}}");
    assert(agent_name == "Muse");
    agent_name.clear();
    identity_reply("{\"ok\":true,\"result\":{\"name\":\"Old route\"}}");
    assert(agent_name.empty());
    identity_reply("{\"ok\":true,\"result\":{\"muse_name\":\"Forbidden\"}}", 403);
    assert(agent_name.empty() && noise_session->isEstablished());
    // The Gadget service round-trips through the real envelope codec; the
    // services this SDK never uses (4, 5) and anything past 6 are rejected.
    ApplicationRequestView req{}; req.verb = StringView("GET"); req.path = StringView("/test");
    assert(noise_session->StartOutboundApplicationRequest(ServiceType::Gadget, 17, req,
        ByteSpan(svc_scratch), ByteSpan(env_scratch)).ok());
    assert(flush_outbound(nullptr, *noise_session, ws_buf));
    assert(sent.back().service == ServiceType::Gadget);
    for (int unused : {4, 5, 7}) {
        assert(!noise_session->StartOutboundApplicationRequest(static_cast<ServiceType>(unused), 17, req,
            ByteSpan(svc_scratch), ByteSpan(env_scratch)).ok());
    }

    // Exercise the support helper and preserve poisoned-session handling.
    poison_noise_session(); assert(!noise_session->isEstablished());
    puts("Gadget transport routes and identity passed");
}
