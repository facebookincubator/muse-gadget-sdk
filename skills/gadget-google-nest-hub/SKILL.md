---
name: gadget-google-nest-hub
description: >-
  Play supported media and control playback or volume on Google Nest Hub through Muse Home
  Link. Use the shared Google Cast skill with generation-specific capabilities; excludes Nest
  Hub Max and general screen or sleep-sensing access.
---

# Google Nest Hub

Use this skill when current discovery identifies the Google Nest Hub.

## Identify the Device

- Confirm the Nest Hub generation/model and firmware from available metadata. Do not conflate it with Nest Hub Max.
- Match the current receiver to the user's intended device; use its current service endpoint, not a remembered address.
- Use only media/display capabilities supported by that generation and its current receiver application. A display does not mean every video codec, app or URL is playable.

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

- No camera, sleep-sensing, general screen/UI control, private setup endpoints or assistant/cloud configuration.
- Do not infer device administration or general remote-control capabilities from Cast support.

## Sources

- [Shared Google Cast skill and protocol sources](../gadget-google-cast/SKILL.md)
