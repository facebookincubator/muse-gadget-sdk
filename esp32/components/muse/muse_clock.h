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

/*
 * The gadget's wall clock, set by the phone (mgcommands.h, Clock: device
 * actions set_unix_time / get_unix_time). The time goes into the system clock
 * (it keeps counting across disconnects and software restarts); the phone's
 * UTC offset and 12/24-hour choice are kept for a clock on the face and
 * persist in NVS when the caller may store them.
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* From the phone: unix_ms (UTC), and the offset / flags when it sent them.
 * store false: they apply in RAM only (an unproven central on a set-up gadget). */
void muse_clock_set(int64_t unix_ms, bool has_offset, int16_t offset_min, bool has_flags, uint8_t flags, bool store);
/* The time now (INT64_MIN: never set since power-on), the offset and flags. */
void muse_clock_get(int64_t *unix_ms, int16_t *offset_min, uint8_t *flags);
/* Local time for a face: false until the phone has set it. */
bool muse_clock_local(struct tm *out);
/* A 24-hour clock (mg_clock_flag_24_hour). */
bool muse_clock_24h(void);

#ifdef __cplusplus
}
#endif
