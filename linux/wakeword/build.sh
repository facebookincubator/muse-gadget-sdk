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

# Builds the native Hey Muse runner for Linux: libmuse_wakeword.so (what
# musegadget.wakeword loads) and ww-run (the same runner on a WAV or raw
# PCM file, for checks).
#
#   build.sh [--out DIR] [--deps DIR] [--offline]
#
# It compiles the runner the ESP32 firmware uses
# (esp32/components/muse_wakeword: ww_runner_espnn.cpp, ww_frontend.c,
# ww_detector.c, models/hey_muse.pte) against the same pinned ESP-IDF
# registry archives the firmware build fetches, each checked against its
# SHA-256 before it is unpacked:
#   pytorch/executorch           ExecuTorch runtime + ESP-NN delegate
#   espressif/esp-nn             the delegate's int8 kernels (portable C)
#   esphome/esp-micro-speech-features   the microfrontend (C)
# Archives come from components-file.espressif.com, or from --deps DIR
# (the same file names) when the machine has no network; --offline refuses
# to download. Needs cc, c++ (C++17), curl (unless --deps), unzip and
# sha256sum. One thread at run time; no Python or torch.
set -euo pipefail

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
WW="$HERE/../../esp32/components/muse_wakeword"
OUT="$HERE/build"
DEPS=""
OFFLINE=0
REGISTRY="https://components-file.espressif.com/components"

# name|version|archive|sha256, in step with the pins in
# ../../esp32/components/muse_wakeword/idf_component.yml
ARCHIVES=(
  "pytorch/executorch|1.6.0-dev.20261112|pytorch__executorch-v1.6.0-dev.20261112.zip|7885747a7a47fe81b72f52b52884c122439a3304b117d0774dc05af3e03ba312"
  "espressif/esp-nn|1.5.0|espressif__esp-nn-v1.5.0.zip|50bdc2b89b54c1fae5d308a5961bb7a261453bbcc4e384ff1c696dea7514e1ce"
  "esphome/esp-micro-speech-features|1.2.3|esphome__esp-micro-speech-features-v1.2.3.zip|052a8f32e7a82f49593bfdc067638a8cee6542443726d1a7e5b01b3bd9d67eac"
)

die() { printf 'wake-word build failed: %s\n' "$*" >&2; exit 1; }

