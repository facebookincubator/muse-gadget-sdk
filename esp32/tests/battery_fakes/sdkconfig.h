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

/* A board with power management and profiling, unless the build asks for one
 * without profiling (-DMUSE_TEST_NO_PM_PROFILING) or without power management
 * at all (-DMUSE_TEST_NO_PM), like most boards with the full UI. */
#ifndef MUSE_TEST_NO_PM
#define CONFIG_PM_ENABLE 1
#define CONFIG_PM_LIGHT_SLEEP_CALLBACKS 1
#ifndef MUSE_TEST_NO_PM_PROFILING
#define CONFIG_PM_PROFILING 1
#endif
#endif
