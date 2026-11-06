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

// ClientSession's zero-copy paths, through the real session, transport,
// framer and service codec:
//   SealBodyChunkInPlace == StartOutboundBodyChunk + WriteNextOutboundWebSocketPayload,
//     message after message, and a refused seal leaves the session as it was;
//   ProcessInboundWebSocketPayload returns a one-frame message in place,
//     reassembles a split one, and after running out of reassembly room
//     restores its receive state so the same payload can be retried.
// The crypto is a stand-in whose "tag" is the AEAD nonce, so the bytes are
// predictable and a nonce that moves when it shouldn't fails Open. Real
// AES-GCM is covered by test_noise_core.py.

#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/ServiceCodec.h>
#include <xplat/noise/core/TransportFrameCodec.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace tn = musegadgets::noise::core;

namespace {

int g_fail = 0;

#define EXPECT(cond)                                                     \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

class NonceTagCrypto : public tn::CryptoBackend {
 public:
  tn::Status fill(tn::ByteSpan out) noexcept {
    if (out.size()) std::memset(out.data(), 0x42, out.size());
    return tn::OkStatus();
  }
  tn::Status Random(tn::ByteSpan out) noexcept override { return fill(out); }
  tn::Status X25519GenerateKeypair(tn::ByteSpan priv, tn::ByteSpan pub) noexcept override {
    (void)fill(priv);
    return fill(pub);
  }
  tn::Status X25519PublicFromPrivate(tn::ConstByteSpan, tn::ByteSpan out) noexcept override {
    return fill(out);
  }
  tn::Status X25519Dh(tn::ConstByteSpan, tn::ConstByteSpan, tn::ByteSpan out) noexcept override {
    return fill(out);
  }
  tn::Status Sha256(tn::ConstByteSpan, tn::ByteSpan out) noexcept override { return fill(out); }
  tn::Status Sha256Concat(tn::ConstByteSpan, tn::ConstByteSpan, tn::ByteSpan out) noexcept override {
    return fill(out);
  }
  tn::Status HkdfSha256(tn::ConstByteSpan, tn::ConstByteSpan, tn::ConstByteSpan,
                        tn::Span<tn::ByteSpan> out) noexcept override {
    for (size_t i = 0; i < out.size(); ++i) (void)fill(out[i]);
    return tn::OkStatus();
  }
  // Ciphertext = plaintext; tag = the 12-byte nonce and four zero bytes.
  tn::Status Aes256GcmSeal(tn::ConstByteSpan, tn::ConstByteSpan nonce, tn::ConstByteSpan,
                           tn::ConstByteSpan in, tn::ByteSpan out) noexcept override {
    if (out.size() < in.size() + kAes256GcmTagSize) return tn::Status::ResourceExhausted();
    if (in.size()) std::memmove(out.data(), in.data(), in.size());
    std::memset(out.data() + in.size(), 0, kAes256GcmTagSize);
    std::memcpy(out.data() + in.size(), nonce.data(), nonce.size());
    return tn::OkStatus();
  }
  tn::Status Aes256GcmOpen(tn::ConstByteSpan, tn::ConstByteSpan nonce, tn::ConstByteSpan,
                           tn::ConstByteSpan in, tn::ByteSpan out) noexcept override {
    if (in.size() < kAes256GcmTagSize) return tn::Status::InvalidArgument();
    size_t size = in.size() - kAes256GcmTagSize;
    if (std::memcmp(in.data() + size, nonce.data(), nonce.size()) != 0) {
      return tn::Status::DataLoss();   // wrong nonce: the "tag" doesn't verify
    }
    if (out.size() < size) return tn::Status::ResourceExhausted();
    if (size) std::memmove(out.data(), in.data(), size);
    return tn::OkStatus();
  }
};

void Establish(tn::ClientSession& s) {
  uint8_t handshake[256] = {};
  EXPECT(s.WriteHandshakeMessage1(tn::ByteSpan(handshake)).ok());
  size_t extra = 0;
  EXPECT(s.ReadHandshakeMessage2(
              tn::ConstByteSpan(handshake, tn::InitiatorHandshake::kMinMessage2Size),
              tn::ByteSpan(), extra)
             .ok());
  EXPECT(s.WriteHandshakeMessage3(tn::ConstByteSpan(), tn::ByteSpan(handshake)).ok());
  EXPECT(s.isEstablished());
}

std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 7);
  return v;
}

