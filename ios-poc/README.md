# Muse Gadget on iPhone: feasibility and proof of concept

**Question:** can an iPhone 15 (iOS 27) act as a Muse gadget, the way an ESP32 or
a Raspberry Pi does with this SDK?

**Short answer:** yes for a foreground "Muse gadget" app. You can't
"install" the SDK on iOS: the ESP32 firmware is C for ESP-IDF, and the Linux SDK
needs BlueZ, systemd and a shell. But the protocol is small and fully public in
this repo, and every piece of it maps to public iOS APIs. This folder holds a
Swift port, `MuseGadgetKit`, and a SwiftUI app around it. Both build for iPhone,
and the port is checked against Meta's own test vectors and against the Python
SDK over a real WebSocket.

Not yet proven:
- whether the **Muse app accepts an iPhone advertiser** (see risk 1);
- whether **iOS keeps the connection alive in the background** (risk 2).

The first on-device test below settles the first.

## How a gadget works

```
 Muse app (phone A)                Gadget (here: phone B)                  Muse cloud
 ───────────────────               ─────────────────────                  ──────────
 BLE central  ──── GATT ────▶  BLE peripheral (setup only)
   get_device_info, pairing v5 (P-256/HKDF/AES-GCM),
   wifi_scan, provision_v2 ──▶ device access/refresh tokens
                                   │
                                   ├── HTTPS GET  api.muse.ai/fetch_vms ─────────▶ VM list + per-VM bearer
                                   └── WSS hatch.metaaivm.com/v1/noise?vm_id= ───▶ Noise XX
                                         POST /link-control (long-lived stream)
                                           ▶ link.register {commands_v2}
                                           ◀ link.invoke  ▶ link.result
                                           ▶ link.heartbeat   ◀ link.unpaired
                                         POST /chat/stream (device → Muse message)
```

The phone running the gadget **cannot also be the phone running the Muse app**:
an iPhone can't connect to its own advertisement. Pairing needs a second
phone, iOS or Android, with the Muse app. After pairing, the gadget only
needs internet.

## The protocol, layer by layer

| Layer | Exact contract | Source of truth | Swift |
|---|---|---|---|
| Advertising | Local name `MuseGadget` + last 6 hex of the identity, uppercase, **no hyphen**. Service UUID `7fdd3d1c-38ea-46cf-8b46-314ecf5f240c`. Linux/ESP32 also send manufacturer data `FF FF <paired 0/1>`. | `linux/.../ble_server.py`, `esp32/main/ble_server.c:857` | `BLEPeripheral.swift` |
| GATT | RX `4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01` (write, write-without-response). TX `d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c` (read, notify). No bonding or encryption. | same | `BLEPeripheral.swift` |
| BLE framing | `0xFE, index, total, fragment`; packet ≤ min(MTU−3, 160); 50 ms between notifies; ≤ 255 chunks; reassembly cap 8 KiB | `ble_framing.py` | `BLEFraming.swift` |
| Setup actions | Plaintext: `get_device_info`, `pairing_client_hello`, `pairing_encrypted`. Encrypted: `pairing_client_finished`, `wifi_scan`, `provision_v2` (`access_token`, `refresh_token`, `token_type:"device"`, `api_url_v2`, `noise_host`, …). Statuses: `pairing_confirmed` (with `sdk_token`), `wifi_connecting`, `wifi_connected`, `auth_ok` / `auth_failed`. | `ble_setup.py`, `esp32/main/ble_server.c:303-490` | `SetupController.swift` |
| Pairing v5 (community) | `pairing_auth:"none"`, epoch 0, policy `confirm_app`. Fixed-format transcript, SHA-256. P-256 ECDH. HKDF-SHA256: salt = SHA256(nonces ‖ transcript hash), info `hatch-link ble setup v1`, then expand `mobile->device` / `device->mobile`. AES-256-GCM records: nonce `dir‖000‖ctr_be64`, AAD `label|session_id|m2d/d2m|ctr`. | `pairing.py`, `tests/vectors/link_pairing_v5.json` | `Pairing.swift` |
| Device API | `GET /fetch_vms` (`Bearer <access>`, `X-API-Version: 1.0.0`). `POST /device_token/refresh` (`Bearer hatch_refresh:<raw>`, body `{device_id, sdk_token}`); rotate every 3 h. | `muse_api.py`, `esp32/main/vm_api.c` | `MuseAPI.swift` |
| Noise | `wss://<noise_host>/v1/noise?vm_id=<encodeURIComponent>`, `Authorization: Bearer <vm_auth_token>`; 401/403 means fetch VMs again. `Noise_XX_25519_AESGCM_SHA256`, empty prologue, fresh static key per session, empty msg3 payload. | `noise/noise_xx.py`, `link_client.py` | `NoiseXX.swift` |
| Envelopes | Each WebSocket binary frame = AES-GCM(`NoiseTransportFrame{chunk_id, index, total, payload}`). Outbound payload `ServiceRequest{service, ServiceFrame{stream_id, request\|body_chunk\|reset}}`; inbound `ServiceResponse{ServiceFrame{response\|body_chunk\|reset}}`. Hand-rolled protobuf. | `noise/{framing,envelope,transport}.py` | `NoiseEnvelope.swift` |
| Control stream | Stream 1 `POST /link-control`, never ended. JSON messages, each with a u32-LE length prefix. Send `link.register`, `link.heartbeat`, `link.result`; receive the register reply, `link.invoke`, `link.unpaired`. Register as `platform:"linux"`, `device_family:"homehub"`; **never `link`**, because the server pushes ESP32 OTA updates to that family. | `link_client.py`, `esp32/main/noise_control.cpp` | `LinkSession.swift` |
| Chat | `POST /chat/stream` `{message, output_modality:"text", device_id}`; reply is an ack `{message_id}`. Voice (ESP32): base64 16 kHz PCM16 WAV in `items[]`, replies via `POST /chat/subscribe` NDJSON. Replies are text only. | `link_client.py`, `esp32/components/muse/` | `LinkSession.sendChat` (text) |

