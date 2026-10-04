---
name: gadget-moonraker-3d-printers
description: >-
  Read 3D-printer status and manage requested print jobs through an existing Moonraker service
  using Muse Home Link. Use for a commissioned printer and existing print files; not arbitrary
  G-code, firmware setup, or configuration changes.
---

# Moonraker 3D Printers

Use this skill when current discovery or a user-confirmed endpoint identifies an existing Moonraker service attached to the intended printer.

## Identify the Device

- Read `/server/info` and `/printer/info` using the authorized endpoint and credentials. Confirm Moonraker/Klipper versions, connection state and printer identity.
- Use `/printer/objects/list` and documented object queries to discover available state, rather than assuming every printer has the same heaters, tools or sensors.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- A commissioned printer with Moonraker already installed and any required API key or access token stored securely.
- For print operations, an explicitly selected file already available to the printer, plus confirmation that the build area and printer are ready. Do not substitute an arbitrary local file.

## Workflow

1. Read server/printer readiness and query relevant objects such as `print_stats`, `virtual_sdcard`, `pause_resume`, and the actual heater/tool objects present.
2. For status requests, report current job, progress, temperatures and error state from those objects. Handle disconnected or shutdown states as unavailable, not idle.
3. For an authorized job, use the documented HTTP request or equivalent JSON-RPC method for `/printer/print/start`, `/printer/print/pause`, `/printer/print/resume` or `/printer/print/cancel`. Provide the exact existing filename for start; URL-encode parameters correctly.
4. Monitor job state and temperatures after the action. Use an outbound WebSocket subscription if supported by the client, otherwise poll the documented object query endpoint.

## Verify the Result

- A successful API call is not proof that printing or pausing completed. Check `print_stats`, pause state and relevant errors.
- If a start request is uncertain, inspect the current job before resubmission; do not restart a running print.

## Limits

- No new server/firmware installation, arbitrary G-code, configuration changes or bypassing thermal/motion protections.
- Canceling a job or moving hardware is a physical action and requires the user's request. Do not treat a generic API key as permission for all administrative endpoints.

## Sources

- [Moonraker external API](https://moonraker.readthedocs.io/en/latest/external_api/introduction/)
- [Moonraker printer API](https://moonraker.readthedocs.io/en/latest/external_api/printer/)
