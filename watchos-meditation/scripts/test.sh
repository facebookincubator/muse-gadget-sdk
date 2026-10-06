#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests build/ModuleCache
xcrun swiftc -module-cache-path build/ModuleCache Shared/CalmPlan.swift Shared/CalmSoundscape.swift Shared/CalmAudioCache.swift Tests/main.swift -o build/tests/calm-tests
build/tests/calm-tests
