#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -eu
sdk_src=$(cd "${1:?Pass the Linux SDK directory from the companion app checkout}" && pwd)
shift
for board in "$@"; do
  case "$board" in
    ma35d1-a1|ma35d1-s1|ma35h0-a1|ma35h0-a2) ;;
    *) printf '%s\n' "Unknown board: $board" >&2; exit 1 ;;
  esac
done
for module in chat cli link_client service; do
  test -f "$sdk_src/src/musegadget/$module.py"
done
cd "$(dirname "$0")/../.."
umask 077
mkdir -p .board-sim/ai-build
for module in chat cli link_client service; do
  cp "$sdk_src/src/musegadget/$module.py" ".board-sim/ai-build/$module.py"
done
cp docker-mac/board-sim/Dockerfile.ai .board-sim/ai-build/Dockerfile
docker build --platform linux/arm64 -t muse-ma35-board-sim-ai:local .board-sim/ai-build
docker compose -f docker-mac/board-sim/compose.yaml \
  -f docker-mac/board-sim/compose-ai.yaml up -d --wait --no-build "$@"
