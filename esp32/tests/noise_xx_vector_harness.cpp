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

// The noise_core crypto backend (X25519 from HACL* or PSA, HKDF, AES-GCM,
// SHA-256) run through the Noise_XX_25519_AESGCM_SHA256 test vector from
// cacophony (vectors/cacophony.txt): both roles, every handshake and transport
// message byte for byte, and the handshake hash. Then the backend's X25519
// against RFC 7748 and against PSA's own X25519 on random keys, and the
// device console's X25519 self-test (X25519Selftest.cpp).

#include <xplat/noise/core/PsaCryptoBackend.h>

#include <psa/crypto.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" void noise_core_x25519_selftest(void);

namespace {

namespace tn = musegadgets::noise::core;
using Bytes = std::vector<uint8_t>;
using Key = std::array<uint8_t, 32>;

Bytes Hex(const char* s) {
  Bytes out;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    auto nib = [](char c) -> uint8_t {
      return static_cast<uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    };
    out.push_back(static_cast<uint8_t>(nib(s[i]) << 4 | nib(s[i + 1])));
  }
  return out;
}

Key ToKey(const Bytes& b) {
  Key k{};
  std::memcpy(k.data(), b.data(), k.size());
  return k;
}

// One side's Noise symmetric state (Noise spec section 5.2).
struct Symmetric {
  tn::CryptoBackend& b;
  Key h{}, ck{}, k{};
  bool hasKey = false;
  uint64_t n = 0;
  bool ok = true;

  explicit Symmetric(tn::CryptoBackend& backend) : b(backend) {
    const char name[] = "Noise_XX_25519_AESGCM_SHA256";
    std::memcpy(h.data(), name, sizeof(name) - 1);
    ck = h;
  }
  void MixHash(const Bytes& data) {
    Key out{};
    ok = ok && b.Sha256Concat(tn::ConstByteSpan(h), tn::ConstByteSpan(data.data(), data.size()), tn::ByteSpan(out)).ok();
    h = out;
  }
  void MixKey(const Key& ikm) {
    Key nextCk{}, tempK{};
    std::array<tn::ByteSpan, 2> outs = {{tn::ByteSpan(nextCk), tn::ByteSpan(tempK)}};
    ok = ok && b.HkdfSha256(tn::ConstByteSpan(ck), tn::ConstByteSpan(ikm), tn::ConstByteSpan(), tn::Span<tn::ByteSpan>(outs)).ok();
    ck = nextCk;
    k = tempK;
    hasKey = true;
    n = 0;
  }
  static std::array<uint8_t, 12> Nonce(uint64_t n) {
    std::array<uint8_t, 12> out{};
    for (int i = 0; i < 8; i++) {
      out[4 + i] = static_cast<uint8_t>(n >> (56 - 8 * i));
    }
    return out;
  }
  Bytes EncryptAndHash(const Bytes& pt) {
    Bytes ct = pt;
    if (hasKey) {
      ct.assign(pt.size() + 16, 0);
      ok = ok && Seal(k, n++, h, pt, ct);
    }
    MixHash(ct);
    return ct;
  }
  Bytes DecryptAndHash(const Bytes& ct) {
    Bytes pt = ct;
    if (hasKey) {
      pt.assign(ct.size() - 16, 0);
      ok = ok && b.Aes256GcmOpen(tn::ConstByteSpan(k), tn::ConstByteSpan(Nonce(n++)), tn::ConstByteSpan(h),
                                 tn::ConstByteSpan(ct.data(), ct.size()), tn::ByteSpan(pt.data(), pt.size())).ok();
    }
    MixHash(ct);
    return pt;
  }
  bool Seal(const Key& key, uint64_t nonce, const Key& ad, const Bytes& pt, Bytes& ct) {
    return b.Aes256GcmSeal(tn::ConstByteSpan(key), tn::ConstByteSpan(Nonce(nonce)), tn::ConstByteSpan(ad),
                           tn::ConstByteSpan(pt.data(), pt.size()), tn::ByteSpan(ct.data(), ct.size())).ok();
  }
  void Split(Key& c1, Key& c2) {
    std::array<tn::ByteSpan, 2> outs = {{tn::ByteSpan(c1), tn::ByteSpan(c2)}};
    ok = ok && b.HkdfSha256(tn::ConstByteSpan(ck), tn::ConstByteSpan(), tn::ConstByteSpan(), tn::Span<tn::ByteSpan>(outs)).ok();
  }
};

Key Dh(tn::CryptoBackend& b, const Key& priv, const Key& pub, bool& ok) {
  Key out{};
  ok = ok && b.X25519Dh(tn::ConstByteSpan(priv), tn::ConstByteSpan(pub), tn::ByteSpan(out)).ok();
  return out;
}

Key Pub(tn::CryptoBackend& b, const Key& priv, bool& ok) {
  Key out{};
  ok = ok && b.X25519PublicFromPrivate(tn::ConstByteSpan(priv), tn::ByteSpan(out)).ok();
  return out;
}

