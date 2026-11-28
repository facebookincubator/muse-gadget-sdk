<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# micro-ecc (vendored)

Upstream: https://github.com/kmackay/micro-ecc, commit
`541b3a78026420a3e369c4c9281c396b5e531113` (2024-11-13), BSD-2-Clause
(`LICENSE.txt`, Copyright (c) 2014, Kenneth MacKay).

Only the files the P-256 build needs are copied, unmodified: `uECC.c`,
`uECC.h`, `uECC_vli.h`, `types.h`, `curve-specific.inc`,
`platform-specific.inc` and the ARM assembly includes. Do not restyle them or
replace their headers.

`../../src/crypto/mgls_uecc.c` compiles `uECC.c` with only secp256r1 enabled
and no compressed points; `mgls_crypto_builtin.c` uses `uECC_make_key`,
`uECC_valid_public_key` and `uECC_shared_secret` and installs the platform's
RNG with `uECC_set_rng` (micro-ecc also randomizes the projective Z
coordinate of every shared-secret computation with it). The signing and
verification code is compiled but unreferenced: link with
`-ffunction-sections -Wl,--gc-sections` to drop it.

On a POSIX host, `platform-specific.inc` compiles a default `/dev/urandom`
RNG; the setup core always replaces it with the platform's RNG at
`mgls_init()`, and embedded targets get no default RNG at all.
