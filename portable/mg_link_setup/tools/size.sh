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
# Code size (-Os, --gc-sections, per object) and worst-case stack per entry
# point (GCC -fcallgraph-info=su) for the embedded targets:
#   rv32imc            (RISCV_GCC, default: the xPack riscv-none-elf-gcc on PATH)
#   Cortex-M4 thumb    (ARM_GCC, default: the Zephyr SDK's arm-zephyr-eabi-gcc)
#   tools/size.sh [extra CFLAGS...]   e.g. -DMGLS_ENABLE_LOG=0
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
B="$ROOT/tests/build/size"
EXTRA=("$@")
RISCV_GCC="${RISCV_GCC:-$(command -v riscv-none-elf-gcc || true)}"
ARM_GCC="${ARM_GCC:-$(ls -d "$HOME"/zephyr-sdk-*/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc 2>/dev/null | tail -1 || true)}"

SRCS=(src/mg_link_setup.c src/mgls_util.c src/mg_token_proof.c src/mgls_store.c
	src/crypto/mgls_crypto_builtin.c src/crypto/mgls_uecc.c tools/size_main.c)
ENTRIES=(mgls_poll mgls_rx mgls_press mgls_connected mgls_disconnected mgls_abort mgtp_rx
	mglss_commit mglss_load)

one() {
	local name="$1" gcc="$2"
	shift 2
	local flags=("$@")
	local d="$B/$name"

	rm -rf "$d"
	mkdir -p "$d"
	echo
	echo "== $name: $("$gcc" --version | head -1)"
	echo "   flags: ${flags[*]} ${EXTRA[*]:-}"
	local objs=()
	for s in "${SRCS[@]}"; do
		local o="$d/$(basename "${s%.c}").o"
		(cd "$d" && "$gcc" -std=c99 -DMGLS_SIZE_STORE "${flags[@]}" ${EXTRA[@]+"${EXTRA[@]}"} \
			-ffunction-sections -fdata-sections -fstack-usage -fcallgraph-info=su \
			-I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/src/crypto" -I"$REPO/protocols" \
			-c "$ROOT/$s" -o "$o")
		objs+=("$o")
	done
	"$gcc" "${flags[@]}" -nostartfiles -Wl,--gc-sections -Wl,-e,mgls_size_entry \
		-Wl,-Map,"$d/size.map" "${objs[@]}" -o "$d/size.elf" -lc -lgcc
	python3 "$HERE/size_report.py" "$d/size.map"
	echo "   worst-case stack (bytes; + the deepest platform callback):"
	python3 "$HERE/stack_report.py" "$d"/*.ci -- "${ENTRIES[@]}"
}

cd "$ROOT"
if [ -x "$RISCV_GCC" ]; then
	one rv32imc "$RISCV_GCC" -march=rv32imc -mabi=ilp32 -Os
else
	echo "no RISC-V GCC (set RISCV_GCC)"
fi
if [ -x "$ARM_GCC" ]; then
	one cortex-m4 "$ARM_GCC" -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os
else
	echo "no ARM GCC (set ARM_GCC)"
fi
