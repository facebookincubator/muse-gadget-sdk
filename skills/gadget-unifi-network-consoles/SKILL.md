---
name: gadget-unifi-network-consoles
description: >-
  Inspect read-only network inventory and status through an existing local UniFi Network
  controller using Muse Home Link. Use an authorized compatible HTTPS API for the intended
  site; not network configuration or client blocking.
---

# UniFi Network Consoles — Read-Only Inventory

Use this skill when the home has an existing local UniFi Network controller/console with a compatible authenticated HTTPS API.

## Identify the Device

- Confirm console/controller product, UniFi OS and Network application versions using the authorized local endpoint.
- Identify the requested site before enumerating its devices or clients. A UniFi access point alone is not necessarily the controller.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- An existing authorized local controller account/session with read-only access where available.
- The correct controller API family and trusted TLS identity. Cloud SSO/account access is not assumed.
- Permission to inspect network inventory; client names, identifiers and activity are private household data.

## Workflow

1. Use a version-compatible local API client and authentication flow. Legacy controllers commonly use `/api`; UniFi OS commonly proxies Network requests under `/proxy/network/api`. Confirm the deployed version rather than probing arbitrary variants.
2. Authenticate over the local HTTPS service, preserving cookies/tokens and any required CSRF handling without logging them.
3. Enumerate authorized sites, then request documented device/client/status reads for the selected site. Community methods include site listing and `stat/device` or `stat/sta` reads under that API's site route.
4. Handle pagination and offline/stale records. Report the controller's observation time and distinguish connected clients from historical inventory.

## Verify the Result

- Check API-level success/error metadata, authorization scope and returned site identity.
- A controller record is not proof that a client is currently reachable or that its smart-home control interface works.

## Limits

- Read-only skill: no restart, adoption, blocking/unblocking, port, Wi-Fi, firewall or network configuration changes.
- No assumed cloud SSO, new controller deployment, guessed private endpoints or official-support claim.
- Do not expose raw client inventories beyond the user's request.

## Sources

- [Community UniFi API client and compatibility notes](https://github.com/Art-of-WiFi/UniFi-API-client)
