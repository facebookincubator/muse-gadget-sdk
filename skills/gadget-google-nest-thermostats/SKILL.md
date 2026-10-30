---
name: gadget-google-nest-thermostats
description: >-
  Read and adjust Google Nest thermostats through Google's Smart Device Management API
  via an existing self-hosted bridge on the home network (for example Home Assistant on
  a Raspberry Pi) using Muse Home Link. Not Google Cast, not the Google Home app, and
  not Matter commissioning.
---

# Google Nest Thermostats — Smart Device Management Bridge

Use this skill when the home's Nest thermostats are already bridged into a local system
Muse can reach — most commonly Home Assistant on a Pi — through Google's Device Access
program, and the user asks about indoor temperature, setpoints, or HVAC mode.

## Identify the Device

- Confirm which thermostats the bridge exposes and match them to rooms with the user;
  SDM device names rarely match room names out of the box.
- Read current ambient temperature, humidity, mode, and setpoint from the bridge before
  changing anything, so a change can be verified against the prior state.

## Guided Setup Check

If no bridge or no thermostat is reachable, stop and give the user these steps — do not
create projects, consent to OAuth, or handle tokens on their behalf.

1. In the Google Device Access Console, create a Device Access project and enable the
   Smart Device Management API. Google charges a one-time US$5 registration fee; it is
   a developer program, and Google can change its terms, quotas, or availability.
2. As the Google account owner, complete the OAuth consent flow and connect the project
   in the local bridge (for example the Home Assistant Google Nest integration). Refresh
   tokens stay in the bridge; if they expire, only the owner can re-consent.
3. List the devices the project exposes and match each thermostat to its room with the
   user before answering questions about it.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The guided setup check passes: a Device Access project exists, the bridge holds valid
  tokens, and the thermostats are reachable through the bridge's local API or entities.
- Note: SDM writes can lag and occasionally report success before the thermostat applies
  them; the workflow below verifies after every write. Newer Nest models may also
  support Matter locally — prefer whichever path the home already runs rather than
  migrating as a side effect.

## Workflow

1. For status questions, read the thermostat traits through the bridge's local API or
   entities: ambient temperature and humidity, HVAC status, mode, active setpoint.
2. For an adjustment the user requested, set the mode or setpoint through the bridge,
   then re-read the device state after a short wait to confirm it applied.
3. Report temperatures in the units the household uses, with the read time.

## Verify the Result

- A successful API response is not proof the thermostat changed: confirm via a fresh
  state read showing the new setpoint or mode.
- If reads return stale data, say so and give the last-update time rather than
  presenting it as current.

## Limits

- No changes to the Device Access project, OAuth grants, account sharing, or thermostat
  schedules and learning features from a skill session.
- Comfort control only: never present thermostat automation as freeze, heat-safety, or
  occupancy protection.
- Temperature history is household data; report what the user asked for, nothing more.

## Sources

- [Google Device Access / Smart Device Management API](https://developers.google.com/nest/device-access)
- [Home Assistant Google Nest integration](https://www.home-assistant.io/integrations/nest/)
