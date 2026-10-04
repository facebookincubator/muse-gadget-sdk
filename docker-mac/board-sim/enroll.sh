#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -eu
cd "$(dirname "$0")/../.."
board=${1:-}
case "$board" in
  ma35d1-a1) port=2224 ;;
  ma35d1-s1) port=2225 ;;
  ma35h0-a1) port=2226 ;;
  ma35h0-a2) port=2227 ;;
  *) printf '%s\n' 'Usage: enroll.sh ma35d1-a1|ma35d1-s1|ma35h0-a1|ma35h0-a2 [relay options]' >&2; exit 1 ;;
esac
shift
exec .mac-venv/bin/python docker-mac/pair-board.py root@127.0.0.1 \
  --port "$port" --ssh-key .board-sim/client_key \
  --known-hosts ".board-sim/${board}_known_hosts" "$@"
