# Deploy the meditation backend on Vercel

This directory is the Vercel project root. It contains a Python 3.12 FastAPI
function with a 120-second configured maximum duration. The watch needs an HTTPS
origin that reaches this function without a Vercel login page. App endpoints use
a separate bearer key; `/health` is public liveness only.

## 1. Prepare dependencies and project

You need a Vercel account/project, an Upstash Redis database with REST access,
and a dedicated enrolled Muse gadget. The enrollment step below reuses the
SDK's existing pairing flow; this backend does not implement cloud-only pairing.

```sh
cd watchos-meditation/Cloud
python3.12 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/python -m pytest -q
vercel login
vercel link
```

Choose your own Vercel account/team and a new project. Keep the root directory
at `watchos-meditation/Cloud` if importing the repository through the dashboard.
For CLI deployment, run commands here. `vercel.json` routes requests to
`api/index.py`; `.python-version` selects Python 3.12. Keep the tested dependency
pins. Local environments, provisioning tools and test files are excluded by
`.vercelignore`. The app itself is built with Xcode, not deployed to Vercel.

The function needs CPU inference, outbound HTTPS and enough memory/time for
NumPy arrays and Kokoro. Check your project's available limits before deploying.
This implementation does not depend on a public GPU demo or a persistent server.

## 2. Create the watch key and set Vercel variables

Use the production domain assigned to your project, for example:

```sh
.venv/bin/python provision.py prepare --address https://YOUR-PROJECT.vercel.app
```

This creates ignored, owner-readable files:

- `../build/cloud-access.json`: the randomly generated raw watch key.
- `../build/vercel-watch-env.json`: its SHA-256 hash.
- `../Watch/CloudBootstrap.json`: the HTTPS origin and raw watch key for your
  personal Xcode build.

Repeating `prepare` preserves the key and can update the origin. Do not delete
the key file casually: a new key also requires updating the server hash and
the app's saved Keychain entry. Run XcodeGen after preparing the bootstrap.

In **Vercel project → Settings → Environment Variables**, add these for
**Production** as sensitive values:

| Variable | Value |
|---|---|
| `UPSTASH_REDIS_REST_URL` | HTTPS REST endpoint from your own Upstash database. |
| `UPSTASH_REDIS_REST_TOKEN` | REST token for that database. |
| `WATCH_API_TOKEN_SHA256` | The 64-character hash from `../build/vercel-watch-env.json`. |

Never put the raw watch key, Redis credentials or Muse enrollment in Git.
The raw key is not a Vercel variable. The watch receives no Redis/Muse credentials.
The server hashes incoming bearer keys and compares with the configured hash.

For the local enrollment tool, copy `.env.example` to `.env.redis.local` and
edit its two values privately. Values must be JSON-quoted strings, as in the
example. Alternatively export the two variables in the local environment.
These credentials are needed locally only for provisioning/checking Redis.

## 3. Transfer a dedicated Muse enrollment once

Follow the [Linux SDK pairing instructions](../../linux/README.md) to pair a
dedicated gadget with Muse. Stop its `musegadget` service before transferring
state, and leave it stopped afterwards. For a Linux installation this is
`sudo systemctl stop musegadget`. Do not share the enrollment with a concurrently
running gadget: refresh tokens rotate and the two clients can invalidate each other.

Securely copy its `identity.json`, `pairing.json` and `sdk_token` into a private
local directory outside the repository. The default Linux state directory is
`/var/lib/musegadget`; a custom `MUSEGADGET_STATE_DIR` changes it. An already-paired
Mac relay using the same SDK state format can also supply these three files.

```sh
.venv/bin/python provision.py enroll --state-dir /private/path/to/device-state
```

The helper checks Redis, validates the identity and required credentials, then
uses `SET NX` to store `muse:credentials`. It refuses to overwrite existing
cloud credentials. This protects newer rotated tokens from stale local copies.
Use a dedicated Redis database for this example: key names are fixed, and one
Muse request at a time holds the shared `muse:lease`.

## 4. Deploy and verify

