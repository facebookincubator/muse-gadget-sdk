---
name: gadget-vesync-humidifier
description: >-
  Cloud only: needs internet access and the user's VeSync account; does not use Muse
  Home Link or the local network. Read and control a VeSync-connected Levoit humidifier
  (e.g. Classic 200S / 300S) through the VeSync cloud API: power, target humidity,
  mode, mist level, current humidity, and water-empty state. VeSync humidifiers have
  no local API.
---

# VeSync Humidifier (Levoit)

Use this skill when the task concerns a VeSync-connected humidifier:
state checks ("how humid is the bedroom"), target-humidity changes,
power, mode, or mist control, and water-empty alerts.

## Identify the Device

- The humidifier is enrolled in the user's VeSync account and named
  there (the account name is the identity; the device also carries a
  model code, e.g. Classic200S).
- Match the user's words against the device names on the account. With
  more than one match, list the candidates and ask; never guess.
- Confirm model before using model-dependent commands: mist-level
  ranges and modes (auto / manual / sleep) vary by model. Use only the
  values the client reports for this model.

## Local Access Verdict

- **VeSync devices are cloud-only.** Control and state run through the
  vendor's account API (`smartapi.vesync.com`, or `smartapi.vesync.eu`
  for EU accounts; the client picks the account's region); the device keeps a
  persistent outbound connection to the vendor cloud and exposes no
  LAN control or state endpoint. Community and vendor tooling
  (e.g. pyvesync) is uniformly cloud-based.
- A LAN probe finding no service is the expected result. Do not
  attempt local pairing hacks or firmware mods for local control.

## Prerequisites

- **Cloud only.** Internet access from Muse to the VeSync API, using
  the normal internet path, never the Home Link proxy; a Muse Home Link
  is not required. If the cloud is unreachable, report that rather
  than trying a local path.
- The user's VeSync account credentials, supplied through approved
  secret storage. Never ask for the password in chat, never put it or
  the session token on a command line, and never print, log, or
  send either anywhere except to the VeSync API through the client.
  Keep the persisted token in the same secret storage, not in a plain
  file. If no credentials are stored, explain this setup instead.

## The Lane

- Direct client on the VeSync account API ([pyvesync](https://github.com/webdjoe/pyvesync)
  or equivalent): password login once → persisted token → device list
  → per-device detail reads and commands.
- State read returns: power, connection status, current humidity,
  target humidity, mode, mist level, water-lacks (empty tank),
  tank-lifted, humidity-high flags.
- Commands: on/off, set target humidity, set mode, set mist level.
- Background polling runs no tighter than every 5 minutes; the reads
  around a requested change (below) are separate. The token is reused;
  the password is only needed again if the token is rejected.

## Changing Settings

1. Act only on the user's request for this named device. A state
   question is never permission to change anything.
2. Read fresh state first. If the device is offline, report that and
   send nothing.
3. Send one command for the requested change.
4. Wait a second or two, then read fresh state again. Report the change
   done only when that read succeeds and shows it; the client's cached
   state after a command does not count. If the read fails, returns
   nothing usable, or does not show the change, say the change is
   unconfirmed and report what the device last showed. Never resend a
   command automatically.

## Safety Limits

- **Never run the humidifier with an empty tank:** check
  `water_lacks` before turning on or raising mist; if the tank lacks
  water, report instead of running it dry.
- Target humidity stays in the healthy indoor band (40–50% unless the
  user directs otherwise); sustained >60% indoor humidity risks mold.
- An account login rejection can mean the account is SSO-only
  ("Sign in with Google") with no password set — do not retry-loop;
  surface it.

## Verification

- A live state read returning the device online with a plausible
  current-humidity value is the health check. Cross-check once
  against the VeSync app or the device's own display on a new
  deployment, and verify any command by read-back.

## Sources

- [pyvesync: VeSync account API client, including Levoit humidifiers](https://github.com/webdjoe/pyvesync)
  (checked against 3.5.1; field names such as `water_lacks` are
  pyvesync's)
