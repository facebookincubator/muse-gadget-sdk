#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -eu
cd "$(dirname "$0")/../.."
umask 077
mkdir -p .board-sim
chmod 700 .board-sim
if [ ! -f .board-sim/client_key ]; then
  ssh-keygen -q -t ed25519 -N '' -C muse-board-simulator -f .board-sim/client_key
fi
docker compose -f docker-mac/board-sim/compose.yaml build ma35d1-a1
docker compose -f docker-mac/board-sim/compose.yaml up -d --wait --no-build
for board in ma35d1-a1 ma35d1-s1 ma35h0-a1 ma35h0-a2; do
  case "$board" in
    ma35d1-a1) port=2224 ;;
    ma35d1-s1) port=2225 ;;
    ma35h0-a1) port=2226 ;;
    ma35h0-a2) port=2227 ;;
  esac
  # Obtain the host key through Docker, never through an unverified SSH scan.
  docker compose -f docker-mac/board-sim/compose.yaml exec -T "$board" \
    cat /etc/ssh/host-keys/ssh_host_ed25519_key.pub > ".board-sim/${board}_host_key.pub"
  awk -v port="$port" '{print "[127.0.0.1]:" port " " $1 " " $2}' \
    ".board-sim/${board}_host_key.pub" > ".board-sim/${board}_known_hosts"
  chmod 600 ".board-sim/${board}_known_hosts"
  printf '%s\n' "$board (SSH localhost:$port)"
  ssh -T -p "$port" -i .board-sim/client_key -o IdentitiesOnly=yes -o BatchMode=yes \
    -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=.board-sim/${board}_known_hosts" \
    root@127.0.0.1 'uname -m; /opt/musegadget/venv/bin/musegadget info'
done
printf '%s\n' 'All four ARM64 board simulators ready. See docker-mac/board-sim/README.md.'