```sh
vercel --prod
curl --fail https://YOUR-PROJECT.vercel.app/health
.venv/bin/python probe.py --address https://YOUR-PROJECT.vercel.app
```

Use `--scope YOUR-TEAM` if needed. Verify the domain used in the watch bootstrap
actually points at the new deployment; an extra manually assigned alias can
remain on an older deployment. Environment changes require a new deployment.
Configure deployment protection so the intended production API is reachable
by the watch's bearer-authenticated HTTPS requests without Vercel browser login.
Do not disable the API's bearer authentication.

`probe.py` checks real Muse connectivity without sending a chat. Add `--chat`
for one harmless test message; never repeatedly resend a user's failed message.
Then, from the app directory:

```sh
cd ..
python3 scripts/probe-guide.py
python3 scripts/probe-audio.py
bash scripts/test.sh
```

The guide probe validates stream completion and saves the response for the
Swift parser test. The audio probe checks distinct sound seeds, complete WAVs,
SHA-256 digests, durations and both neural voices. It writes private audition
artifacts under `build/`. Listen on the watch to assess actual sound quality,
startup latency and wrist-down/background playback.

## Endpoints and data flow

`POST /v1/muse` accepts `check` or `chat`, a canonical UUID `request_id`, and
for chat a session UUID and message. It streams bounded NDJSON ending in
`turn_finished`. The SDK opens an outbound encrypted Muse connection per request;
there is no permanent WebSocket server or watch-side socket connection.
No host commands are advertised; tool invocations are rejected.

`POST /v1/calm/audio` accepts a request UUID plus a bounded sound recipe, or
`kind: "voice"`, `voice: "af_heart"`/`"af_nicole"` and text of 1–240 characters
(maximum 1000 UTF-8 bytes). The shared renderer also supports `am_michael`.
Sound and voice have separate Redis generation leases. Audio is fully rendered
before the WAV response is streamed. See [sound generation](../SOUND_GENERATION.md).

Redis stores enrollment and rotated tokens, a 150-second Muse lease, 24-hour
Muse request IDs, 115-second sound/voice leases and one-hour audio request IDs.
It does not store the audio or spoken text. Muse processes the guide prompt and
reply; Vercel processes the recipe and TTS text. The included shared relay also
supports optional walking-news requests; Muse Calm does not use that feature.

Kokoro's pinned fp16 weights and voice embeddings download on first use, are
SHA-256 verified and cached under `/tmp`. They are about 192 MB combined. Temporary
storage is disposable, so a cold worker may download them again. The sound path
uses NumPy and needs no model download. Cold starts, synthesis and large WAV
transfers can exhaust a request budget; saved watch audio reduces startup waits
but cannot guarantee uninterrupted fresh generation.

## Troubleshooting and rollback

- `401`: check the watch key against the deployed hash; check Keychain precedence.
- HTML or a redirect: check deployment protection and the exact production alias.
- `409`: a lease/request ID is busy or already accepted. Do not repeat a Muse
  chat automatically; audio clients retry transient failures with fresh IDs.
- `503` or timeout: inspect Vercel logs for Redis connectivity, expired enrollment,
  cold model downloads, CPU time or memory limits. Never log credentials.
- Enrollment already exists: stop provisioning; do not overwrite rotated state.

Roll back **code** to a previous Vercel deployment if needed and check all aliases.
Do not restore an old Redis credentials snapshot: it can contain invalid refresh
tokens. Returning enrollment to a local gadget requires stopping cloud traffic
and transferring the current credentials under the lease, not restarting from
the original pre-cloud pairing file.

This is a single-user example. A multi-user service needs separate authentication,
enrollment isolation, per-user storage/quotas and an app provisioning flow.

## SDK snapshot

`musegadget/` is the narrow transport snapshot used by the working prototype,
including its `chat.py` subscription/correlation adapter and `LinkSession`
chat additions. It is vendored so this directory deploys independently of the
repository root. It is not a replacement for the full Linux service; it does
not include BLE pairing or a shell executor. Noise transport code is retained.
Copyright notices and `LICENSE-SDK` are included. Tests cover the relay boundary;
keep the snapshot explicit when updating SDK internals.
