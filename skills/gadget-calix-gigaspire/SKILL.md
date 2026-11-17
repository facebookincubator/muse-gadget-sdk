---
name: gadget-calix-gigaspire
description: Read read-only status including connected devices and DHCP and wireless and system information from Calix GigaSpire routers through a Linux Muse gadget on the home network and not through a Muse Home Link tunnel without changing any router settings or handling router credentials
---

## Calix GigaSpire Routers

Use this skill when you want read-only status from a Calix GigaSpire router acting as the home gateway, such as connected devices, DHCP leases, wireless state, or system information, answered through an existing local connector rather than by changing any router setting.

## Identify the Device

Confirm the target is a Calix GigaSpire router running EXOS and acting as the home gateway, for example a GigaSpire BLAST u6 series unit. Identification rests on the device family and model labelling, its EXOS software, and its gateway role on the local network, not on a device type fingerprint reported by the gateway itself. If the device is a different router family, a standalone access point, or a mesh satellite that is not the gateway, stop and report that this skill does not cover it.

Transport on the local network is HTTPS with a self-signed gateway certificate. The connector runs on a LAN host that already trusts this gateway. Never instruct the user to disable TLS validation in a way that leaves them unprotected, and never ask the user to weaken browser or system trust settings for this skill.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

In practice this skill runs from a Linux Muse gadget on the router's own LAN, using an existing read-only local connector tool the user already runs there, rather than re-implementing router login. The skill itself does not implement gateway login, does not collect a router password, and does not create a new connector or credential store.

Router credentials are never entered by the skill, never pasted into chat, and never leave the host where the user's connector already keeps them. If no such connector is already present and working on that LAN host, stop and report that the prerequisite is missing instead of improvising a login.

The existing connector must be one the user set up themselves, with their own router admin credentials, for their own gateway. Never use a connector or credentials configured for another person's router, and never share router credentials.

## Workflow

1. Confirm the device identity as described above and confirm the Linux Muse gadget is on the router's own LAN.
2. Confirm the question is a read-only status question. A state question is never permission to change anything.
3. Use the existing connector's read commands for the information requested. Available reads are a connectivity ping check, the connected device list, DHCP lease state, wireless network state, WPS state, system and board information, installed apps, routes including IPv6, and the station list.
4. Let the connector handle the gateway login shape. The gateway uses a nonce login where the password is hashed with the nonce before posting, followed by a session. **Load-bearing trap — every post-login request must carry the session cookies together with an X-CSRF-Token header taken from the second segment of the csrf_token cookie issued by that same login; a missing or stale token makes the gateway reset the connection outright rather than returning an authorization error. A fresh login is the fix, not retries with the old token.**
5. Do not resend requests in a tight loop against the gateway, and do not probe the gateway's failed-login lockout behavior. If a read fails after a connection reset, allow a fresh login through the connector rather than repeating the stale request.
6. Present only the connector's secret-scrubbed output. Wireless keys, passwords, and similar fields are dropped by the connector and must never be requested or displayed by the skill.
7. Interpret the results with the verification traps below before reporting an answer.

## Verify the Result

Check that the answer came from the intended gateway and from the read operation that matches the question, and cross-check related reads when a conclusion depends on them.

Apply these interpretation traps. Several reads return success with an empty body when a feature is dark — an empty body is not an empty list, and it should be reported as a feature that returned nothing rather than as proof that nothing is configured. The device table is the gateway's remembered known-host list, not a live network scan, so absence from that table does not prove a device is gone from the network. Gateway device-type fingerprints are heuristics and can be wrong, so do not present a fingerprint as a certain identification.

If reads disagree, or a read returns an empty body where a populated result was expected, say so plainly and report what was actually returned instead of filling the gap with an assumption.

## Limits

This skill is read-only only. It never changes any router setting.

This skill runs only from a Linux Muse gadget on the router's own LAN. It does not work through a Muse Home Link tunnel.

Excluded by name are DHCP reservation changes, device renaming, WPS pairing changes, MAC filtering or quarantine actions, SSID or password changes, radio changes, firmware updates, and router reboots. If a request asks for any of those, stop and report that it is outside this skill's scope.

Router credentials are never entered by the skill, never pasted into chat, and never leave the host where the user's connector already keeps them. Secret fields such as wireless keys and passwords are never requested or displayed.

Do not probe the gateway's failed-login lockout behavior, and do not resend requests in a tight loop against the gateway. A state question is never permission to change anything.

## Sources

This skill is the documented exception to the official-docs-only convention. No public official API documentation for the GigaSpire local interface exists, and Calix publishes its EXOS material through a provider-gated channel. The workflow above therefore rests on bench-verified local evidence from a running installation rather than on public official documentation.

No private connector internals are described here. Credentials stay on the gateway-side host in the connector's own approved storage.
