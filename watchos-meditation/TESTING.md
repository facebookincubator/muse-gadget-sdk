# Verification

## Source package — 2026-10-06

This package contains the working Muse Calm build 5 sources, made independent
of the separate local watch app. Shared protocol/client sources are included,
Xcode configuration has no personal team, and provisioning accepts your own
backend and SDK state directory. Personal credentials, models, downloaded audio,
generated Xcode projects and build output are excluded.

- Swift: **35 checks passed** with `bash scripts/test.sh`, covering guide parsing,
  clock/phrase scheduling, recipes, retry policy and saved-audio selection.
- Python: **75 tests passed** with `python -m pytest -q` in `Cloud/`, covering
  authentication, bounded inputs, Redis leases/request IDs, cancellation, audio
  streaming/validation, sound generation, token refresh and portable provisioning.
- The staged source was checked against the local watch/Redis credentials and
  for common token/private-key formats; no matching secrets were found.
- Standalone unsigned watchOS build **passed**. The produced build 5 app contains
  the audio background mode and executable, with no bootstrap or historical water
  recording. Relative documentation links resolve.

The original personal build previously completed live Muse guide and audio
checks and was installed on a physical watch. This source packaging does not
deploy another public backend or perform a new enrollment. New deployments must
run the documented probes with their own credentials.

## Reproduce

```sh
# From watchos-meditation, with Xcode selected:
bash scripts/test.sh
xcodegen generate
xcodebuild -project MuseCalm.xcodeproj -scheme MuseCalm \
  -configuration Debug -destination 'generic/platform=watchOS' \
  -derivedDataPath build CODE_SIGNING_ALLOWED=NO build

cd Cloud
.venv/bin/python -m pytest -q
```

After completing [backend setup](Cloud/README.md), run both live probe scripts
from the app directory, then rerun Swift tests to validate the saved guide JSON.
Live checks use your account and backend resources; the unit tests use local
data and mock network boundaries.

Listen on the physical watch to verify audio routing, voice quality, crossfades,
the first download, faster cached startup, wrist-down playback and End/cancel.
Sample-level checks cannot establish whether a procedural texture sounds soothing.
The package does not promise uninterrupted generation during cold starts or
network outages, or continuous playback after its buffer runs out.

## Release and rollback

App version 1.0/build 5 contains full-screen selection lists, persistent cached
openings, Muse-directed CPU sound recipes and Kokoro speech. Older experimental
Stable Audio assets and generators are not shipped. No storage migration is
required for a clean install; the app can recognize its earlier cache format.

Reinstall a prior app build to roll back the client. Roll back Vercel code using
the deployment history and verify the production alias. Do not roll back Redis
Muse credentials, because refresh tokens rotate. The current cache is local to
the app; uninstalling removes its saved recordings.
