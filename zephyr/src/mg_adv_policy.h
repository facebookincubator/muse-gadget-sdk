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

#ifndef MG_ADV_POLICY_H
#define MG_ADV_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Advertising interval: fast only in the 30 s
 * windows (boot, a press with no app connected, a disconnect), then slow;
 * before setup too: never fast without end (a gadget that advertised fast
 * forever while unprovisioned ran flat). slow_ms 0: always fast
 * (a build without a slow interval).
 */
static inline bool mg_adv_want_fast(uint32_t slow_ms, int64_t now_ms, int64_t fast_until_ms)
{
	return slow_ms == 0 || now_ms < fast_until_ms;
}

#endif /* MG_ADV_POLICY_H */
