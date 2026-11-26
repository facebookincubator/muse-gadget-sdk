<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Protocols

Wire contracts shared by the gadget firmware and the Muse apps. Include these
headers; don't copy their constants into other files.

Gadget firmware (the ESP32 and Zephyr SDKs' gadgets) includes `mgcommands.h` in
place.

| File | What it defines |
|---|---|
| [`mgcommands.h`](mgcommands.h) | The musegadgets BLE protocol: advertising, the GATT service (Control and Data), the required standard services (Battery, Device Information, Nordic UART), commands, push-to-talk, audio formats and playback, video, UI and still image streams, UI input, the token proof, recovery mode, the device session (the gadget's own VM session through the phone), the clock, haptics, link modes, offline clips and the legacy skills. |
| [`UI.md`](UI.md) | UI streaming: a phone's screen or a web page on a gadget at the panel's own size, frames only on change with progressive refinement, the square-to-disc warp, and the gadget's touch back (touch events and HID over GATT). |
| [`VIDEO.md`](VIDEO.md) | Video from a phone to a gadget's display: the frame format both transports share, the Wi-Fi protocol, the encoder settings (round-display mask, intra refresh, prebuilt clips), the decoder's optimizations, measured numbers, and video over this protocol (mg_command_stream_video). |
| [`test-vectors/mg-token-proof-v1.json`](test-vectors/mg-token-proof-v1.json) | Token proof vectors, from [`test-vectors/mg_token_proof_ref.py`](test-vectors/mg_token_proof_ref.py) (Python standard library only; run it to check the file). |
| [`test-vectors/mg-session-v1.json`](test-vectors/mg-session-v1.json) | Device session exchanges byte for byte, from [`test-vectors/mg_session_ref.py`](test-vectors/mg_session_ref.py), a reference device in Python. |
| [`test-vectors/mg-session-seal-v1.json`](test-vectors/mg-session-seal-v1.json) | Sealed device session messages, from [`test-vectors/mg_session_seal_ref.py`](test-vectors/mg_session_seal_ref.py). |
| [`test-vectors/mg-skills-v2.json`](test-vectors/mg-skills-v2.json) | Legacy skills exchanges byte for byte, from [`test-vectors/mg_skills_ref.py`](test-vectors/mg_skills_ref.py). |
| [`test-vectors/mg-haptic-v1.json`](test-vectors/mg-haptic-v1.json) | Haptics scenarios: device_action packets and local events against the device's answers and its motor (mgcommands.h, Haptics). |
| [`test-vectors/mg-framed-v1.json`](test-vectors/mg-framed-v1.json) | Framed Data exchanges (a video stream and audio playback sharing the Data characteristic), from [`test-vectors/mg_framed_ref.py`](test-vectors/mg_framed_ref.py). |
| [`test-vectors/mg-update-v2.json`](test-vectors/mg-update-v2.json) | Bluetooth firmware updates over SMP: the one confirm press a link without the token proof needs on a release gadget, set up or not (rc 10 while the gadget asks, rc 0 after the press, rc 11 on a timeout or cancel) and the cases that need no press, from [`test-vectors/mg_update_ref.py`](test-vectors/mg_update_ref.py) (standard library only; run it to check the file). |

Each reference checks its vectors when run with no argument
(`python3 protocols/test-vectors/mg_session_ref.py`) and regenerates them
with `--write`; the inputs are fixed, so the output never changes.
[`test-vectors/README.md`](test-vectors/README.md) lists the files and
their license.

The header is plain C99 and also compiles as C++ and Objective-C, where the
enums become `NS_ENUM`s that Swift can import.

## Gadget setup over Muse Link

A musegadgets device gets its device token pair through the Muse Link BLE
setup protocol, not over mgcommands. The transport is unchanged: the setup
service `7fdd3d1c-38ea-46cf-8b46-314ecf5f240c` (RX `4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01`,
TX `d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c`), its chunk framing, community
pairing v5 with a physical button press, the `pairing_encrypted` records, and
`pairing_confirmed`, which carries `sdk_token` when the firmware has one. The
reference implementation is `esp32/main/link_pairing.c`, `ble_server.c` and
`pairing_transcript.c`, with vectors in `esp32/tests/vectors/link_pairing_v5.json`.
Gadgets add:

- **`device_info`** gains two fields. `wifi` is `"required"`, `"optional"` or
  `"none"`; absent means `"required"`, which is what every firmware without
  this change sends. `mgcommands: 1` says the device also serves the
  musegadgets service. ESP32 BLE-only builds and the Zephyr gadget send
  `wifi: "none"`, ESP32 Wi-Fi boards with musegadgets `wifi: "optional"`;
  other firmware omits both.
- **`provision_v2`** still needs `access_token`, `refresh_token` and
  `token_type: "device"`. `ssid` and `password`:
  - `required`: required, as before.
  - `optional`: both absent or empty makes the provisioning **token-only**;
    a nonempty `ssid` runs the Wi-Fi flow as before.
  - `none`: must be absent or empty, else the device answers
    `error_wifi_unsupported` and stores nothing.

  `username`, `api_url`, `api_url_v2` and `noise_host` are stored unless
  `wifi` is `"none"`; token-only provisioning ignores `ota_url` and `ota_force`,
  and a re-setup (below) ignores `ota_url` whatever its Wi-Fi.
- **Token-only provisioning** stores, in one commit, the token pair, the
  token proof key K (mgcommands.h, Token proof), the setup-complete marker and
  a marker that Wi-Fi was skipped, so boot recovery keeps a setup without
  Wi-Fi. Only then does it send the encrypted status `auth_ok`; a failed
  commit sends `error_storage` and leaves nothing behind. It never sends
  `wifi_*` statuses, joins Wi-Fi or reaches a VM. Afterwards the device stops
  advertising Link setup (musegadgets advertising continues); it can be set
  up again at any time (Gadget setup v2, below).
- **Reset** (the device's hold-to-reset or menu, the token proof's `clear`
  after a match, or, on a device that declares
  `mg_session_cap_sealed_clear`, the sealed session's `mg_session_clear`
  instead) erases the tokens, K and both markers and returns the device to
  Link setup advertising.

The app derives K from the access token it provisioned and the `node_id`
from `device_info`, keeps K (not the tokens) for the device, and runs the
token proof on every musegadgets connection. Apps key K by the BLE
peripheral they ran Link setup with, so gadget firmware must advertise Link
setup and the musegadgets service from the same identity address (ESP32
uses one public-address advertiser that takes turns between the two UUIDs).

### Gadget setup v2

What every BLE Muse Gadget does on top of the above, and what the Muse apps
can rely on. Wi-Fi Link
firmware, the public ESP32 Wi-Fi builds and the Linux reference
(`linux/src/musegadget/pairing.py`, which confirms in the app with
`confirm_app`) aren't BLE gadgets and don't change.

- **Advertising.** The Link setup advertisement is the flags, the setup
  service UUID and manufacturer data `FF FF <flags>`: the test company id
  0xFFFF and one flags byte.

  | bit | mask | meaning |
  |---|---|---|
  | 0 | 0x01 | set up (as before; Wi-Fi firmware sends 0x00 or 0x01) |
  | 1 | 0x02 | a Muse Gadget over Bluetooth: it serves musegadgets (or another gadget protocol) and doesn't need Wi-Fi to be set up |
  | 2-7 | | 0; apps ignore them |

  Every BLE gadget sets bit 1. Before setup it advertises Link setup (0x02),
  alone or taking turns with its musegadgets advertisement from the same
  address. Set up, it advertises musegadgets; it may keep Link setup in
  rotation, with 0x03. The scan response carries its name:
  `MuseGadget-XXXXXX` on the SDK's boards, `MuseGadget-<PRODUCT>-XXXXXX` on
  products in the Muse Gadget catalog (PRODUCT the catalog model code, e.g.
  `MuseGadget-XN54-1A2B3C`; XXXXXX the last three bytes of the BD address in
  uppercase hex). The SDKs take the product code from their build
  configuration (empty on SDK boards, whose names don't change). With a
  product code an ESP32 name ends in the Bluetooth address's bytes unless
  the board keeps the Wi-Fi MAC's, as the ESP32 catalog boards (M5SW, EMO1,
  SCW1, WC6) do; without one it keeps the Wi-Fi MAC's, as before, unless
  its build asks for the Bluetooth address. An `MG-` prefix was considered
  for products and deferred: no gadget advertises it, though the bench
  client still accepts it.
- **Node family.** Every device built with this SDK registers
  `device_family: "gadget"` (link.register), and so does the Linux client:
  every ESP32 build, the Home Link builds without BLE gadget audio, the
  Wi-Fi-only builds and the C5 DevKitC-1 build included. Only firmware Muse
  ships for its own Home Link products, which isn't built from this SDK (no
  capability bit 1, not a `MuseGadget-*` device), registers `"link"`. A set-up device keeps its node id (Node ids,
  below). `model_id` is the board's own lowercase id on every build (ESP32
  `CONFIG_MUSE_DEVICE_MODEL_ID` from the board's overlay; `DEVICE.md`). A
  catalog product code appears only in the advertised name, never as the
  model id. The apps' re-pair and Forget rules and their Devices lists tell
  gadgets from Muse's own devices by this family, never by the id.
- **Node ids.** Release firmware offers `node_id` `homelink-<id>` in
  `device_info`, as a plain Home Link does. A build switch (ESP32
  `CONFIG_MUSE_GADGET_NODE_ID_PREFIX`, on in bench builds; Linux
  `MUSEGADGET_GADGET_NODE_ID=1`) makes a gadget's new setup offer
  `gadget-<id>` instead (the same six lowercase hex digits). The id is
  stored with K in the setup's commit, so a gadget set up earlier keeps its
  `homelink-<id>` and keeps working until it is set up again; with the
  switch on, the next `provision_v2` that commits, a re-provisioning
  included, gives it `gadget-<id>`. A stored id is never renamed. Apps pass
  the id through. A credentials-only gadget
  registers no node. `device_info.model` stays `"hatch_link"`, Link
  setup's literal.
- **Setting up again (re-provisioning).** The setup service stays after
  setup, and a set-up gadget takes a new setup at any time, on a new
  connection or the one the app already has (the app finds it by its
  musegadgets advertisement): `pairing_client_hello` is answered as before
  setup, never `error_pairing_unavailable` because it is set up (except, on
  a release build, a gadget whose only key is its talk key outside its setup
  mode: see The press), with the same handshake and the same press. The
  `provision_v2` commit replaces the
  token pair, K and the setup markers atomically: after `auth_ok` the old
  device token and K are gone (never two token sets), and a token proof
  made with the old K on a live musegadgets connection no longer counts. A
  setup that fails or is abandoned before the commit (a refused
  `provision_v2`, a storage error, a timeout, a disconnect, a power loss)
  leaves the old token pair and K, and the gadget stays set up. Nothing has
  to be forgotten, reset or cleared first; hold-to-reset and clear (the
  token proof's, or the sealed session's on a device that declares
  `mg_session_cap_sealed_clear`) still work. ESP32 gadgets keep the old
  setup aside in a journal until the commit (`config_store.h`), Zephyr
  gadgets in a second slot. A session whose `provision_v2`
  committed (`auth_ok` went out) is used up: another `provision_v2` on it
  gets `error_pairing_confirm_required` and changes nothing, so every setup,
  first or again, takes a new `pairing_client_hello` and a new confirmation. An
  attempt that failed committed nothing, and its session may try again where
  the gadget keeps it (after `wifi_failed`, or a refused `provision_v2`).
- **The press.** `pairing_policy` is always `confirm_press`. Setup and
  re-setup are confirmed by one press on the unit: one press of the confirm
  key named on the screen or in the docs ("Press <key> to pair"), or on
  touch gadgets one tap on the on-screen Confirm disc ("Pair with Muse",
  "Tap to pair"), whose hit area is generous (the disc, its ring and a
  margin of 10 % of the screen's size around them); a tap elsewhere does
  nothing. No hold, no minimum or maximum press time, and no timer on the
  prompt: it stays up until the press, a dismiss, or the phone gives up
  (`pairing_ready`'s `confirm_timeout_seconds` is the phone's give-up time).
  A key or touch that went down before the setup, re-setup or update prompt
  showed never confirms it, not even at its release; the next press does.
  It's an order check with no timing.
  `pairing_confirmed` goes out only after that press, which counts only once
  the encrypted `confirm_required` went out; a press with nothing armed
  confirms nothing (it's the gadget's normal input then), set up or not.
  While the prompt is up the confirm key belongs to it (no talk turn
  starts). No tap ever dismisses or fails it: only the back gesture (a
  swipe, or a separate back key), the phone cancelling, or on a re-setup a
  talk-key press (below) closes it. On a set-up gadget's release build the
  talk key never confirms a re-setup: the prompt is confirmed by the tap or
  by a key that isn't the talk key, and a talk-key press while it shows only
  dismisses it (no talk turn, no confirm), so a stranger who raises the
  prompt never gets the owner's next talk press. A set-up gadget whose only
  key is its talk key takes a re-setup, on a release build, only in its
  setup mode (its button held at boot into its normal firmware, with Link
  setup under its normal identity; it ends at the confirmed re-setup or
  5 min after that boot; a SAFE of either kind is a recovery mode and never
  completes Link setup), where that key confirms; outside it a
  `pairing_client_hello` gets `error_pairing_unavailable` in plaintext, no
  session starts and nothing shows. Test and bench builds are always in
  setup mode. First setup of a gadget that isn't set up keeps any one press
  or tap, the talk key included. Only test and bench builds may press for it
  (`>pair.confirm`, below), through the same confirm path, so
  `pairing_confirmed` carries `sdk_token` either way; no shipping build
  confirms by itself. A gadget without a button uses a documented input (the
  EchoEar-2ST: a press on its screen), never confirming on connect.
- **The re-setup prompt's rate.** On a set-up gadget's release build a
  `pairing_client_hello` on the prompt's own connection is never refused
  while a re-setup prompt is up: it takes over the prompt (which stays on
  screen, now bound to the newest hello, with no new flash), and the one
  press confirms that hello's setup; a takeover hello that fails validation
  ends the prompt: its error goes out (in plaintext, or encrypted under the
  session still current if the device refuses it before wiping that
  session), nothing is confirmed, the device may drop the link, and the next
  valid hello prompts afresh and needs its own press. A hello from another
  connection while a prompt is up gets `error_operation_in_progress` and
  leaves the prompt bound to the first connection's hello, so nobody else
  can take over the owner's press; so does one from another connection while
  a confirmed setup still runs, until its `provision_v2` commits or its
  session ends. With no prompt up, only a hello within 10 s after a dismiss
  (the back gesture, or the talk key on the re-setup prompt) gets
  `error_operation_in_progress`. That code (the one a gadget answers while
  another operation runs) goes out in plaintext, since no keys exist yet,
  and shows nothing; a new connection may show the prompt at once. Test and
  bench builds are exempt from the dismiss rule only.
- **SAFE and Bluetooth updates.** On a release build, set up or not (a
  gadget that holds no device token, after Forget too), SAFE mode the gadget
  entered by itself (failed boots, a crash guard) takes no Bluetooth image
  update (SMP rc 11) and never prompts; SAFE entered with
  the gadget's button stays open, so the way back is the button route into
  SAFE or USB. The owner's app (a link whose token proof matched) updates
  with no confirm; on a release build the gadget, set up or not (after
  Forget too), asks another link for one physical press on it
  (mgcommands.h, Token proof).
- **The SDK token.** The firmware holds an SDK token (`mgst_...`), built in
  from a configuration fragment kept outside the repository, or written
  once (the ESP32 console's `>sdk_token=`); `pairing_confirmed` carries it as
  `sdk_token`. Without one the gadget still pairs far enough for the app to
  show its "SDK token missing" error, and logs that it has none. Firmware
  never logs any of a token's characters: whether it has one, its length
  and a SHA-256 prefix at most. The token pair and K survive a restart, and the token
  proof matches after one.

### Automated setup tests

Automated tests have nobody to press the button. Bench builds take the
press on their USB serial console, which asks for the same physical access:
they print `@pair.pending` when a confirmation starts waiting and confirm on
the line `>pair.confirm` (answering `@pair.confirm confirmed`, or `none`).
ESP32 boards with the Muse UI built with `MUSE_BENCH=1` and the Zephyr
bench build have it, so rigs flash ESP32 boards with
`MUSE_BENCH=1 tools/muse/board.sh flash <board>`; other builds answer
`@pair.confirm disabled`. A rig's script answers it during setup. It
changes nothing on the air.

## Security

Known limits:

- Relays: a device in radio range that forwards the token proof between
  the owner's phone and a gadget can read or erase its offline clips, power
  it off, change its stored settings, stream video or play audio on it,
  press its HID keys, run its legacy skills, play haptics and update its
  firmware with no press until the connection ends. Release firmware opens
  the microphone for a client only after a recent press, gesture or wake
  word on the gadget. Where images are signed, they are signed with the
  repository's development key, which is public, so a valid signature
  doesn't show who built the image. Firmware the relay installs can
  therefore open the microphone whenever it likes. Without the proof, a
  central can't stream or play audio on a set-up gadget. This protocol is
  for community gadgets; link encryption and a sealed command protocol
  come in a later revision.
- Re-setup: the re-setup prompt is limited per connection, so a nearby
  device can raise it again by reconnecting; the talk key never confirms
  it, so the owner can only dismiss it.
- Firmware updates: on a release build, a Bluetooth update from a link
  without a matched proof takes one physical confirm press on the gadget,
  set up or not (after Forget too). A key or touch that went down before
  the update prompt showed never confirms it, not even at its release; the
  next press does. It's an order check with no timing. The press binds to the upload that
  asked for it (its length and whole-image SHA-256) for 600 s, across link
  drops; a cancel answers rc 11 once and binds nothing; the prompt shows at
  most once per 10 s per connection. On a
  release gadget with a token, a nearby device can start a firmware update
  without the token proof, and the owner's next physical button press (the
  talk key included) installs it, and on a gadget that isn't set up the
  same press does. Where images are signed, they are signed with the repository's
  development key, which is public, so a valid signature doesn't show who
  built the image.
- Sealed messages (seal v1): the tag doesn't cover the sub-command, so a
  party on the link that also chooses a message's plaintext can relabel it
  (Known limits of seal v1, in mgcommands.h). A later revision binds it.

## Skills

A skill is something a gadget can do for Muse beyond the base protocol,
called by name with JSON parameters. One schema serves BLE and Wi-Fi: a skill
is a Wi-Fi gadget's `link.register` command entry plus its name.

The BLE skill command is legacy: new firmware registers its commands as its
own `link.register` entries over the device session (mgcommands.h, Device
session), as a Wi-Fi gadget does, and Muse runs them directly. It lists
`mg_command_session`, not `mg_command_skill`. The rest of this section stays
for gadgets that shipped skills over BLE.

```json
{
  "name": "device.led",
  "description": "Set the gadget's LED colour. Values 0-255; all 0 turns it off.",
  "required": {"r": {"type": "integer", "description": "Red 0-255"},
               "g": {"type": "integer", "description": "Green 0-255"},
               "b": {"type": "integer", "description": "Blue 0-255"}},
  "optional": {"duration_ms": {"type": "integer", "description": "How long to show it, in ms (default 5000)"}},
  "timeout_ms": 5000
}
```

- `type` is `string`, `integer`, `number`, `boolean`, `object` or `array`
  (apps treat anything else as `string`). `timeout_ms` defaults to 10000.
- Names are lowercase dotted `<domain>.<verb>`, ASCII, at most 48 bytes;
  descriptions at most 300 bytes. Muse reads the descriptions: say what the
  skill does, its ranges and what it returns.
- The **skills document** a gadget reports is
  `{"v":1,"skills":[...],"manifest":{"url":"...","revision":"..."}}`, with
  `skills` and `manifest` each optional and at least one present. The
  manifest points at the full list for the build at a fixed revision; apps
  pass it to Muse and don't fetch it.
- Results are `{"ok":true,"payload":{...}}` or
  `{"ok":false,"error":{"code":"...","message":"..."}}`, with codes
  `unknown_skill`, `invalid_params`, `busy`, `unsupported`, `failed` and
  `timeout`.
- Over BLE, legacy firmware carries the document and invocations in
  `mg_command_skill` (0x88) chunks (mgcommands.h, Skills, has the framing
  and rules). A Wi-Fi
  gadget adds every skill to `link.register` as a normal command and
  answers `device.skills` with the same document.
- The phone app adds base skills itself, built from the rest of the
  protocol: `device.status` (always), `device.ota` (only when the gadget
  exposes the MCUmgr SMP service) and `device.buzz` (gadgets that list
  `haptics_enabled`). Firmware never lists those names; a gadget skill with
  one of them is ignored.
- No skill is privileged yet: a client that may invoke at all (on a
  release build of a set-up gadget, one whose token proof matched) may run
  any listed skill, so firmware lists only skills that are safe to run.
