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
# rv32imc instruction counts and measured stack under QEMU (virt, -icount):
#   tools/qemu/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
B="$ROOT/tests/build/qemu"
GCC="${RISCV_GCC:-$(command -v riscv-none-elf-gcc || true)}"   # e.g. xPack's riscv-none-elf-gcc
[ -x "$GCC" ] || { echo "no RISC-V GCC (set RISCV_GCC)" >&2; exit 2; }
ARCH="${ARCH:--march=rv32imc_zicsr -mabi=ilp32}"
mkdir -p "$B"
python3 "$ROOT/tests/gen_vectors.py" "$REPO/esp32/tests/vectors/link_pairing_v5.json" \
	"$REPO/protocols/test-vectors/mg-token-proof-v1.json" "$ROOT/tests/build/vectors_gen.h"
# shellcheck disable=SC2086
"$GCC" $ARCH -Os -std=gnu99 -DMGLS_TEST_HOOKS -DMGLS_ENABLE_LOG=0 -ffunction-sections \
	-nostartfiles -Wl,--gc-sections -T "$HERE/link.ld" \
	-I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/src/crypto" -I"$REPO/protocols" -I"$ROOT/tests/build" \
	"$HERE/crt0.S" "$HERE/bench_rv32.c" "$ROOT"/src/mg_link_setup.c "$ROOT"/src/mgls_util.c \
	"$ROOT"/src/mg_token_proof.c "$ROOT"/src/crypto/mgls_crypto_builtin.c \
	"$ROOT"/src/crypto/mgls_uecc.c -o "$B/bench.elf" -lc -lgcc
perl -e 'alarm 600; exec @ARGV' qemu-system-riscv32 -machine virt -bios none -nographic -icount shift=0 \
	-kernel "$B/bench.elf"