Bytes Cat(const Bytes& a, const Bytes& b) {
  Bytes out = a;
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

int Fail(const char* what) {
  std::printf("noise XX vector: FAIL %s\n", what);
  return 1;
}

// cacophony vectors/cacophony.txt, protocol_name Noise_XX_25519_AESGCM_SHA256.
int CheckXxVector(tn::CryptoBackend& b) {
  const Bytes prologue = Hex("4a6f686e2047616c74");
  const Key is = ToKey(Hex("e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1"));
  const Key ie = ToKey(Hex("893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a"));
  const Key rs = ToKey(Hex("4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893"));
  const Key re = ToKey(Hex("bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b"));
  const Bytes hash = Hex("1b7aefb1125762aa21a252890d00af54519638b76437444538f9a52f21e2e0dc");
  struct Msg {
    const char* payload;
    const char* ciphertext;
  };
  static const Msg kMsgs[] = {
      {"4c756477696720766f6e204d69736573",
       "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79444c756477696720766f6e204d69736573"},
      {"4d757272617920526f746862617264",
       "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843757117acceb05bd7a45733bc22015c97a9d0cbaf41b80"
       "446d5988ff5127235d76b79eade70f473d6a4ef521fdcbeda5340d01e028ba793fc059f2724a83af05f12dda0448a7621a926b379a92477fd"},
      {"462e20412e20486179656b",
       "c90f1cf77eba4e50edb038991565e36c9758943a989229b6051244dc4fbecb6946744b401af2ee1a5881b65fbb87fd07cb6a328ececc9ce6"
       "ce84c399dc332d4fd521fa4bb7f467ce909395"},
  };
  bool ok = true;
  const Key iePub = Pub(b, ie, ok), isPub = Pub(b, is, ok), rePub = Pub(b, re, ok), rsPub = Pub(b, rs, ok);
  Symmetric I(b), R(b);
  I.MixHash(prologue);
  R.MixHash(prologue);

  // -> e
  Bytes m0 = Bytes(iePub.begin(), iePub.end());
  I.MixHash(m0);
  m0 = Cat(m0, I.EncryptAndHash(Hex(kMsgs[0].payload)));
  if (m0 != Hex(kMsgs[0].ciphertext)) return Fail("message 0");
  R.MixHash(Bytes(m0.begin(), m0.begin() + 32));
  if (R.DecryptAndHash(Bytes(m0.begin() + 32, m0.end())) != Hex(kMsgs[0].payload)) return Fail("message 0 read");

  // <- e, ee, s, es
  Bytes m1 = Bytes(rePub.begin(), rePub.end());
  R.MixHash(m1);
  R.MixKey(Dh(b, re, iePub, ok));
  m1 = Cat(m1, R.EncryptAndHash(Bytes(rsPub.begin(), rsPub.end())));
  R.MixKey(Dh(b, rs, iePub, ok));
  m1 = Cat(m1, R.EncryptAndHash(Hex(kMsgs[1].payload)));
  if (m1 != Hex(kMsgs[1].ciphertext)) return Fail("message 1");
  const Key reSeen = ToKey(Bytes(m1.begin(), m1.begin() + 32));
  I.MixHash(Bytes(m1.begin(), m1.begin() + 32));
  I.MixKey(Dh(b, ie, reSeen, ok));
  const Key rsSeen = ToKey(I.DecryptAndHash(Bytes(m1.begin() + 32, m1.begin() + 80)));
  I.MixKey(Dh(b, ie, rsSeen, ok));
  if (I.DecryptAndHash(Bytes(m1.begin() + 80, m1.end())) != Hex(kMsgs[1].payload)) return Fail("message 1 read");

  // -> s, se
  Bytes m2 = I.EncryptAndHash(Bytes(isPub.begin(), isPub.end()));
  I.MixKey(Dh(b, is, reSeen, ok));
  m2 = Cat(m2, I.EncryptAndHash(Hex(kMsgs[2].payload)));
  if (m2 != Hex(kMsgs[2].ciphertext)) return Fail("message 2");
  const Key isSeen = ToKey(R.DecryptAndHash(Bytes(m2.begin(), m2.begin() + 48)));
  R.MixKey(Dh(b, re, isSeen, ok));
  if (R.DecryptAndHash(Bytes(m2.begin() + 48, m2.end())) != Hex(kMsgs[2].payload)) return Fail("message 2 read");

  if (!ok || !I.ok || !R.ok) return Fail("a backend call");
  if (Bytes(I.h.begin(), I.h.end()) != hash || R.h != I.h) return Fail("handshake hash");

  // Transport messages: initiator's c1 out, the responder's c2 out, with an
  // empty associated data.
  Key c1{}, c2{}, d1{}, d2{};
  I.Split(c1, c2);
  R.Split(d1, d2);
  if (c1 != d1 || c2 != d2) return Fail("split");
  // Messages 3-5 alternate from the responder: 3 and 5 on c2, 4 on c1.
  static const Msg kTransport[] = {
      {"4361726c204d656e676572", "bc3fa77f6aca3e8466d7dc6bea10013e88a6a29add5132b461806c"},
      {"4a65616e2d426170746973746520536179", "250b01074cdfe0df2ecf8ccbf1737b15a2ddb5b52fd9a396604e9c793cee3b3bb9"},
      {"457567656e2042f6686d20766f6e2042617765726b",
       "449d4d433b3cdc3d02bf6fc881774b9df54366ebcffb9689bb13f14709822cd7ef42bcdb4d"},
  };
  uint64_t n1 = 0, n2 = 0;
  const Key noAd{};
  for (int i = 0; i < 3; i++) {
    const bool fromResponder = i % 2 == 0;
    const Bytes pt = Hex(kTransport[i].payload);
    Bytes ct(pt.size() + 16, 0);
    // An empty associated data: Seal with a zero-length span, not the key-sized one.
    if (!b.Aes256GcmSeal(tn::ConstByteSpan(fromResponder ? c2 : c1), tn::ConstByteSpan(Symmetric::Nonce(fromResponder ? n2++ : n1++)),
                         tn::ConstByteSpan(noAd.data(), 0), tn::ConstByteSpan(pt.data(), pt.size()),
                         tn::ByteSpan(ct.data(), ct.size())).ok() ||
        ct != Hex(kTransport[i].ciphertext)) {
      return Fail("transport message");
    }
  }
  std::printf("noise XX vector: 3 handshake + 3 transport messages and the handshake hash match\n");
  return 0;
}

// PSA's X25519 directly, as the backend does it without HACL*.
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
      psa_raw_key_agreement(PSA_ALG_ECDH, id, u.data(), u.size(), out.data(), out.size(), &n) == PSA_SUCCESS &&
      n == out.size();
  if (id) {
    psa_destroy_key(id);
  }
  return ok;
}

