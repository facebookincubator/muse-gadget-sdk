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

#pragma once

#include <stdbool.h>

/*
 * Who may change the image over Bluetooth (protocols/mgcommands.h, Token proof; test-vectors/mg-update-v2.json;
 * mg_ble.c smp_may_change, pure so the host tests run it). Reads stay open.
 * - No press: bench, automation and test builds (release false), this connection's matched token proof (proven,
 *   normal mode: the apps; never owner_ok, which is true on a gadget that isn't set up), a SAFE the user entered with
 *   the key (button_safe).
 * - A SAFE the gadget entered itself has no way to prompt (its screen is off): rc 11 (CLOSED).
 * - Anything else (an unproven link, set up or not): ONE confirm press on the gadget (muse_local_press_ask through the
 *   request front, mg_smp_front.h, which asks about each change whole; the wired console's arm counts as it): rc 10
 *   while it asks, rc 11 for what can't be asked or isn't the bound image's, and once its prompt timed out or was
 *   cancelled.
 */
typedef enum {
    MG_SMP_GATE_OPEN = 0,
    MG_SMP_GATE_NEEDS_PRESS,   /* ask for the confirm press */
    MG_SMP_GATE_CLOSED,        /* a self-entered SAFE: no way to prompt, rc 11 */
} mg_smp_gate_t;

static inline mg_smp_gate_t mg_smp_gate(bool safe, bool button_safe, bool proven, bool release)
{
    if (!release || (proven && !safe) || (safe && button_safe)) {
        return MG_SMP_GATE_OPEN;
    }
    return safe ? MG_SMP_GATE_CLOSED : MG_SMP_GATE_NEEDS_PRESS;
}
