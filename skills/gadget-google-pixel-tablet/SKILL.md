---
name: gadget-google-pixel-tablet
description: >-
  Play supported Cast media and control playback or volume on Google Pixel Tablet through Muse
  Home Link. Use the shared Google Cast skill only while the tablet exposes its receiver in
  supported docked, locked Hub Mode.
---

# Google Pixel Tablet

Use this skill when current discovery identifies the Google Pixel Tablet with its Cast receiver currently available.

## Identify the Device

- Confirm Pixel Tablet model metadata and a currently advertised receiver. The tablet being on Wi-Fi is not sufficient.
- Match the current receiver to the user's intended device; use its current service endpoint, not a remembered address.
- The tablet must be in its supported docked, locked Hub Mode with Cast reception enabled. Check current availability; do not assume undocked or unlocked operation.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- The device is already provisioned and available as a receiver in its supported operating state.
- For new playback, an existing user-authorized media source must be reachable by the receiver. Device access alone does not supply media hosting.

## Workflow

1. Use the [Google Cast skill](../gadget-google-cast/SKILL.md) for discovery interpretation, receiver/media status, playback controls, volume, media loading and verification. That skill is the single protocol procedure.
2. Apply this profile's model and operating-state restrictions before selecting an operation. Missing media status while idle is not proof that the device is unreachable.
3. For an existing group, follow the shared skill's separate group-target rules and confirm that the user intended all group members.

## Verify the Result

- Follow the shared skill's operation-specific checks and report whether the result was actually observed on this device.

## Limits

- No arbitrary undocked casting, battery/screen inspection, screenshots, general tablet automation, private setup routes or device configuration.
- Do not infer device administration or general remote-control capabilities from Cast support.

## Sources

- [Shared Google Cast skill and protocol sources](../gadget-google-cast/SKILL.md)
