# Muse Watch prototype

A native watchOS voice-chat app and a macOS Bluetooth relay. The watch uses
system dictation, displays streamed Muse text, and speaks the completed reply
with `AVSpeechSynthesizer`. Dictation opens Apple's text-entry sheet; tap its
microphone if the keyboard appears, then review the text and tap **Send**.

The Mac must be awake, online, nearby, and running **Muse Watch Relay**. This is
foreground, turn-based voice chat, not a background or full-duplex assistant.

## Health check-in (pending physical-device verification)

First verify the basic connection, dictation and spoken reply below. Then open
**Health check-in**, choose 7, 30 or 90 days, and tap **Read Apple Health**.
Apple asks for read permission by category; select only the categories you want.
The public SDK catalog contains 189 quantity/category types; types newer than
this watch's OS are skipped. Workouts are included separately. No Health data
is read at launch or sent automatically.

The review displays available measurements, sleep, symptom entries, health
notifications, nutrition, reproductive health, and other permitted categories.
It reads up to the latest 128 records per category within the selected period,
not an all-history export. Quantity summaries preserve units, dates, source
counts and sampling limits; cumulative increments are not added into daily
totals. Sleep intervals are merged within each stage to avoid duplicate time,
but stages from different sources can still overlap and must not be added.
Missing records do not distinguish denied access from no data.

Deselect any summaries you do not want to share, add current symptoms or
questions, and tap **Send & hear check-in**. This sends selected summaries and
context through the Mac to your Muse conversation and enables spoken replies.
Muse is prompted to explain observations briefly, state uncertainty, ask one
follow-up at a time, and avoid diagnoses or treatment changes. Continue by
using normal dictation. This is health information, not a clinical appointment.

This watch-only implementation excludes clinical documents, medication lists,
ECG waveforms, routes, demographics and other specialized records. It cannot
read every app's private data or provide a complete medical chart. Selected
summaries exceeding the relay's 32 KB message budget are explicitly marked as
omitted. **Clear health summary** cancels pending queries and clears local
summary state, but does not remove already-sent Muse conversation messages.

## Heart-rate context

Tap **Heart rate → Read Apple Health** on the watch and grant read access.
Review up to 12 samples from the past 24 hours, optionally add context such as
rest or recent exercise, and tap **Ask Muse to explain**. That action sends
heart-rate values, timestamps, and your context as a chat message through the
Mac relay to your Muse account. No Health data is sent automatically. Samples
remain in app memory until cleared or the app exits; sent samples become part
of the Muse conversation and follow that account's retention settings.

This reads existing intermittent HealthKit samples, not a live sensor stream.
The prompt asks Muse to explain observations and uncertainty without diagnosing
illness or inferring that a measurement was taken at rest. The app requests
read-only heart-rate access and never writes Health data. No readings may mean
either no recent samples or unavailable read permission; Apple deliberately
does not let an app distinguish those cases. The target includes the HealthKit
entitlement, which must also be enabled in its Apple provisioning profile.

## Build

Requires a full Xcode installation with watchOS support, XcodeGen, and Python 3.9 or later. Tested with Xcode 26.3 and a Series 11
running watchOS 26.1. Deployment targets are watchOS 10 and macOS 14; your Xcode
version must also support the physical device's OS.

From the repository root:

```sh
(cd watchos && bash scripts/build.sh)
(cd watchos && bash scripts/test.sh)
```

The build prepares its own Python SDK runtime and native Bluetooth pairing
helper from `Backend/`, `Bluetooth/` and `../linux/src/musegadget`. Set
`MUSE_RUNTIME_APP` to reuse a compatible prebuilt runtime app instead. The scripts
respect `DEVELOPER_DIR`, otherwise use the selected full Xcode installation or
`/Applications/Xcode.app`. For another installation, set it explicitly:

```sh
export DEVELOPER_DIR="/path/to/Xcode.app/Contents/Developer"
```

The build script generates an ignored local `MuseWatch.xcodeproj`, builds both targets unsigned,
and ad hoc signs the Mac relay. No personal Apple team is committed. To install
on a physical watch, open the project, select **MuseWatch → Signing & Capabilities**,
choose your team with automatic signing, and use a unique bundle identifier if
needed. The profile must include HealthKit. Project regeneration overwrites
manual project settings; keep your team and identifier changes local or pass
`DEVELOPMENT_TEAM` and `PRODUCT_BUNDLE_IDENTIFIER` to `xcodebuild`.

Connect and trust the iPhone on the Mac, enable Developer Mode on the paired
watch, keep it unlocked and nearby with Wi-Fi enabled, choose it as the run
destination, and run. See [TESTING.md](TESTING.md) for verified behavior and
known device-preparation limitations.

