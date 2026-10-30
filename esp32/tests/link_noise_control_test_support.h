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

#pragma once

#include <memory>
#include <xplat/noise/core/ClientSession.h>
using namespace musegadgets::noise::core;

// Deterministic crypto boundary for pressure/failure injection. The real
// ClientSession, transport, framing, service codec and control sender run below.
// Crypto correctness is covered separately by test_noise_core.py.
class TestCrypto : public CryptoBackend {
 public:
    bool fail_seal = false;
    unsigned seals = 0;
    Status fill(ByteSpan out) noexcept {
        if (out.size()) memset(out.data(), 0x42, out.size());
        return OkStatus();
    }
    Status Random(ByteSpan out) noexcept override { return fill(out); }
    Status X25519GenerateKeypair(ByteSpan priv, ByteSpan pub) noexcept override {
        assert(fill(priv).ok()); return fill(pub);
    }
    Status X25519PublicFromPrivate(ConstByteSpan, ByteSpan out) noexcept override {
        return fill(out);
    }
    Status X25519Dh(ConstByteSpan, ConstByteSpan, ByteSpan out) noexcept override {
        return fill(out);
    }
    Status Sha256(ConstByteSpan, ByteSpan out) noexcept override { return fill(out); }
    Status Sha256Concat(ConstByteSpan, ConstByteSpan, ByteSpan out) noexcept override {
        return fill(out);
    }
    Status HkdfSha256(ConstByteSpan, ConstByteSpan, ConstByteSpan,
                     Span<ByteSpan> out) noexcept override {
        for (size_t i = 0; i < out.size(); ++i) assert(fill(out[i]).ok());
        return OkStatus();
    }
    Status Aes256GcmSeal(ConstByteSpan, ConstByteSpan, ConstByteSpan,
                        ConstByteSpan in, ByteSpan out) noexcept override {
        ++seals;
        if (fail_seal) return Status::ResourceExhausted();
        assert(out.size() >= in.size() + kAes256GcmTagSize);
        if (in.size()) memmove(out.data(), in.data(), in.size());
        memset(out.data() + in.size(), 0, kAes256GcmTagSize);
        return OkStatus();
    }
    Status Aes256GcmOpen(ConstByteSpan, ConstByteSpan, ConstByteSpan,
                        ConstByteSpan in, ByteSpan out) noexcept override {
        assert(in.size() >= kAes256GcmTagSize);
        size_t size = in.size() - kAes256GcmTagSize;
        assert(out.size() >= size);
        if (size) memmove(out.data(), in.data(), size);
        return OkStatus();
    }
};

static TestCrypto crypto;
static std::unique_ptr<ClientSession> noise_session;
static uint8_t svc_scratch[12288], env_scratch[12288];
// The session writes its payload WS_HEADROOM (32) bytes into ws_buf.
alignas(32) static uint8_t ws_buf[32 + ClientSession::kMaxOutboundWebSocketPayloadSize];
static void reset_noise_session() {
    crypto = TestCrypto{};
    noise_session = std::make_unique<ClientSession>(crypto);
    uint8_t handshake[256] = {};
    assert(noise_session->WriteHandshakeMessage1(ByteSpan(handshake)).ok());
    size_t extra = 0;
    assert(noise_session->ReadHandshakeMessage2(
        ConstByteSpan(handshake, InitiatorHandshake::kMinMessage2Size),
        ByteSpan(), extra).ok());
    assert(noise_session->WriteHandshakeMessage3(ConstByteSpan(), ByteSpan(handshake)).ok());
    assert(noise_session->isEstablished());
    crypto.seals = 0;
}
static void poison_noise_session() {
    if (!noise_session->isEstablished()) return;
    BodyChunkView body;
    assert(noise_session->StartOutboundBodyChunk(ServiceType::Daemon, 2, body,
        ByteSpan(svc_scratch), ByteSpan(env_scratch)).ok());
    crypto.fail_seal = true;
    assert(!noise_session->WriteNextOutboundWebSocketPayload(ByteSpan(ws_buf)).ok());
    assert(!noise_session->isEstablished());
}
