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
# Host tests for portable/mg_device_session, with plain cc:
#   tests/run.sh          strict build (-Werror, the -W set of mg_link_setup),
#                         the exported-symbol prefix check, the seal vectors
#                         (-O2, ASan + UBSan, with the backend's SHA-256
#                         (MGSEAL_EXT_SHA, also on PSA when mbedcrypto is
#                         installed), without the streaming path), the
#                         reference checker, and layer 2 end to end (the C
#                         gadget under ASan + UBSan against the Python
#                         references: test_session.py), and with MGDS_DATA=1 (the
#                         data path: every exchange of mg-session-v1.json byte
#                         for byte, the re-ask series on the host clock, sealed
#                         data both ways), and with MGDS_STALL=1 (layer 2's stall
#                         rules)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
LS="$REPO/portable/mg_link_setup"
B="$HERE/build"
CC="${CC:-cc}"
mkdir -p "$B"
cd "$ROOT"

SEAL=(src/mg_session_seal.c)
SESSION=(src/mg_device_session.c)
CRYPTO=("$LS/src/crypto/mgls_crypto_builtin.c" "$LS/src/crypto/mgls_uecc.c")
INC=(-Iinclude -Isrc -I"$LS/include" -I"$LS/src" -I"$LS/src/crypto" -I"$REPO/protocols" -I"$B")
STRICT=(-std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
	-Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Werror)
SAN=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
VEC="$REPO/protocols/test-vectors/mg-session-seal-v1.json"

step() { printf '\n== %s\n' "$*"; }

step "strict build (-Werror), with and without the streaming tag, with the backend's SHA-256"
for f in "${SEAL[@]}" "${SESSION[@]}"; do
	"$CC" "${STRICT[@]}" "${INC[@]}" -Os -c "$f" -o "$B/strict.o"
done
"$CC" "${STRICT[@]}" -DMGSEAL_STREAM=0 "${INC[@]}" -Os -c src/mg_session_seal.c -o "$B/strict0.o"
"$CC" "${STRICT[@]}" -DMGSEAL_EXT_SHA=1 "${INC[@]}" -Os -c src/mg_session_seal.c -o "$B/strict2.o"
"$CC" "${STRICT[@]}" -DMGDS_STATS=0 -DMGDS_FRAME_MAX=20 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict1.o"
"$CC" "${STRICT[@]}" -DMGDS_SEALED_DATA=0 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict3.o"
"$CC" "${STRICT[@]}" -DMGDS_TX_CTR_U8=1 -DMGSEAL_EXT_SHA=1 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict5.o"
"$CC" "${STRICT[@]}" -DMGDS_KEEP_KEYS=0 -DMGDS_TAG_LEND=1 -DMGDS_END_ON_BAD_TAG=1 "${INC[@]}" -Os -c src/mg_device_session.c \
	-o "$B/strict4.o"
"$CC" "${STRICT[@]}" -DMGDS_DATA=1 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict6.o"
"$CC" "${STRICT[@]}" -DMGDS_STALL=1 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict8.o"
"$CC" "${STRICT[@]}" -DMGDS_STALL=1 -DMGDS_DATA=1 -DMGDS_STATS=0 -DMGDS_FRAME_MAX=20 "${INC[@]}" -Os -c \
	src/mg_device_session.c -o "$B/strict9.o"
"$CC" "${STRICT[@]}" -DMGDS_DATA=1 -DMGDS_KEEP_KEYS=0 -DMGDS_TAG_LEND=1 -DMGDS_END_ON_BAD_TAG=1 -DMGDS_STATS=0 \
	-DMGDS_FRAME_MAX=20 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict7.o"

step "the sealed clear (MGDS_CAP_SEALED_CLEAR=1, or MGTP_SEALED_CLEAR=1: one switch); the two names set differently don't build"
"$CC" "${STRICT[@]}" -DMGDS_CAP_SEALED_CLEAR=1 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict12.o"
"$CC" "${STRICT[@]}" -DMGDS_CAP_SEALED_CLEAR=1 -DMGDS_DATA=1 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict13.o"
printf '#include "mg_device_session.h"\ntypedef char caps_sealed_clear[(MGDS_CAPS & 0x20u) ? 1 : -1];\n' > "$B/sc_caps.c"
"$CC" -std=c99 -DMGTP_SEALED_CLEAR=1 "${INC[@]}" -c "$B/sc_caps.c" -o "$B/sc_caps.o"
for opts in "-DMGTP_SEALED_CLEAR=0 -DMGDS_CAP_SEALED_CLEAR=1" "-DMGTP_SEALED_CLEAR=1 -DMGDS_CAP_SEALED_CLEAR=0"; do
	# shellcheck disable=SC2086
	if "$CC" -std=c99 $opts "${INC[@]}" -c src/mg_device_session.c -o "$B/sc_mismatch.o" 2>/dev/null; then
		echo "mg_device_session.c built with $opts" >&2
		exit 1
	fi
