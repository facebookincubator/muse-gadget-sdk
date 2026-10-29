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

/* Drives the wake word session core for
 * test_muse_wakeword.py. Modes:
 *
 *   core <start_chunks> <end_silence_chunks> <max_chunks> <energy 0|1>
 *     stdin: one tick per line: <ev><voiced><ptt_down><ptt_up><playback><enabled>
 *       ev: '.' none, 'W' wake, 's' speech start, 'e' speech end; flags 0/1.
 *     stdout: "<state> <actions>" per tick, then a stats line. The ev
 *     column scripts the runner events directly; a voiced tick feeds a
 *     loud chunk, otherwise silence, and the energy helper judges it.
 *
 *   ring <cap_frames> <chunk_frames> <pushes> <pops>
 *     Pushes that many chunks of a running frame counter, pops, prints the
 *     popped frames, the snapshot, and the drop count.
 *
 *   energy <rms_threshold>
 *     stdin: one line of int16 values. Prints 1 if voiced, else 0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_wakeword.h"

#define CHUNK 320

static int core_main(int argc, char **argv)
{
    if (argc != 6) {
        return 2;
    }
    muse_wakeword_config_t cfg = {
        .start_timeout_chunks = (uint32_t)strtoul(argv[2], NULL, 10),
        .end_silence_chunks = (uint32_t)strtoul(argv[3], NULL, 10),
        .max_capture_chunks = (uint32_t)strtoul(argv[4], NULL, 10),
        .energy_endpointing = argv[5][0] == '1',
    };
    muse_wakeword_core_t core;
    muse_wakeword_core_init(&core, &cfg);

    /* Read all ticks first: their ev column is the event script. */
    char line[64];
    size_t cap = 1024, n = 0;
    char(*ticks)[8] = malloc(cap * sizeof(*ticks));
    ww_event_t *script = malloc(cap * sizeof(*script));
    while (fgets(line, sizeof(line), stdin)) {
        if (strlen(line) < 6) {
            continue;
        }
        if (n == cap) {
            cap *= 2;
            ticks = realloc(ticks, cap * sizeof(*ticks));
            script = realloc(script, cap * sizeof(*script));
        }
        memcpy(ticks[n], line, 6);
        ticks[n][6] = '\0';
        script[n] = line[0] == 'W'   ? WW_EVENT_WAKE
                    : line[0] == 's' ? WW_EVENT_SPEECH_START
                    : line[0] == 'e' ? WW_EVENT_SPEECH_END
                                     : WW_EVENT_NONE;
        n++;
    }
    int16_t pcm[CHUNK];
    for (size_t i = 0; i < n; i++) {
        bool voiced_flag = ticks[i][1] == '1';
        int16_t amp = voiced_flag ? 4000 : 0;
        for (size_t j = 0; j < CHUNK; j++) {
            pcm[j] = amp;
        }
        muse_wakeword_input_t in = {
            /* Scripted events are gated by the listening opt-in, as a
             * runner's feed() results are; playback suppression stays
             * with the core, which is what these tests exercise. */
            .event = ticks[i][5] == '1' ? script[i] : WW_EVENT_NONE,
            .voiced = muse_wakeword_energy_voiced(pcm, CHUNK, MUSE_WAKEWORD_ENERGY_RMS_DEFAULT),
            .ptt_down = ticks[i][2] == '1',
            .ptt_up = ticks[i][3] == '1',
            .playback = ticks[i][4] == '1',
            .enabled = ticks[i][5] == '1',
        };
        uint32_t actions = muse_wakeword_core_tick(&core, &in);
        printf("%d %u\n", (int)muse_wakeword_core_state(&core), (unsigned)actions);
    }
    muse_wakeword_stats_t st = muse_wakeword_core_stats(&core);
    printf("stats %u %u %u %u %u\n", (unsigned)st.wakes, (unsigned)st.cancels, (unsigned)st.submits,
           (unsigned)st.conversions, (unsigned)st.suppressed_wakes);
    free(ticks);
    free(script);
    return 0;
}

static int ring_main(int argc, char **argv)
{
    if (argc != 6) {
        return 2;
    }
    size_t cap_frames = strtoul(argv[2], NULL, 10);
    size_t chunk = strtoul(argv[3], NULL, 10);
    size_t pushes = strtoul(argv[4], NULL, 10);
    size_t pops = strtoul(argv[5], NULL, 10);
    int16_t *storage = malloc(cap_frames * sizeof(int16_t));
    int16_t *buf = malloc(chunk * sizeof(int16_t));
    muse_ww_ring_t ring;
    muse_ww_ring_init(&ring, storage, cap_frames);
    size_t next = 0;
    for (size_t p = 0; p < pushes; p++) {
        for (size_t j = 0; j < chunk; j++) {
            buf[j] = (int16_t)next++;
        }
        muse_ww_ring_push(&ring, buf, chunk);
    }
    printf("popped");
    for (size_t p = 0; p < pops; p++) {
        size_t got = muse_ww_ring_pop(&ring, buf, chunk);
        for (size_t j = 0; j < got; j++) {
            printf(" %d", buf[j]);
        }
        if (!got) {
            printf(" -");
        }
    }
    printf("\nsnapshot");
    int16_t *snap = malloc(cap_frames * sizeof(int16_t));
    size_t n = muse_ww_ring_snapshot(&ring, snap, cap_frames);
    for (size_t j = 0; j < n; j++) {
        printf(" %d", snap[j]);
    }
    printf("\ndrops %u fill %u\n", (unsigned)muse_ww_ring_drops(&ring), (unsigned)muse_ww_ring_fill(&ring));
    free(storage);
    free(buf);
    free(snap);
    return 0;
}

static int energy_main(int argc, char **argv)
{
    if (argc != 3) {
        return 2;
    }
    int16_t pcm[8192];
    size_t n = 0;
    long v;
    while (n < sizeof(pcm) / sizeof(pcm[0]) && scanf("%ld", &v) == 1) {
        pcm[n++] = (int16_t)v;
    }
    printf("%d\n", muse_wakeword_energy_voiced(pcm, n, strtol(argv[2], NULL, 10)) ? 1 : 0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    if (!strcmp(argv[1], "core")) {
        return core_main(argc, argv);
    }
    if (!strcmp(argv[1], "ring")) {
        return ring_main(argc, argv);
    }
    if (!strcmp(argv[1], "energy")) {
        return energy_main(argc, argv);
    }
    return 2;
}
