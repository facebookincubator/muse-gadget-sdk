---
name: gadget-tesla
description: >-
  Read Tesla status, drive and charge history, and efficiency analytics from an existing
  self-hosted TeslaMate and Fleet Telemetry stack on the home network using Muse Home Link.
  If a setup layer is missing, guide the user through that layer's setup instead of guessing.
  Read-only by default; vehicle commands only on explicit user request through the owner's
  existing command path. Not an official Tesla integration or a Tesla-account sign-in.
---

# Tesla — Fleet API, TeslaMate and Fleet Telemetry (Self-Hosted)

Use this skill when the home runs (or wants to run) a self-hosted Tesla data stack —
typically a Raspberry Pi or home server with TeslaMate, optionally Tesla's Fleet
Telemetry server publishing to a local MQTT broker and analytics database — and the user
asks about the car: charge state, recent drives, efficiency, self-driving share, or
charging cost.

## Identify the Setup

- Inventory the layers before querying anything: Fleet API registration, TeslaMate
  (API, MQTT broker, Postgres), and, if present, the fleet-telemetry server and the
  analytics store it feeds. State which layers exist; the analytics below degrade
  gracefully without telemetry.
- Identify the vehicle from the local stack (TeslaMate car id, VIN suffix), never from a
  Tesla account lookup. With more than one car, confirm which car the question is about.

## Guided Setup Check

Run this check the first time the skill is used, and whenever a layer turns out to be
missing. For each missing layer, stop and give the user these steps — do not improvise
installations, registrations, or commands on their behalf.

1. **Developer account and Fleet API app.** Create an account at developer.tesla.com and
   register an application. Generate a key pair and host the public key at
   `https://<their-domain>/.well-known/appspecific/com.tesla.3p.public-key.pem`, then
   register the partner account for that domain. Tesla includes a monthly free credit
   (currently $10); typical reads cost fractions of a cent beyond it.
2. **Virtual key on the vehicle.** Enroll the app's virtual key on the car when Tesla
   prompts for it (Tesla app or in-car). Note: the key pair from step 1 is also the
   signing key for vehicle commands. The fleet-telemetry server's TLS key is a different
   key — reusing it for commands returns `InvalidSignature`.
3. **TeslaMate.** Install TeslaMate (Docker is typical) and sign in with owner tokens.
   In car settings, use the sleep-friendly configuration: streaming API off,
   `suspend_min` 12, `suspend_after_idle_min` 10, and the LFP flag set if the pack is
   LFP. Polling-only plus telemetry push gives rich data while letting the car sleep.
4. **Fleet Telemetry (optional, recommended).** Run Tesla's open-source fleet-telemetry
   server with a restart policy (for example `unless-stopped`), register a telemetry
   configuration for the vehicle via the Fleet API, and point its records at the local
   MQTT broker. Fields worth subscribing to: `Soc`, `Odometer`, `EnergyRemaining`,
   `VehicleSpeed`, `Gear`, `Location`, and `SelfDrivingMilesSinceReset`. The vehicle
   streams only while awake and stops when it sleeps, so telemetry costs effectively
   no vampire drain.
5. Label TeslaMate geofences (home, work) now — every later analysis gets easier.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The guided setup check passes, or the user accepts answers limited to the layers
  that exist.
- Read-only credentials for the local databases and MQTT broker, stored by the host,
  never pasted into chat.

## Workflow

1. Answer from local data first. Current state (SoC, plugged in, asleep) comes from
   TeslaMate's API or MQTT topics (`teslamate/cars/<id>/...`); drives and charges from
   its database; 10-second telemetry from the telemetry store.
2. Efficiency: compute per-drive Wh/mi from energy used and distance, and compare with
   the vehicle's EPA rating. Self-driving share comes from the delta of the telemetry
   self-driving-miles counter across a drive, divided by drive distance.
3. Vampire drain: measure SoC change across parked, unplugged, asleep periods only;
   exclude charging and the first minutes after parking, and label the result an
   estimate.
4. Charging: report session start/end, SoC delta, kWh added, and whether the session
   overlapped the household's peak time-of-use window.
5. Only when local data cannot answer and the user asks: make a live Fleet API read.
   Never wake the vehicle to satisfy curiosity.

## Verify the Result

- Cross-check headline numbers against a second local source (TeslaMate UI vs database,
  telemetry vs drive record). State the source and observation time for every number;
  the layers update on different cadences and will disagree slightly.
- A database record is history, not current state — say how fresh it is.
- If analytics suddenly go stale, check that the telemetry container is running before
  suspecting the vehicle.

## Limits

- No vehicle commands (locks, climate, charge start/stop, sentry) unless the user
  explicitly requests one and an owner-controlled command proxy is already deployed.
  Never send Fleet API commands directly from a skill session.
- Never change TeslaMate polling or suspend settings, telemetry field subscriptions, or
  Fleet API scopes as a side effect of answering a question.
- Drive routes and location history are sensitive household data: report aggregates by
  default, exact locations only when the user asks.
- Keep exact-time automations (scheduled pre-cooling, charge-complete alerts) in the
  local automation layer, not in Muse — they must fire even if the model is down.
- Community pattern, not an official integration; Tesla can change Fleet API pricing,
  scopes, or telemetry fields at any time.

## Sources

- [Tesla Fleet API documentation](https://developer.tesla.com/docs/fleet-api)
- [Tesla Fleet Telemetry (open-source server)](https://github.com/teslamotors/fleet-telemetry)
- [TeslaMate documentation](https://docs.teslamate.org/)