Not ported, and not possible or useful on iOS:
- the ESP32's `/link-tunnel`: a raw IP tunnel with NAPT into the home LAN;
- `device.discover`: ARP/SSDP/SNMP/NetBIOS sweeps;
- OTA updates;
- the official `fleet_ecdsa_p256_v1` pairing, which needs a Meta key fused into eFuse.

Community pairing needs no secret you can't get; an SDK token from
gadgets.muse.ai is enough.

## Are the iOS APIs sufficient?

| Need | iOS API | Verdict |
|---|---|---|
| GATT server with write + notify | `CBPeripheralManager`, `CBMutableCharacteristic` | ✅ |
| Advertise name + 128-bit service UUID | `startAdvertising([LocalNameKey, ServiceUUIDsKey])` | ✅ foreground only |
| Manufacturer-data "paired flag" | not allowed in iOS advertising | ⚠️ dropped; the SDK calls it "informational" |
| MTU-sized chunks | `CBCentral.maximumUpdateValueLength` | ✅ |
| Force-disconnect the phone after an error | none on a peripheral | ⚠️ resets state instead; error paths only |
| P-256 ECDH, HKDF, AES-GCM, X25519, HMAC, SHA-256 | CryptoKit | ✅ **byte-exact against the vectors** |
| WSS with an `Authorization` header and binary frames | `URLSessionWebSocketTask` | ✅ including 401 detection |
| Wi-Fi scan and join | no public scan API | ✅ not needed: like the Linux SDK, offer one open "current connection" network and ignore what comes back |
| Online check | `NWPathMonitor` | ✅ |
| Token storage | Keychain (`AfterFirstUnlockThisDeviceOnly`) | ✅ |
| Staying connected 24/7 | iOS suspends background sockets | ❌ **main limitation**: the gadget is online while the app is in the foreground |
| Commands | battery/thermal/storage (`UIDevice`, `ProcessInfo`), TTS (`AVSpeechSynthesizer`), torch, screen | ✅ no shell: `system.run`/`file.*` don't apply |
| Voice gadget | `AVAudioEngine` 16 kHz PCM16 capture | ✅ (not built yet) |

## Risks, in order

1. **The Muse app's scanner may not list an iPhone.** Three iOS differences
   are visible to the app:
   - no manufacturer data;
   - the GAP Device Name is the iPhone's name, not `MuseGadgetXXXXXX`;
   - the local name may come from the scan response.

   Nothing in this repo shows the app requires any of these, but its code isn't
   here, so only a real test can tell. Fallbacks:
   - Rename the iPhone, in Settings > General > About > Name, to exactly the BLE
     name the app shows. This fixes the GAP name.
   - If the app insists on manufacturer data, pair from a Mac or a Pi running
     the same identity, then move the tokens: the pairing is tied to the random
     node id, not to the hardware.
2. **Background.** iOS suspends the app soon after it leaves the screen. The
   proof of concept keeps the screen awake. For a permanent gadget, use a
   dedicated phone on a charger with Guided Access. There is no supported way
   to hold the WebSocket open in the background: VoIP push needs CallKit, and
   the VM won't send Apple push notifications.
3. **Server policy.** Registering as `linux`/`homehub` from an iPhone works on
   the protocol level, as the interop test shows. Meta could still restrict
   platforms or require SDK tokens differently later; review the Gadget SDK
   Terms.
4. **Tooling for iOS 27.** This Mac has Xcode 26.6, which ships the iOS 26.5
   SDK; the builds here target it. Running on an iOS 27 iPhone needs Xcode 27
   and a signing team. A free Apple ID works, but its builds expire after 7 days.

## What's verified here

