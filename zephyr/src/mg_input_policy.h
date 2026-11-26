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

/* Real presses vs injected input: whether a press came from the button's
 * GPIO is set at the source (mg_button.c's input callback) and travels with
 * that event, never read from a shared "last input" variable. A Link setup
 * confirm takes any one press; what follows serves the update prompt: a
 * press counts only once it was queued after the prompt was armed. The
 * boot's holds (SAFE, reset) read the GPIO directly and have no injection
 * path. Host test_input_policy. */

#ifndef MG_INPUT_POLICY_H
#define MG_INPUT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Whether a button event came after the prompt was armed: queued after it
 * (seq_after), and for the real button's press one that began after it. The
 * press's time is taken at the input callback, which a debouncing driver
 * calls debounce_ms after the edge (gpio-keys), so a press that began just
 * before the prompt reaches the callback after it; such a press doesn't count
 * (as the ESP32 bounds a press). */
static inline bool mg_input_after_arm(bool seq_after, bool real, bool pressed, uint32_t t_ms,
				      uint32_t armed_ms, uint32_t debounce_ms)
{
	if (!seq_after) {
		return false;
	}
	return !(real && pressed) || (int32_t)(t_ms - armed_ms) >= (int32_t)debounce_ms;
}

/* At the queue's exit: injected events queued before the prompt are dropped;
 * real ones always go through, in order (a release must follow its press). */
static inline bool mg_input_keep_at_prompt(bool real, bool after_arm)
{
	return real || after_arm;
}

#endif /* MG_INPUT_POLICY_H */
