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
 * set_mic_gain's level (mgcommands.h: 1-100, a level, not a register
 * value or dB) onto a board's capture gain in dB. Portable: no IDF.
 *
 * Level 50 is the board's tuned gain (tuned_db, MUSE_MIC_GAIN_TUNED): what it
 * captures at when no client set a level. 1-50 map linearly in dB from the
 * lowest usable gain (floor_db, the codec's lowest step, 0 dB) up to tuned;
 * 50-100 from tuned up to the highest gain at which normal speech about
 * 30 cm away doesn't clip (ceil_db, muse_board_t.mic_gain_speech_max_db),
 * which equals tuned on a board with no headroom (then 50-100 all give tuned).
 * Each half rounds to the nearest step_db above floor_db, never past its
 * ends: monotonic, 50 exactly tuned when tuned is on a step. Levels outside
 * 1-100 are refused before this (invalid_value, mg_proto.c); here they clamp.
 */

#define MG_MIC_GAIN_STEP_DB 3   /* esp_codec_dev's PGAs (ES7210, ES7243E) step 3 dB; ES8311 6 */
#define MG_MIC_GAIN_LEVEL_TUNED 50

/* lo + (hi - lo) * num / den, rounded to the nearest step above floor_db, kept within lo..hi. */
static inline int mg_mic_gain_lerp(int lo, int hi, int num, int den, int floor_db, int step_db)
{
    /* round(lo + (hi - lo) * num / den) in integers, then onto the step grid */
    int db10 = lo * 10 + ((hi - lo) * num * 10 * 2 + den) / (2 * den);
    int steps = ((db10 - floor_db * 10) * 2 + 10 * step_db) / (20 * step_db);
    int db = floor_db + steps * step_db;
    while (db > hi && db - step_db >= floor_db) {
        db -= step_db;   /* the nearest step rounded over the end: the one under it */
    }
    while (db < lo && db + step_db <= hi) {
        db += step_db;
    }
    return db;
}

static inline int mg_mic_gain_db(int level, int floor_db, int tuned_db, int ceil_db, int step_db)
{
    if (step_db <= 0) {
        step_db = 1;
    }
    if (tuned_db < floor_db) {
        tuned_db = floor_db;
    }
    if (ceil_db < tuned_db) {
        ceil_db = tuned_db;   /* no headroom: 50-100 stay at tuned */
    }
    if (level < 1) {
        level = 1;
    }
    if (level > 100) {
        level = 100;
    }
    if (level <= MG_MIC_GAIN_LEVEL_TUNED) {
        return mg_mic_gain_lerp(floor_db, tuned_db, level - 1, MG_MIC_GAIN_LEVEL_TUNED - 1, floor_db, step_db);
    }
    return mg_mic_gain_lerp(tuned_db, ceil_db, level - MG_MIC_GAIN_LEVEL_TUNED, 100 - MG_MIC_GAIN_LEVEL_TUNED,
                            floor_db, step_db);
}
