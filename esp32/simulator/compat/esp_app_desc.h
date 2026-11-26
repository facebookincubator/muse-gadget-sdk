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
/* The simulator's stand-in for ESP-IDF's app description: the version shown on About and Status. */
typedef struct { char version[32]; char project_name[32]; } esp_app_desc_t;
static inline const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = { "1.0.0+sim", "muse-gadget" };
    return &d;
}
