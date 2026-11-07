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

# Raspberry Pi 2 community deployment notes

The Linux SDK has been deployed on a Raspberry Pi 2 Model B rev 1.1 running
Raspberry Pi OS 13 (Trixie, armhf, 32-bit). The board has no Bluetooth and no
Wi-Fi; it is wired to Ethernet. Start with the standard [Linux installation and
pairing instructions](../README.md#install); an SDK token and access to the
Muse app are required.

This board is outside the "Raspberry Pi with Bluetooth" line in the
requirements: it has no radio of any kind. The setup below pairs once on a
machine that does, then moves the pairing state to the Pi. It is a
community-tested setup, not an official board support profile.

## Why a board without Bluetooth works

Bluetooth LE carries only the provisioning handshake. Once `pairing.json`
exists, the daemon talks to Muse over `wss://<noise_host>/v1/noise` (a Noise XX
session, then `link.register`), and `musegadget doctor` reports Bluetooth as
`WARN` rather than `FAIL`. The pairing is bound to the device identity in
`identity.json`, not to the machine that performed the BLE handshake.

So a host without a radio can run the gadget if the pairing is produced
elsewhere and the state is copied across.

## Device prerequisites

- Raspberry Pi 2 Model B rev 1.1 (BCM2836, armv7). This board is 32-bit only:
  use a 32-bit Raspberry Pi OS image, never arm64.
- Raspberry Pi OS Bullseye or later. These notes use Trixie (armhf) with
  Python 3.13.5.
- Wired Ethernet, with DNS and a correct system clock. There is no onboard
  Wi-Fi, and the Muse app's pairing flow expects the pairing machine to be
  online.
- `git`, plus `build-essential` and `python3-dev` if `websockets` has no
  matching wheel. `cryptography`, `dbus` and `gi` come from the distribution,
  which is why the installer builds its venv with `--system-site-packages`.
- A second Linux machine with Bluetooth LE and the Muse app, used once.
  Check that it can advertise: `bluetoothctl show` should list
  `Roles: peripheral` and an `Advertising Features` block.

## Install on the Pi without pairing

The first install creates the device identity, which the pairing machine will
then adopt.

```sh
sudo apt update
sudo apt install -y git build-essential python3-dev

git clone https://github.com/facebookincubator/muse-gadget-sdk
cd muse-gadget-sdk/linux
bash install.sh --from . --sdk-token mgst_... --no-pair --yes
```

`--no-pair` skips the pairing attempt, which can't work on a board with no
controller. Expect the warning:

```
no Bluetooth adapter found. You can install, but pairing with the Muse app needs Bluetooth LE.
```

Note the identity:

```sh
sudo musegadget info
# node id:   homelink-XXXXXX
# BLE name:  MuseGadgetXXXXXX
# paired:    no
```

## Pair on the machine with Bluetooth

Use a machine that is not already a Muse gadget. Copying the Pi's identity
over another gadget's state would replace that gadget's identity and keep its
old pairing, so the installer would not open pairing at all; the `mkdir` below
fails if `/var/lib/musegadget` already exists.

Copy the identity to the pairing machine first, so the app pairs the Pi's
existing node instead of creating a second device:

```sh
sudo mkdir -m 0700 /var/lib/musegadget &&
ssh USER@PI 'sudo tar -C /var/lib/musegadget -cf - identity.json sdk_token' \
  | sudo tar -C /var/lib/musegadget -xf -
```

Then install there; the installer reuses the seeded identity and opens the
pairing window:

```sh
git clone https://github.com/facebookincubator/muse-gadget-sdk
cd muse-gadget-sdk/linux
bash install.sh --from . --yes
```

In the Muse app, within the ten-minute window, turn on **Settings > Devices >
Developer mode**, then **Settings > Devices > Add Device**, pick
`MuseGadgetXXXXXX`, accept the community-device warning, and pick the network
shown when asked for Wi-Fi. If the window closes, reopen it with
`sudo musegadget pair`.

Confirm, then stop the pairing machine so only one host holds the identity:

```sh
sudo musegadget info                    # paired: yes
sudo systemctl disable --now musegadget
```

## Move the pairing to the Pi

On the Pi, pull the three state files and restart the service:

```sh
ssh USER@PAIRING-HOST 'sudo tar -C /var/lib/musegadget -cf - identity.json pairing.json sdk_token' \
  | sudo tar -C /var/lib/musegadget -xf -
sudo systemctl restart musegadget
```

Run as root, the extract keeps `0600 root:root` inside a `0700` directory, and
no archive is left behind. Both `ssh ... sudo tar` pipelines run sudo on the
remote host without a terminal, so they need passwordless sudo there.

A community script that does the same transfer with extra checks (token
format, matching identity, refusal to overwrite a different device) is at
[alston808/muse-gadget-headless](https://github.com/alston808/muse-gadget-headless/tree/17076b748044548cbc2b42d427f518151f03179a)
(snapshot `17076b7`). It runs with sudo and handles device credentials, so
read it before you run it.

## Verify

```sh
sudo systemctl status musegadget
sudo journalctl -u musegadget -f
sudo musegadget doctor
```

The service should be `active (running)`, and the log should show a Noise
session and a registration, with no Bluetooth activity:

```
VM fetch: 1 VMs
link_client: Noise session established
link_client: sent link.register as homelink-XXXXXX
link_client: registered with the Muse
```

`doctor` reports Pairing, SDK token, Service and Muse API TCP as `OK`, and
Bluetooth as `WARN`. The end-to-end check is from the Muse app: ask it to run
`hostname` on the device. It must return the Pi's hostname, not the pairing
machine's.

Once the Pi is registered, delete the pairing machine's copy of the device
tokens there:

```sh
sudo musegadget unpair
```

This only removes its local `pairing.json`; Muse is not told, and the Pi stays
paired.

## Limitations

- Re-pairing needs the Bluetooth machine again. The Pi cannot pair on its own,
  so keep that machine's install and `identity.json` for as long as the
  pairing might need to be redone: run `sudo musegadget pair` there, then
  repeat the move and the unpair.
- Only one host may hold a pairing at a time. Leaving the pairing machine's
  service enabled gives two live `link-control` sessions for the same device.
- Tested on one board and one image. 1 GB of RAM is sufficient because the
  only PyPI dependency is `websockets`, but heavier models of the same
  generation are untested.

## Remove

```sh
cd muse-gadget-sdk/linux
bash install.sh --uninstall --purge
```

Then remove the device in the Muse app.
