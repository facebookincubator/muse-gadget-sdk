---
name: gadget-vesync-purifier
description: "Cloud only: needs internet access and the user's VeSync account; does not use Muse Home Link or the local network. Read and control a VeSync-connected Levoit air purifier through the VeSync cloud API — power, mode, fan level, filter life, and air quality where the model has a sensor. VeSync purifiers have no local API."
---

# VeSync Air Purifiers

Use this skill when the user asks to check or control a Levoit air purifier connected to their VeSync account — for example reading power state, mode, fan level, filter life, or air quality, or changing power, mode, fan speed, display, or child lock. This is a separate skill for purifiers only: never apply humidifier operations or humidity targets to a purifier.

## Identify the Device

Exact model identification is mandatory before offering any operation. VeSync purifier capabilities are model-fragmented: modes, fan levels, and sensors differ between models that share the VeSync and Levoit names, so never infer a capability from the VeSync or Levoit name alone.

1. Sign in to the VeSync cloud (see Prerequisites) and fetch the account device list. One device list covers all VeSync device classes.
2. Find the purifier the user means in that list (if more than one could match, ask which) and record its exact model from the device type code the list reports (for example, `LAP-C201S-AUSR` is a Core 200S).
3. Check that exact model's row in the Model Guard Table below before offering, reading, or changing anything. If the model is not in the table, use only the modes, fan levels, and sensors the client lists for that exact device type (pyvesync's per-model `modes`, `fan_levels` and `features`) — do not borrow another model's row.

## Cloud-Only Verdict

VeSync devices have no local API. This skill does not use Muse Home Link or the local network. Every read and every control call goes through the VeSync cloud API over the internet, using the user's VeSync account. No internet means no status and no control — say so plainly rather than implying a local fallback exists.

## Prerequisites

- The user's VeSync account email and password. Signing in produces a session token and an account ID, using the same VeSync cloud login as all other VeSync device classes.
- Credentials are sent only to the VeSync API, through the client — never to any other service.
- Sign in only with the user's own VeSync account. Account credentials and session tokens are personal — never use another person's account, and never share or publish your own credentials or token.
- The password and the session token live only in user-only approved storage. They are never pasted into chat and never logged.
- If the user's VeSync account was created with Google or Apple sign-in, it has no password until the user sets one in the VeSync app. If sign-in fails for that reason, give the user that step — set a password in the VeSync app first — instead of improvising around it.
- Do not hard-code a region. The region endpoint (US or EU) is corrected at login; use the endpoint the login flow returns.

## Model Guard Table

Check the row for the user's exact model before offering any operation. Only offer a mode, fan level, or reading that the row supports. Setting a fan level puts the purifier in manual mode.

| Model | Modes | Fan levels | Air-quality sensing | Notes |
|---|---|---|---|---|
| Levoit Core 200S | Manual and sleep ONLY — no auto mode | 1–3 | None — no air-quality or PM sensor | Reports filter-life percentage. Never report an air-quality value for this model. |
| Levoit Core 300S | Manual, sleep, and auto | 1–3 | Air quality level and PM2.5 | Reports filter-life percentage. |
| Levoit Core 400S, 600S | Manual, sleep, and auto | 1–4 | Air quality level and PM2.5 | Reports filter-life percentage. |
| Levoit Vital 100S, 200S | Manual, sleep, auto, and pet | 1–4 | Air quality level and PM2.5 | Reports filter-life percentage. |
| Levoit EverestAir | Manual, sleep, auto, and turbo | 1–3 | PM1, PM2.5, and PM10 sensing | Reports filter-life percentage. |

## Workflow

1. Identify the device: fetch the VeSync device list, confirm the exact model, and check its row in the Model Guard Table.
2. Read state first, before any write: power state, current mode, fan level, and filter-life percentage. Read air quality / PM values only where the model has a sensor per the guard table — the Core 200S has none, so never report an air-quality value for it, and never present a missing sensor reading as zero or as clean air.
3. Confirm the requested operation is allowed for that exact model: writes are power, mode, and fan speed where the guard table row for the exact model allows them, plus display on/off and child lock only where the device's state read reports them.
4. Get clear consent for the change itself. A state question ("is it running?", "what's the filter at?") is never permission to change anything. A rejected or cancelled prompt is not authorization — stop there.
5. Send the single requested change through the VeSync cloud API. If the device is already at the requested value, treat an already-set value as success and send nothing.
6. Never resend a command automatically. If a call fails or the result is unclear, report what happened and let the user decide the next step.

## Verify the Result

After any write, wait briefly — cloud state lags — then re-read the device state through the VeSync cloud and confirm the reported power state, mode, fan level, display, or child lock matches what was requested. Report the verified state, not just that a command was sent. If the read-back fails or comes back empty, report the change as unconfirmed rather than trusting any cached state the client shows. Background refreshes of device state (the app's, or another integration's polling) are separate from this read-back; only the read-back around the requested change verifies it. For a read-only request, report exactly what the cloud returned, including filter-life percentage, and report air quality only for models with a sensor per the guard table.

## Limits

- Cloud only: no internet means no status and no control. There is no local or Home Link path for these devices.
- The VeSync cloud API is unofficial and reverse-engineered. It can change or break without notice; if calls start failing, say so rather than guessing at a fix.
- No firmware updates, no device removal, no account changes, and no schedule or timer editing through this skill.
- Purifiers only: never apply humidifier operations or humidity targets to a purifier, and never apply purifier operations to another VeSync device class.
- Never offer a mode, fan level, or sensor reading the guard table does not support for the user's exact model.

## Sources

- [pyvesync on PyPI](https://pypi.org/project/pyvesync/) — the Python library documenting the VeSync cloud API, login flow, device list, and purifier controls this skill is built on. Checked against pyvesync 3.5.1, the version pinned by the Home Assistant VeSync integration (Home Assistant 2026.11.1), through which this skill's device validation was read.
- [VeSync official site](https://www.vesync.com) and [Levoit official site](https://levoit.com) — model identification, model families (Core, Vital, EverestAir), and manufacturer-stated capabilities for each purifier model.
