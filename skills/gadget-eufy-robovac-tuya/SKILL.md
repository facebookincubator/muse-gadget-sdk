---
name: gadget-eufy-robovac-tuya
description: >-
  Read status and send basic cleaning or docking commands to compatible Tuya-based eufy
  RoboVacs through Muse Home Link. Use with a confirmed model schema and existing local key;
  excludes non-Wi-Fi and AIOT/X10 models.
---

# eufy RoboVac — Compatible Tuya Models

Use this skill when the exact Wi-Fi RoboVac model and firmware are confirmed to use the documented local Tuya interface.

## Identify the Device

- Confirm the model code, local device ID, protocol and matching datapoint schema; do not use a broad generation table.
- This is not support for non-Wi-Fi models such as 11S or for X10/AIOT models.

## Prerequisites

Read and follow the Home Link networking and safety rules in `~/docs/devices/home_link.md`.

- Existing local key, device ID and a model-appropriate command/datapoint map supplied securely.
- A compatible local eufy/TinyTuya client using HomeLink's TCP transport, as described in the [Tuya skill](../gadget-tuya-wifi-devices/SKILL.md).
- A ready robot and user permission for the intended cleaning or docking action.

## Workflow

1. Initialize the local client with the confirmed model/protocol, current endpoint and supplied credentials. Read state before sending commands.
2. Decode cleaning mode, battery, error and docking state using the exact model's schema. Do not copy datapoint numbers from a different RoboVac.
3. For the user's requested action, use the documented clean/start, pause/stop or return-to-dock operation supported by that model.
4. Read new status and monitor the relevant mission transition. Keep unsupported map, room and station operations out of the command set.

## Verify the Result

- Confirm a reported mission/state change; a TCP or encrypted-packet acknowledgment alone is insufficient.
- Distinguish a command to dock from successful docking, and report error/battery states without overriding them.
- Read state before retrying an uncertain movement command.

## Limits

- No AIOT/X10 support, non-Wi-Fi model support, broad room/map/station promises or cloud key retrieval in the runtime path.
- No credential extraction through debugging, provisioning changes or re-pairing.
- The existing documented model match and credentials are mandatory; do not write guessed datapoints.

## Sources

- [eufy-robovac local client](https://github.com/apexad/eufy-robovac)
- [Community RoboVac protocol implementation](https://github.com/mitchellrj/eufy_robovac)
- [TinyTuya](https://github.com/jasonacox/tinytuya)
