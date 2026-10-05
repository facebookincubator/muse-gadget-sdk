#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${DEVELOPER_DIR:-}" ]]; then
  selected="$(xcode-select -p 2>/dev/null || true)"
  if [[ -d "$selected/Platforms/WatchOS.platform" ]]; then
    export DEVELOPER_DIR="$selected"
  elif [[ -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  else
    echo 'Set DEVELOPER_DIR to the Contents/Developer directory of a full Xcode installation.' >&2
    exit 1
  fi
fi
if [[ -z "${MUSE_RUNTIME_APP:-}" ]]; then
  bash scripts/prepare-runtime.sh
fi
xcodegen generate
xcodebuild -project MuseWatch.xcodeproj -scheme MuseWatchRelay -configuration Debug -derivedDataPath build CODE_SIGNING_ALLOWED=NO build
codesign --force --deep --sign - 'build/Build/Products/Debug/MuseWatchRelay.app'
xcodebuild -project MuseWatch.xcodeproj -scheme MuseWatch -configuration Debug -destination 'generic/platform=watchOS' -derivedDataPath build CODE_SIGNING_ALLOWED=NO build
