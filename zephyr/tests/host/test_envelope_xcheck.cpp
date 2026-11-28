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

// mg_envelope.c against esp32/components/noise_core's ServiceCodec.cpp, the
// codec the Wi-Fi gadgets and the vectors follow: random frames and
// envelopes must encode to the same bytes (or both be refused), and random
// and damaged bytes must decode to the same fields (or both be refused).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "mg_envelope.h"
}

#include <xplat/noise/core/ServiceCodec.h>

using namespace musegadgets::noise::core;

namespace {

int checks, failures;

#define XCHECK(cond, ...)                                                     \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      if (failures++ < 20) {                                                   \
        std::fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond);        \
        std::fprintf(stderr, __VA_ARGS__);                                     \
        std::fprintf(stderr, "\n");                                            \
      }                                                                        \
    }                                                                          \
  } while (0)

struct Rng {
  uint64_t s;
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<uint32_t>(s >> 11);
  }
  uint32_t below(uint32_t n) { return next() % n; }
};

std::string rand_str(Rng& r, size_t max) {
  std::string s(r.below(static_cast<uint32_t>(max + 1)), '\0');
  for (auto& c : s) {
    c = static_cast<char>(r.below(256));
  }
  return s;
}

int64_t rand_i64(Rng& r) {
  switch (r.below(5)) {
    case 0:
      return 0;
    case 1:
      return r.below(200);
    case 2:
      return -static_cast<int64_t>(r.below(1000)) - 1;
    case 3:
      return static_cast<int64_t>((static_cast<uint64_t>(r.next()) << 32) | r.next());
    default:
      return r.below(1u << 30);
  }
}

// One frame, in both codecs' terms, keeping the strings alive.
struct Both {
  std::string verb, path, body, reason;
  std::vector<std::pair<std::string, std::string>> hdrs;
  std::vector<HeaderView> cpp_h;
  std::vector<mg_env_header> c_h;
  ServiceFrameView cpp{};
  mg_env_frame c{};
};

