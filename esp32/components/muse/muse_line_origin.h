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
 * Where a command line came from, for the release lock on state-changing words: a release build gates them only
 * when they arrive wirelessly. The entry point says it, never inferred: only the physical UART / USB-CDC console
 * task passes MUSE_LINE_WIRED. The Muse BLE characteristic, the Nordic UART, any relay or wireless shell, and
 * anything that doesn't know pass MUSE_LINE_WIRELESS, the zero value, so a missing origin fails closed.
 */
#pragma once

typedef enum {
    MUSE_LINE_WIRELESS = 0,
    MUSE_LINE_WIRED = 1,
} muse_line_origin_t;

_Static_assert(MUSE_LINE_WIRELESS == 0, "an unknown origin must be wireless (fail closed)");
