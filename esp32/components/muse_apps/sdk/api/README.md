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

# The apps API, version 1.1 (draft)

`api.json` defines the API WebAssembly apps get on a gadget: every function
with its arguments and WAMR signature, the events, the constants, the LVGL
widgets and properties apps may use, the sensor kinds, the limits, and the
manifest. It doesn't list a gadget's sensors or other hardware: the gadget
describes those itself (below). Everything else here is generated from it:

| File | What it is |
|---|---|
| `../gadget.h` | The header apps include (in `apps/`, beside `build.py`) |
| `REFERENCE.md` | The reference, for people and for Muse |
| `../../muse_apps_api.h` | The firmware's tables: imports with their signatures and permissions, widgets, properties, well-known devices |
| `api.capabilities.json` | `api.json` without the prose (about 12 KB), for `app.api` to serve |
| `../examples/dashboard.c` | An app written against it (hand-written) |

After changing `api.json`, run `python3 components/muse_apps/sdk/api/gen_api.py`, which checks it
and rewrites the generated files, and `api.json` itself in its canonical
layout. `tests/test_app_api.py` fails if they're out of date, and builds the
example to check that the header's imports match the signatures. Then run
`python3 components/muse_apps/sdk/muse_brief.py`: `components/muse_apps/sdk/MUSE.md`, the guide Muse gets, carries
`gadget.h` whole.

The firmware serves 1.1 (`components/muse_apps`, built with `MUSE_APPS=1`;
[component instructions](../../AGENTS.md)). It replaced API version 0,
whose immediate-mode canvas and raw VM requests are gone.

## Changing the API

- Ids and constant values are for good: never reuse or renumber one. A removed
  property or function keeps its number, unused.
- Adding a function, widget, property, constant, event or sensor kind is a
  minor version (1.1). Removing or changing one is a major version.
- A widget property is an entry in `props`, not a new function: `ui_set`,
  `ui_get`, `ui_set_text` and `ui_get_text` take them all. Add a function only
  for what a property can't carry (arrays, images, a chart's series).
- Keep to LVGL's names and values where they fit (`ALIGN_`, `DIR_`), so
  LVGL's documentation still helps Muse.
- A new sensor or other device needs no change here. Add a well-known name
  only once several boards have that kind of device and apps should run on
  all of them.

## How the gadget describes its hardware

`api.json` fixes how apps talk to devices, not which devices there are.
Each driver in the firmware registers its device with a description, and the
gadget reports what's registered: to Muse in `device.guide`, and to apps through
`dev_count`, `dev_name`, `dev_find` and `dev_describe`. A description is
JSON (Devices in `REFERENCE.md`):

```json
{"name": "soil_moisture", "desc": "Capacitive soil moisture probe on the Grove port",
 "fields": [{"name": "moisture", "unit": "%", "min": 0, "max": 100}],
 "rate": 1, "actions": [], "events": []}
```

- **Readings** are up to 8 floats in the order of `fields`, with a time:
  `dev_read` gives the latest; `dev_open` streams them into a queue for
  `dev_take`, at up to `rate` Hz.
- **Actions** take up to 8 float parameters and optional text: a relay's
  `set(on)`, a servo's `move(degrees, ms)`, an e-ink add-on's `show(text)`.
  `dev_action` finds one by name, `dev_call` runs it.
- **Events** are things a device reports by itself (`tap`, `door_open`),
  after `dev_open`, as `EV_DEV_EVENT`.
- **Well-known names** (`temperature`, `humidity`, `accel`, `battery`,
  `vibration`...) are listed in `api.json` with fixed fields, units, actions
  and events, so an app for one runs on any gadget that has it. A driver that
  uses one must match it, which the firmware can check against
  `muse_apps_api.h`.
- **Permissions:** an app names the devices it needs (`requires`) or can use
  (`uses`) in its manifest; it can't find any other. The gadget refuses an app
  whose `requires` names a device it lacks.

In the firmware (`components/capabilities_registry`, `capabilities_registry.h`), a driver fills in a
descriptor and its callbacks:

```c
static const capability_field_t s_fields[] = {{"moisture", "%", 0, 100}};
static int soil_read(const capability_t *dev, float *v) { v[0] = s_latest; return 1; }
static const capability_t s_soil = {
    .name = "soil_moisture", .desc = "Capacitive soil moisture probe on the Grove port",
    .fields = s_fields, .n_fields = 1, .rate = 1, .read = soil_read,
};
capabilities_registry_register(&s_soil);
```

Streaming devices also give `start(rate)` and `stop()`, push readings with
`capabilities_registry_push()` and events with `capabilities_registry_event()`; devices with actions
give `call(action, params, text)`, which mustn't block. Muse's own
`sensors.read` command would read the same registry, so Muse and apps see
one set of devices. Buttons, the knob, touch and the screen stay in the core
API (`BUTTON_`, `EV_KNOB`, `sys_info`, `sys_brightness`): every app needs
them.

Calls to the VM go through typed functions (`chat_`, `file_read`, `state_`)
rather than raw routes: the gadget owns the VM's formats, its routes and the
frame limits, so apps don't break when those change.
