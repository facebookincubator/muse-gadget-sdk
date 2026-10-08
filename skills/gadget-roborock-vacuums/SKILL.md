---
name: gadget-roborock-vacuums
description: >-
  Read status and send supported cleaning, pause, or dock commands to compatible Roborock
  vacuums through Muse Home Link. Use python-roborock's encrypted local TCP channel with
  existing credentials; not Mi Home UDP miIO or A01 wet/dry devices.
---

# Roborock Vacuums

Use this skill when the confirmed Roborock-app-compatible vacuum supports the local channel described by python-roborock and its existing local credentials are available.

## Identify the Device

- Confirm model, firmware, device UID and local protocol compatibility; do not select this skill solely from a Roborock vendor label.
- Use the current local endpoint. The community client's local channel uses TCP 58867; this is different from the UDP miIO path used by some Mi Home devices.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Existing local key and device identity supplied securely, plus the model information needed by the compatible client.
- A local-only python-roborock channel or equivalent encrypted implementation routed through HomeLink. Disable or omit cloud bootstrap/fallback paths.
- Permission for the requested robot movement and a safe, prepared cleaning area.

## Workflow

1. Initialize the documented local channel with the current host, local key and device UID. Preserve protocol negotiation, message framing/encryption, sequence correlation and keepalive behavior.
2. Read the model's supported status, battery, current mission and errors. Use its vacuum command model, not an unrelated A01 wet/dry appliance protocol.
3. For a requested operation, send the documented local cleaning, pause or return-to-dock command supported by this model.
4. Correlate the response and poll/subscribe to fresh state as supported. If local communication fails, report it rather than quietly switching to a cloud channel.

## Verify the Result

- Check that status reflects the requested mission; acknowledgment alone does not prove movement or completion.
- Returning to dock is not the same as docked/charging. Report faults or blocked movement without automatic retries.
- Inspect state before repeating a command after an uncertain response.

## Limits

- No cloud account/bootstrap, map retrieval, room-map setup or cloud-dependent station functions in this skill.
- Mi Home UDP miIO and A01 wet/dry devices are not interchangeable with this vacuum path.
- Do not assume every Roborock generation supports the same local protocol.

## Sources

- [python-roborock device support](https://github.com/Python-roborock/python-roborock)
- [Roborock local TCP channel](https://github.com/Python-roborock/python-roborock/blob/HEAD/roborock/devices/transport/local_channel.py)