mg_env_bytes bytes(const std::string& s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

void make(Rng& r, Both& b) {
  b.verb = r.below(3) ? std::string("GET") : rand_str(r, 6);
  b.path = r.below(4) ? "/" + rand_str(r, 20) : rand_str(r, 4);
  b.body = rand_str(r, r.below(4) ? 40 : 300);
  b.reason = rand_str(r, 12);
  size_t nh = r.below(4);
  for (size_t i = 0; i < nh; i++) {
    b.hdrs.emplace_back(rand_str(r, 8), rand_str(r, 8));
  }
  for (auto& h : b.hdrs) {
    b.cpp_h.push_back({StringView(h.first), StringView(h.second)});
    b.c_h.push_back({bytes(h.first), bytes(h.second)});
  }
  int64_t stream = r.below(8) ? rand_i64(r) : 0;
  bool end = r.below(2);
  int32_t status = static_cast<int32_t>(rand_i64(r));
  int32_t code = static_cast<int32_t>(r.below(3) ? r.below(7) : rand_i64(r));
  ServiceFrameKind kinds[] = {ServiceFrameKind::None, ServiceFrameKind::Request,
                              ServiceFrameKind::Response, ServiceFrameKind::BodyChunk,
                              ServiceFrameKind::Reset};
  mg_env_kind ckinds[] = {MG_ENV_NONE, MG_ENV_REQUEST, MG_ENV_RESPONSE, MG_ENV_BODY_CHUNK,
                          MG_ENV_RESET};
  uint32_t k = r.below(16) ? 1 + r.below(4) : 0;

  b.cpp.stream_id = stream;
  b.cpp.kind = kinds[k];
  b.cpp.request = {StringView(b.verb), StringView(b.path),
                   Span<const HeaderView>(b.cpp_h.data(), b.cpp_h.size()),
                   ConstByteSpan(reinterpret_cast<const uint8_t*>(b.body.data()), b.body.size()),
                   end};
  b.cpp.response = {status, Span<const HeaderView>(b.cpp_h.data(), b.cpp_h.size()),
                    ConstByteSpan(reinterpret_cast<const uint8_t*>(b.body.data()),
                                  b.body.size()),
                    end};
  b.cpp.body_chunk = {
      ConstByteSpan(reinterpret_cast<const uint8_t*>(b.body.data()), b.body.size()), end};
  b.cpp.reset = {static_cast<ResetCode>(code), StringView(b.reason)};

  b.c.stream_id = stream;
  b.c.kind = ckinds[k];
  b.c.verb = bytes(b.verb);
  b.c.path = bytes(b.path);
  b.c.headers = b.c_h.data();
  b.c.n_headers = b.c_h.size();
  b.c.body = bytes(b.body);
  b.c.end_body = end;
  b.c.status = status;
  b.c.reset_code = code;
  b.c.reset_reason = bytes(b.reason);
}

std::string hex(const uint8_t* p, size_t n) {
  std::string s;
  char t[4];
  for (size_t i = 0; i < n && i < 64; i++) {
    std::snprintf(t, sizeof(t), "%02x", p[i]);
    s += t;
  }
  return s;
}

void test_encode(Rng& r, int rounds) {
  static uint8_t a[2048], c[2048];
  for (int i = 0; i < rounds; i++) {
    Both b;
    make(r, b);
    size_t cap = r.below(8) ? sizeof(a) : r.below(64);
    // Frames.
    auto sws = EncodeServiceFrame(b.cpp, ByteSpan(a, cap));
    int n = mg_env_encode_frame(&b.c, c, cap);
    XCHECK(sws.ok() == (n >= 0), "frame round %d: cpp %d c %d", i, sws.ok(), n);
    if (sws.ok() && n >= 0) {
      XCHECK(sws.size() == static_cast<size_t>(n) && std::memcmp(a, c, n) == 0,
             "frame round %d: %s vs %s", i, hex(a, sws.size()).c_str(), hex(c, n).c_str());
    }
    if (!sws.ok() || n < 0) {
      continue;
    }
    // The frame in each envelope.
    size_t flen = static_cast<size_t>(n);
    uint8_t frame[2048];
    std::memcpy(frame, c, flen);
    uint8_t svc = static_cast<uint8_t>(r.below(8));   // 4, 5 and 7 refused by both, 6 (Gadget) taken
    size_t ecap = r.below(8) ? sizeof(a) : flen + r.below(4);
    sws = EncodeServiceRequestEnvelope(static_cast<ServiceType>(svc),
                                       ConstByteSpan(frame, flen), ByteSpan(a, ecap));
    n = mg_env_encode_request_envelope(svc, frame, flen, c, ecap);
    XCHECK(sws.ok() == (n >= 0) && (!sws.ok() || (sws.size() == static_cast<size_t>(n) &&
                                                   std::memcmp(a, c, n) == 0)),
           "request envelope round %d", i);
    if (sws.ok()) {
      int m = mg_env_encode_request_message(svc, &b.c, c, ecap);
      XCHECK(m == n && std::memcmp(a, c, n) == 0, "request message round %d", i);
    }
    sws = EncodeServiceResponseEnvelope(ConstByteSpan(frame, flen), ByteSpan(a, ecap));
    n = mg_env_encode_response_envelope(frame, flen, c, ecap);
    XCHECK(sws.ok() == (n >= 0) && (!sws.ok() || (sws.size() == static_cast<size_t>(n) &&
                                                   std::memcmp(a, c, n) == 0)),
           "response envelope round %d", i);
    if (sws.ok()) {
      int m = mg_env_encode_response_message(&b.c, c, ecap);
      XCHECK(m == n && std::memcmp(a, c, n) == 0, "response message round %d", i);
    }
    // The body chunk prefix.
    size_t dlen = 1 + r.below(70000);
    int64_t stream = r.below(8) ? rand_i64(r) : 0;
    size_t pcap = r.below(8) ? 32 : r.below(12);
    sws = EncodeBodyChunkRequestPrefix(static_cast<ServiceType>(svc), stream, dlen,
                                       ByteSpan(a, pcap));
    n = mg_env_body_chunk_request_prefix(svc, stream, dlen, c, pcap);
    XCHECK(sws.ok() == (n >= 0) && (!sws.ok() || (sws.size() == static_cast<size_t>(n) &&
                                                   std::memcmp(a, c, n) == 0)),
           "prefix round %d: svc %u stream %lld len %zu", i, svc,
           static_cast<long long>(stream), dlen);
  }
}

bool same_bytes(StringView a, mg_env_bytes b) {
  return a.size() == b.n && (b.n == 0 || std::memcmp(a.data(), b.p, b.n) == 0);
}

bool same_bytes(ConstByteSpan a, mg_env_bytes b) {
  return a.size() == b.n && (b.n == 0 || std::memcmp(a.data(), b.p, b.n) == 0);
}

void compare_decode(const uint8_t* p, size_t n, int round) {
  HeaderView ch[3];
  mg_env_header cc[3];
  DecodedServiceFrame a;
  mg_env_frame b;
  auto res = DecodeServiceFrameDetailed(ConstByteSpan(p, n), Span<HeaderView>(ch, 3), a);
  int err = mg_env_decode_frame(p, n, cc, 3, &b);
  XCHECK(res.ok() == (err == 0), "decode round %d: cpp %d c %d (%s)", round, res.ok(), err,
         hex(p, n).c_str());
  if (res.ok() && err == 0) {
    bool same = a.stream_id == b.stream_id && static_cast<int>(a.kind) == static_cast<int>(b.kind);
    switch (a.kind) {
      case ServiceFrameKind::Request:
        same = same && same_bytes(a.request.verb, b.verb) && same_bytes(a.request.path, b.path) &&
            same_bytes(a.request.body, b.body) && a.request.end_body == b.end_body &&
            a.request.headers.size() == b.n_headers;
        for (size_t i = 0; same && i < b.n_headers; i++) {
          same = same_bytes(a.request.headers[i].key, b.headers[i].key) &&
              same_bytes(a.request.headers[i].value, b.headers[i].value);
        }
        break;
      case ServiceFrameKind::Response:
        same = same && a.response.status == b.status && same_bytes(a.response.body, b.body) &&
            a.response.end_body == b.end_body && a.response.headers.size() == b.n_headers;
        for (size_t i = 0; same && i < b.n_headers; i++) {
          same = same_bytes(a.response.headers[i].key, b.headers[i].key) &&
              same_bytes(a.response.headers[i].value, b.headers[i].value);
        }
        break;
      case ServiceFrameKind::BodyChunk:
        same = same && same_bytes(a.body_chunk.data, b.body) && a.body_chunk.end_body == b.end_body;
        break;
      case ServiceFrameKind::Reset:
        same = same && static_cast<int32_t>(a.reset.code) == b.reset_code &&
            same_bytes(a.reset.reason, b.reset_reason);
        break;
      default:
        break;
    }
    XCHECK(same, "decode round %d: fields differ (%s)", round, hex(p, n).c_str());
  }
  // The envelopes too.
  DecodedServiceRequestEnvelope re;
  mg_env_bytes pl;
  uint8_t svc = 0;
  res = DecodeServiceRequestEnvelopeDetailed(ConstByteSpan(p, n), re);
  err = mg_env_decode_request_envelope(p, n, &svc, &pl);
  XCHECK(res.ok() == (err == 0) &&
             (!res.ok() || (static_cast<int>(re.service) == svc && same_bytes(re.payload, pl))),
         "request envelope decode round %d", round);
  DecodedServiceResponseEnvelope se;
  res = DecodeServiceResponseEnvelopeDetailed(ConstByteSpan(p, n), se);
  err = mg_env_decode_response_envelope(p, n, &pl);
  XCHECK(res.ok() == (err == 0) && (!res.ok() || same_bytes(se.payload, pl)),
         "response envelope decode round %d", round);
}

void test_decode(Rng& r, int rounds) {
  static uint8_t buf[2048];
  for (int i = 0; i < rounds; i++) {
    size_t n = 0;
    if (r.below(4)) {
      // A valid frame, then maybe damaged.
      Both b;
      make(r, b);
      int len = mg_env_encode_frame(&b.c, buf, sizeof(buf));
      if (len <= 0) {
        continue;
      }
      n = static_cast<size_t>(len);
      switch (r.below(5)) {
        case 0:
          break;
        case 1:
          buf[r.below(static_cast<uint32_t>(n))] ^= static_cast<uint8_t>(1u << r.below(8));
          break;
        case 2:
          n = r.below(static_cast<uint32_t>(n));
          break;
        case 3:
          buf[r.below(static_cast<uint32_t>(n))] = static_cast<uint8_t>(r.below(256));
          buf[r.below(static_cast<uint32_t>(n))] = static_cast<uint8_t>(r.below(256));
          break;
        default:
          // An unknown field in front.
          std::memmove(&buf[3], buf, n);
          buf[0] = static_cast<uint8_t>((r.below(30) + 6) << 3 | r.below(8));
          buf[1] = static_cast<uint8_t>(r.below(3));
          buf[2] = static_cast<uint8_t>(r.below(256));
          n += 3;
          break;
      }
    } else {
      // Noise.
      n = r.below(24);
      for (size_t k = 0; k < n; k++) {
        buf[k] = static_cast<uint8_t>(r.below(256));
      }
    }
    compare_decode(buf, n, i);
  }
}

}  // namespace

int main() {
  Rng r{0x9e3779b97f4a7c15ull};
  test_encode(r, 20000);
  test_decode(r, 50000);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
