/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_ble_gate.h"

#include <string.h>

bool muse_ble_gate_is_provisioning(const char *cmd)
{
    static const char *const CMDS[] = { "wifi.ssid", "wifi.pass", "wifi.connect", "wifi.forget",
                                        "hatch.host", "hatch.vm", "hatch.token", "hatch.token+" };
    for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        if (!strcmp(cmd, CMDS[i])) {
            return true;
        }
    }
    return false;
}

bool muse_ble_gate_in_build(const char *cmd, bool test_build)
{
    return test_build || !muse_ble_gate_is_provisioning(cmd);
}

bool muse_ble_gate_link_ok(bool encrypted, bool authenticated, bool secure_connections)
{
    return encrypted && authenticated && secure_connections;
}

bool muse_ble_gate_allows(const char *cmd, bool set_up, int64_t now_us, int64_t window_until_us)
{
    if (!muse_ble_gate_is_provisioning(cmd)) {
        return true;   /* volume, brightness, tests: nothing about who owns the gadget */
    }
    return !set_up || (window_until_us > 0 && now_us < window_until_us);
}
