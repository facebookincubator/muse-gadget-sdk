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

#include "mg_unlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mg_unlock_init(mg_unlock_t *u)
{
    memset(u, 0, sizeof(*u));
}

uint32_t mg_unlock_restart_take(mg_unlock_t *u, uint32_t *count, bool *clear)
{
    uint32_t ms = u->restart_ms;
    *count = u->restart_count;
    *clear = u->restart_clear;
    u->restart_ms = 0;
    u->restart_count = 0;
    u->restart_clear = false;
    return ms;
}

bool mg_unlock_auto_armed(const mg_unlock_t *u, int64_t now_us)
{
    return u->auto_until_us && now_us < u->auto_until_us;
}

void mg_unlock_pair_auto(mg_unlock_t *u, int64_t now_us, bool on)
{
    u->auto_until_us = on ? now_us + MG_UNLOCK_AUTO_US : 0;
}

bool mg_unlock_auto_take(mg_unlock_t *u, int64_t now_us)
{
    bool armed = mg_unlock_auto_armed(u, now_us);
    u->auto_until_us = 0;
    return armed;
}

bool mg_unlock_automation_word(const char *line)
{
    if (!strncmp(line, "!IN ", 4)) {
        const char *w = line + 4;
        return strcmp(w, "voice?") && strcmp(w, "wake?") && strcmp(w, "vib?") &&
               strcmp(w, "mic.gain?") && strcmp(w, "img?") && strcmp(w, "dev?") && strcmp(w, "spk.dsp") &&
               strcmp(w, "spk.dsp?") && strcmp(w, "ptt.minrise?") && strcmp(w, "vol?");
    }
    if (line[0] == '!' || line[0] == '?') {
        return true;
    }
    return strchr(line, '=') != NULL || !strcmp(line, "safe.off");
}

bool mg_unlock_owner_word(const char *line)
{
    return !strcmp(line, "!IN voice?") || !strcmp(line, "!IN img?");   /* voice activity, the shown image's path */
}

bool mg_unlock_owner_ok_in(const char *line, bool proven, bool set_up, bool release)
{
    return !mg_unlock_owner_word(line) || !release || !set_up || proven;
}

bool mg_unlock_remote_ok(const char *line, bool proven, bool safe_mode)
{
    if (proven || !mg_unlock_automation_word(line)) {
        return true;
    }
    return safe_mode && (!strcmp(line, "safe.off") || !strcmp(line, "safe=0"));
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool mg_unlock_pause_parse(const char *text, uint32_t *secs, uint8_t addr[6], bool *has_addr)
{
    uint32_t v = 0;
    size_t n = 0;
    for (; text[n] >= '0' && text[n] <= '9' && n < 4; n++) {
        v = v * 10 + (uint32_t)(text[n] - '0');
    }
    if (!n || v < 1 || v > 600 || (text[n] && text[n] != ',')) {
        return false;
    }
    *secs = v;
    *has_addr = false;
    if (!text[n]) {
        return true;
    }
    const char *a = text + n + 1;
    if (strlen(a) != 17) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        int hi = hexval(a[3 * i]), lo = hexval(a[3 * i + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && a[3 * i + 2] != ':')) {
            return false;
        }
        addr[i] = (uint8_t)(hi << 4 | lo);
    }
    *has_addr = true;
    return true;
}

bool mg_unlock_line(mg_unlock_t *u, const char *line, int64_t now_us, char *out, size_t cap)
{
    if (!strcmp(line, "?UNLOCK") || !strcmp(line, "!UNLOCK") || !strncmp(line, "!UNLOCK ", 8)) {
        snprintf(out, cap, "UNLOCK not needed");   /* the console is open */
        return true;
    }
    if (!strcmp(line, "!LOCK")) {
        u->restart_clear = true;
        snprintf(out, cap, "UNLOCK not needed (restart schedule cleared)");
        return true;
    }
    if (!strncmp(line, "!RESTART ", 9)) {
        char *end;
        unsigned long ms = strtoul(line + 9, &end, 10), count = 0;
        bool two = *end == ' ';
        if (two) count = strtoul(end + 1, &end, 10);
        if (!strcmp(line, "!RESTART 0")) {
            u->restart_clear = true;
            snprintf(out, cap, "RESTART schedule cleared");
        } else if (*end || (two && (count < 1 || count > 7 || ms < 30000 || ms > 600000)) ||
                   (!two && (ms < 1000 || ms > 600000))) {
            snprintf(out, cap, two ? "RESTART refused: <30000..600000 ms> <1..7>" : "RESTART refused: 1000..600000 ms");
        } else {
            u->restart_ms = (uint32_t)ms;
            u->restart_count = (uint32_t)count;
            if (two) {
                snprintf(out, cap, "RESTART every %lu ms, %lu times", ms, count);
            } else {
                snprintf(out, cap, "RESTART in %lu ms", ms);
            }
        }
        return true;
    }
    if (!strcmp(line, "!PAIR AUTO") || !strcmp(line, "!PAIR AUTO 1") || !strcmp(line, "!PAIR AUTO 0")) {
        bool on = strcmp(line, "!PAIR AUTO 0") != 0;
        mg_unlock_pair_auto(u, now_us, on);
        snprintf(out, cap, on ? "PAIR AUTO on (one pairing, 10 min)" : "PAIR AUTO off");
        return true;
    }
    return false;
}