done
echo "  ok"

step "a board that can't reach MG_MIN_ATT_MTU (MGDS_ATT_MTU_MAX) doesn't declare mg_session_cap_sealed"
printf '#include "mg_device_session.h"\ntypedef char caps_sealed[(MGDS_CAPS & 0x01u) == WANT ? 1 : -1];\n' > "$B/mtu_caps.c"
"$CC" -std=c99 -DMGDS_ATT_MTU_MAX=64 -DWANT=0u "${INC[@]}" -c "$B/mtu_caps.c" -o "$B/mtu_caps.o"
"$CC" -std=c99 -DMGDS_ATT_MTU_MAX=100 -DWANT=1u "${INC[@]}" -c "$B/mtu_caps.c" -o "$B/mtu_caps.o"
"$CC" -std=c99 -DWANT=1u "${INC[@]}" -c "$B/mtu_caps.c" -o "$B/mtu_caps.o"
echo "  ok"

step "mgds_t keeps its size with MGDS_DATA=1"
printf '#include <stdio.h>\n#include "mg_device_session.h"\nint main(void) { printf("%%zu\\n", sizeof(mgds_t)); return 0; }\n' \
	> "$B/mgds_size.c"
"$CC" -std=c99 "${INC[@]}" "$B/mgds_size.c" -o "$B/mgds_size0"
"$CC" -std=c99 -DMGDS_DATA=1 "${INC[@]}" "$B/mgds_size.c" -o "$B/mgds_size1"
size0="$("$B/mgds_size0")"
size1="$("$B/mgds_size1")"
if [ "$size0" != "$size1" ]; then
	echo "mgds_t is $size0 B, $size1 B with MGDS_DATA=1" >&2
	exit 1
fi
echo "  ok ($size0 B on this host)"

step "every exported symbol carries a prefix"
"$CC" -std=c99 "${INC[@]}" -Os -c src/mg_session_seal.c -o "$B/sym_seal.o"
"$CC" -std=c99 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/sym_session.o"
"$CC" -std=c99 -DMGDS_DATA=1 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/sym_session_data.o"
bad="$(nm -g "$B/sym_seal.o" "$B/sym_session.o" "$B/sym_session_data.o" | awk '$2 ~ /^[TDBSC]$/ {print $3}' | sed 's/^_//' |
	grep -Ev '^(mgseal_|mgds_)' || true)"
if [ -n "$bad" ]; then
	echo "unprefixed symbols: $bad" >&2
	exit 1
fi
echo "  ok"

python3 "$HERE/gen_seal_vectors.py" "$VEC" "$B/seal_vectors.h"

step "seal vectors: -O2"
"$CC" -std=c99 -O2 "${INC[@]}" tests/test_seal.c "${SEAL[@]}" "${CRYPTO[@]}" -o "$B/test_seal"
"$B/test_seal"
step "seal vectors: ASan + UBSan"
"$CC" -std=c99 "${SAN[@]}" "${INC[@]}" tests/test_seal.c "${SEAL[@]}" "${CRYPTO[@]}" -o "$B/test_seal_san"
"$B/test_seal_san"
step "seal vectors: MGSEAL_EXT_SHA=1 (the backend's incremental SHA-256), -O2 and ASan + UBSan"
"$CC" -std=c99 -O2 -DMGSEAL_EXT_SHA=1 "${INC[@]}" tests/test_seal.c "${SEAL[@]}" "${CRYPTO[@]}" -o "$B/test_seal_ext"
"$B/test_seal_ext"
"$CC" -std=c99 "${SAN[@]}" -DMGSEAL_EXT_SHA=1 "${INC[@]}" tests/test_seal.c "${SEAL[@]}" "${CRYPTO[@]}" \
	-o "$B/test_seal_ext_san"
"$B/test_seal_ext_san"
if pkg-config --exists mbedcrypto 2>/dev/null; then
	step "seal vectors: MGSEAL_EXT_SHA=1 on the PSA backend ($(pkg-config --modversion mbedcrypto)), ASan + UBSan"
	# shellcheck disable=SC2046
	"$CC" -std=c99 "${SAN[@]}" -DMGSEAL_EXT_SHA=1 -DMGLS_CRYPTO_PSA $(pkg-config --cflags mbedcrypto) "${INC[@]}" \
		tests/test_seal.c "${SEAL[@]}" "$LS/src/crypto/mgls_crypto_psa.c" \
		$(pkg-config --libs mbedcrypto) -o "$B/test_seal_psa"
	"$B/test_seal_psa"
