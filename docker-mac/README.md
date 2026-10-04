# Pair the Muse Linux container using Mac BLE

The Muse service runs in a Debian Linux container. A small macOS Core Bluetooth
helper handles the phone's Bluetooth setup because Docker Desktop cannot give
Linux BlueZ direct access to the Mac's built-in radio. Pairing uses the upstream
SDK's Python setup, ECDH/encryption and provisioning code without modification.
The helper transports BLE packets through local subprocess pipes, with no network
listener. After pairing, the Linux service uses the saved identity over the network.

To enroll an Ethernet-only Linux board such as the Nuvoton MA35D1 or MA35H0 through the
Mac instead, see [board enrollment over USB](BOARD-ENROLLMENT.md). The Mac supplies
BLE for setup; the board supplies its identity, encryption and credential storage.
The [four-board Docker simulator](board-sim/README.md) runs independent ARM64
instances for MA35D1-A1/S1 and MA35H0-A1/A2 before hardware is available.

## Requirements

- macOS 14 or later on Intel or Apple Silicon, with Bluetooth enabled.
- Xcode Command Line Tools (`xcode-select --install`), Python 3.9 or later,
  and Docker Desktop with Docker Compose.
- The Muse phone app with Developer mode enabled and an
  [SDK token](https://gadgets.muse.ai/settings/sdk-tokens).

This uses Bluetooth Low Energy (BLE) through Apple's CoreBluetooth framework.
The Mac acts as a GATT peripheral; the phone initiates setup. It does not use
Bluetooth Classic or the macOS Bluetooth device-pairing panel.

## Build, pair, and start

Run these from the repository root:

```sh
sh docker-mac/setup-mac.sh
.mac-venv/bin/python docker-mac/pair-mac.py
docker compose up --build -d
```

Setup creates a local Python environment, installs the Linux SDK and its
dependencies, and builds an ad-hoc signed native helper. Pairing asks for the SDK
token with hidden input and saves it in `.muse-state/sdk_token`. Pair before
starting Docker so the host creates the private state directory first.

Alternatively, double-click `docker-mac/start.command`. It performs setup when
needed, pairs if credentials are absent, and starts the container. Later runs
reuse the existing pairing.

Allow the Bluetooth permission when macOS asks. Keep the Mac's
“Muse Bluetooth Pairing” window open during discovery and setup. In the Muse phone app, enable
Settings → Devices → Developer mode, add a device, and select the `MuseGadget…`
name printed by the helper. Choose “Use current connection” when offered. The
setup window lasts ten minutes. App consent is required to finish pairing.
Closing the Mac window stops advertising and ends the attempt. A timeout or
permission failure does not start Docker when using `start.command`.

The helper advertises the full device name by itself and publishes the standard
Muse GATT service for connection. On this Mac, the combined name/UUID advertisement
was not discovered by the Muse iPhone app; full-name advertising completed actual
encrypted setup. `MUSE_BLE_NAME_ONLY=0` restores combined advertising for diagnosis.

The service waits for credentials when unpaired. Confirm connection:

```sh
docker compose logs --tail 40 muse
docker compose exec muse musegadget info
```

A completed deployment logs `registered with the Muse`. A healthy container by
itself does not prove successful pairing or registration.

## Credentials and access

`.muse-state/` is mode 0700, holds the SDK token in `sdk_token` (0600), device
identity and eventual pairing credentials. It is ignored by Git and excluded from
the Docker build. Both the Mac pairing helper and Linux container use this exact
directory. The token is read from a file, never from a Docker build argument.
If `MUSEGADGET_SDK_TOKEN` is already set on the Mac, pairing validates and saves it
to the same private file so Docker can use it. Avoid putting tokens on the command
line. `MUSEGADGET_STATE_DIR` can override the Mac state directory, but the Compose
bind mount must also point at that directory.

Muse executes commands as the container's `muse` account. Its home is a Docker
volume. The Mac's home and Downloads are not mounted as command workspace.

## Rebuild helper and tests

The current helper is compiled with Swift and ad-hoc signed as
`docker-mac/build/MuseBluetooth.app`. To recreate it:

```sh
sh docker-mac/build-helper.sh
```

The WebSocket dependency uses the upstream hash-pinned lock file. Cryptography
comes from the Linux SDK's package requirements on Mac and Debian's package in
the container. Run Linux tests:

```sh
docker build --target test -f docker-mac/Dockerfile -t muse-gadget-tests:local .
docker run --rm muse-gadget-tests:local
```

The Mac transport and token-handoff checks require pytest:

```sh
.mac-venv/bin/python -m pip install pytest
PYTHONPATH=linux/src:docker-mac .mac-venv/bin/python -m pytest -q docker-mac/test_mac_ble.py docker-mac/test_pair_mac.py docker-mac/test_pair_board.py
```

## Platform limits

The default advertisement contains the local device name; the GATT service UUID
is available after connecting. CoreBluetooth does not
expose BlueZ's manufacturer-data field or forced peripheral disconnect API. The
helper resets its GATT service after protocol errors. Actual phone discovery and
pairing must be verified with the Muse app; they cannot be established by unit
tests alone. The helper queues notifications when CoreBluetooth applies
backpressure and forwards the phone's MTU to the SDK's packet framer.
A Mac Bluetooth permission denial or powered-off radio is reported
as a pairing failure.

Stop the service with `docker compose stop`. This preserves pairing and identity.

To replace a pairing, run `.mac-venv/bin/python docker-mac/pair-mac.py --force`
and approve setup again in the phone app. The identity and device name are reused.
