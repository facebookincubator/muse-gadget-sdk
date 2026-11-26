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

/* Host harness for components/secure_random/secure_random.c: the BT
 * controller's status and Wi-Fi's start faked, the RNG's draws counted.
 * stdin lines: "bt on|off", "wifi on|off", "draw <n>"; prints each result. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "secure_random.h"
#if WITH_WIFI_FAKE
#include "esp_wifi.h"
#endif

int test_draws;
static bool bt_on, wifi_on;

esp_bt_controller_status_t esp_bt_controller_get_status(void)
{
    return bt_on ? ESP_BT_CONTROLLER_STATUS_ENABLED : ESP_BT_CONTROLLER_STATUS_INITED;
}

#if WITH_WIFI_FAKE
/* Left out of the BLE-only build: secure_random.c must not name it there. */
esp_err_t esp_wifi_get_max_tx_power(int8_t *power)
{
    *power = 80;
    return wifi_on ? ESP_OK : ESP_ERR_WIFI_NOT_STARTED;
}
#endif

int main(void)
{
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strcmp(line, "bt on")) bt_on = true;
        else if (!strcmp(line, "bt off")) bt_on = false;
        else if (!strcmp(line, "wifi on")) wifi_on = true;
        else if (!strcmp(line, "wifi off")) wifi_on = false;
        else if (!strncmp(line, "draw ", 5)) {
            uint8_t buf[32];
            memset(buf, 0x5C, sizeof(buf));
            size_t n = (size_t)atoi(line + 5);
            int before = test_draws;
            bool ok = secure_random(buf, n);
            size_t zero = 0, pattern = 0;
            for (size_t i = 0; i < n; i++) {
                zero += buf[i] == 0;
                pattern += buf[i] == 0xA5;
            }
            printf("draw %d drew=%d zero=%zu rng=%zu untouched=%d\n", ok, test_draws - before, zero, pattern,
                   n < sizeof(buf) ? buf[n] == 0x5C : 1);
        }
        fflush(stdout);
    }
    return 0;
}
