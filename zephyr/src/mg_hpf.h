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


#ifndef MG_HPF_H_
#define MG_HPF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * One-pole DC-blocking high-pass, y[n] = x[n] - x[n-1] + a * y[n-1] with
 * a = 1 - 2 pi fc / fs, for the microphone ahead of the gain. A PDM mic
 * starts with a large DC offset that drifts for seconds; this removes it.
 * The first sample after mg_hpf_reset() primes the state (x[-1] = x[0],
 * y[-1] = 0), so a capture starts at zero instead of with a settling step.
 */
struct mg_hpf {
	float a;    /* < 0: off */
	float x1;
	float y1;
	bool primed;
};

/* fc_hz 0 turns it off (samples pass unchanged). */
void mg_hpf_init(struct mg_hpf *f, unsigned int fc_hz, unsigned int rate_hz);
/* A new capture: the next sample primes the filter. */
void mg_hpf_reset(struct mg_hpf *f);
void mg_hpf_run(struct mg_hpf *f, int16_t *pcm, size_t n);

#endif
