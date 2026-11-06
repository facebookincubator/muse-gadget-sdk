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

/* A Muse build with calls on, as Kconfig would generate it (off with
 * MUSE_TEST_CALLS_OFF, where Kconfig leaves the symbols out). */
#pragma once
#ifndef MUSE_TEST_CALLS_OFF
#define CONFIG_MUSE_REALTIME_VOICE 1
#define CONFIG_MUSE_REALTIME_VOICE_APP_ID "hatch-web"
#define CONFIG_MUSE_REALTIME_VOICE_IDLE_S 45
#define CONFIG_MUSE_REALTIME_VOICE_MAX_S 900
#endif
