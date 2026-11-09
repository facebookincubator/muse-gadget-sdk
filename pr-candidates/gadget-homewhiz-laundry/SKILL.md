---
name: gadget-homewhiz-laundry
description: >-
  Read state of HomeWhiz-connected laundry appliances (Arçelik / Beko /
  Blomberg / Grundig washer, dryer, washer-dryer) through the direct
  HomeWhiz cloud lane. Covers cycle state, substate, program, and time
  remaining for cycle-finished detection. There is no local API and no
  remote start; state reads only.
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

- **There is no local API.** The HomeWhiz protocol is a cloud-account
  protocol end to end (established by reverse engineering the shipped
  client, 2026-10): brand-cloud login issues access/refresh tokens,
  which exchange for temporary AWS credentials, which read the
  appliance's AWS IoT device shadow. No documented or discovered LAN
  endpoint serves state or control.
- Appliance Wi-Fi radios are power-save devices: they appear in LAN
  sweeps only intermittently and answer no stable local service. A LAN
  probe that finds nothing is the expected result, not a fault.
- Do not attempt a local pairing, port scan beyond a confirmation
  probe, or a third-party "local" integration that re-implements the
  cloud login against the appliance — none exists.

## The Lane

- Direct client implementing the protocol above: login → token →
  temporary AWS credentials → IoT shadow reads. One `state` call
  returns every appliance on the account: connected, running, device
  state, substate (e.g. "washing cycle complete; remove laundry"),
  program, remaining/elapsed minutes, warnings.
- A state call is slow (tens of seconds — per-appliance shadow reads).
  Poll no tighter than 5 minutes; cache between polls.
- Cycle-finished detection = a running → not-running transition
  between polls, with the substate confirming completion.

## Safety Limits

- **No remote start exists in the protocol** (the machine's own safety
  interlock requires physical presence to start). Never claim a cycle
  was started remotely; writes are limited to settings-class changes
  the shadow exposes, and state reading is the supported operation.
- State is a cloud shadow: it can lag the machine. Report the read
  timestamp with any state claim.
- Credentials live in the deployment's secret store; never print,
  log, or transmit them.

## Verification

- A live `state` read returning both appliances with plausible
  state/substate is the health check. Cross-check once against the
  HomeWhiz mobile app or the appliance display before trusting a
  new deployment.
