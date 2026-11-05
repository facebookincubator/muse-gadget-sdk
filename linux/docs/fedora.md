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

# Fedora installation notes

Use the standard [Linux installation and pairing instructions](../README.md#install).
The installer supports Fedora alongside Debian/Ubuntu and Arch.

## Prerequisites

- An x86_64 or aarch64 Fedora system with `dnf`, systemd and Python 3.9 or later.
- Bluetooth LE for pairing, plus an SDK token and the Muse app.
- The distro's Python interpreter and D-Bus/GObject bindings; do not substitute
  a Python environment that cannot see those system packages.

Fedora/RHEL-like derivatives are detected with an "untested" warning. They must
meet the same prerequisites; detection is not a compatibility guarantee.

## Packages and service

The installer uses `rpm -q` to check packages and `dnf install -y` to install
`bluez`, `python3`, `python3-dbus`, `python3-gobject`, `python3-cryptography`,
`curl` and `ca-certificates`. `python3-gobject` provides the `gi` bindings that
Debian calls `python3-gi`. It also enables `bluetooth.service`.

The normal SDK environment, systemd unit and pairing flow are unchanged.

## Reported validation and limits

The Fedora contributor reported successful installation, pairing and remote
commands on Fedora 44 (KDE), x86_64, Python 3.14.8 on October 24, 2026, with
SELinux enforcing its default policy. Other derivatives and optional voice
configurations were not validated by that report.

- If `ls /sys/class/bluetooth` is empty, connect a Bluetooth LE adapter before
  pairing. Android also requires the phone's location services for BLE pairing.
- `bash install.sh --uninstall` removes the service and `/opt/musegadget`, but
  leaves the distro packages and, unless `--purge` is used, pairing state.

Fedora support contributed by phlux404.
