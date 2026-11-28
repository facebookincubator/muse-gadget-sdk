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

// Host harness for noise_core's PsaCryptoBackend built as the firmware builds
// it (NOISE_CORE_SECURE_RANDOM): each random draw only while
// secure_random_ready() says a radio is on; refused (FailedPrecondition) with
// the outputs untouched otherwise.

#include <array>
#include <cstdint>
#include <cstdio>

#include <xplat/noise/core/PsaCryptoBackend.h>

#include "secure_random.h"

namespace nc = musegadgets::noise::core;

static bool g_radio_on;
static int g_asked;

extern "C" bool secure_random_ready(void) {
  g_asked++;
  return g_radio_on;
}

static bool AllZero(const std::array<uint8_t, 32>& a) {
  for (uint8_t b : a) {
    if (b != 0) return false;
  }
  return true;
}

int main() {
  nc::PsaCryptoBackend backend;
  std::array<uint8_t, 32> r{}, priv{}, pub{};

  g_radio_on = false;
  if (backend.Random(nc::ByteSpan(r)) != nc::Status::FailedPrecondition() || !AllZero(r)) {
    std::printf("FAIL Random drew without a radio\n");
    return 1;
  }
  if (backend.X25519GenerateKeypair(nc::ByteSpan(priv), nc::ByteSpan(pub)) !=
          nc::Status::FailedPrecondition() ||
      !AllZero(priv) || !AllZero(pub)) {
    std::printf("FAIL X25519GenerateKeypair drew without a radio\n");
    return 1;
  }
  if (g_asked != 2) {
    std::printf("FAIL asked %d\n", g_asked);
    return 1;
  }

  g_radio_on = true;
  if (!backend.Random(nc::ByteSpan(r)).ok() || AllZero(r)) {
    std::printf("FAIL Random with the radio on\n");
    return 1;
  }
  if (!backend.X25519GenerateKeypair(nc::ByteSpan(priv), nc::ByteSpan(pub)).ok() || AllZero(priv) ||
      AllZero(pub)) {
    std::printf("FAIL X25519GenerateKeypair with the radio on\n");
    return 1;
  }
  std::array<uint8_t, 32> pub2{};
  if (!backend.X25519PublicFromPrivate(nc::ConstByteSpan(priv), nc::ByteSpan(pub2)).ok() || pub2 != pub) {
    std::printf("FAIL the key pair doesn't match\n");
    return 1;
  }
  std::printf("PASS noise draws gated\n");
  return 0;
}