fi
step "seal vectors: MGSEAL_STREAM=0, ASan + UBSan"
"$CC" -std=c99 "${SAN[@]}" -DMGSEAL_STREAM=0 "${INC[@]}" tests/test_seal.c "${SEAL[@]}" "${CRYPTO[@]}" \
	-o "$B/test_seal_ns"
"$B/test_seal_ns"
step "the reference regenerates the vectors"
python3 "$REPO/protocols/test-vectors/mg_session_seal_ref.py"
step "layer 2 end to end: the gadget (ASan + UBSan) against the references"
"$CC" -std=c99 "${SAN[@]}" "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" "${CRYPTO[@]}" \
	-o "$B/sim_session"
MGDS_SIM="$B/sim_session" python3 -W ignore "$HERE/test_session.py"
step "layer 2 end to end with MGDS_SEALED_DATA=0 (sealed data's counter rules without its tag)"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_SEALED_DATA=0 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" \
	"${CRYPTO[@]}" -o "$B/sim_session_nd"
MGDS_SIM="$B/sim_session_nd" MGDS_SEALED_DATA=0 python3 -W ignore "$HERE/test_session.py"
step "layer 2 end to end with MGDS_KEEP_KEYS=0 and MGDS_TAG_LEND=1 (the smallest mgds_t: keys derived per message, the tag state lent)"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_KEEP_KEYS=0 -DMGDS_TAG_LEND=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" \
	"${SEAL[@]}" "${CRYPTO[@]}" -o "$B/sim_session_lend"
MGDS_SIM="$B/sim_session_lend" MGDS_KEEP_KEYS=0 MGDS_TAG_LEND=1 python3 -W ignore "$HERE/test_session.py"
step "layer 2 with MGDS_END_ON_BAD_TAG=1 (a bad tag ends the sealed session)"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_END_ON_BAD_TAG=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" \
	"${CRYPTO[@]}" -o "$B/sim_session_end"
MGDS_SIM="$B/sim_session_end" MGDS_END_ON_BAD_TAG=1 python3 -W ignore "$HERE/test_session.py" EndOnBadTag
step "layer 2 end to end with MGDS_CAP_SEALED_CLEAR=1 (the cap advertised), with and without MGDS_DATA"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_CAP_SEALED_CLEAR=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" \
	"${CRYPTO[@]}" -o "$B/sim_session_sc"
MGDS_SIM="$B/sim_session_sc" MGDS_CAP_SEALED_CLEAR=1 python3 -W ignore "$HERE/test_session.py"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_CAP_SEALED_CLEAR=1 -DMGDS_DATA=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" \
	"${SEAL[@]}" "${CRYPTO[@]}" -o "$B/sim_session_sc_data"
MGDS_SIM="$B/sim_session_sc_data" MGDS_CAP_SEALED_CLEAR=1 MGDS_DATA=1 python3 -W ignore "$HERE/test_session.py"
step "layer 2 end to end with MGSEAL_EXT_SHA=1"
"$CC" -std=c99 "${SAN[@]}" -DMGSEAL_EXT_SHA=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" \
	"${CRYPTO[@]}" -o "$B/sim_session_ext"
MGDS_SIM="$B/sim_session_ext" python3 -W ignore "$HERE/test_session.py"
step "layer 2 with MGDS_TX_CTR_U8=1 and MGSEAL_EXT_SHA=1 (a RAM-tight board: the uint8_t device counter and its end)"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_TX_CTR_U8=1 -DMGSEAL_EXT_SHA=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" \
	"${SEAL[@]}" "${CRYPTO[@]}" -o "$B/sim_session_u8"
MGDS_SIM="$B/sim_session_u8" MGDS_TX_CTR_U8=1 python3 -W ignore "$HERE/test_session.py"
step "layer 2: MGDS_STATS=0, the smallest fragment buffer"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_STATS=0 -DMGDS_FRAME_MAX=20 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" \
	"${SEAL[@]}" "${CRYPTO[@]}" -o "$B/sim_session_small"
MGDS_SIM="$B/sim_session_small" MGDS_FRAME_MAX=20 python3 -W ignore "$HERE/test_session.py"
step "the exchange reference checks its vectors"
python3 "$REPO/protocols/test-vectors/mg_session_ref.py"
step "layer 2 end to end with MGDS_DATA=1 (the data path: every exchange byte for byte, the re-ask series, sealed data both ways)"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_DATA=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" "${CRYPTO[@]}" \
	-o "$B/sim_session_data"
