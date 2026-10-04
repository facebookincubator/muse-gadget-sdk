#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -eu
cd "$(dirname "$0")"
bundle=build/MuseBluetooth.app/Contents
mkdir -p "$bundle/MacOS" build/module-cache
cp Info.plist "$bundle/Info.plist"
xcrun swiftc -swift-version 5 -module-cache-path "$PWD/build/module-cache" \
  -target "$(uname -m)-apple-macosx14.0" \
  -framework CoreBluetooth -framework AppKit -O BluetoothBridge.swift -o "$bundle/MacOS/MuseBluetooth"
codesign --force --sign - build/MuseBluetooth.app
