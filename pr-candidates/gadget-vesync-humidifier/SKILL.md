---
name: gadget-vesync-humidifier
description: >-
  Read and control a VeSync-connected Levoit humidifier (e.g. Classic
  200S / 300S) through the direct VeSync cloud lane: power, target
  humidity, mode, mist level, current humidity, and water-empty state.
  VeSync devices are cloud-only; there is no local API.
---

# VeSync Humidifier (Levoit)

Use this skill when the task concerns a VeSync-connected humidifier:
state checks ("how humid is the bedroom"), target-humidity changes,
power, mode, or mist control, and water-empty alerts.

## Identify the Device

- The humidifier is enrolled in the user's VeSync account and named
  there (the account name is the identity; the device also carries a
  model code, e.g. Classic200S).
- Confirm model before using model-dependent commands: mist-level
  ranges and modes (auto / manual / sleep) vary by model.

## Local Access Verdict

- **VeSync devices are cloud-only.** Control and state run through the
  vendor's account API (`smartapi.vesync.com`); the device keeps a
  persistent outbound connection to the vendor cloud and exposes no
  LAN control or state endpoint. Community and vendor tooling
  (e.g. pyvesync) is uniformly cloud-based.
- A LAN probe finding no service is the expected result. Do not
  attempt local pairing hacks or firmware mods for local control.

## The Lane

- Direct client on the VeSync account API (pyvesync or equivalent):
  password login once → persisted token → device list → per-device
  detail reads and commands.
- State read returns: power, connection status, current humidity,
  target humidity, mode, mist level, water-lacks (empty tank),
  tank-lifted, humidity-high flags.
- Commands: on/off, set target humidity, set mode, set mist level.
  Every command is verified by a state read-back.
- Poll no tighter than 5 minutes; the token is reused, the password
  is only needed again if the token is rejected.

## Safety Limits

- **Never run the humidifier with an empty tank:** check
  `water_lacks` before turning on or raising mist; if the tank lacks
  water, report instead of running it dry.
- Target humidity stays in the healthy indoor band (40–50% unless the
  user directs otherwise); sustained >60% indoor humidity risks mold.
- An account login rejection can mean the account is SSO-only
  ("Sign in with Google") with no password set — do not retry-loop;
  surface it.
- Credentials live in the deployment's secret store; never print,
  log, or transmit them.

## Verification

- A live state read returning the device online with a plausible
  current-humidity value is the health check. Cross-check once
  against the VeSync app or the device's own display on a new
  deployment, and verify any command by read-back.
