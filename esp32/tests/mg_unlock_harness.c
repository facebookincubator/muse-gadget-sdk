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

/* Host harness for mg_unlock.c (the console's open bench words): stdin
 * lines "t <us>", "auto", "take", "restart", "arm" / "disarm" (the USB
 * console's >pair.auto / >pair.auto=0), "pause <text>" (ble.pause's argument),
 * "auto? <line>" (whether a Nordic UART line changes state), "remote <proven>
 * <safe> <line>" (whether a release build runs it),
 * or a Nordic UART line after "> ";
 * prints each reply. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mg_unlock.h"

int main(void)
{
    mg_unlock_t u;
    mg_unlock_init(&u);
    long long now = 1;
    char line[512], out[160];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "t ", 2)) now = atoll(line + 2);
        else if (!strcmp(line, "auto")) printf("auto %d\n", mg_unlock_auto_armed(&u, now));
        else if (!strcmp(line, "take")) printf("take %d\n", mg_unlock_auto_take(&u, now));
        else if (!strncmp(line, "pause ", 6)) {
            uint32_t secs = 0; uint8_t a[6] = { 0 }; bool has = false;
            bool ok = mg_unlock_pause_parse(line + 6, &secs, a, &has);
            if (!ok) printf("pause no\n");
            else if (!has) printf("pause %u\n", (unsigned)secs);
            else printf("pause %u %02x%02x%02x%02x%02x%02x\n", (unsigned)secs, a[0], a[1], a[2], a[3], a[4], a[5]);
        }
        else if (!strncmp(line, "auto? ", 6)) printf("%s\n", mg_unlock_automation_word(line + 6) ? "automation" : "diagnostic");
        else if (!strncmp(line, "remote ", 7) && strlen(line) > 11)
            printf("%s\n", mg_unlock_remote_ok(line + 11, line[7] == '1', line[9] == '1') ? "runs" : "locked");
        else if (!strcmp(line, "arm")) mg_unlock_pair_auto(&u, now, true);
        else if (!strcmp(line, "disarm")) mg_unlock_pair_auto(&u, now, false);
        else if (!strcmp(line, "restart")) {
            uint32_t count; bool clear;
            uint32_t ms = mg_unlock_restart_take(&u, &count, &clear);
            printf("restart %u %u %d\n", (unsigned)ms, (unsigned)count, clear);
        }
        else if (!strncmp(line, "> ", 2)) printf("%s\n", mg_unlock_line(&u, line + 2, now, out, sizeof(out)) ? out : "(not mine)");
        fflush(stdout);
    }
    return 0;
}
