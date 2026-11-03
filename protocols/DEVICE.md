<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# How a gadget describes itself to Muse

A gadget tells Muse what it is in `link.register`, and how to use it through
the `device.guide` command. This is the contract for both. The ESP32 firmware
(`../esp32`) and the Linux client (`../linux`) follow it, and so should a
client of your own.

| Where | How to set it |
|---|---|
| ESP32 | `CONFIG_MUSE_DEVICE_MODEL_ID`, `CONFIG_MUSE_DEVICE_MODEL_NAME` and `CONFIG_MUSE_DEVICE_GUIDE` in the board's overlay (`../esp32/AGENTS.md`, Describe your device) |
| Linux | `model_id`, `model_name` and `guide` of `DeviceDescription` (`../linux/AGENTS.md`, Building a gadget) |

## Identity in `link.register`

Beside the node id, display name, version and commands, `params` carry:

| Field | Value |
|---|---|
| `device_family` | `"gadget"`, always. Every device built with this SDK registers it; other families are reserved for Muse's own devices. |
| `model_id` | The hardware model: 1 to 64 lowercase letters, digits and hyphens (`[a-z0-9-]{1,64}`). |
| `metadata.model_name` | The hardware's name as people know it, such as `Seeed SenseCAP Watcher`: 1 to 128 bytes of UTF-8 with no control characters (U+0000 to U+001F, U+007F to U+009F). |

`model_id`:

- Names the hardware, not the build. Every firmware profile for the same
  board sends the same id, such as a full-UI build and a status-light build.
- Is stable: Muse may rely on it, so a board keeps its id. Hardware that works
  differently, such as a new revision with other controls, gets a new one.
- Ids that start with `muse-` or `esp-link` are reserved for Muse and for the
  boards in this SDK, which use `muse-gadget-<board>`. Use a prefix of your
  own for your hardware, such as `acme-robot`.
- A device that doesn't set one reports the SDK's default: `muse-gadget`
  (`Muse Gadget`) on ESP32, `muse-gadget-linux` (`Linux gadget`) on Linux.

## `device.guide`

An ordinary command, listed in `commands_v2` like any other. It takes no
parameters, and its result's `payload` is:

```json
{"format": "markdown", "guide": "# Acme Lamp\n\nThe lamp on the desk ..."}
```

`guide` is at most 4096 bytes of UTF-8 Markdown, written for Muse. What
belongs in it:

- What the device is, in a sentence or two.
- Its controls and what it shows: buttons, lights, screen, switches.
- How to recover it: what each error state means and how to reset it.
- Rules a command's description can't carry, phrased so they hold on every
  build: "if it lists `device.discover`, a scan takes up to 90 seconds".

What doesn't: what each command does (its description says that), notes for
people building the firmware, instructions to Muse beyond how to use the
device, and anything meant to override what the user asks for.

## Validation and compatibility

- Both clients refuse values outside these rules: the ESP32 build fails, and
  Linux's `DeviceDescription` raises `ValueError`.
- Firmware built before this contract may register another family and model
  id, and no `metadata.model_name` or `device.guide`, until it's reflashed.
- `device.guide` is an ordinary command, so it works wherever commands do.
