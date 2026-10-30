---
name: gadget-google-nest-hub-max
description: >-
  Play supported media and control playback or volume on Google Nest Hub Max through Muse Home
  Link. Use its available receiver and the shared Google Cast skill; this does not expose the
  camera, microphone, or security recordings.
---

# Google Nest Hub Max

Use this skill when current discovery identifies the Google Nest Hub Max.

## Identify the Device

- Match Nest Hub Max metadata rather than assuming any Nest display is the same model.
- Match the current receiver to the user's intended device; use its current service endpoint, not a remembered address.
- Display media is limited by the active receiver/application's supported formats and commands. The built-in camera is not exposed by this media interface.

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

- No camera streams, security recordings, microphone access, private setup-token operations or assistant/cloud features.
- Do not infer device administration or general remote-control capabilities from Cast support.

## Sources

- [Shared Google Cast skill and protocol sources](../gadget-google-cast/SKILL.md)
