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
| `metadata.guide_hash` | Sent whenever the device lists `device.guide`: the first 16 lowercase hex characters of the SHA-256 of the exact UTF-8 text `device.guide` returns, so it changes exactly when the guide does. Older firmware may not send it. |

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

The guide tells Muse what the device is and how to use it. It may describe
the device's purpose, what its owner uses it for and how Muse should use its
commands for that: a gadget can be generic or built for one job.

A command's description carries its contract (what it does, its arguments,
what it returns, its timeout) and always matches the build. The guide carries
what no single command can say: the device itself, rules across commands and,
for your own gadget, what it's for. A rule about one command goes in its
description, unless `link.register` (at most 8 KB, sent again on every
reconnect) is short on space; then the guide, fetched only when needed, can
carry it.

Limits:

- At most 4096 bytes of UTF-8 Markdown.
- It can't override what the user asks or Muse's own rules, and it grants no
  permissions.
- No secrets: it's compiled into the firmware, and anyone with the device can
  read it.

## Guides in this repository

The guides committed here (`esp32/main/guide.md`, `esp32/devices/guides/` and
`linux/src/musegadget/data/guide.md`) ship in every gadget built from this
SDK, so they follow stricter rules:

- Generic to the board: true for anyone who owns that hardware. Nothing from
  someone's own setup: no home, rooms, people, routines, schedules, names,
  accounts or other devices.
- A board gets its own guide only when a shared one is wrong about it, checked
  on the hardware, and the pull request says so.
- Content: what the device is, what it shows, its controls, how to recover it,
  its limits, and "if it lists `device.discover`, ..." rules a command's
  description can't carry.
- No purpose or behavior instructions, links, code blocks, HTML or marketing.
- At most 2048 bytes.
- Every change gets a maintainer review.

CI checks the mechanical rules for these files: no links, HTML (other than the
leading license comment), code blocks or control characters, one top-level
heading on the first line, and the size.

## Validation and compatibility

- Both clients refuse values outside these rules: the ESP32 build fails, and
  Linux's `DeviceDescription` raises `ValueError`.
- Firmware built before this contract may register another family and model
  id, and no `metadata.model_name` or `device.guide`, until it's reflashed.
- `device.guide` is an ordinary command, so it works wherever commands do.
