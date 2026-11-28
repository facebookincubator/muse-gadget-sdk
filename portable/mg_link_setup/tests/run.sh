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
# Host tests for the portable Muse Link setup core, with plain cc:
#   tests/run.sh            everything (strict build, unit tests in four
#                           builds, end-to-end with ASan/UBSan)
#   tests/run.sh unit|e2e
# Needs python3; the end-to-end tests need `cryptography` (a venv is made at
# the repository root's .venv if none is active). PSA tests run when
# `pkg-config mbedcrypto` finds mbedTLS / TF-PSA-Crypto.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
B="$HERE/build"
CC="${CC:-cc}"
WHAT="${1:-all}"
mkdir -p "$B"
cd "$ROOT"

CORE=(src/mg_link_setup.c src/mgls_util.c src/mgls_store.c src/mg_token_proof.c)
BUILTIN=(src/crypto/mgls_crypto_builtin.c src/crypto/mgls_uecc.c)
INC=(-Iinclude -Isrc -Isrc/crypto -I"$REPO/protocols" -I"$B")
STRICT=(-std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
	-Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Werror)
SAN=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)

step() { printf '\n== %s\n' "$*"; }

python_with_cryptography() {
	if python3 -c 'import cryptography' 2>/dev/null; then
		echo python3
		return
	fi
	if [ ! -x "$REPO/.venv/bin/python" ]; then
		python3 -m venv "$REPO/.venv" >&2
	fi
	if ! "$REPO/.venv/bin/python" -c 'import cryptography' 2>/dev/null; then
		"$REPO/.venv/bin/pip" install -q cryptography >&2
	fi
	echo "$REPO/.venv/bin/python"
}

build() {
	step "strict build of the core and both crypto backends (-Werror)"
	for f in "${CORE[@]}" src/crypto/mgls_crypto_builtin.c; do
		"$CC" "${STRICT[@]}" "${INC[@]}" -Os -c "$f" -o "$B/strict.o"
	done
	"$CC" -std=c99 -Wall -Wextra -Werror "${INC[@]}" -Os -c src/crypto/mgls_uecc.c -o "$B/strict.o"
	if pkg-config --exists mbedcrypto 2>/dev/null; then
		# shellcheck disable=SC2046
		"$CC" "${STRICT[@]}" -Wno-cast-qual -DMGLS_CRYPTO_PSA $(pkg-config --cflags mbedcrypto) \
			"${INC[@]}" -Os -c src/crypto/mgls_crypto_psa.c -o "$B/strict.o"
	fi
	"$CC" "${STRICT[@]}" -DMGLS_ENABLE_LOG=0 "${INC[@]}" -Os -c src/mg_link_setup.c -o "$B/strict.o"
	"$CC" "${STRICT[@]}" -DMGLS_ENABLE_SIMULATED_PRESS "${INC[@]}" -Os -c src/mg_link_setup.c -o "$B/strict.o"
	step "every exported symbol carries a prefix (no clash with a vendor's micro-ecc or crypto)"
	local objs=()
	for f in "${CORE[@]}" "${BUILTIN[@]}"; do
		"$CC" -std=c99 "${INC[@]}" -Os -c "$f" -o "$B/sym_$(basename "${f%.c}").o"
		objs+=("$B/sym_$(basename "${f%.c}").o")
	done
	local bad
	bad="$(nm -g "${objs[@]}" | awk '$2 ~ /^[TDBSC]$/ {print $3}' | sed 's/^_//' |
		grep -Ev '^(mgls_|mgtp_|mglss_|mglsc_|mgj_)' || true)"
	if [ -n "$bad" ]; then
		echo "unprefixed symbols: $bad" >&2
		exit 1
	fi
	echo "  ok"
	python3 "$HERE/gen_vectors.py" "$REPO/esp32/tests/vectors/link_pairing_v5.json" \
		"$REPO/protocols/test-vectors/mg-token-proof-v1.json" "$B/vectors_gen.h"
}