// RFC 7748 sections 5.2 (both vectors, 1 and 1,000 iterations) and 6.1 (both
// key pairs, the shared secret both ways), then random keys against PSA.
int CheckX25519(tn::CryptoBackend& b) {
  int pass = 0, total = 0;
  auto check = [&](bool ok, const char* what) {
    total++;
    pass += ok ? 1 : 0;
    if (!ok) {
      std::printf("x25519: FAIL %s\n", what);
    }
  };
  static const char* const kVectors[][3] = {
      {"a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
       "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
       "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"},
      {"4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
       "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
       "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"},
  };
  for (const auto& v : kVectors) {
    bool ok = true;
    check(Dh(b, ToKey(Hex(v[0])), ToKey(Hex(v[1])), ok) == ToKey(Hex(v[2])) && ok, "5.2 vector");
  }
  {
    // k = X25519(k, u), u = the previous k, from k = u = 9.
    Key k{}, u{};
    k[0] = 9;
    u[0] = 9;
    bool ok = true;
    for (int i = 1; i <= 1000 && ok; i++) {
      const Key r = Dh(b, k, u, ok);
      u = k;
      k = r;
      if (i == 1) {
        check(ok && k == ToKey(Hex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079")),
              "5.2 one iteration");
      }
    }
    check(ok && k == ToKey(Hex("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51")),
          "5.2 1,000 iterations");
  }
  {
    const Key a = ToKey(Hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"));
    const Key bob = ToKey(Hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"));
    const Key shared = ToKey(Hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    bool ok = true;
    const Key ap = Pub(b, a, ok), bp = Pub(b, bob, ok);
    check(ok && ap == ToKey(Hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")),
          "6.1 Alice's public key");
    check(ok && bp == ToKey(Hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")),
          "6.1 Bob's public key");
    check(Dh(b, a, bp, ok) == shared && ok, "6.1 Alice's shared secret");
    check(Dh(b, bob, ap, ok) == shared && ok, "6.1 Bob's shared secret");
  }
  const int kRandom = 4;
  int same = 0;
  for (int i = 0; i < kRandom; i++) {
    Key k{}, peerPriv{}, y{};
    bool ok = b.Random(tn::ByteSpan(k)).ok() && b.Random(tn::ByteSpan(peerPriv)).ok();
    const Key peer = Pub(b, peerPriv, ok);
    const Key x = Dh(b, k, peer, ok);
    same += ok && PsaX25519(y, k, peer) && x == y ? 1 : 0;
  }
  check(same == kRandom, "same as PSA on random keys");
  std::printf("x25519: %d/%d checks pass (RFC 7748 5.2 incl. 1,000 iterations, 6.1; %d/%d random keys same as PSA)\n",
              pass, total, same, kRandom);
  return pass == total ? 0 : 1;
}

}  // namespace

int main() {
  tn::PsaCryptoBackend backend;
  int r = CheckXxVector(backend);
  if (r != 0) {
    return r;
  }
  r = CheckX25519(backend);
  noise_core_x25519_selftest();
  return r;
}
