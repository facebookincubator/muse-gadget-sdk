---
name: gadget-homewhiz-laundry
description: >-
  Cloud only: needs internet access and the user's HomeWhiz account; does not use Muse
  Home Link or the local network. Read the state of HomeWhiz-connected laundry
  appliances (Arçelik / Beko / Blomberg / Grundig washer, dryer, washer-dryer) through
  the HomeWhiz cloud: cycle state, substate, program and time remaining, for
  cycle-finished detection. Read-only: never starts, stops or changes a cycle.
---

# HomeWhiz Laundry Appliances

Use this skill when the task concerns a HomeWhiz-connected washer or
dryer: "is the laundry done", cycle-finished alerts, or appliance state
in a home census.

## Identify the Device

- HomeWhiz is Arçelik's connected-appliance platform (brands include
  Beko, Blomberg, Grundig, Arçelik). The appliance carries a model code
  and serial on its rating plate and in the HomeWhiz mobile app.
- Confirm the appliance is enrolled in the user's HomeWhiz account —
  the cloud account, not the LAN, is the identity that matters.

## Local Access Verdict

- **There is no LAN API for Wi-Fi appliances.** State comes from the
  HomeWhiz cloud: an account login issues tokens, which exchange for
  temporary AWS credentials, which read the appliance's AWS IoT device
  shadow. The community HomeWhiz integration implements this flow (see
  Sources).
- Some appliances also offer a local Bluetooth connection, used by the
  same community integration. This skill does not cover it.
- Appliance Wi-Fi radios are power-save devices: they appear in LAN
  sweeps only intermittently and answer no stable local service. A LAN
  probe that finds nothing is the expected result, not a fault. Do not
  scan or pair over the LAN.

## Prerequisites

- **Cloud only.** Internet access from Muse to the HomeWhiz cloud,
  using the normal internet path, never the Home Link proxy; a Muse
  Home Link is not required. If the cloud is unreachable, report that
  rather than trying a local path.
- The user's HomeWhiz account credentials, supplied through approved
  secret storage. Never ask for the password in chat, never put it or
  any token on a command line, and never print, log, or transmit them.
  An account created with "Sign in with Google" has no password until
  the user sets one through the app's password reset.
- A client for the cloud flow above, such as the community HomeWhiz
  integration's cloud code (see Sources).

## Workflow

- One state read returns every appliance on the account: connected,
  running, device state, substate (e.g. "washing cycle complete;
  remove laundry"), program, remaining/elapsed minutes, warnings.
- A state read is slow (tens of seconds — per-appliance shadow reads).
  Poll no tighter than 5 minutes; cache between polls.
- Cycle-finished detection = a running → not-running transition
  between polls, with the substate confirming completion.

## Limits

- **Read-only.** Never start, stop, pause or change a cycle or any
  setting, even when the appliance's remote-control mode is enabled.
  Never claim a cycle was started remotely.
- State is a cloud shadow: it can lag the machine. Report the read
  timestamp with any state claim.

## Verify the Result

- A live state read that lists the account's enrolled appliances with
  plausible state/substate is the health check. Cross-check once
  against the HomeWhiz mobile app or the appliance display before
  trusting a new deployment.

## Sources

- [Community HomeWhiz integration (cloud and Bluetooth clients)](https://github.com/home-assistant-HomeWhiz/home-assistant-HomeWhiz)
