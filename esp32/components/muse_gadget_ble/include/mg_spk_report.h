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

/*
 * Which speaker_volume changes the client hears (mgcommands.h, Settings
 * reports): every change made on the device (a key,
 * a slider, a mute, the call's controls) is reported unsolicited, and a
 * change the client made itself with set_settings is not reported back.
 *
 * A client's set can take two settings writes (the level, then the speaker
 * switch), and each one notifies: in between, the level the client would
 * read is neither the old one nor the new one (muted at 40, the client asks
 * for 70: the level goes to 70 while the speaker is still off, so 0). So the
 * client's set is bracketed: nothing it writes is reported, and once it's
 * done the level is compared with what the client asked for. A device change
 * that landed meanwhile (another task) shows as a difference and is
 * reported then.
 *
 * Pure (no IDF): the caller serializes the calls and sends the report.
 */

#include <stdbool.h>

typedef struct {
    int reported;       /* the level the client last saw or set (-1: none yet) */
    int client_depth;   /* > 0 while the client's own set is being applied */
    int client_value;   /* ...and the level it asked for */
} mg_spk_report_t;

static inline void mg_spk_report_init(mg_spk_report_t *r)
{
    r->reported = -1;
    r->client_depth = 0;
    r->client_value = -1;
}

/* The client's set_settings(speaker_volume, level) starts. */
static inline void mg_spk_client_begin(mg_spk_report_t *r, int level)
{
    r->client_depth++;
    r->client_value = level;
}

/* ...and ends, with the level the client would read now. True: report it (a
 * device change landed while the client's was applied). */
static inline bool mg_spk_client_end(mg_spk_report_t *r, int now)
{
    if (r->client_depth > 0) {
        r->client_depth--;
    }
    r->reported = r->client_value;
    if (now == r->reported) {
        return false;
    }
    r->reported = now;
    return true;
}

/* The level or the speaker switch changed (the settings' notify), now the
 * level the client would read. True: report it. */
static inline bool mg_spk_changed(mg_spk_report_t *r, int now)
{
    if (r->client_depth > 0 || now == r->reported) {
        return false;
    }
    r->reported = now;
    return true;
}
