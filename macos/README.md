# Muse Companion for macOS

A native SwiftUI companion with a rotating SceneKit mascot, a prompt composer,
and streamed text replies from your Muse. Drag the mascot to orbit the camera,
scroll to zoom, or pause its rotation. The pixel mouth moves while answer
text streams, with the face turned toward you. Choose Hover, Wiggle, Hop, Happy
dance, or Peek from the motion menu, or press Shuffle for a random choice. Send with **⌘ Return**. **⌘ N** starts a
new side chat.

## Requirements

- macOS 14 or later, on Intel or Apple silicon.
- Swift 6 or later (Xcode or the Command Line Tools), to build.
- Docker Desktop running locally.
- A **previously paired** Linux SDK service running in a Docker container,
  updated to the SDK in this checkout. Pairing remains the Linux SDK's job;
  this companion does not implement Bluetooth setup or require XQuartz.

## Build and launch

From this directory:

```sh
swift test --disable-xctest
bash build.sh
open "dist/Muse Companion.app"
```

The build script produces an ad hoc signed local app. Distribution to other
Macs requires your own signing and notarization. The app has no third-party
Swift dependencies. Open `Package.swift` in Xcode to develop it.

The default container name is `muse-gadget-linux`. Change it under **Muse
Companion > Settings** if needed. The connection indicator checks whether the
container is running and has the streaming CLI; successful chat confirms the
Muse connection. Errors appear beside the composer.

## Update an existing paired container

Build the Linux runtime from the repository root:

```sh
docker build -f linux/Dockerfile --target runtime -t muse-gadget-linux:local .
```

Recreate your existing container with this image, keeping its pairing-state
mount at `/var/lib/musegadget` and its home volume at `/home/muse`. If you use
Compose, run `docker compose up -d --no-build` from your existing Compose
project. Check for active invokes before restarting, and confirm the service
logs say `registered with the Muse` afterwards. Do not delete the volumes or
pair again to upgrade the SDK.

The image runs device commands as the unprivileged `muse` account. Mac home
directories are not needed by the app or image. Docker on macOS does not pass
the Mac's Bluetooth adapter to Linux; initial pairing must already have been
completed through your chosen Linux/BLE setup.

## How chat works

1. Swift launches `docker exec -i <container> musegadget chat --json
   --session-id <uuid> -` using `Process` with an argument array.
2. The prompt travels on stdin, not through shell interpolation or process
   arguments. The CLI connects to the service's permission-controlled Unix
   socket.
3. The existing service subscribes to `/chat/subscribe`, scoped to the side
   chat's `session_id`, **before** posting to
   `/chat/stream`, over its authenticated Noise session. Device control commands
   continue on `/link-control` while replies stream. Muse returns 404 when a
   side chat does not exist yet: only in that case, its first prompt creates
   the chat before subscribing. The prompt is sent once. Exceptionally fast
   first replies can arrive during this initial subscription gap.
4. Assistant messages are correlated to the POST's acknowledgement and session.
   Parentless message starts are accepted only in the acknowledged session;
   subsequent deltas follow that assistant message id. The CLI
   emits `ack`, `status`, `reply`, `done`, or `error` NDJSON events. `reply.text`
   is a complete snapshot of that message so final persisted events replace,
   rather than duplicate, streamed text.
5. The app displays selectable Markdown text and keeps a side-chat UUID in
   UserDefaults. Prompts and replies are not persisted locally. New Chat starts
   a fresh UUID; the Muse account retains its own conversation history.

The app does not read SDK tokens or pairing files, expose a network listener,
or establish a second device identity. A completed turn settles after three
quiet seconds with all known assistant messages complete and the agent idle;
the reply wait is bounded to three minutes. Very long or unusually delayed
multi-message turns may hit this limit. Closing the window ends the local
viewer; it does not cancel an agent task already accepted by Muse.

## Tests

```sh
swift test --disable-xctest
```

The Swift Testing decoder tests split UTF-8 at every byte boundary, exercise multiple events,
and reject malformed, oversized, or truncated output. The Linux tests cover
reply correlation, buffering before the POST acknowledgement, subscription
limits, disconnect cleanup, and concurrent device control using a fake VM with
a real Noise handshake, including the first-message 404 setup. A real paired
Muse is required for an end-to-end chat. A debug build also accepts
`MUSE_TEST_PROMPT`, `MUSE_TEST_REPLY` (output text path), and `MUSE_TEST_SNAPSHOT`
(own-window PNG path) for an opt-in live UI smoke test. These probes are
compiled out of release builds.

## Mascot and license

The SceneKit mascot extrudes the original pixel silhouette into a closed 3D
mesh and maps the unmodified Jollybot artwork onto its front. The source is
[`esp32/avatar/jollybot.gif`](../esp32/avatar/jollybot.gif). This is a mesh based on that artwork rather
than a supplied official 3D asset. The repository's [artwork exclusion](../README.md#license)
continues to apply to the character design. The application and transport code
are Apache 2.0; this contribution does not grant rights to Muse trademarks or
relicense the Jollybot artwork.
