#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -euo pipefail
cd "$(dirname "$0")"
# A local macOS Python helper reuses the SDK's pairing and Noise protocol.
if [ ! -x .runtime/bin/python3 ]; then python3 -m venv .runtime; fi
.runtime/bin/python3 -m pip install --only-binary=:all: 'cryptography>=44,<45' 'websockets>=13,<16'
swift build -c release
binary_dir="$(swift build -c release --show-bin-path)"
app="dist/Muse Companion.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$binary_dir/MuseCompanion" "$app/Contents/MacOS/MuseCompanion"
cp -R "$binary_dir/MuseCompanion_MuseCompanion.bundle" "$app/Contents/Resources/"
cp Info.plist "$app/Contents/Info.plist"
mkdir -p "$app/Contents/Resources/Runtime/sdk/musegadget" "$app/Contents/Resources/Runtime/python" "$app/Contents/Helpers/MuseBluetooth.app/Contents/MacOS"
# Resolve venv executable symlinks inside the app so its signature is valid.
# Python's linked framework is still supplied by the build Mac's installation.
python3 - "$app/Contents/Resources/Runtime" <<'PY'
from pathlib import Path
import shutil
import sys
runtime = Path(sys.argv[1])
destination = runtime / 'python'
if destination.exists(): shutil.rmtree(destination)
shutil.copytree('.runtime', destination, symlinks=False, ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '*.pyo'))
sdk = runtime / 'sdk/musegadget'
if sdk.exists(): shutil.rmtree(sdk)
shutil.copytree('../linux/src/musegadget', sdk, ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '*.pyo'))
for cache in runtime.glob('__pycache__'):
    shutil.rmtree(cache)
PY
cp Backend/native_backend.py Backend/mac_ble.py "$app/Contents/Resources/Runtime/"
swiftc Bluetooth/main.swift -o "$app/Contents/Helpers/MuseBluetooth.app/Contents/MacOS/MuseBluetooth"
cp Bluetooth/Info.plist "$app/Contents/Helpers/MuseBluetooth.app/Contents/Info.plist"
codesign --force --sign - "$app/Contents/Helpers/MuseBluetooth.app"
swift make-icon.swift dist/Muse.iconset
iconutil -c icns dist/Muse.iconset -o "$app/Contents/Resources/Muse.icns"
codesign --force --deep --sign - "$app"
printf 'Built %s\n' "$PWD/$app"
