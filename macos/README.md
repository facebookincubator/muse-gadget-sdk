# Muse Companion for macOS

A native SwiftUI companion with a rotating SceneKit mascot, a prompt composer,
and streamed text replies from your Muse. Drag the mascot to orbit the camera,
scroll to zoom, or pause its rotation. The pixel mouth moves while answer
text streams, with the face turned toward you. Choose Hover, Wiggle, Hop, Happy
dance, or Peek from the motion menu, or press Shuffle for a random choice. Send with **⌘ Return**. **⌘ N** starts a
new side chat.

## Direct pairing on this Mac

The app pairs over the Mac's Bluetooth adapter and connects directly to Muse
using the Mac's internet connection. It starts its own SDK session on macOS;
Docker, Linux, and XQuartz are not required.

1. Open **Pair this Mac** in the chat header.
2. Paste your SDK token from [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens)
   into the secure field and choose **Start pairing**. If a token is already
   saved for this Mac, the app reuses it.
3. Allow Bluetooth if macOS asks. Keep the Bluetooth pairing window open.
4. In the Muse phone app, enable **Settings → Devices → Developer mode**, choose
   **Add Device**, select the displayed `MuseGadgetXXXXXX` name, and approve.
5. After pairing, wait for **Connected directly • ready to chat**. Then send a
   message with **⌘ Return**. **Cancel** ends a local reply wait.

Pairing remains open for up to ten minutes. **Stop pairing** or closing the
Bluetooth helper window ends setup. Existing pairing reconnects when the app
opens. No Linux gadget credentials are imported; this Mac has its own identity.

## Build and launch

Requires macOS 14+, Swift 6+, and a local Python 3.9+ installation. From here:

```sh
swift test --disable-xctest
bash build.sh
open "dist/Muse Companion.app"
```

The build creates an ad hoc signed app and a native CoreBluetooth helper. It
copies the SDK's Python sources and an environment containing cryptography and
websockets into the app. The environment currently uses the build Mac's Python
installation, so this build is intended for that Mac; distributing a portable
app requires bundling Python itself, signing and notarization.

## Connection and storage

The SDK handles the existing community pairing protocol, device-token refresh,
Noise session, reconnects and streamed chat replies. CoreBluetooth handles only
BLE packets. The companion registers as a macOS Home Link with no shell or file
commands. Prompts and SDK tokens travel through private stdin pipes, without
shell interpolation or credentials in process arguments. No local TCP or Unix
socket is exposed by this app.

Identity, SDK token and phone-provisioned device credentials are stored under
`~/Library/Application Support/Muse Companion/Device/`, readable only by the
current user. They persist across launches. Conversations use a side-chat UUID
stored in UserDefaults; prompts and replies remain in memory locally. The Muse
account retains its own conversation history. Closing the app stops its local
connection and BLE helper; it does not cancel an agent task already accepted by
Muse. Reply waits retain the SDK's three-minute limit.

## Tests

```sh
swift test --disable-xctest
PYTHONPATH=../linux/src .runtime/bin/python3 -m unittest discover -s Backend -p 'test_native_*.py'
```

The tests cover event decoding, direct repeated text turns, cancellation,
registration, SDK-token validation and credential permissions. BLE transport
tests use a fake native helper. The SDK suite covers encrypted pairing fixtures
and real Noise handshakes against a fake VM. Pairing with the Muse phone app and
a live reply are required for an end-to-end hardware check.

## Mascot and license

The SceneKit mascot extrudes the original pixel silhouette into a closed 3D
mesh and maps the unmodified Jollybot artwork onto its front. The source is
[`esp32/avatar/jollybot.gif`](../esp32/avatar/jollybot.gif). This is a mesh based on that artwork rather
than a supplied official 3D asset. The repository's [artwork exclusion](../README.md#license)
continues to apply to the character design. The application and transport code
are Apache 2.0; this contribution does not grant rights to Muse trademarks or
relicense the Jollybot artwork.
