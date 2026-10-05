---
name: gadget-miele-g7566-dishwasher
description: >-
  Read local Miele G 7566 dishwasher status through Muse Home Link when its module supports
  the documented protocol and existing GroupID/GroupKey are available. Consider commands only
  after confirming exact capabilities and local enablement; remote start is not promised.
---

# Miele G 7566 Dishwasher

Use this skill when a Miele G 7566 has a compatible local communications module and the household's existing GroupID and GroupKey are available.

## Identify the Device

- Match `_mieleathome._tcp` discovery and the appliance's model/type metadata. A `group` identifier is not the secret GroupKey.
- An unsigned 404 response with a Miele content type can indicate an authentication requirement; it does not prove usable reads or permitted control.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Existing GroupID/GroupKey and a client implementing the documented MieleH256 signing, encrypted bodies and authenticated response handling.
- The exact device route returned by the local appliance API, not a guessed serial number.
- For any permitted physical command, a user request, a safely prepared appliance and all required panel-side enablement.

## Workflow

1. Implement or use the local-protocol client component described by MieleRESTServer without deploying its REST server. Preserve its signing/encryption rules, exact signed host/path/headers, response validation and secure handling of the GroupKey.
2. Use authenticated `GET /Devices/` to discover the device route, then the documented local `Ident` and `State` resources beneath that route. Read only fields actually returned by this module.
3. Report available status, program, remaining time and faults using the module's documented field meanings. Do not copy cloud-only field names, program lists or filling-level features into the local interface.
4. Only consider a command if the exact model/module protocol documents it and current appliance state permits it. Remote start is conditional, not promised: the user must prepare/arm the machine locally and all interlocks must allow it.
5. For an allowed requested operation, use that local protocol's documented command, then poll authenticated state. If the capability or prerequisites cannot be established, remain read-only and explain what is missing.

## Verify the Result

- Keep model identification, successful decryption, readable state and successful physical operation as separate checks.
- A sleeping module may return stale/invalid information. Do not wake or start it merely to check access without permission.
- Inspect running/program state after any authorized command; HTTP acceptance alone does not prove start or stop.

## Limits

- Support is conditional on this unit's communications module and exposed capabilities.
- MieleRESTServer endpoints such as `/generate-summary` and `/start/<name>` belong to its wrapper server, not the appliance. Do not send them to the dishwasher.
- No cloud login, key provisioning, reset/recommissioning, generic program writes, new server deployment or bypassed door/remote-start interlocks.

## Sources

- [Miele local-protocol client and compatibility notes](https://github.com/akappner/MieleRESTServer)
