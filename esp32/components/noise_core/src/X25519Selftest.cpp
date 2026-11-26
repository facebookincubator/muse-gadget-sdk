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

// The backend's X25519 against RFC 7748 (sections 5.2 and 6.1, the 1,000-step
// iteration too when it is fast), against PSA's own X25519 on random keys, and
// timed: `>noise.x25519` on a Muse console runs it on its own task.

#include <xplat/noise/core/PsaCryptoBackend.h>

#include <psa/crypto.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(ESP_PLATFORM)
// FreeRTOS and esp_timer on the device: std::thread / std::chrono would pull
// pthread and libstdc++'s clocks into every image (352 B of internal .bss and
// up to 19 KB of flash, with HACL* on or off), for a console self-test.
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <chrono>
#include <thread>
#endif

namespace {

using musegadgets::noise::core::ByteSpan;
using musegadgets::noise::core::ConstByteSpan;
using musegadgets::noise::core::PsaCryptoBackend;
using Key = std::array<uint8_t, 32>;

Key FromHex(const char* hex) {
  Key out{};
  for (size_t i = 0; i < out.size(); i++) {
    auto nib = [](char c) -> uint8_t {
      return static_cast<uint8_t>(
          c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    };
    out[i] = static_cast<uint8_t>(nib(hex[2 * i]) << 4 | nib(hex[2 * i + 1]));
  }
  return out;
}

#if defined(ESP_PLATFORM)
int64_t NowUs() {
  return esp_timer_get_time();
}

void SleepAMoment() {
  vTaskDelay(pdMS_TO_TICKS(1) > 0 ? pdMS_TO_TICKS(1) : 1);
}
#else
int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void SleepAMoment() {
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
#endif

// PSA's X25519, as the backend does it without HACL*.
bool PsaX25519(Key& out, const Key& scalar, const Key& u) {
  Key k = scalar;
  k[0] &= 248;
  k[31] &= 127;
  k[31] |= 64;
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&a, PSA_ALG_ECDH);
  psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
  psa_set_key_bits(&a, 255);
  psa_key_id_t id = 0;
  bool ok = psa_import_key(&a, k.data(), k.size(), &id) == PSA_SUCCESS;
  size_t n = 0;
  ok = ok &&
      psa_raw_key_agreement(
          PSA_ALG_ECDH, id, u.data(), u.size(), out.data(), out.size(), &n) ==
          PSA_SUCCESS &&
      n == out.size();
  if (id) {
    psa_destroy_key(id);
  }
  return ok;
}

bool Dh(PsaCryptoBackend& b, Key& out, const Key& k, const Key& u) {
  return b.X25519Dh(ConstByteSpan(k), ConstByteSpan(u), ByteSpan(out)).ok();
}

} // namespace

extern "C" void noise_core_x25519_selftest(void) {
  PsaCryptoBackend backend;
  if (psa_crypto_init() != PSA_SUCCESS) {
    std::printf("@x25519 PSA init failed\n");
    return;
  }
#if NOISE_CORE_X25519_HACL
  const char* impl = "HACL*";
  const int iterations = 1000;
#else
  const char* impl = "PSA";
  const int iterations = 1;
#endif
  int pass = 0, total = 0;
  auto check = [&](bool ok, const char* what) {
    total++;
    pass += ok ? 1 : 0;
    if (!ok) {
      std::printf("@x25519 FAIL %s\n", what);
    }
  };

  // RFC 7748 section 5.2.
  static const char* const kVectors[][3] = {
      {"a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
       "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
       "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"},
      {"4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
       "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
       "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"},
  };
  for (const auto& v : kVectors) {
    Key out{};
    check(
        Dh(backend, out, FromHex(v[0]), FromHex(v[1])) &&
            out == FromHex(v[2]),
        "5.2 vector");
  }
  // RFC 7748 section 5.2, iterated: k = X25519(k, u), u = old k.
  {
    Key k{}, u{}, r{};
    k[0] = 9;
    u[0] = 9;
    bool ok = true;
    for (int i = 1; i <= iterations && ok; i++) {
      // Sleep between iterations, every one: a device's other tasks and its
      // idle tasks (the task watchdog's, 15 s) run between; the self-test's
      // first version, without sleeps, tripped the task watchdog.
      SleepAMoment();
      ok = Dh(backend, r, k, u);
      u = k;
      k = r;
      if (i == 1) {
        check(
            ok &&
                k ==
                    FromHex(
                        "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"),
            "5.2 one iteration");
      }
    }
    if (iterations == 1000) {
      check(
          ok &&
              k ==
                  FromHex(
                      "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"),
          "5.2 1,000 iterations");
    }
  }
  // RFC 7748 section 6.1: key pairs and the shared secret both ways.
  {
    const Key a = FromHex(
        "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const Key b = FromHex(
        "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    Key ap{}, bp{}, s1{}, s2{};
    check(
        backend.X25519PublicFromPrivate(ConstByteSpan(a), ByteSpan(ap)).ok() &&
            ap ==
                FromHex(
                    "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"),
        "6.1 Alice's public key");
    check(
        backend.X25519PublicFromPrivate(ConstByteSpan(b), ByteSpan(bp)).ok() &&
            bp ==
                FromHex(
                    "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"),
        "6.1 Bob's public key");
    const Key k = FromHex(
        "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    check(Dh(backend, s1, a, bp) && s1 == k, "6.1 Alice's shared secret");
    check(Dh(backend, s2, b, ap) && s2 == k, "6.1 Bob's shared secret");
  }
  // Against PSA's X25519 on random keys (a generated key pair's public key is
  // the peer), timing both.
  int same = 0;
  int64_t backendUs = 0, psaUs = 0;
  const int kRandom = 4;
  for (int i = 0; i < kRandom; i++) {
    Key k{}, peerPriv{}, peer{}, x{}, y{};
    bool okPeer = backend.Random(ByteSpan(k)).ok() &&
        backend.Random(ByteSpan(peerPriv)).ok() &&
        backend.X25519PublicFromPrivate(ConstByteSpan(peerPriv), ByteSpan(peer))
            .ok();
    int64_t t0 = NowUs();
    bool okB = okPeer && Dh(backend, x, k, peer);
    int64_t t1 = NowUs();
    bool okP = PsaX25519(y, k, peer);
    int64_t t2 = NowUs();
    backendUs += t1 - t0;
    psaUs += t2 - t1;
    same += okB && okP && x == y ? 1 : 0;
  }
  check(same == kRandom, "same as PSA on random keys");
  std::printf(
      "@x25519 %s: %d/%d checks pass (RFC 7748 5.2%s, 6.1; %d/%d random keys same as PSA); "
      "backend %lld us an operation, PSA %lld us\n",
      impl,
      pass,
      total,
      iterations == 1000 ? " incl. 1,000 iterations" : "",
      same,
      kRandom,
      static_cast<long long>(backendUs / kRandom),
      static_cast<long long>(psaUs / kRandom));
}
