---
name: gadget-google-nest-audio
description: >-
  Play audio and control playback or volume on Google Nest Audio through Muse Home Link. Use
  the shared Google Cast skill for an available receiver or existing group; not for creating
  stereo pairs or reconfiguring groups.
---

# Google Nest Audio

Use this skill when current discovery identifies the Google Nest Audio.

## Identify the Device

- Confirm Nest Audio model metadata, current receiver ID and whether the requested target is one speaker or an already-configured group.
- Match the current receiver to the user's intended device; use its current service endpoint, not a remembered address.
- This is an audio-only receiver. Pair/group topology must be observed rather than inferred from friendly names.

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

- No stereo-pair creation, group reconfiguration, private setup-token operations, microphone access or assistant/cloud routines.
- Do not infer device administration or general remote-control capabilities from Cast support.

## Sources

- [Shared Google Cast skill and protocol sources](../gadget-google-cast/SKILL.md)
