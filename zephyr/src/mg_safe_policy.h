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

/* SAFE frees the BLE recovery tier from a silent holder: with one connection
 * slot, a central that holds it in SAFE and writes nothing (a phone's silent auto-reconnect) is dropped
 * after MG_SAFE_IDLE_MS without a write of its own (an mg, Nordic UART or SMP
 * write; the link's start counts as one), never while SMP is handling a
 * request or holds an upload's writer, so the gadget advertises again for
 * whoever came to rescue it. A central that keeps writing keeps the slot.
 * Host test_safe_idle. */

#ifndef MG_SAFE_POLICY_H
#define MG_SAFE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define MG_SAFE_IDLE_MS 60000u

static inline bool mg_safe_idle_drop(bool safe, bool link_up, uint32_t now_ms, uint32_t last_write_ms,
				     bool smp_busy)
{
	return safe && link_up && !smp_busy && (uint32_t)(now_ms - last_write_ms) >= MG_SAFE_IDLE_MS;
}

#endif /* MG_SAFE_POLICY_H */