unit() {
	step "unit tests: builtin crypto, -O2"
	"$CC" -std=c99 -O2 "${INC[@]}" tests/test_unit.c "${CORE[@]}" "${BUILTIN[@]}" -o "$B/test_unit"
	"$B/test_unit"
	step "unit tests: builtin crypto, ASan + UBSan"
	"$CC" -std=c99 "${SAN[@]}" "${INC[@]}" tests/test_unit.c "${CORE[@]}" "${BUILTIN[@]}" \
		-o "$B/test_unit_san"
	"$B/test_unit_san"
	step "unit tests: MGDS_CAP_SEALED_CLEAR=1 (one switch: the token proof's clear only sealed, insecure, nothing erased)"
	"$CC" -std=c99 "${SAN[@]}" -DMGDS_CAP_SEALED_CLEAR=1 "${INC[@]}" tests/test_unit.c "${CORE[@]}" "${BUILTIN[@]}" \
		-o "$B/test_unit_sc"
	"$B/test_unit_sc"
	step "the sealed clear's two names set differently don't build"
	for opts in "-DMGTP_SEALED_CLEAR=0 -DMGDS_CAP_SEALED_CLEAR=1" "-DMGTP_SEALED_CLEAR=1 -DMGDS_CAP_SEALED_CLEAR=0"; do
		# shellcheck disable=SC2086
		if "$CC" -std=c99 $opts "${INC[@]}" -c src/mg_token_proof.c -o "$B/sc_mismatch.o" 2>/dev/null; then
			echo "mg_token_proof.c built with $opts" >&2
			exit 1
		fi
	done
	echo "  ok"
	step "unit tests: a test build (MGLS_RELEASE=0: the hooks' test defaults)"
	"$CC" -std=c99 "${SAN[@]}" -DMGLS_RELEASE=0 "${INC[@]}" tests/test_unit.c "${CORE[@]}" "${BUILTIN[@]}" \
		-o "$B/test_unit_test"
	"$B/test_unit_test"
	step "a release build can't name the hooks' test defaults; MGLS_STATIC_HOOKS refuses NULL"
	printf '#include "mg_link_setup.h"\nvoid *x = (void *)MGLS_HOOK_TEST_DEFAULT(set_up);\n' > "$B/no_default.c"
	"$CC" -std=c99 -DMGLS_RELEASE=0 "${INC[@]}" -c "$B/no_default.c" -o "$B/no_default.o" # a test build names it
	if "$CC" -std=c99 "${INC[@]}" -c "$B/no_default.c" -o "$B/no_default.o" 2>/dev/null; then
		echo "MGLS_HOOK_TEST_DEFAULT compiled in a release build" >&2
		exit 1
	fi
	printf '#include "mg_link_setup.h"\nMGLS_STATIC_HOOKS(NULL, NULL);\n' > "$B/null_hooks.c"
	if "$CC" -std=c99 "${INC[@]}" -c "$B/null_hooks.c" -o "$B/null_hooks.o" 2>/dev/null; then
		echo "MGLS_STATIC_HOOKS(NULL, NULL) compiled" >&2
		exit 1
	fi
	echo "  ok"
	step "the simulated press doesn't build into a release build (MGLS_RELEASE=1)"
	if "$CC" -std=c99 -DMGLS_ENABLE_SIMULATED_PRESS -DMGLS_RELEASE=1 "${INC[@]}" -c src/mg_link_setup.c \
		-o "$B/sim_release.o" 2>/dev/null; then
		echo "MGLS_ENABLE_SIMULATED_PRESS built with MGLS_RELEASE=1" >&2
		exit 1
	fi
	"$CC" -std=c99 -DMGLS_ENABLE_SIMULATED_PRESS -DMGLS_RELEASE=0 "${INC[@]}" -c src/mg_link_setup.c \
		-o "$B/sim_release.o" # the bench build does
	echo "  ok"
	step "unit tests: builtin crypto with the computed (constant-time) AES S-box"
	"$CC" -std=c99 -O2 -DMGLS_AES_CT_SBOX "${INC[@]}" tests/test_unit.c "${CORE[@]}" \
		"${BUILTIN[@]}" -o "$B/test_unit_ct"
	"$B/test_unit_ct"
	if pkg-config --exists mbedcrypto 2>/dev/null; then
		step "unit tests: PSA backend ($(pkg-config --modversion mbedcrypto)), ASan + UBSan"
		# shellcheck disable=SC2046
		"$CC" -std=c99 "${SAN[@]}" -DMGLS_CRYPTO_PSA $(pkg-config --cflags mbedcrypto) "${INC[@]}" \
			tests/test_unit.c "${CORE[@]}" src/crypto/mgls_crypto_psa.c \
			$(pkg-config --libs mbedcrypto) -o "$B/test_unit_psa"
		"$B/test_unit_psa"
	else
		step "PSA backend: skipped (no pkg-config mbedcrypto)"
	fi
}

e2e() {
	local py
	py="$(python_with_cryptography)"
	step "end-to-end: phone (Python, cryptography) vs the gadget (ASan + UBSan)"
	"$CC" -std=c99 "${SAN[@]}" -DMGLS_TEST_HOOKS -DMGLS_ENABLE_SIMULATED_PRESS "${INC[@]}" \
		tests/sim.c "${CORE[@]}" "${BUILTIN[@]}" -o "$B/sim"
	MGLS_SIM="$B/sim" MGLS_REPO="$REPO" "$py" "$HERE/test_e2e.py"
	if pkg-config --exists mbedcrypto 2>/dev/null; then
		step "end-to-end with the PSA backend"
		# shellcheck disable=SC2046
		"$CC" -std=c99 -g -O1 -DMGLS_CRYPTO_PSA -DMGLS_TEST_HOOKS -DMGLS_ENABLE_SIMULATED_PRESS \
			$(pkg-config --cflags mbedcrypto) "${INC[@]}" tests/sim.c "${CORE[@]}" \
			src/crypto/mgls_crypto_psa.c $(pkg-config --libs mbedcrypto) -o "$B/sim_psa"
		MGLS_SIM="$B/sim_psa" MGLS_REPO="$REPO" "$py" "$HERE/test_e2e.py"
	fi
}

case "$WHAT" in
all) build; unit; e2e ;;
unit) build; unit ;;
e2e) build; e2e ;;
*) echo "usage: $0 [all|unit|e2e]" >&2; exit 2 ;;
esac
step "passed"
