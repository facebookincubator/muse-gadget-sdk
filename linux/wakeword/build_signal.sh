#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0.
set -eu
if [ "$#" -gt 1 ]; then
    printf 'Usage: %s [OUTPUT_DIR]\n' "$0" >&2
    exit 2
fi
root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
esp=$(CDPATH='' cd -- "$root/../../esp32/components/muse_wakeword" && pwd)
out=${1:-"$root/build"}
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)
cc=${CC:-cc}
temporary="$out/.libmuse_wakeword_signal.so.tmp.$$"
trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -fvisibility=hidden -shared \
    -I"$root" -I"$esp" -I"$esp/include" "$root/ww_signal.c" \
    "$esp/ww_frontend.c" "$esp/ww_detector.c" \
    -Wl,-z,defs -Wl,-z,text -Wl,-soname,libmuse_wakeword_signal.so \
    -lm -o "$temporary"
mv -- "$temporary" "$out/libmuse_wakeword_signal.so"
printf 'Built %s\n' "$out/libmuse_wakeword_signal.so"
