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

// Host-test stand-in. The harness can pick another version with
// -DFAKE_IDF_MAJOR/-DFAKE_IDF_MINOR to check ble_coex.c's version guard.
#pragma once

#ifndef FAKE_IDF_MAJOR
#define FAKE_IDF_MAJOR 6
#endif
#ifndef FAKE_IDF_MINOR
#define FAKE_IDF_MINOR 0
#endif
#define ESP_IDF_VERSION_VAL(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(FAKE_IDF_MAJOR, FAKE_IDF_MINOR, 1)
