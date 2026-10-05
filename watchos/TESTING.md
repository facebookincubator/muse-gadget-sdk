# Verification and device testing

The prototype has been built with Xcode 26.3 on macOS 15.8.1. The signed watch
app installed on a physical Apple Watch Series 11 running watchOS 26.1. The Mac
backend completed phone enrollment with a saved SDK token and reported online.
The user reached the watch's system dictation sheet. No personal Health data
was retrieved as part of automated verification.

Full watch-to-Muse conversation, repeated-turn speech, approval revocation and
HealthKit behavior still require the hardware smoke test in [README.md](README.md).
Do not infer those outcomes from compilation or backend enrollment.

## Automated checks

From `watchos/`:

```sh
bash scripts/test.sh
bash scripts/build.sh
.runtime/bin/python3 -m pip install pytest
PYTHONPATH=../linux/src .runtime/bin/python3 -m pytest -q ../linux/tests Backend
codesign --verify --deep --strict build/Build/Products/Debug/MuseWatchRelay.app
```

Swift checks cover fragmented UTF-8/JSON framing, command allowlists, repeated
conversation turns, and synthetic Health summaries including percentages,
overlapping sleep intervals, missing data and message-size limits. Python tests
cover reply streaming against a fake VM, backend turn/cancellation behavior,
SDK-token validation and file permissions, and the native BLE helper interface.

## Xcode shared-cache preparation

During physical deployment, Xcode lost its developer connection while fetching
the watch's shared cache. Logs showed CoreDevice 4011 followed by POSIX 32
(`Broken pipe`), and subsequent launch attempts returned CoreDevice 4000 /
RemotePairing 1001 tunnel timeouts. The Mac had 15 GiB available. Installation
had already succeeded; the underlying cause of the connection drop was not
established.

Keep the watch unlocked and nearby, enable Wi-Fi on watch and Mac, and check
network reachability when retrying Xcode device preparation. An installed app
can also be opened from the watch's app list. A successful install is separate
from successful debugger preparation. No cache deletion or device reset is
required by the app.

## Distribution limits

This is a development prototype. Physical installation requires local signing
and HealthKit provisioning. The relay's Python framework is supplied by the
build Mac; the resulting app is not a portable or notarized release. Runtime
credentials live outside the checkout and must never be committed.

## Fresh PR checkout validation

- 232 Swift protocol, conversation and synthetic Health checks passed.
- macOS relay and watchOS unsigned builds passed with Xcode 26.3.
- All 186 SDK/backend tests passed on the current Python environment and again
  on Python 3.9 with cryptography 3.3.2 and websockets 13.1.
- Generated Xcode projects, user data, builds, local credentials and signing
  material are excluded; project.yml recreates the project locally.
