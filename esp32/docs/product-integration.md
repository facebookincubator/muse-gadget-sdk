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

# Product integration

An enclosing ESP-IDF project can add private control commands without copying
SDK application code. Set these **before** including
`esp32/cmake/project.cmake` and calling `project()`:

```cmake
set(PRODUCT_CONTROL_COMPONENT my_product_control)
set(PRODUCT_CONTROL_EXTENSION_HEADER product_control_extension.h)
set(PRODUCT_CONTROL_EXTENSION_IMPL product_control_extension.inc)
include(path/to/sdk/esp32/cmake/project.cmake)
project(my_product)
```

The SDK exports the selection as IDF build properties, including to IDF's
separate early requirements expansion. All three values must be set together,
or all absent. The component must be one external component name (not `main`);
it exports the include files through `INCLUDE_DIRS`. Neither file is discovered
implicitly. A command-only component can use
`idf_component_register(INCLUDE_DIRS "integration")` without any source files.
Its implementation is compiled once, as part of the shared control session.

## Product control commands

The header is included after SDK headers and before command builders. Declare a
registration function and define `PRODUCT_CONTROL_REGISTER_COMMANDS` to it:

```cpp
static void product_register_commands(cJSON *commands);
#define PRODUCT_CONTROL_REGISTER_COMMANDS product_register_commands
```

The implementation is included after `queue_result` and the JSON builders. It
can call `add_command`, `string_param`, and `queue_result`. Define
`PRODUCT_CONTROL_INVOKE` to a function of this shape:

```cpp
static bool product_invoke(const char *command, cJSON *params,
                          noise_ctrl_session_generation_t generation,
                          const char *request_id);
#define PRODUCT_CONTROL_INVOKE product_invoke
```

Return false for unrelated commands so normal SDK dispatch continues. Return
true only when the hook owns the reply. Input pointers are borrowed until this
function returns: asynchronous work must copy everything it keeps, including
the request ID, and retain the original generation. `queue_result` owns the
result tree; the shared sender drops results from stale sessions. Do not block
the session task, duplicate core command names, or exceed the 8 KiB registration
limit. The hook does not add authentication or serialize asynchronous work by
itself; those remain the command implementation's responsibility.

The SDK still compiles its existing shared `main/ota.c`; the extension mechanism
does not replace it or introduce another updater. Ordinary public SDK builds do
not advertise or dispatch remote `device.ota`. Adding a control extension does
not change public partitions, OTA defaults, BLE protocol or setup policy.

## Transport compatibility

Control, tunnel (including in-place batches/resets), and agent identity use
`ServiceType::Gadget`: `/api/gadget/control`, `/api/gadget/tunnel`, and
`/api/gadget/me` (`result.muse_name`). There is no legacy daemon-route fallback.
A refused control stream reconnects normally; a refused identity fetch only
leaves the name unknown. Extra `noise_ctrl_req_*` requests still target
`ServiceType::Daemon`. The `musegadgets::noise` namespace is unchanged.

## Focused host checks

`test_product_hooks.py` checks complete control-component selection, the
single shared updater source, and public/product registration and dispatch.
`test_gadget_transport.py` runs the real session and codec on the three
routes. The existing tunnel, daemon-request, pairing and OTA suites cover their
separate traffic and behavior. `test_minimal_profile.py` covers console
selection and service-frame bounds with daemon requests on and off. New compiled
harnesses use ASan/UBSan by default (`HOST_SANITIZERS` can explicitly select or
disable them).
