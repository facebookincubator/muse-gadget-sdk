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

#include "ww_runner.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bench triggers for ww_runner_bench.c, the on-device stand-in used
 * to bring up and debug the audio tap, wake task, and turn plumbing
 * without running the ExecuTorch runner. It does no speech
 * detection: a wake comes from an explicit trigger below or from a
 * loud-then-quiet energy burst (a clap), shaped by these constants,
 * which tests/test_muse_wakeword.py pins.
 */
#define WW_BENCH_LOUD_RMS 4000          /* sustained chunk: about -18 dBFS */
#define WW_BENCH_PEAK_MIN 3000          /* transient peak: about -21 dBFS */
#define WW_BENCH_SUBFRAME_RMS 1200      /* in any 5 ms slice: about -29 dBFS */
#define WW_BENCH_SUBFRAME_FRAMES 80     /* 5 ms at 16 kHz */
#define WW_BENCH_BURST_MAX_CHUNKS 8     /* longer is a shout/noise, not a clap */
#define WW_BENCH_QUIET_CHUNKS 2
#define WW_BENCH_COOLDOWN_CHUNKS 50     /* 1 s */

/* The next feed returns WW_EVENT_WAKE once (serial "wake=1"). */
void ww_runner_bench_trigger(ww_runner_t *r);
/* The next two feeds return WAKE then SPEECH_START, so a whole
 * hands-free turn can be driven over serial with nobody speaking. */
void ww_runner_bench_trigger_test(ww_runner_t *r);

#ifdef __cplusplus
}
#endif
