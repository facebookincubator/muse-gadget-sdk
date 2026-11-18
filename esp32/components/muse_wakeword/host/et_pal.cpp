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

/*
 * ExecuTorch's platform layer for the runner off the chip (host builds
 * such as the host tests): the ESP-IDF component ships only its ESP-IDF
 * one. Logging is compiled out (ET_LOG_ENABLED=0).
 */

#include <cstdlib>
#include <ctime>

#include <executorch/runtime/platform/platform.h>

extern "C" {

void et_pal_init(void) {}

ET_NORETURN void et_pal_abort(void)
{
    std::abort();
}

et_timestamp_t et_pal_current_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (et_timestamp_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

et_tick_ratio_t et_pal_ticks_to_ns_multiplier(void)
{
    return {1, 1};
}

void et_pal_emit_log_message(et_timestamp_t, et_pal_log_level_t, const char *, const char *, size_t,
                             const char *, size_t)
{
}

void *et_pal_allocate(size_t size)
{
    return std::malloc(size);
}

void et_pal_free(void *ptr)
{
    std::free(ptr);
}

} // extern "C"
