# Muse Gadget SDK on Fedora

Fedora support for the Linux installer, added as a third distro family
alongside the existing Debian/Ubuntu and Arch paths.

**Status: verified working.** Installed and paired successfully on
Fedora 44 (KDE), x86_64, Python 3.14.8, on 2026-10-24. The `musegadget`
service came up active, the host appeared in Muse's device list, and
remote commands ran on it. SELinux (enforcing, default policy) did not
block the install or Bluetooth.

## What changed

All changes are in `linux/install.sh`, on this fork's `fedora` branch:

- `FEDORA_PACKAGES=(bluez python3 python3-dbus python3-gobject python3-cryptography curl ca-certificates)`
  — Fedora's names for the same dependencies the Debian/Arch branches use
  (`python3-gobject` is Fedora's name for the `gi` bindings).
- `check_system` accepts `ID=fedora`, and treats `ID_LIKE` containing
  `fedora`/`rhel` as the Fedora family with an "untested" warning,
  mirroring how derivatives are handled for the other families. The
  unsupported-distro message now names Fedora as supported.
- Package presence is checked with `rpm -q` and installs run through
  `dnf install -y`, mirroring the `pacman -Qq` / `pacman -S` pattern.
- `enable_bluez` enables `bluetooth.service` on Fedora, as it already
  did on Arch.

Everything else — the pinned Python environment, the systemd unit, the
pairing flow — is distro-agnostic and runs unchanged. The SDK's only
PyPI dependency is `websockets` (pure Python), so Fedora's newer Python
does not strain the hash-pinned lock file.

## Notes for Fedora users

- A Bluetooth adapter is required for pairing (built-in or a USB dongle
  such as the TP-Link UB500). Check with `ls /sys/class/bluetooth` —
  if it's empty, plug the dongle in before the pairing step.
- Your phone's location services must be on for BLE pairing (Android
  requirement, not a Fedora one).
- Rollback: `bash linux/install.sh --uninstall` removes the service
  and `/opt/musegadget`. The dnf packages stay; they are ordinary system
  libraries.

## Credits

Patch by phlux404, developed and tested with Muse.
Installer is Meta's open-source Muse Gadget SDK installer
(Apache-licensed); this file describes the Fedora branch only.
