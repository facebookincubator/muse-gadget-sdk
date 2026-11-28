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
 * The Nordic UART log mirror's budget (the console mirror never
 * blocks any work, drops are counted, and its notifies are capped). The
 * mirror shares the stack's msys blocks with the protocol (Control replies,
 * buffer_updates, video credits, ATT write responses): drained greedily, up
 * to four notifies a pass with nothing else holding it back, a busy stream's
 * log lines could take the blocks those need. So the mirror sends only while
 * it has fewer than MG_NUS_INFLIGHT_MAX notifies in the stack and more than
 * MG_NUS_MSYS_KEEP blocks stay free for the protocol, after the protocol's own
 * sends; a line with no room in its ring is dropped and counted (nus_dropped
 * on the status line).
 */
#pragma once

#include <stdbool.h>

#define MG_NUS_INFLIGHT_MAX 2
#define MG_NUS_MSYS_KEEP 8

static inline bool mg_nus_may_send(unsigned inflight, int msys_free)
{
    return inflight < MG_NUS_INFLIGHT_MAX && msys_free > MG_NUS_MSYS_KEEP;
}
