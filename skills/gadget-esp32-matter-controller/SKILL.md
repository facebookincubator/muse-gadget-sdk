---
name: gadget-esp32-matter-controller
description: >-
  Add and control Matter devices (Wi-Fi, and Thread through the home's border router) with the
  matter.* commands of an ESP32 gadget whose firmware runs a Matter controller. Use when the
  gadget registered matter.commission and the user wants to add, find, control, read, share or
  remove a Matter device.
---

# ESP32 Matter Controller

Use this skill when the gadget registered `matter.commission`. The gadget is a Matter
controller with a fabric of its own; its devices are listed by `matter.nodes`. Matter traffic
(UDP, IPv6, multicast discovery) stays on the gadget.

## Prerequisites

Follow the Home Link safety rules in `~/docs/devices/home_link.md`; its networking rules don't
apply, since Matter traffic stays on the gadget.

- **The gadget adds devices that are on the network and in pairing mode.** It has no
  Bluetooth commissioning.
- **A device already in another app** is the usual case:
  - the app: Apple Home, Google Home, Alexa, SmartThings, etc.;
  - ask the user to share it from that app ("add to another app", "share with other
    services"), then use the setup code it shows;
  - Thread devices are added this way: the gadget has no Thread radio and reaches them through
    the home's border router.
- **A new device** (not set up anywhere yet) is first set up with its own app or another
  ecosystem's, then shared to the gadget the same way.
- **A device already on the home network that opens its own pairing window** (a bridge, an
  Ethernet device, or a Thread device made visible by the home's border router) can be added
  with the setup code on its label.

## Workflow

1. **Find what is waiting** (optional): `matter.commissionables` lists the devices in pairing
   mode on the network (`via: "network"`). Each has `vendor_id`, `product_id`, `discriminator`,
   `name` when it gives one, and `commissioning_mode`: 1 means its own window (the label's
   setup code works), 2 a window opened by another app (use that app's setup code). Use it to
   confirm the device is reachable, or to tell the user which device a setup code belongs to.
   - It lists only devices waiting to be added. For everything else on the network, use
     `device.discover` where the gadget offers it.
2. **Add it:** `matter.commission` with `code`, the setup code (QR text `MT:...` or the
   11/21-digit manual code), and a short `label`. It answers with `node_id` within a minute or
   two.
3. **Find what the device offers** (step 5), then control it with `matter.invoke` (`node_id`,
   `endpoint`, `cluster`, `command`, optional `fields`, `timed_ms`), `matter.read` and
   `matter.write`.
4. **Remove it:** `matter.remove`. The device leaves this gadget's fabric; other apps keep it.
   Use `forget: true` only if `matter.remove` fails because the device was reset or is gone: it
   drops the device from the list without contacting it.
5. **To find IDs, read from the device itself:**
   - endpoint 0, cluster 29 (Descriptor), attribute 3 (PartsList): the endpoints;
   - each endpoint's cluster 29, attribute 1 (ServerList): its clusters;
   - each cluster's attributes 65529 (AcceptedCommandList), 65531 (AttributeList) and 65532
     (FeatureMap).

   Read one endpoint or cluster at a time: a read returns at most 48 values (`truncated`).

The first `matter.*` command after the gadget starts takes a couple of seconds longer while
its controller starts. Meanwhile, other commands answer `busy`: retry.

## When the User Asks

- **Let another app add the device too:** `matter.open_window` returns a manual setup code to
  enter there within `timeout_s` (default 300).

## Values

Command fields and written values use `esp-matter`'s JSON notation: each key is `TAG:TYPE`,
where TAG is the field's ID from the cluster definition and TYPE is one of `U8 U16 U32 U64`,
`I8 I16 I32 I64`, `BOOL`, `FP` (float), `DFP` (double), `STR`, `BYT` (base64), `OBJ` (a
struct), `ARR-<TYPE>` (a list) or `NULL`.

64-bit values (`U64`, `I64`: event numbers, some IDs): send any beyond 2^53 as a decimal
string, e.g. `{"0:U64": "18446744073709551615"}`; results give any value above 4294967295 as a
string.

- Invoke `fields`: `{"0:U8": 128, "1:U16": 10, "2:U8": 0, "3:U8": 0}` (LevelControl
  MoveToLevelWithOnOff: level, transition in tenths of a second, options mask, override).
- Write `value`, one item keyed `0`: `{"0:U8": 2}`, `{"0:NULL": null}`, a list
  `{"0:ARR-OBJ": [{"1:U64": 1, "3:U16": 1, "4:U32": 6}]}`.
- Read results come back as plain JSON: numbers, booleans, strings and lists as they are,
  structs keyed `TAG:TYPE`, octet strings base64.

Common IDs:

| Cluster | Attributes | Commands |
|---|---|---|
| OnOff (6) | 0 OnOff (BOOL), 0x4003 StartUpOnOff (U8, nullable) | 0 Off, 1 On, 2 Toggle |
| LevelControl (8) | 0 CurrentLevel (U8, 1-254) | 4 MoveToLevelWithOnOff `{"0:U8","1:U16","2:U8","3:U8"}` |
| ColorControl (768) | 7 ColorTemperatureMireds (U16) | 10 MoveToColorTemperature `{"0:U16","1:U16","2:U8","3:U8"}` |
| DoorLock (257) | 0 LockState (U8: 1 locked, 2 unlocked) | 0 Lock, 1 Unlock (with `timed_ms`, e.g. 10000) |
| WindowCovering (258) | 14 CurrentPositionLiftPercent100ths (U16) | 0 UpOrOpen, 1 DownOrClose, 2 Stop, 5 GoToLiftPercentage `{"0:U16"}` |
| Thermostat (513) | 0 LocalTemperature (I16, 0.01 °C), 18 OccupiedHeatingSetpoint | 0 SetpointRaiseLower `{"0:U8","1:I8"}` |
| TemperatureMeasurement (1026) | 0 MeasuredValue (I16, 0.01 °C) | — |
| OccupancySensing (1030) | 0 Occupancy (U8 bitmap) | — |
| BasicInformation (40, endpoint 0) | 1 VendorName, 3 ProductName, 5 NodeLabel, 10 SoftwareVersionString | — |

For any other cluster, take the IDs, field tags and types from its definition in
[controller-clusters.matter](https://github.com/project-chip/connectedhomeip/blob/d46cc8c2886cbefc338544bdb2e2f8128f3e9970/src/controller/data_model/controller-clusters.matter)
(the Matter version the firmware is built with): read only that cluster's block. Map its types
to the notation above: `int8u` is `U8`, `int16s` is `I16`, `boolean` is `BOOL`, `char_string`
is `STR`, `octet_string` is `BYT`, an enum or bitmap of 8 bits is `U8`, `percent100ths` is
`U16`, `temperature` is `I16`, a struct is `OBJ`.

## Verify the Result

- After `matter.invoke` or `matter.write`, read the attribute back. A device that answered is
  reachable; the physical state may still need the user to confirm.
- Errors name what went wrong (the message says more):
  - `invalid_param`: a parameter is missing or malformed, including a setup code that doesn't
    check out and fields or values that don't fit the notation;
  - `discover_failed`: the gadget couldn't browse the network; retry;
  - `busy`: one Matter operation runs at a time (or the controller is starting); retry shortly;
  - `unknown_node`: not in `matter.nodes`;
  - `too_many_devices`: the gadget's list is full; remove a device first;
  - `unreachable`: the device didn't answer (off, out of range, or a Thread device whose border
    router isn't reachable);
  - `attestation_failed`: the device isn't a certified Matter device, or its manufacturer is
    newer than the gadget's list; it was not added;
  - `commissioning_failed`: the device wasn't found on the network, refused the setup code, or
    failed a later stage (named in the message). Check that it is in pairing mode (see
    `matter.commissionables`) and the setup code is the one its app shows now.
  - `command_failed`, `read_failed`, `write_failed`, `remove_failed`, `window_failed`: the
    device refused, or didn't answer, that request;
  - `timeout`: no answer in time, often an unreachable device; check it, then read its state
    before retrying;
  - `matter_unavailable` or `storage`: the gadget's controller isn't running, or couldn't save;
    tell the user.

## Limits

- One operation at a time. No subscriptions or events: poll with `matter.read`.
- No Bluetooth: a device has to be on the network, in a pairing window, to be added.
- About 16 devices on boards without PSRAM (the gadget's list is fixed in its firmware). Node
  IDs are never reused.
- The gadget holds this fabric's keys. Resetting or unpairing the gadget forgets the fabric and
  every device on it; those devices then keep a stale entry until they are reset.

## Sources

- [`esp-matter`](https://github.com/espressif/esp-matter): the controller in the gadget's
  firmware, and its [controller guide](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/controller.html)
  (the `TAG:TYPE` notation, as its console commands use it)
- [Matter cluster definitions](https://github.com/project-chip/connectedhomeip/blob/d46cc8c2886cbefc338544bdb2e2f8128f3e9970/src/controller/data_model/controller-clusters.matter)
- [Matter Application Cluster Specification](https://csa-iot.org/developer-resource/specifications-download-request/)
