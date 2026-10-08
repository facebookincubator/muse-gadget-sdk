---
name: gadget-lutron-smart-bridges
description: >-
  Read or control lights and shades on compatible Lutron Smart Bridges through Muse Home Link.
  Use authorized HAP pairing or independently paired LEAP; preserve any existing Apple Home
  setup.
---

# Lutron Smart Bridges: Light and Shade Control

Use this skill when fresh discovery identifies a compatible Lutron Smart Bridge and the user requests light or shade status/control.

## Identify the Device

- Match bridge model and current advertised HAP or documented LEAP service, not the brand alone.
- Use exact discovered endpoints. Lutron product families do not all expose the same protocols.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- HAP requires the eight-digit setup code for a pairable bridge or previously authorized controller pairing keys.
- A bridge already in Apple Home does not automatically give Muse its credentials. Do not reset or remove an existing pairing. If access is unavailable, use only a supported independently authorized alternative or request setup.
- LEAP uses a separate physical pairing flow and client certificate/private key; HAP credentials do not substitute for them.

## Workflow

1. Prefer the HAP path when authorized pairing is available. Use a maintained HAP client over the shared network access.
2. When the advertisement has `ff=1`, select `PairSetupWithAuth`; plain `PairSetup` can fail at M2 with `kTLVError_Authentication`. Keep that exchange on one persistent connection.
3. Store successful pairing material in approved credential storage (private file permissions if file-backed). Open an encrypted session and enumerate actual accessories, services and characteristics.
4. Read the target light/shade state and supported ranges. Resolve ambiguous generic accessory names before writing; any physical identification action must be explicitly requested.
5. Write only the requested characteristic, then read back the individual result. For partial batch failures, inspect each target before retrying uncertain operations.
6. For a compatible bridge using LEAP instead, follow pylutron-caseta's pairing routine with the user present, store its own credentials, enumerate devices with the authenticated client and use its documented level/cover operations.

## Verify the Result

- Check the intended accessory's resulting on/brightness/position state, not merely a successful session or write acknowledgement.
- For shade movement, observe progress and final state; preserve obstruction and motor behavior.
- If pairing or a command fails, report that exact limitation without implying the bridge has no local API.

## Limits

- No assumed LIP/telnet access, cloud fallback, broad product compatibility or replacement of someone's existing Home.
- Do not add schedules, change bridge configuration or discover room mappings by unrequested toggling.

## Sources

- [pylutron-caseta](https://github.com/gurumitts/pylutron-caseta)
- [aiohomekit HAP client](https://github.com/Jc2k/aiohomekit)
