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

#include "esp_err.h"
#include "esp_iris.h"
#include "iris_ota_support.h"

/*
 * ESP-Iris on the ESP-Mosaico's USB-C port, as Espressif's application
 * template starts it: the board's Vibe Mode firmware switches a running app
 * back to Vibe Mode over it to install the next build, a crash loop falls back
 * to Vibe Mode, and it marks a newly installed app healthy a few seconds in.
 */
void muse_mosaico_iris_start(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    iris_ota_support_start();
}