while (( $# )); do
    case "$1" in
        --out) (( $# >= 2 )) || die "--out needs a directory"; OUT=$2; shift 2 ;;
        --deps) (( $# >= 2 )) || die "--deps needs a directory"; DEPS=$2; shift 2 ;;
        --offline) OFFLINE=1; shift ;;
        -h|--help) sed -n '16,33p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

CC=${CC:-cc}
CXX=${CXX:-c++}
for tool in "$CC" "$CXX" unzip sha256sum; do
    command -v "$tool" >/dev/null || die "$tool is needed"
done
mkdir -p "$OUT"
OUT=$(CDPATH='' cd -- "$OUT" && pwd)
# Scratch space of its own, so no --out can make this delete anything
# that was already there (with --out . from linux/, "src" would be the
# package itself).
WORK=$(mktemp -d "$OUT/.ww-build.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
SRC="$WORK/src"
OBJ="$WORK/obj"
mkdir -p "$OUT/archives" "$SRC" "$OBJ"

for entry in "${ARCHIVES[@]}"; do
    IFS='|' read -r name version file sha <<<"$entry"
    archive="$OUT/archives/$file"
    if [[ -n $DEPS && -f $DEPS/$file ]]; then
        cp -- "$DEPS/$file" "$archive.part"
        mv -- "$archive.part" "$archive"
    elif [[ ! -f $archive ]]; then
        (( ! OFFLINE )) || die "$file is not in ${DEPS:-the archive directory} and --offline is set"
        command -v curl >/dev/null || die "curl is needed to download $file (or pass --deps)"
        printf 'Downloading %s %s\n' "$name" "$version"
        curl -fsSL --retry 3 -o "$archive.part" "$REGISTRY/$name/$version/$file" ||
            die "couldn't download $name $version"
        mv -- "$archive.part" "$archive"
    fi
    actual=$(sha256sum -- "$archive" | awk '{print $1}')
    if [[ $actual != "$sha" ]]; then
        rm -f -- "$archive"
        die "$file doesn't match its SHA-256 (got $actual)"
    fi
    dir="$SRC/${name#*/}"
    mkdir -p "$dir"
    unzip -q -o "$archive" -d "$dir"
done

ET="$SRC/executorch"
NN="$SRC/esp-nn"
MF="$SRC/esp-micro-speech-features"

# The runtime's own source list, as the ESP-IDF component builds it.
mapfile -t et_core < <(sed -n '/^set(ET_CORE_SRCS/,/^)/p' "$ET/et_generated.cmake" | grep -o '"src/[^"]*"' | tr -d '"')
mapfile -t et_espnn < <(sed -n '/^set(ET_ESPNN_SRCS/,/^)/p' "$ET/et_generated.cmake" | grep -o '"src/[^"]*"' | tr -d '"')
(( ${#et_core[@]} > 10 && ${#et_espnn[@]} == 3 )) || die "unexpected ExecuTorch source lists"

COMMON=(-O2 -fPIC -ffunction-sections -fdata-sections -DNDEBUG -w)
ETDEF=(-DC10_USING_CUSTOM_GENERATED_MACROS -DET_USE_BUILTIN_FUNCTION_NAME=0
       -DFLATBUFFERS_MAX_ALIGNMENT=1024 -DET_ENABLE_PROGRAM_VERIFICATION=1
       -DET_LOG_ENABLED=0 -DET_MIN_LOG_LEVEL=Error)
CXXONLY=(-std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics)
INC_ET=(-I"$ET/include" -I"$ET/priv_include")
INC_NN=(-I"$NN/include" -I"$NN/src/common")
INC_MF=(-I"$MF/include")

MAXJ=$(nproc 2>/dev/null || echo 2)
pids=()
failed=0
n=0
compile() {
    local lang=$1 src=$2; shift 2
    local obj
    obj="$OBJ/$(printf '%s' "$src" | sha256sum | cut -c1-12)-$(basename "$src").o"
    if [[ $lang == c++ ]]; then
        "$CXX" "${COMMON[@]}" "${CXXONLY[@]}" "$@" -c "$src" -o "$obj"
    else
        "$CC" "${COMMON[@]}" -std=c11 "$@" -c "$src" -o "$obj"
    fi
}
# Each compile's own exit status, by PID: `wait PID` reports a job that
# already finished, which `wait -n` doesn't on every bash.
add() {
    compile "$@" &
    pids+=("$!")
    n=$((n + 1))
    if (( ${#pids[@]} >= MAXJ )); then
        wait "${pids[0]}" || failed=1
        pids=("${pids[@]:1}")
    fi
}
for f in "${et_core[@]}" "${et_espnn[@]}"; do
    add c++ "$ET/$f" -I"$WW/host" "${INC_ET[@]}" "${INC_NN[@]}" "${ETDEF[@]}"
done
add c++ "$WW/host/et_pal.cpp" "${INC_ET[@]}" "${ETDEF[@]}"
# esp-nn's portable C kernels; CONFIG_NN_OPTIMIZED picks its generic
# optimised C over the plain reference loops (same results).
for f in "$NN"/src/*/*_ansi.c "$NN"/src/convolution/esp_nn_conv_opt.c \
         "$NN"/src/convolution/esp_nn_depthwise_conv_opt.c "$NN"/src/softmax/esp_nn_softmax_opt.c \
         "$NN"/src/common/esp_nn_multicore.c; do
    add cc "$f" -I"$WW/host" "${INC_NN[@]}" -DCONFIG_NN_OPTIMIZED=1
done
for f in "$MF"/src/*.c; do
    add cc "$f" "${INC_MF[@]}" -I"$MF/src"
done
add cc "$WW/ww_frontend.c" -I"$WW" -I"$WW/include" "${INC_MF[@]}" -ffp-contract=off
add cc "$WW/ww_detector.c" -I"$WW" -I"$WW/include"
add c++ "$WW/ww_runner_espnn.cpp" -I"$WW" -I"$WW/include" "${INC_ET[@]}" "${ETDEF[@]}"
# Linux only: the detector tap's 24 to 16 kHz resampler.
add cc "$HERE/ww_resample.c" -ffp-contract=off
# The model, embedded 16-aligned as in the firmware.
pte="$WW/models/hey_muse.pte"
sed -e "s|@HEY_MUSE_PTE_PATH@|$pte|" -e "s|@HEY_MUSE_PTE_LEN@|$(stat -c %s "$pte")|" \
    "$WW/hey_muse_pte.S.in" > "$OBJ/hey_muse_pte.S"
add cc "$OBJ/hey_muse_pte.S"
for pid in "${pids[@]}"; do
    wait "$pid" || failed=1
done
(( ! failed )) || die "compilation failed"
printf 'Compiled %d files\n' "$n"

objs=("$OBJ"/*.o)
cat > "$OBJ/exports.map" <<'EOF'
{ global: ww_runner_create; ww_runner_destroy; ww_runner_reset; ww_runner_feed;
          ww_runner_set_listening; ww_runner_set_suppressed; ww_runner_ok;
          ww_runner_set_score_hook; muse_ww_resampler_create; muse_ww_resampler_destroy;
          muse_ww_resampler_reset; muse_ww_resampler_feed; local: *; };
EOF
"$CXX" -shared -o "$OUT/libmuse_wakeword.so.part" "${objs[@]}" -Wl,--gc-sections -Wl,-O1 \
    -Wl,--version-script="$OBJ/exports.map" -Wl,-z,defs -Wl,-z,noexecstack -Wl,-z,relro,-z,now -lm
"${STRIP:-strip}" --strip-unneeded "$OUT/libmuse_wakeword.so.part" 2>/dev/null || true
mv -- "$OUT/libmuse_wakeword.so.part" "$OUT/libmuse_wakeword.so"
# shellcheck disable=SC2016  # $ORIGIN is for the linker, not the shell
"$CC" -O2 -std=c11 -I"$WW/include" "$HERE/tools/ww_run.c" -o "$OUT/ww-run.part" \
    -L"$OUT" -l:libmuse_wakeword.so -Wl,-rpath,'$ORIGIN' -lm
mv -- "$OUT/ww-run.part" "$OUT/ww-run"
printf 'Built %s (%s bytes) and %s\n' "$OUT/libmuse_wakeword.so" "$(stat -c %s "$OUT/libmuse_wakeword.so")" "$OUT/ww-run"
