# Enroll a Linux board through Mac BLE and USB

This relay lets an Ethernet-only Linux board, including a Nuvoton MA35D1 or MA35H0,
enroll with the Muse phone app without a Bluetooth radio on the board.
The phone uses BLE to the Mac. The Mac forwards opaque BLE packets through an
authenticated SSH connection over USB networking. The board runs the existing
SDK pairing controller and keeps its identity and device credentials.

```text
Muse phone app -- BLE --> Mac relay -- SSH / USB Ethernet --> Linux board
                                                           |
                                                           +-- Ethernet --> Muse
```

The Mac advertises the board's `MuseGadgetXXXXXX` name. Enrollment creates
credentials for that board identity; it never copies the local Docker gadget's
pairing or enrolls both devices under one identity. Encryption, app consent,
API verification and atomic credential storage stay in the upstream SDK.
The optional SDK token is passed inside the SSH pipe, never on the SSH command
line. No enrollment listener is opened on either device.

## Board prerequisites

- Linux with Python 3.9+, cryptography 3.3.2+, WebSocket dependencies, and CA
  certificates. This is the Linux SDK; it does not run on the Cortex-M4 RTOS.
- A USB **device-mode** data port and USB Ethernet configured in the board's
  kernel/image. A USB-C cable alone does not create a network interface.
- SSH access over that USB network, with a verified host key and key/agent
  authentication. The relay uses `BatchMode=yes` and `StrictHostKeyChecking=yes`.
- Internet access on the board before setup, through its RJ45 Ethernet port
  or a separately configured Mac internet-sharing connection.
- The SDK from this PR installed on the board, including `musegadget.pair_stdio`.
  No BlueZ, D-Bus or Bluetooth hardware is needed for this enrollment path.

Nuvoton documents the USB controller, PHY and gadget/configfs support in
section 3.6.2 of its [MA35D1 Linux BSP manual](https://www.nuvoton.com/export/resource-files/en-us--UM_EN_MA35D1_Linux_BSP.pdf)
and [MA35H0 Linux BSP manual](https://www.nuvoton.com/export/resource-files/en-us--UM_EN_MA35H0_Linux_BSP.pdf).
Enable a USB Ethernet function such as `CONFIG_USB_CONFIGFS_ECM` in addition to
the controller/configfs support and configure an IP address and SSH server.
Use the board's USB host/device port, not the UART debug connector. Exact port,
device-tree, IP and power settings depend on the board revision and Linux image.

On a Debian-based image, install the SDK from a checkout of this branch:

```sh
sudo apt-get update
sudo apt-get install python3 python3-venv python3-cryptography ca-certificates openssh-server
sudo python3 -m venv --system-site-packages /opt/musegadget/venv
sudo /opt/musegadget/venv/bin/python -m pip install --require-hashes -r linux/src/musegadget/data/requirements.lock
sudo /opt/musegadget/venv/bin/python -m pip install --no-deps ./linux
```

For Yocto or Buildroot, include those dependencies and SSH in the image using
its package/build tools. The desktop Docker connector does not need to run for
board enrollment.

## Enroll from the Mac

Build the existing Mac helper from the repository root:

```sh
sh docker-mac/setup-mac.sh
```

First verify SSH access and the board's host key through your usual SSH login.
The example below uses `192.168.7.2`; replace it with the board's actual USB
network address. The SSH account must be able to write the board state directory.
A trusted administrative board account can use `--sudo` if `sudo -n` is already
authorized; the relay does not change SSH or sudo policy.

```sh
.mac-venv/bin/python docker-mac/pair-board.py root@192.168.7.2 --sdk-token-prompt
```

Enter your SDK token with hidden input, allow Mac Bluetooth permission, and
keep the pairing window open. In the Muse phone app, enable Developer mode,
add the board name printed by the relay, choose **Use current connection**, and
approve setup. The window lasts ten minutes.

If the board already has a valid `/var/lib/musegadget/sdk_token`, omit the token
option. To use a local token file instead of a prompt:

```sh
.mac-venv/bin/python docker-mac/pair-board.py root@192.168.7.2 --sdk-token-file .muse-state/sdk_token
```

`--remote-python`, `--state-dir` and `--port` support other board installations.
If overriding the state directory, configure the board's runtime service to use
that same directory. Enrollment keeps the identity across subsequent attempts;
an existing pairing is protected unless `--force` is explicitly passed.
Stop the board's connector before replacing its pairing, after checking that
Muse is not running a command. Closing the Mac window, losing USB/SSH, an invalid
SDK token, permission denial or timeout cancels setup. A successful handoff prints
`Board enrolled. Its credentials remain on the board.`

## Run independently afterward

Start the board's Linux SDK connector as an ordinary command account. On an
image with a `muse` account, for example:

```sh
sudo /opt/musegadget/venv/bin/musegadget run --run-as muse
```

Use your image's service manager for persistent startup. Verify
`Noise session established` and `registered with the Muse` in its logs.
The state directory is owner-only (0700); SDK token and device credential files
are owner-only (0600). Routine token refresh happens automatically.

Enrollment is normally a one-time operation. The board survives reboots and
does not need the Mac or phone afterward. You can disconnect USB once the board
has separate power and an independent internet connection. If the Mac is still
providing power or sharing Wi-Fi over USB, disconnecting it removes that resource.
The relay itself does not configure DHCP, NAT or macOS Internet Sharing.

## Verification and limits

A Docker ARM64 stand-in with MA35H0 and MA35D1 memory profiles is available
in [board-sim](board-sim/README.md). It can test encrypted enrollment over real
SSH before a physical board is available.

Run the packet, lifecycle and enrollment tests without hardware:

```sh
PYTHONPATH=linux/src:docker-mac .mac-venv/bin/python -m pytest -q linux/tests docker-mac/test_pair_board.py docker-mac/test_mac_ble.py docker-mac/test_pair_mac.py
```

The integration test uses separate Mac-helper and board-endpoint subprocesses,
published synthetic P-256 pairing vectors, real encrypted provisioning, and a
stubbed Muse token-verification API. It checks that the board keeps its identity,
saves private credentials and never writes them to Mac state or logs.

The Mac BLE helper has separately completed real iPhone pairing. MA35D1/MA35H0 USB
enumeration, SSH connectivity and live Muse enrollment through this new relay
still require a physical board test. This feature does not replace the phone's
BLE setup with a direct USB phone connection or provide SDK-token-only enrollment.
