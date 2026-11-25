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

# How a gadget tells Muse that it crashed

A gadget that crashed says so, once, the next time it registers with its Muse:
one small `link.error_report` message on the control stream. This is the
contract for it. The ESP32 firmware (`../esp32`) and the Linux client
(`../linux`) follow it, and so should a client of your own.

| Where | What sends it | How to turn it off |
|---|---|---|
| ESP32 | `main/error_report.c`, after a panic, watchdog, brownout, power-glitch, CPU-lockup or eFuse reset | `CONFIG_HOMEHUB_ERROR_REPORT=n` |
| Linux | `musegadget/error_report.py`, after `musegadget run` ended in an exception or was killed | `MUSEGADGET_ERROR_REPORT=0` in the service's environment |

## The message

```json
{"method":"link.error_report","report":{"schema_version":1,"kind":"crash","reason":"panic","consecutive_count":1,"version":"1.0.9","build_sha":"a1b2c3d4e5f60718","uptime_seconds":5321,"fault":{"address":"0x42012345","return_address":"0x4200a1b2","cause":"Load access fault"}}}
```

It goes on the control stream like `link.register`, as one length-prefixed JSON
message, after Muse has accepted the registration. It has no `id` and gets no
reply.

| Field | Value |
|---|---|
| `schema_version` | `1`. |
| `kind` | `"crash"`. |
| `reason` | How the previous run ended, from the table below. |
| `consecutive_count` | How many runs in a row ended badly, this one included: 1 or more. A run that ends normally resets it. |
| `version` | Optional. The version that crashed, which is not always the one that registered: a bootloader can roll back to an older build. 1 to 31 printable ASCII characters. |
| `build_sha` | Optional. Which binary crashed, for builds that share a version: 8 to 64 lowercase hex digits. The ESP32 sends the first 16 hex digits of its ELF file's SHA-256, which `get_device_info` reports under the same name and ESP-IDF logs at boot as `ELF file SHA256`. Linux sends none. |
| `uptime_seconds` | Optional. How long the crashed run had been up, in whole seconds. |
| `fault` | Optional. What is known about the fault itself, each part optional. |
| `fault.address` | The instruction that faulted: `0x` and eight hex digits. |
| `fault.return_address` | The return-address register at the fault, in the same form: the caller when the faulting function calls nothing itself, otherwise a point in that function after its last call. It is one address, not a backtrace. The ESP32 firmware sends it on RISC-V chips only. |
| `fault.cause` | The fault's name, such as `Load access fault`, `abort` or `ValueError`: 1 to 47 printable ASCII characters. |
| `fault.detail` | Where or why, from the program's own text: an assertion's message, or `file.py:372 in function`. 1 to 95 printable ASCII characters. |

Printable ASCII is space to `~`. The whole message stays under 512 bytes.

| `reason` | Sent by | Meaning |
|---|---|---|
| `panic` | ESP32 | An exception, a failed assertion or an `abort()`. |
| `interrupt_watchdog`, `task_watchdog`, `watchdog` | ESP32 | A watchdog reset the chip. |
| `brownout`, `power_glitch` | ESP32 | The supply voltage fell too low or glitched. |
| `cpu_lockup`, `efuse` | ESP32 | The chip reset itself after a double exception or an eFuse error. |
| `unhandled_exception` | Linux | An exception ended the service. |
| `unclean_exit` | Linux | The service vanished while the machine stayed up: killed, or out of memory. |

Losing power, a reset button, a restart the gadget asked for (an update,
unpairing) and waking from deep sleep are not crashes, and send nothing.

## What it must never carry

The report is sent without asking the owner each time, so it is limited to
what describes the program and not the person or the place:

- No logs, and no text that came from outside the program: an exception's
  message, a path, a URL, a command's parameters or output.
- No network names or addresses, and no identifier of the device, its owner
  or its Muse. Muse already knows which device the stream belongs to.
- `fault.cause` and `fault.detail` come only from the program's own source:
  an exception's type, an assertion's text, a file name, a line, a function.

For anything more, Muse asks the owner first.

## Sending it

- Send it at most once per crash, and only after the reply to `link.register`
  says the device is registered.
- Keep it until a send completes. A connection that drops first leaves it for
  the next one.
- If the gadget crashes again before it could send, the newer crash replaces
  the older one, and `consecutive_count` says there was more than one unless a
  run in between ended normally.
- Muse records at most one report per device per minute. A Muse that doesn't
  know the message ignores it, so a gadget needs no version check.
- The ESP32 keeps its record in memory that survives a reset and nowhere
  else: nothing is written to flash, and losing power or a factory reset
  discards it. Linux keeps it in `error_report.json` in the state directory.

## Compatibility

- Muse ignores fields it doesn't know and records a `reason` it doesn't know
  as `unknown`, so a new field or reason needs no new `schema_version`.
- Muse drops a report whose known fields break the rules above, rather than
  trimming it.
