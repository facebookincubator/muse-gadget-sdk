#!/bin/bash
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

# Live-path bench: plays a raw clip through Muse Gadgets Camera's own camera path
# (crop/scale, encoder, buffering, the link) to a gadget, then reports
# capture-to-shown latency and scores what the gadget showed.
#
#   tools/livebench/run.sh TAG CLIP SIZE [extras...]
#
# CLIP: files/CLIP.yuv on the phone, 640x480 I420 at 30 fps (push with
#   adb push clip.yuv /sdcard/Android/data/com.musegadgets.camstream/files/CLIP.yuv)
#   and the same file as $CLIPS/CLIP_640x480.yuv here, for the score.
# SIZE: the encoded size (--ei size). Extras go to the app (e.g. --es sw hw,
#   --ei pf 0, --ei live 0, --ez adaptive false --ei kbps 250).
# LOOPS (default 2): times through the clip. TARGET: gadget name (default: the
# first found). PANEL: the panel size to score at (default 2 x SIZE).
# Needs tinyhost: ./build.sh here first.
set -euo pipefail
tag=$1; clip=$2; sz=$3; shift 3
here=$(cd "$(dirname "$0")" && pwd)
loops=${LOOPS:-2}
out=${OUT:-/tmp/livebench}/$tag
CLIPS=${CLIPS:?set CLIPS to the directory holding CLIP_640x480.yuv}
pkg=com.musegadgets.camstream
adb shell am force-stop $pkg
adb logcat -c
adb shell rm -rf /sdcard/Android/data/$pkg/files/bench
adb shell am start -n $pkg/.MainActivity ${TARGET:+--es target "$TARGET"} --ez auto true --es source "file:$clip.yuv" \
    --ez benchdump true --ez latlog true --ei benchloops "$loops" --ei size "$sz" "$@" >/dev/null
sleep $((10 * loops + 10))
rm -rf "$out"; mkdir -p "$out"
adb logcat -d -s MuseCam:* > "$out/log.txt"
adb shell am force-stop $pkg
adb pull /sdcard/Android/data/$pkg/files/bench "$out/" >/dev/null
# latency: one "lf N lat enc queue air" line per frame the gadget reported drawn
grep -a ' lf ' "$out/log.txt" | awk '{print $(NF-3)}' | sort -n | \
    awk '{a[NR]=$1} END {if (NR) printf "latency capture->shown: median %d p90 %d max %d ms (%d frames)\n", a[int(NR/2)+1], a[int(NR*0.9)+1], a[NR], NR}'
grep -a 'stats:' "$out/log.txt" | sed 's/.*stats: //' | tr '|' '\n' | grep -o 'decode [0-9]* ms' | \
    awk '{s+=$2; n++} END {if (n) printf "gadget decode: mean %.1f ms\n", s/n}'
python3 "$here/score.py" "$out/bench" "$CLIPS/${clip}_640x480.yuv" "$sz"
