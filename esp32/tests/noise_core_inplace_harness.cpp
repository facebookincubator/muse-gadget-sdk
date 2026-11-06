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

// The zero-copy paths must produce the same bytes as the copying ones:
//   EncodeBodyChunkRequestPrefix + data == the envelope of a BodyChunk frame
//   OrderedTransportFramer::EncodeSingleFrameInPlace == Start + EncodeNext
//   DecodeInboundFrame hands a one-frame message back in place, and
//   reassembles split ones like DecodeAndAppendInboundFrame.

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

std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 31);
  return v;
}

void CheckEnvelopePrefix(tn::ServiceType service, int64_t streamId, size_t n) {
  std::vector<uint8_t> data = Pattern(n, static_cast<uint8_t>(n));
  // The copying path.
  std::vector<uint8_t> frame(n + 64);
  tn::BodyChunkView chunk;
  chunk.data = tn::ConstByteSpan(data.data(), data.size());
  chunk.end_body = false;
  tn::StatusWithSize f = tn::EncodeBodyChunkFrame(
      streamId, chunk, tn::ByteSpan(frame.data(), frame.size()));
  EXPECT(f.ok());
  std::vector<uint8_t> envelope(n + 128);
  tn::StatusWithSize e = tn::EncodeServiceRequestEnvelope(
      service,
      tn::ConstByteSpan(frame.data(), f.size()),
      tn::ByteSpan(envelope.data(), envelope.size()));
  EXPECT(e.ok());
  // The in-place path.
  uint8_t prefix[64];
  tn::StatusWithSize p = tn::EncodeBodyChunkRequestPrefix(
      service, streamId, n, tn::ByteSpan(prefix, sizeof(prefix)));
  EXPECT(p.ok());
  EXPECT(p.size() + n == e.size());
  EXPECT(std::memcmp(prefix, envelope.data(), p.size()) == 0);
  EXPECT(std::memcmp(envelope.data() + p.size(), data.data(), n) == 0);
  // Too small an output, and no data, are refused.
  EXPECT(!tn::EncodeBodyChunkRequestPrefix(
              service, streamId, n, tn::ByteSpan(prefix, p.size() - 1))
              .ok());
  EXPECT(!tn::EncodeBodyChunkRequestPrefix(
              service, streamId, 0, tn::ByteSpan(prefix, sizeof(prefix)))
              .ok());
}

void CheckSingleFrame(uint64_t chunkId, size_t n) {
  std::vector<uint8_t> msg = Pattern(n, static_cast<uint8_t>(chunkId));
  // The copying path.
  tn::OrderedTransportFramer a;
  a.setNextChunkIdForTesting(chunkId);
  EXPECT(a.StartOutboundMessage(tn::ConstByteSpan(msg.data(), n)).ok());
  std::vector<uint8_t> out(n + 64);
  tn::StatusWithSize enc =
      a.EncodeNextOutboundFrame(tn::ByteSpan(out.data(), out.size()));
  EXPECT(enc.ok());
  EXPECT(!a.HasOutboundMessage());
  // The in-place path.
  const size_t off = 40;
  std::vector<uint8_t> buf(off + n + 16, 0xEE);
  std::memcpy(buf.data() + off, msg.data(), n);
  tn::OrderedTransportFramer b;
  b.setNextChunkIdForTesting(chunkId);
  tn::OrderedTransportFramer::InPlaceFrame in =
      b.EncodeSingleFrameInPlace(tn::ByteSpan(buf.data(), buf.size()), off, n);
  EXPECT(in.status.ok());
  EXPECT(in.size == enc.size());
  EXPECT(in.offset + in.size == off + n);
  EXPECT(std::memcmp(buf.data() + in.offset, out.data(), in.size) == 0);
  EXPECT(!b.HasOutboundMessage());
  // And it decodes back to the message, in place.
  tn::OrderedTransportFramer r;
  std::vector<uint8_t> scratch(8, 0x5A);
  tn::ConstByteSpan view;
  tn::InboundFrameResult dec = r.DecodeInboundFrame(
      tn::ConstByteSpan(buf.data() + in.offset, in.size),
      tn::ByteSpan(scratch.data(), scratch.size()),
      view);
  EXPECT(dec.frame_status == tn::InboundFrameStatus::Complete);
  EXPECT(dec.size == n && view.size() == n);
  EXPECT(view.data() == buf.data() + off);
  EXPECT(scratch[0] == 0x5A);  // nothing copied
}

