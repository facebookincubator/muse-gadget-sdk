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

/* confirm_disc_harness.c's stand-in for muse_restart's muse_local_press.h: the update prompt's press (the
 * harness defines them). */
#pragma once
#include <stdbool.h>
bool muse_local_press_note(const char *what);
void muse_local_press_cancel(const char *what);
bool muse_local_press_prompting(void);