// One body chunk through the copying path: the websocket payload bytes.
std::vector<uint8_t> CopySend(tn::ClientSession& s, int64_t stream, const std::vector<uint8_t>& data) {
  static std::vector<uint8_t> svc(20000), env(20000), ws(tn::ClientSession::kMaxOutboundWebSocketPayloadSize);
  tn::BodyChunkView body;
  body.data = tn::ConstByteSpan(data.data(), data.size());
  body.end_body = false;
  EXPECT(s.StartOutboundBodyChunk(tn::ServiceType::Daemon, stream, body,
                                  tn::ByteSpan(svc.data(), svc.size()),
                                  tn::ByteSpan(env.data(), env.size()))
             .ok());
  tn::StatusWithSize w = s.WriteNextOutboundWebSocketPayload(tn::ByteSpan(ws.data(), ws.size()));
  EXPECT(w.ok());
  EXPECT(!s.HasOutboundWebSocketPayload());
  return std::vector<uint8_t>(ws.begin(), ws.begin() + static_cast<long>(w.size()));
}

void CheckSealInPlace() {
  NonceTagCrypto ca, cb;
  tn::ClientSession a(ca), b(cb);
  Establish(a);
  Establish(b);
  const size_t head = tn::ClientSession::kMaxBodyChunkInPlaceHeadroom;
  const size_t sizes[] = {1, 300, 4096, 8192};
  for (size_t round = 0; round < 4; ++round) {
    const size_t n = sizes[round];
    std::vector<uint8_t> data = Pattern(n, static_cast<uint8_t>(round));
    // In place: data sits at `head`, with tailroom for the tag.
    std::vector<uint8_t> buf(head + n + tn::Transport::kTagSize, 0xEE);
    std::memcpy(buf.data() + head, data.data(), n);
    tn::ClientSession::InPlacePayload p = a.SealBodyChunkInPlace(
        tn::ServiceType::Daemon, 2, tn::ByteSpan(buf.data(), buf.size()), head, n);
    EXPECT(p.status.ok());
    std::vector<uint8_t> copied = CopySend(b, 2, data);
    EXPECT(p.size == copied.size());
    EXPECT(p.offset + p.size == buf.size());   // ends at the tag's last byte
    EXPECT(p.size == copied.size() &&
           std::memcmp(buf.data() + p.offset, copied.data(), copied.size()) == 0);

    // Refusals leave the session's nonce and chunk id alone: the next
    // round still matches the copying session.
    std::vector<uint8_t> tight(head + n, 0);   // no tailroom
    EXPECT(a.SealBodyChunkInPlace(tn::ServiceType::Daemon, 2,
                                  tn::ByteSpan(tight.data(), tight.size()), head, n)
               .status.IsResourceExhausted());
    std::vector<uint8_t> nohead(4 + n + tn::Transport::kTagSize, 0);   // no headroom
    EXPECT(a.SealBodyChunkInPlace(tn::ServiceType::Daemon, 2,
                                  tn::ByteSpan(nohead.data(), nohead.size()), 4, n)
               .status.IsResourceExhausted());
    EXPECT(a.isEstablished());
  }
  // While a multi-frame message is going out, an in-place seal is refused
  // (as the copying path would refuse a second message).
  std::vector<uint8_t> big = Pattern(tn::OrderedTransportFramer::kMaxChunkPayload + 10, 9);
  std::vector<uint8_t> svc(big.size() + 200), env(big.size() + 200);
  tn::BodyChunkView body;
  body.data = tn::ConstByteSpan(big.data(), big.size());
  EXPECT(a.StartOutboundBodyChunk(tn::ServiceType::Daemon, 2, body,
                                  tn::ByteSpan(svc.data(), svc.size()),
                                  tn::ByteSpan(env.data(), env.size()))
             .ok());
  std::vector<uint8_t> buf(head + 10 + tn::Transport::kTagSize, 0);
  EXPECT(a.SealBodyChunkInPlace(tn::ServiceType::Daemon, 2,
                                tn::ByteSpan(buf.data(), buf.size()), head, 10)
             .status.IsFailedPrecondition());
}

// The server side of an inbound body chunk: transport frames, each sealed
// with the stand-in crypto at the given receive nonce.
struct Server {
  tn::OrderedTransportFramer framer;
  uint64_t nonce = 0;
  std::vector<uint8_t> msg;

  void Start(int64_t stream, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> frame(data.size() + 64);
    tn::BodyChunkView body;
    body.data = tn::ConstByteSpan(data.data(), data.size());
    tn::StatusWithSize f = tn::EncodeBodyChunkFrame(stream, body, tn::ByteSpan(frame.data(), frame.size()));
    EXPECT(f.ok());
    msg.assign(data.size() + 128, 0);
    tn::StatusWithSize e = tn::EncodeServiceResponseEnvelope(
        tn::ConstByteSpan(frame.data(), f.size()), tn::ByteSpan(msg.data(), msg.size()));
    EXPECT(e.ok());
    msg.resize(e.size());
    framer.setNextChunkIdForTesting(static_cast<uint64_t>(1000 + stream));
    EXPECT(framer.StartOutboundMessage(tn::ConstByteSpan(msg.data(), msg.size())).ok());
  }
  std::vector<uint8_t> Next() {
    std::vector<uint8_t> out(tn::OrderedTransportFramer::kMaxEncodedFrameSize + 16);
    tn::StatusWithSize enc = framer.EncodeNextOutboundFrame(tn::ByteSpan(out.data(), out.size()));
    EXPECT(enc.ok());
    out.resize(enc.size() + 16);
    std::memset(out.data() + enc.size(), 0, 16);
    for (int i = 0; i < 8; ++i) {
      out[enc.size() + 4 + i] = static_cast<uint8_t>(nonce >> (56 - 8 * i));
    }
    ++nonce;
    return out;
  }
};

