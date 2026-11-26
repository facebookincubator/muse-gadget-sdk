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

#ifndef MGLS_UECC_CONFIG_H_
#define MGLS_UECC_CONFIG_H_

/* micro-ecc built for P-256 only (included before uECC.h everywhere). */
#define uECC_SUPPORTS_secp160r1       0
#define uECC_SUPPORTS_secp192r1       0
#define uECC_SUPPORTS_secp224r1       0
#define uECC_SUPPORTS_secp256r1       1
#define uECC_SUPPORTS_secp256k1       0
#define uECC_SUPPORT_COMPRESSED_POINT 0
#define uECC_ENABLE_VLI_API           0
/*
 * Every external micro-ecc symbol gets the mgls_ prefix, always: vendor SDKs
 * ship their own micro-ecc (exporting the same uECC_* names), and two copies
 * must link side by side.
 */
#define uECC_set_rng mgls_uECC_set_rng
#define uECC_get_rng mgls_uECC_get_rng
#define uECC_curve_private_key_size mgls_uECC_curve_private_key_size
#define uECC_curve_public_key_size mgls_uECC_curve_public_key_size
#define uECC_secp160r1 mgls_uECC_secp160r1
#define uECC_secp192r1 mgls_uECC_secp192r1
#define uECC_secp224r1 mgls_uECC_secp224r1
#define uECC_secp256r1 mgls_uECC_secp256r1
#define uECC_secp256k1 mgls_uECC_secp256k1
#define uECC_make_key mgls_uECC_make_key
#define uECC_shared_secret mgls_uECC_shared_secret
#define uECC_compress mgls_uECC_compress
#define uECC_decompress mgls_uECC_decompress
#define uECC_valid_public_key mgls_uECC_valid_public_key
#define uECC_compute_public_key mgls_uECC_compute_public_key
#define uECC_sign mgls_uECC_sign
#define uECC_sign_deterministic mgls_uECC_sign_deterministic
#define uECC_sign_with_k mgls_uECC_sign_with_k
#define uECC_verify mgls_uECC_verify

#ifndef uECC_OPTIMIZATION_LEVEL
#define uECC_OPTIMIZATION_LEVEL 2
#endif
#ifndef uECC_SQUARE_FUNC
#define uECC_SQUARE_FUNC 0
#endif

#endif
