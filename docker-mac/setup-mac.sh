#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -eu
cd "$(dirname "$0")/.."
umask 077
mkdir -p .muse-state
chmod 700 .muse-state
python3 -m venv .mac-venv
.mac-venv/bin/python -m pip install --require-hashes -r linux/src/musegadget/data/requirements.lock
# Select a compatible prebuilt cryptography wheel on both Intel and Apple Silicon.
# A newer source-only release would otherwise require Rust and OpenSSL tooling.
.mac-venv/bin/python -m pip install --only-binary=cryptography -e ./linux
sh docker-mac/build-helper.sh
printf '%s\n' 'Ready. Run: .mac-venv/bin/python docker-mac/pair-mac.py'