void CheckInbound() {
  NonceTagCrypto c;
  tn::ClientSession s(c);
  Establish(s);
  Server srv;
  std::vector<uint8_t> transport(tn::OrderedTransportFramer::kMaxEncodedFrameSize + 16);
  tn::HeaderView headers[4];

  // One frame: the body is returned where it was decrypted, and the
  // reassembly scratch is never touched.
  std::vector<uint8_t> small = Pattern(500, 1);
  srv.Start(5, small);
  std::vector<uint8_t> p = srv.Next();
  std::vector<uint8_t> reasm(64, 0x5A);
  tn::ClientSessionInboundResult r = s.ProcessInboundWebSocketPayload(
      tn::ConstByteSpan(p.data(), p.size()), tn::ByteSpan(transport.data(), transport.size()),
      tn::ByteSpan(reasm.data(), reasm.size()), tn::Span<tn::HeaderView>(headers, 4));
  EXPECT(r.ok() && r.frame_status == tn::InboundFrameStatus::Complete);
  EXPECT(r.frame.kind == tn::ServiceFrameKind::BodyChunk && r.frame.stream_id == 5);
  const tn::ConstByteSpan d = r.frame.body_chunk.data;
  EXPECT(d.size() == small.size() && std::memcmp(d.data(), small.data(), small.size()) == 0);
  EXPECT(d.data() >= transport.data() && d.data() < transport.data() + transport.size());
  EXPECT(reasm[0] == 0x5A && reasm[63] == 0x5A);

  // Split over two frames, with too little reassembly room the first time:
  // the session reports NeedMore/ResourceExhausted and rewinds its receive
  // nonce and framer, so the very same payload succeeds with more room.
  // (Without the rewind the retry fails: the stand-in tag is the nonce.)
  std::vector<uint8_t> big = Pattern(tn::OrderedTransportFramer::kMaxChunkPayload + 3000, 2);
  srv.Start(6, big);
  std::vector<uint8_t> f1 = srv.Next();
  std::vector<uint8_t> f2 = srv.Next();
  std::vector<uint8_t> tiny(1000);
  r = s.ProcessInboundWebSocketPayload(
      tn::ConstByteSpan(f1.data(), f1.size()), tn::ByteSpan(transport.data(), transport.size()),
      tn::ByteSpan(tiny.data(), tiny.size()), tn::Span<tn::HeaderView>(headers, 4));
  EXPECT(r.frame_status == tn::InboundFrameStatus::NeedMore && r.status.IsResourceExhausted());
  EXPECT(s.isEstablished());
  std::vector<uint8_t> room(big.size() + 200);
  r = s.ProcessInboundWebSocketPayload(
      tn::ConstByteSpan(f1.data(), f1.size()), tn::ByteSpan(transport.data(), transport.size()),
      tn::ByteSpan(room.data(), room.size()), tn::Span<tn::HeaderView>(headers, 4));
  EXPECT(r.ok() && r.frame_status == tn::InboundFrameStatus::NeedMore);
  r = s.ProcessInboundWebSocketPayload(
      tn::ConstByteSpan(f2.data(), f2.size()), tn::ByteSpan(transport.data(), transport.size()),
      tn::ByteSpan(room.data(), room.size()), tn::Span<tn::HeaderView>(headers, 4));
  EXPECT(r.ok() && r.frame_status == tn::InboundFrameStatus::Complete);
  const tn::ConstByteSpan bd = r.frame.body_chunk.data;
  EXPECT(bd.size() == big.size() && std::memcmp(bd.data(), big.data(), big.size()) == 0);
  EXPECT(bd.data() >= room.data() && bd.data() < room.data() + room.size());

  // A replayed payload (an old nonce) fails and the session is dead.
  r = s.ProcessInboundWebSocketPayload(
      tn::ConstByteSpan(f2.data(), f2.size()), tn::ByteSpan(transport.data(), transport.size()),
      tn::ByteSpan(room.data(), room.size()), tn::Span<tn::HeaderView>(headers, 4));
  EXPECT(!r.ok());
}

}  // namespace

int main() {
  CheckSealInPlace();
  CheckInbound();
  if (g_fail) {
    std::printf("%d failures\n", g_fail);
    return 1;
  }
  std::printf("session in-place ok\n");
  return 0;
}
