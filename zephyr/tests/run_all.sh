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

# Runs every host test: the native_sim unit tests (four configurations) and
# the BabbleSim scenarios. Linux only (x86-64 for BabbleSim, which needs
# 32-bit host builds: gcc-multilib). Needs ZEPHYR_BASE, and BSIM_OUT_PATH /
# BSIM_COMPONENTS_PATH for BabbleSim. Build output goes under $OUT (a new
# private directory by default).
#
# build: the XIAO image's build checks instead (any host with the Zephyr
# SDK's Arm toolchain): the release build and the configurations that leave
# a part out (CONFIG_MG_AUDIO_QUEUE=n, CONFIG_MG_BATTERY_ADC=n, the bench
# console without the Nordic UART: CONFIG_MG_BENCH=y, CONFIG_MG_NUS=n) must
# link.
#
#   tests/run_all.sh [unit|bsim|all|build]
set -u

here=$(cd "$(dirname "$0")" && pwd)
app=$(cd "$here/.." && pwd)
what=${1:-all}
case $what in
unit | bsim | all | build) ;;
*)
	echo "usage: $0 [unit|bsim|all|build]" >&2
	exit 2
	;;
esac
out=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/mg-tests.XXXXXX")} || exit 2
board=${UNIT_BOARD:-native_sim/native/64}
fail=0

# west build, retried when a host tool dies without a real error message
# (compilers and Python crash now and then under QEMU user emulation on
# arm64 Macs, cc1 with "internal compiler error"; harmless elsewhere).
wbuild() {
	local log=$1
	shift
	for i in 1 2 3 4 5 6 7 8; do
		west build "$@" > "$log" 2>&1 && return 0
		grep -q "internal compiler error" "$log" ||
			! grep -qE ": (fatal )?error:|CMake Error at /work|warning: attempt to assign" "$log" ||
			return 1
		set -- $(printf '%s\n' "$@" | grep -vx -- -p)
	done
	return 1
}

run_unit() {
	local name=$1
	shift
	echo "== unit: $name"
	wbuild "$out/unit_$name.log" -p -b "$board" "$app/tests/unit" -d "$out/unit_$name" -- "$@" ||
		{ tail -30 "$out/unit_$name.log"; fail=1; return; }
	# Fresh simulated flash each run: a flash.bin left by another
	# configuration (RRAM-style vs NOR-style) would not be erased.
	"$out/unit_$name/zephyr/zephyr.exe" -flash_in_ram > "$out/unit_$name.run.log" 2>&1 ||
		fail=1
	grep -E "^SUITE|PROJECT EXECUTION" "$out/unit_$name.run.log"
	grep -q "PROJECT EXECUTION SUCCESSFUL" "$out/unit_$name.run.log" || fail=1
}

bsim_build() {
	local dir=$1 src=$2
	shift 2
	wbuild "$out/$dir.log" -p -b nrf54l15bsim/nrf54l15/cpuapp "$src" -d "$out/$dir" -- "$@" ||
		{ tail -30 "$out/$dir.log"; fail=1; return 1; }
}

xiao_build() {
	local name=$1
	shift
	echo "== xiao: $name"
	wbuild "$out/xiao_$name.log" --sysbuild -p -b xiao_nrf54l15/nrf54l15/cpuapp "$app" \
		-d "$out/xiao_$name" -- -DSB_EXTRA_CONF_FILE=sysbuild-dev.conf "$@" ||
		{ tail -30 "$out/xiao_$name.log"; fail=1; }
}

mkdir -p "$out"
if [ "$what" = build ]; then
	xiao_build release
	xiao_build no-audio-queue -DCONFIG_MG_AUDIO_QUEUE=n
	xiao_build no-battery-adc -DCONFIG_MG_BATTERY_ADC=n
	xiao_build bench-no-nus -DCONFIG_MG_BENCH=y -DCONFIG_MG_NUS=n
fi
if [ "$what" = unit ] || [ "$what" = all ]; then
	run_unit plain
	run_unit rram -DCONFIG_FLASH_SIMULATOR_EXPLICIT_ERASE=n -DCONFIG_MG_LC3_24K=y
	run_unit tap -DCONFIG_MG_TAP_TO_TALK=y -DCONFIG_MG_TAP_TO_TALK_MAX_MS=2000 -DCONFIG_ZTEST_VERIFY_RUN_ALL=n
	run_unit slot1 -DCONFIG_FLASH_SIMULATOR_EXPLICIT_ERASE=n \
		-DCONFIG_MG_AUDIO_QUEUE=y -DCONFIG_MG_QUEUE_IN_SLOT1=y -DDTC_OVERLAY_FILE=slot1.overlay \
		-DCONFIG_MG_HAPTICS=n
fi
if [ "$what" = bsim ] || [ "$what" = all ]; then
	echo "== bsim: building"
	# gadget-lc3clips: offline clips in LC3 16 kb/s (CONFIG_MG_CLIP_LC3_16KBPS,
	# off by default). gadget-homelink: the default node id
	# (CONFIG_MG_NODE_ID_GADGET=n, as release images), set up end to end, with
	# the board hooks' stand-in (bsim/hooks.conf: tests/board_hooks.c through
	# tests/app_ext.cmake and Kconfig.ext, the board's audio source,
	# CONFIG_MG_SMP_GATE_BOARD).
	bsim_build gadget "$app" -DEXTRA_CONF_FILE=tests/bsim/gadget.conf &&
		bsim_build gadget-lc3clips "$app" -DEXTRA_CONF_FILE=tests/bsim/gadget.conf \
			-DCONFIG_MG_CLIP_LC3_16KBPS=y &&
		bsim_build gadget-homelink "$app" \
			-DEXTRA_CONF_FILE="tests/bsim/gadget.conf;tests/bsim/hooks.conf" \
			-DCONFIG_MG_NODE_ID_GADGET=n &&
		bsim_build central "$app/tests/bsim/central" &&
		for s in offline setup offline-lc3clips setup_homelink; do
			g=$out/gadget/zephyr/zephyr.exe
			sc=$s
			if [ "$s" = offline-lc3clips ]; then
				g=$out/gadget-lc3clips/zephyr/zephyr.exe
				sc=offline
			elif [ "$s" = setup_homelink ]; then
				g=$out/gadget-homelink/zephyr/zephyr.exe
			fi
			"$here/bsim/run.sh" "$g" "$out/central/zephyr/zephyr.exe" "$sc" 40 \
				> "$out/bsim_$s.log" 2>&1 || fail=1
			grep -E "INFO: \(|Test end|ERROR" "$out/bsim_$s.log" | sed 's/^d_0[01]: //'
			tail -1 "$out/bsim_$s.log"
			if [ "$s" = setup_homelink ]; then
				# The app called the board's hooks.
				for h in "board hooks: reset cause" "board hooks: setup state: not set up" \
					"board hooks: setup state: set up"; do
					grep -q "$h" "$out/bsim_$s.log" && echo "hook: $h" ||
						{ echo "FAIL: no \"$h\""; fail=1; }
				done
			fi
		done
fi
[ $fail -eq 0 ] && echo "ALL PASSED" || echo "SOME FAILED (logs in $out)"
exit $fail
