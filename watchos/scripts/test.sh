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
mkdir -p build/tests build/ModuleCache
xcrun swiftc -module-cache-path build/ModuleCache Shared/Wire.swift Shared/HeartRateSummary.swift Shared/HealthReview.swift Tests/main.swift -o build/tests/protocol-tests
build/tests/protocol-tests
