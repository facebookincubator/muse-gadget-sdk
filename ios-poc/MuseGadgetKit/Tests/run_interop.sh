#!/usr/bin/env bash
# Run the Swift LinkSession against a fake VM built from the Linux SDK's Noise code.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
port="${FAKE_VM_PORT:-18765}"

cd "$repo/linux"
uv run --quiet --with . python "$here/fake_vm.py" "$port" > "$here/.fake_vm.log" 2>&1 &
vm=$!
for _ in $(seq 50); do grep -q READY "$here/.fake_vm.log" 2>/dev/null && break; sleep 0.2; done

cd "$here/.."
FAKE_VM_PORT="$port" swift test --filter LiveInteropTests
status=0
wait "$vm" || status=$?
cat "$here/.fake_vm.log"
rm -f "$here/.fake_vm.log"
exit "$status"
