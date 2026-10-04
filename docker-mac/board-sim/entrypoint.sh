#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -euo pipefail
umask 077
mkdir -p /run/sshd /root/.ssh /etc/ssh/host-keys /var/lib/musegadget /run/musegadget
chmod 700 /root/.ssh /etc/ssh/host-keys /var/lib/musegadget
cp /opt/muse/client_key.pub /root/.ssh/authorized_keys
chmod 600 /root/.ssh/authorized_keys
if [ ! -f /etc/ssh/host-keys/ssh_host_ed25519_key ]; then
  ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/host-keys/ssh_host_ed25519_key
fi
musegadget run --run-as muse &
service_pid=$!
/usr/sbin/sshd -D -e &
ssh_pid=$!
cleanup() {
  kill "$service_pid" "$ssh_pid" 2>/dev/null || true
  wait "$service_pid" "$ssh_pid" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 0' INT TERM
wait -n "$service_pid" "$ssh_pid"
