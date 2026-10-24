// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "noise_control.h"

// Fixed-size RAM history. Producers must call from a task, not an ISR.
// GPIO and value describe hardware only; no actions are bound to events.
void reterminal_e100x_events_button(int key, int gpio, bool pressed, uint32_t presses);
void reterminal_e100x_events_sd(bool inserted);
// E1003 only: one contact's down/up transition in panel coordinates.
void reterminal_e100x_events_touch(bool pressed, int x, int y);

// events.read returns immediately. events.wait completes asynchronously through
// the existing command-result channel, without introducing a wire protocol.
cJSON *reterminal_e100x_events_command(const char *command, const cJSON *params,
                            const char *request_id,
                            noise_ctrl_session_generation_t generation);
