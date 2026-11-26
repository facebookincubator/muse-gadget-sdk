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

#ifndef HOST_SHIM_UTIL_H_
#define HOST_SHIM_UTIL_H_

#define ARG_UNUSED(x) (void)(x)
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
/* Every IS_ENABLED() branch is off on the host (the device key auth default,
 * the clips flag of a re-setup's commit, the gadget- offer); tests/unit runs
 * them with Zephyr's own. Host builds turn options on with #if defined(). */
#define IS_ENABLED(x) 0
#define BUILD_ASSERT(cond, msg) _Static_assert(cond, msg)

#endif
