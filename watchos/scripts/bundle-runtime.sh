#!/bin/bash
set -euo pipefail
source_app="${MUSE_RUNTIME_APP:-$SRCROOT/build/MuseWatchRuntime.app}"
runtime="$source_app/Contents/Resources/Runtime"
if [[ ! -x "$runtime/python/bin/python3" ]]; then
  echo 'Missing SDK runtime. Run scripts/prepare-runtime.sh first.' >&2
  exit 1
fi
destination="$TARGET_BUILD_DIR/$CONTENTS_FOLDER_PATH"
mkdir -p "$destination/Resources" "$destination/Helpers"
/usr/bin/ditto "$runtime" "$destination/Resources/Runtime"
/usr/bin/ditto "$source_app/Contents/Helpers" "$destination/Helpers"