MGDS_SIM="$B/sim_session_data" MGDS_DATA=1 python3 -W ignore "$HERE/test_session.py"
step "layer 2 with MGDS_DATA=1, MGDS_KEEP_KEYS=0 and MGDS_TAG_LEND=1"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_DATA=1 -DMGDS_KEEP_KEYS=0 -DMGDS_TAG_LEND=1 "${INC[@]}" tests/sim_session.c \
	"${SESSION[@]}" "${SEAL[@]}" "${CRYPTO[@]}" -o "$B/sim_session_data_lend"
MGDS_SIM="$B/sim_session_data_lend" MGDS_DATA=1 MGDS_KEEP_KEYS=0 MGDS_TAG_LEND=1 python3 -W ignore "$HERE/test_session.py"
step "layer 2 with MGDS_DATA=1, MGSEAL_EXT_SHA=1"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_DATA=1 -DMGSEAL_EXT_SHA=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" \
	"${CRYPTO[@]}" -o "$B/sim_session_data_ext"
MGDS_SIM="$B/sim_session_data_ext" MGDS_DATA=1 python3 -W ignore "$HERE/test_session.py"
step "layer 2 with MGDS_DATA=1, MGDS_STATS=0 and the smallest fragment buffer"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_DATA=1 -DMGDS_STATS=0 -DMGDS_FRAME_MAX=20 "${INC[@]}" tests/sim_session.c \
	"${SESSION[@]}" "${SEAL[@]}" "${CRYPTO[@]}" -o "$B/sim_session_data_small"
MGDS_SIM="$B/sim_session_data_small" MGDS_DATA=1 MGDS_FRAME_MAX=20 python3 -W ignore "$HERE/test_session.py"
step "layer 2 with MGDS_DATA=1 and MGDS_END_ON_BAD_TAG=1"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_DATA=1 -DMGDS_END_ON_BAD_TAG=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" \
	"${SEAL[@]}" "${CRYPTO[@]}" -o "$B/sim_session_data_end"
MGDS_SIM="$B/sim_session_data_end" MGDS_DATA=1 MGDS_END_ON_BAD_TAG=1 python3 -W ignore "$HERE/test_session.py" EndOnBadTag
step "layer 2 with MGDS_STALL=1 (the stall clock while owed, the 2 min bound on an update part-way)"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_STALL=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" "${CRYPTO[@]}" \
	-o "$B/sim_session_stall"
MGDS_SIM="$B/sim_session_stall" MGDS_STALL=1 python3 -W ignore "$HERE/test_session.py"
step "layer 2 with MGDS_STALL=1 and MGDS_DATA=1"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_STALL=1 -DMGDS_DATA=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" \
	"${CRYPTO[@]}" -o "$B/sim_session_stall_data"
MGDS_SIM="$B/sim_session_stall_data" MGDS_STALL=1 MGDS_DATA=1 python3 -W ignore "$HERE/test_session.py"
step "layer 2 with MGDS_GIVE_UP_LATER=1 (a released held update's [0] from the next poll), with and without MGDS_DATA"
"$CC" "${STRICT[@]}" -DMGDS_GIVE_UP_LATER=1 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict10.o"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_GIVE_UP_LATER=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" \
	"${CRYPTO[@]}" -o "$B/sim_session_gul"
MGDS_SIM="$B/sim_session_gul" MGDS_GIVE_UP_LATER=1 python3 -W ignore "$HERE/test_session.py"
"$CC" -std=c99 "${SAN[@]}" -DMGDS_GIVE_UP_LATER=1 -DMGDS_DATA=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" \
	"${SEAL[@]}" "${CRYPTO[@]}" -o "$B/sim_session_gul_data"
MGDS_SIM="$B/sim_session_gul_data" MGDS_GIVE_UP_LATER=1 MGDS_DATA=1 python3 -W ignore "$HERE/test_session.py"
step "layer 2 with MGDS_ASK_QUIET=1 (no ask of layer 2's own while the port has no task), with MGDS_DATA"
"$CC" "${STRICT[@]}" -DMGDS_ASK_QUIET=1 -DMGDS_DATA=1 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict11.o"
if "$CC" "${STRICT[@]}" -DMGDS_ASK_QUIET=1 "${INC[@]}" -Os -c src/mg_device_session.c -o "$B/strict11e.o" 2>/dev/null; then
	echo "MGDS_ASK_QUIET without MGDS_DATA compiled: the #error is missing" >&2
	exit 1
fi
"$CC" -std=c99 "${SAN[@]}" -DMGDS_ASK_QUIET=1 -DMGDS_DATA=1 "${INC[@]}" tests/sim_session.c "${SESSION[@]}" "${SEAL[@]}" \
	"${CRYPTO[@]}" -o "$B/sim_session_quiet"
MGDS_SIM="$B/sim_session_quiet" MGDS_ASK_QUIET=1 MGDS_DATA=1 python3 -W ignore "$HERE/test_session.py"
step "passed"