From `MuseGadgetKit/`:

```sh
swift test                 # 10 tests (+1 skipped live test)
./Tests/run_interop.sh     # Swift client ↔ Python fake VM over a real WebSocket
```

- **Pairing** matches all three vectors in `linux/tests/vectors/link_pairing_v5.json`:
  - transcript and transcript hash;
  - ECDH;
  - session secret and both keys;
  - session id;
  - the `client_finished` ciphertext and tag;
  - the device's `pairing_ready` for the `confirm_app` vector.
- **Noise XX, envelopes and framing** are byte-identical to the Python SDK
  (`Tests/MuseGadgetKitTests/Vectors/gen_noise_vectors.py`): msg1, msg3, the
  encrypted `/link-control` request and `link.register` chunk, and decryption of
  VM frames.
- **Full app-side setup**, from a phone simulated in the test: device info,
  plaintext refusal, hello, client_finished → `pairing_confirmed` + `sdk_token`,
  `wifi_scan`, `provision_v2` → `wifi_connecting`, `wifi_connected`, `auth_ok`.
- **Live interop:** `URLSessionWebSocketTask` against a fake VM built from the
  Linux SDK's own Noise code:
  - a wrong bearer gives HTTP 401, reported as `authRejected`;
  - then Noise, `/link-control`, `link.register`, `link.heartbeat`,
    `link.invoke` → `link.result`, `/chat/stream`, and `link.unpaired`.
- `xcodebuild` for `generic/platform=iOS` succeeds for the kit and the app, with
  no warnings. The app launches in the Simulator.

## First proof of concept on a real iPhone

You need:
- the iPhone 15 on iOS 27, with Xcode 27 and your team set in `MuseGadgetApp/project.yml`;
- a second phone with the Muse app;
- an SDK token from gadgets.muse.ai.

```sh
cd ios-poc/MuseGadgetApp && xcodegen generate && open MuseGadget.xcodeproj
```

1. **Discovery (decides risk 1).** Run the app on the iPhone 15, paste the SDK
   token and tap **Open pairing**. On the other phone, turn on Muse app >
   Settings > Devices > Developer mode, then Add Device. Pass if
   `MuseGadgetXXXXXX` appears. If it doesn't, rename the iPhone to that name
   and retry.
2. **Pairing.** Pick it and continue past the community-device warning. On the
   Wi-Fi step, choose the network shown; no password is needed. The app log
   should show these, in order:
   - `pairing_confirmed`;
   - `wifi_connected`;
   - `auth_ok`;
   - `paired`.
3. **Cloud.** The log should show `Noise session established`, then
   `registered with the Muse`.
4. **Commands.** Ask Muse "what's my iPhone's battery?" (`device.health`), "say
   hello on my phone" (`speech.say`), "show 'Dinner's ready' on my phone"
   (`display.show_text`), and "turn on my phone's flashlight".
5. **Device to Muse.** Send a message from the app and confirm it appears in
   the Muse chat.
6. **Background.** Lock the phone for 1, 5 and 30 minutes, then unlock; note
   when the session drops and that it reconnects.

If step 1 passes, everything after it uses protocol paths already verified
above.

### Pairing without a second phone

The Muse Mac app can't pair gadgets: it has no Bluetooth permission and no
gadget-setup code. Instead, let the Mac stand in for the gadget during the
one-time pairing, and let the iPhone run the Muse app:

```sh
cd ios-poc/MuseGadgetKit
swift run MusePairMac --sdk-token mgst_… --out ~/muse-pairing.json
# In the Muse app on the iPhone: Developer mode > Add Device > MuseGadgetXXXXXX
xcrun devicectl device copy to --device <IPHONE_ID> --domain-type appDataContainer \
  --domain-identifier com.biswarupmondal.musegadget \
  --source ~/muse-pairing.json --destination Documents/muse-import.json
rm ~/muse-pairing.json
```

On its next launch, Muse Gadget moves the identity and tokens into its
Keychain and deletes the file. The tokens belong to the node id, not the
hardware.

## Layout

```
ios-poc/
  MuseGadgetKit/            Swift package, CryptoKit + Foundation + CoreBluetooth only
    Sources/MuseGadgetKit/  BLEFraming, Pairing, SetupController, BLEPeripheral,
                            NoiseXX, NoiseEnvelope, LinkSession, MuseAPI, GadgetService
    Tests/                  vector tests, setup-flow test, live interop (fake_vm.py)
  MuseGadgetApp/            SwiftUI app (XcodeGen project.yml); PhoneCommands.swift
                            holds the commands Muse can run on the iPhone
```

Next steps after the proof of concept:
- a voice gadget: mic → `/chat/stream` voice note, `/chat/subscribe` → `AVSpeechSynthesizer`;
- more commands: camera snapshot, location, Shortcuts via URL;
- a dedicated-device setup with Guided Access.
