#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -eu
cd "$(dirname "$0")/.."
if [ ! -x .mac-venv/bin/python ] || [ ! -x docker-mac/build/MuseBluetooth.app/Contents/MacOS/MuseBluetooth ]; then
  sh docker-mac/setup-mac.sh
fi
if [ -f .muse-state/pairing.json ]; then
  docker compose up --build -d
  docker compose exec muse musegadget info
else
  .mac-venv/bin/python docker-mac/pair-mac.py
  docker compose up --build -d
fi
