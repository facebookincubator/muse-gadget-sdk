#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Host tests for portable/mg_rng, with plain cc: a strict build (-Werror, mg_link_setup's -W set), the exported-symbol
# prefix check, and tests/test_rng.c (the CAVP HMAC-DRBG vector, the health
# tests, the cold and warm seeds, the forward writes and their failures) at -O2 and under ASan + UBSan, on the builtin
# crypto backend and, when mbedcrypto is installed (pkg-config), on PSA.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
LS="$REPO/portable/mg_link_setup"
B="$HERE/build"
CC="${CC:-cc}"
mkdir -p "$B"
cd "$ROOT"

CRYPTO=("$LS/src/crypto/mgls_crypto_builtin.c" "$LS/src/crypto/mgls_uecc.c")
INC=(-Iinclude -I"$LS/include" -I"$LS/src" -I"$LS/src/crypto")
STRICT=(-std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
	-Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Werror)
SAN=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)

step() { printf '\n== %s\n' "$*"; }

step "strict build (-Werror)"
"$CC" "${STRICT[@]}" "${INC[@]}" -Os -c src/mg_rng.c -o "$B/strict.o"
if "$CC" "${STRICT[@]}" -DMGRNG_LEAN=1 "${INC[@]}" -c src/mg_rng.c -o "$B/x.o" 2>/dev/null; then
	echo "src/mg_rng.c built with MGRNG_LEAN" >&2; exit 1
fi

step "every exported symbol carries the prefix"
bad="$(nm -g "$B/strict.o" | awk '$2 ~ /^[TDBSC]$/ {print $3}' | sed 's/^_//' | grep -Ev '^mgrng_' || true)"
if [ -n "$bad" ]; then
	echo "unprefixed symbols: $bad" >&2
	exit 1
fi
echo "  ok"

step "the cutoffs tool gives the defaults (H = 0.5: 41 / 410)"
python3 "$HERE/cutoffs.py" 0.5 | grep -q 'rct_cutoff=41 apt_cutoff=410'
python3 "$HERE/cutoffs.py" 0.4996 | grep -q 'h_milli=499 '      # rounded down, never up
python3 "$HERE/cutoffs.py" 30 | grep -q 'apt_cutoff=2$'          # a high H: never under 2
echo "  ok"

step "the lean configuration (MGRNG_LEAN): strict, prefix, known answers from tests/lean_model.py, ASan + UBSan"
"$CC" "${STRICT[@]}" -DMGRNG_LEAN=1 "${INC[@]}" -Os -c src/mg_rng_lean.c -o "$B/strict_lean.o"
bad="$(nm -g "$B/strict_lean.o" | awk '$2 ~ /^[TDBSC]$/ {print $3}' | sed 's/^_//' | grep -Ev '^mgrng_' || true)"
[ -z "$bad" ] || { echo "unprefixed symbols: $bad" >&2; exit 1; }
python3 "$HERE/lean_model.py" > "$B/lean_ka.h"
"$CC" -std=c99 "${SAN[@]}" -DMGRNG_LEAN=1 "${INC[@]}" -I"$B" tests/test_rng_lean.c src/mg_rng_lean.c "${CRYPTO[@]}" \
	-o "$B/test_rng_lean"
"$B/test_rng_lean"
step "test_rng: -O2"
"$CC" -std=c99 -O2 "${INC[@]}" tests/test_rng.c "${CRYPTO[@]}" -o "$B/test_rng"
"$B/test_rng"
step "test_rng: ASan + UBSan"
"$CC" -std=c99 "${SAN[@]}" "${INC[@]}" tests/test_rng.c "${CRYPTO[@]}" -o "$B/test_rng_san"
"$B/test_rng_san"

if pkg-config --exists mbedcrypto 2>/dev/null; then
	step "test_rng: the PSA backend ($(pkg-config --modversion mbedcrypto)), ASan + UBSan"
	"$CC" -std=c99 "${SAN[@]}" -DMGLS_CRYPTO_PSA $(pkg-config --cflags mbedcrypto) "${INC[@]}" tests/test_rng.c \
		"$LS/src/crypto/mgls_crypto_psa.c" $(pkg-config --libs mbedcrypto) -o "$B/test_rng_psa"
	"$B/test_rng_psa"
	"$CC" -std=c99 "${SAN[@]}" -DMGRNG_LEAN=1 -DMGLS_CRYPTO_PSA $(pkg-config --cflags mbedcrypto) "${INC[@]}" -I"$B" \
		tests/test_rng_lean.c src/mg_rng_lean.c "$LS/src/crypto/mgls_crypto_psa.c" $(pkg-config --libs mbedcrypto) \
		-o "$B/test_rng_lean_psa"
	"$B/test_rng_lean_psa"
fi

step "passed"