void CheckSingleFrameRefusals() {
  std::vector<uint8_t> buf(100, 0);
  tn::OrderedTransportFramer f;
  // No room for the fields in front of the message.
  f.setNextChunkIdForTesting(7);
  EXPECT(f.EncodeSingleFrameInPlace(tn::ByteSpan(buf.data(), buf.size()), 2, 50)
             .status.IsResourceExhausted());
  // Another message in progress.
  uint8_t msg[3] = {1, 2, 3};
  f.setNextChunkIdForTesting(8);
  EXPECT(f.StartOutboundMessage(tn::ConstByteSpan(msg, sizeof(msg))).ok());
  EXPECT(f.EncodeSingleFrameInPlace(tn::ByteSpan(buf.data(), buf.size()), 40, 10)
             .status.IsFailedPrecondition());
  // Larger than one frame.
  tn::OrderedTransportFramer g;
  std::vector<uint8_t> big(tn::OrderedTransportFramer::kMaxChunkPayload + 100);
  EXPECT(g.EncodeSingleFrameInPlace(
              tn::ByteSpan(big.data(), big.size()),
              64,
              tn::OrderedTransportFramer::kMaxChunkPayload + 1)
             .status.IsResourceExhausted());
}

void CheckSplitMessageReassembles() {
  const size_t n = tn::OrderedTransportFramer::kMaxChunkPayload + 1000;
  std::vector<uint8_t> msg = Pattern(n, 3);
  tn::OrderedTransportFramer tx;
  tx.setNextChunkIdForTesting(99);
  EXPECT(tx.StartOutboundMessage(tn::ConstByteSpan(msg.data(), n)).ok());
  tn::OrderedTransportFramer rx;
  std::vector<uint8_t> reassembly(n);
  std::vector<uint8_t> frame(tn::OrderedTransportFramer::kMaxEncodedFrameSize);
  int frames = 0;
  tn::ConstByteSpan view;
  tn::InboundFrameResult last;
  while (tx.HasOutboundMessage()) {
    tn::StatusWithSize enc =
        tx.EncodeNextOutboundFrame(tn::ByteSpan(frame.data(), frame.size()));
    EXPECT(enc.ok());
    last = rx.DecodeInboundFrame(
        tn::ConstByteSpan(frame.data(), enc.size()),
        tn::ByteSpan(reassembly.data(), reassembly.size()),
        view);
    ++frames;
  }
  EXPECT(frames == 2);
  EXPECT(last.frame_status == tn::InboundFrameStatus::Complete);
  EXPECT(view.data() == reassembly.data() && view.size() == n);
  EXPECT(std::memcmp(reassembly.data(), msg.data(), n) == 0);
}

}  // namespace

int main() {
  const tn::ServiceType services[] = {
      tn::ServiceType::Daemon, static_cast<tn::ServiceType>(1)};
  const int64_t streams[] = {1, 2, 127, 128, 300, 1LL << 40};
  const size_t sizes[] = {1, 2, 100, 127, 128, 1500, 8192, 16383, 16384, 60000};
  for (tn::ServiceType s : services) {
    for (int64_t id : streams) {
      for (size_t n : sizes) CheckEnvelopePrefix(s, id, n);
    }
  }
  const uint64_t ids[] = {0, 1, 127, 128, 0x123456789ABCull, ~0ull};
  for (uint64_t id : ids) {
    for (size_t n : sizes) CheckSingleFrame(id, n);
  }
  CheckSingleFrameRefusals();
  CheckSplitMessageReassembles();
  if (g_fail == 0) std::printf("ok\n");
  return g_fail == 0 ? 0 : 1;
}
