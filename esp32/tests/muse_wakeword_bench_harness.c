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

/* Drives the bench runner (ww_runner_bench.c) for test_muse_wakeword.py.
 * argv[1] is a pattern; one character per step:
 *   L feed a loud chunk      Q feed a quiet chunk
 *   C feed a clap transient  c feed a soft 5 ms burst
 *   T trigger, then feed Q   W trigger-test, then feed Q
 *   0/1 listening off/on     s/u suppressed on/off     R reset
 * Prints the event from every feed, space-separated. */

#include <stdio.h>
#include <string.h>

#include "ww_runner.h"
#include "ww_runner_bench.h"

#define CHUNK 320

static ww_runner_t *s_runner;
static int s_printed;

static void feed(int16_t amp)
{
    int16_t pcm[CHUNK];
    for (size_t i = 0; i < CHUNK; i++) {
        pcm[i] = amp;
    }
    printf("%s%d", s_printed++ ? " " : "", (int)ww_runner_feed(s_runner, pcm, CHUNK));
}

/* A short hand-clap transient: 20 very loud samples (1.25 ms) and then
 * silence in the same 20 ms chunk. Its whole-chunk RMS is only 3000,
 * below a sustained-sound threshold, while its first 5 ms is loud. */
static void feed_clap(void)
{
    int16_t pcm[CHUNK] = { 0 };
    for (size_t i = 0; i < 20; i++) {
        pcm[i] = 12000;
    }
    printf("%s%d", s_printed++ ? " " : "", (int)ww_runner_feed(s_runner, pcm, CHUNK));
}

/* Energy in one 5 ms slice, but too little peak for a clap. */
static void feed_soft_burst(void)
{
    int16_t pcm[CHUNK] = { 0 };
    for (size_t i = 0; i < 80; i++) {
        pcm[i] = 2000;
    }
    printf("%s%d", s_printed++ ? " " : "", (int)ww_runner_feed(s_runner, pcm, CHUNK));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    s_runner = ww_runner_create(NULL);
    if (!s_runner) {
        return 3;
    }
    for (const char *p = argv[1]; *p; p++) {
        switch (*p) {
        case 'L': feed(8000); break;
        case 'Q': feed(0); break;
        case 'C': feed_clap(); break;
        case 'c': feed_soft_burst(); break;
        case 'T':
            ww_runner_bench_trigger(s_runner);
            feed(0);
            break;
        case 'W':
            ww_runner_bench_trigger_test(s_runner);
            feed(0);
            break;
        case '0': ww_runner_set_listening(s_runner, false); break;
        case '1': ww_runner_set_listening(s_runner, true); break;
        case 's': ww_runner_set_suppressed(s_runner, true); break;
        case 'u': ww_runner_set_suppressed(s_runner, false); break;
        case 'R': ww_runner_reset(s_runner); break;
        default: return 2;
        }
    }
    putchar('\n');
    ww_runner_destroy(s_runner);
    return 0;
}
