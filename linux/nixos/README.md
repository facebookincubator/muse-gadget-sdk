# musegadget on NixOS

A Nix flake and NixOS module for the Linux daemon, for systems where
`install.sh` does not apply. The whole install is declarative: the
package, the systemd unit, the Bluetooth pairing settings and the
trust store handling below are all part of the system configuration,
and removing the module removes the install.

## Requirements

- NixOS (or Nix on another Linux for the package alone), with flakes
  enabled.
- Bluetooth LE, onboard or a USB dongle — needed only for pairing;
  the running service never touches Bluetooth.
- An SDK token (see below) and the Muse app on iOS or Android.

## Install

Add the flake as an input and enable the module:

```nix
# flake.nix
inputs.musegadget.url = "github:facebookincubator/muse-gadget-sdk?dir=linux/nixos";

# configuration.nix
imports = [ inputs.musegadget.nixosModules.musegadget ];

services.musegadget = {
  enable = true;
  runAs = "youruser";   # the account Muse's commands run as
};
```

`runAs` has no default and must be set: the daemon runs as root (it
owns the device credentials and the local command socket) and drops
to this account per command, exactly as on Debian. The account's
sudo rights are the security decision; the module makes it visible
in the configuration instead of in an install script.

Then `nixos-rebuild switch`.

## SDK token

Every gadget needs an SDK token to pair. Create one at
[gadgets.muse.ai/settings/sdk-tokens](https://gadgets.muse.ai/settings/sdk-tokens).

The token is **not** a module option: the Nix store is
world-readable, so a token in the configuration would be public.
Place it by hand, once:

```sh
sudo systemctl start musegadget     # first start creates /var/lib/musegadget
sudo systemctl stop musegadget
sudo install -m 600 sdk_token /var/lib/musegadget/sdk_token
sudo systemctl start musegadget
```

## Pairing

1. On Android, turn **location on** — Android will not BLE-scan with
   it off. In the Muse app, enable Developer mode under
   Settings > Devices.
2. Run `musegadget pair`.
3. In the app, add a device and choose the `MuseGadget…` entry, then
   Connect.

Success looks like this in `journalctl -u musegadget`:

```
musegadget.link_client: Noise session established
musegadget.link_client: sent link.register as homelink-…
musegadget.link_client: registered with the Muse
```

The module sets the two Bluetooth settings pairing depends on:
`GATT.ExchangeMTU = 256` (required for Android pairing) and the
`battery` plugin disabled, matching the stock install.

## Trust store

musegadget checks its TLS trust store before connecting and refuses
while the store trusts the compromised roots listed in
`src/musegadget/tls.py` (see "Root CAs" in the Linux README).
nixpkgs' `cacert` bundle still ships one of them — the Comodo AAA
root, as a reissued certificate with the same key — so on a stock
NixOS the daemon exits with EX_CONFIG and the message
`the system trust store still trusts Comodo AAA Services root`.

This package handles it the NixOS way: it builds a filtered copy of
nixpkgs' own bundle (`filter-ca-bundle.py`, dropping the distrusted
roots matched by certificate hash, key hash, or subject name — the
same matching `tls.py` performs, including the bundle's
TRUSTED CERTIFICATE blocks, which is where the AAA root appears) and
wraps the binary with `SSL_CERT_FILE` pointing at it. The service
and the CLI see the same store, the rest of the system keeps the
distro bundle untouched, and the filtered bundle updates with
nixpkgs. It is exposed as `passthru.caBundle` for inspection.

Relatedly, the module sets `RestartPreventExitStatus = 78`:
EX_CONFIG is musegadget's "fix the configuration first" exit, and
the unit must not restart-loop on it.

## Tests

The package runs the SDK's pytest suite during the build. Tests that
probe the host environment (the Debian installer's behavior, the
host trust store, ESP32 firmware tooling, sandbox-incompatible
process semantics) are disabled in `package.nix`, each with its
reason recorded there. On NixOS 26.05 / x86_64 the suite reports
1314 passed, 0 failed with those disables.

## Verified

Live install on NixOS 26.05 (Intel J3710 mini PC, onboard
Bluetooth): system rebuilt with the module enabled, daemon started,
paired with the Muse app and registered with the Muse service.