## Pair and talk

1. Open `build/Build/Products/Debug/MuseWatchRelay.app` and allow Bluetooth.
2. Enter a gadget SDK token from <https://gadgets.muse.ai/settings/sdk-tokens>.
   You can reuse the token from your existing Muse Companion setup; the relay
   saves its own copy and pairs as a separate device.
3. Click **Start Muse Bluetooth pairing**. In the official Muse iPhone app,
   enable **Settings → Devices → Developer mode**, add the displayed
   **MuseGadget…**, and choose the current internet connection. Wait for
   **Muse connected** on the Mac.
4. On the watch, tap **Connect**, select **Muse Watch Relay**, and accept any
   system Bluetooth pairing prompt. Click **Allow my watch** on the Mac.
5. Tap the watch microphone, dictate a message, review it, and tap **Send**.
   The reply appears as text and is spoken after the turn finishes. The menu
   offers speech on/off, a new conversation, and disconnect. To increase speech
   volume, open watch Control Center, tap the volume control, and raise media
   volume. The dictation sheet may show a keyboard first; tap its microphone.

Keep Muse's enrollment helper open until pairing finishes. SDK credentials are
stored separately in `~/Library/Application Support/Muse Watch/Device`, so the
existing Mac companion's identity and conversations are not replaced.

## Integration and trust boundaries

```
Official Muse iPhone app -- Meta enrollment BLE --> Mac SDK identity
                                                      |
Watch dictation -- encrypted BLE + Mac approval --> Mac relay
Watch speech    <-- streamed reply snapshots -------   |
                                                      +-- SDK Noise session --> Muse
```

Apple Watch can act as a Bluetooth central but cannot advertise the peripheral
service required by Meta's gadget enrollment. This project therefore runs
the existing SDK on the Mac; it does not claim to implement direct standalone
watch-to-Muse enrollment. See [Apple's peripheral-role documentation](https://developer.apple.com/documentation/corebluetooth/cbperipheralmanager).

The relay reuses the bundled macOS SDK backend and its original BLE
pairing helper. SDK enrollment, token renewal, encrypted Noise sessions, and
chat subscriptions are handled by that code. The new watch link has its own
service UUID and accepts only `check`, `chat`, and `cancel`. It cannot provision
credentials, execute shell commands, or stop the host. Credentials never travel
to the watch. The existing SDK executor supports chat only.

The relay requires encrypted Bluetooth characteristic access and explicit Mac
approval of the connecting watch. It remembers the CoreBluetooth central ID;
**Forget approved watch** revokes that application-level approval. Approve only
after initiating a connection from your own watch. This uses the OS Bluetooth
bond and local approval, not a separately audited application cryptosystem.
No LAN HTTP server or additional cloud API key is used.

Both directions use bounded UTF-8 newline-delimited JSON, fragmented according
to the peer's Bluetooth MTU. Writes wait for acknowledgments; notifications
respect backpressure. A lost connection never automatically resubmits a prompt.
Cancel stops waiting for a reply; it cannot undo a task Muse already accepted.

The watch stores a conversation UUID locally and keeps message text in memory.
Muse retains its own account-side conversation history. Apple's system dictation
follows the device's dictation settings; the relay receives text, not recordings.

## Hardware smoke test

- Verify the app launches on the physical watch, not just a simulator.
- Deny a watch on the Mac: it must not receive chat data or submit prompts.
- Approve the watch, dictate **Reply with: watch connection works**, and send.
- Confirm one user turn, a streamed reply, and exactly one spoken final reply.
- Send a second turn and verify the previous reply is not spoken again.
- Stop speech, switch off speech, and confirm text replies still work.
- Turn off the Mac's Bluetooth during a reply; the watch must show a disconnect
  and must not resend the prompt when reconnected.
- Revoke watch approval; further chat must require approval again.

These steps require real hardware, an Apple signing account, Bluetooth consent,
and a paired Muse account. Build and protocol-test success alone do not establish
that the live microphone, pairing, cloud response, or watch speaker worked.

## Source provenance

The relay builds a runtime from `Backend/` and `Bluetooth/`, adapted from the
macOS companion prototype, and reuses the [Linux SDK](../linux)'s enrollment,
token refresh, Noise session and chat implementation. Source and runtime
notices are preserved. The code follows the repository's [Apache 2.0 license](../LICENSE).
No Jollybot artwork is bundled in the watch app or relay. The runtime uses the
build Mac's installed Python framework, so this is a local development build,
not a portable, notarized distribution.
