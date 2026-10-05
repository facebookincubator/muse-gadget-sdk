#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ! -x .runtime/bin/python3 ]]; then python3 -m venv .runtime; fi
.runtime/bin/python3 -m pip install --only-binary=:all: 'cryptography>=44,<45' 'websockets>=13,<16'
app="build/MuseWatchRuntime.app"
mkdir -p "$app/Contents/Resources/Runtime" "$app/Contents/Helpers/MuseBluetooth.app/Contents/MacOS"
python3 - "$app/Contents/Resources/Runtime" <<'PY'
from pathlib import Path
import shutil
import sys
runtime = Path(sys.argv[1])
for source, destination in [(Path('.runtime'), runtime / 'python'),
                            (Path('../linux/src/musegadget'), runtime / 'sdk/musegadget')]:
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(source, destination, symlinks=False,
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '*.pyo'))
for name in ('native_backend.py', 'mac_ble.py'):
    shutil.copy2(Path('Backend') / name, runtime / name)
shutil.copy2('../LICENSE', runtime / 'LICENSE')
PY
xcrun --sdk macosx swiftc Bluetooth/main.swift -o "$app/Contents/Helpers/MuseBluetooth.app/Contents/MacOS/MuseBluetooth"
cp Bluetooth/Info.plist "$app/Contents/Helpers/MuseBluetooth.app/Contents/Info.plist"
codesign --force --sign - "$app/Contents/Helpers/MuseBluetooth.app"
