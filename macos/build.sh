#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -euo pipefail
cd "$(dirname "$0")"
swift build -c release
binary_dir="$(swift build -c release --show-bin-path)"
app="dist/Muse Companion.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$binary_dir/MuseCompanion" "$app/Contents/MacOS/MuseCompanion"
cp -R "$binary_dir/MuseCompanion_MuseCompanion.bundle" "$app/Contents/Resources/"
cp Info.plist "$app/Contents/Info.plist"
swift make-icon.swift dist/Muse.iconset
iconutil -c icns dist/Muse.iconset -o "$app/Contents/Resources/Muse.icns"
codesign --force --sign - "$app"
printf 'Built %s\n' "$PWD/$app"
